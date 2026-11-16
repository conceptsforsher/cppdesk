// test_server.cpp — tests for the server foundation (service framework,
// ConnInner queue, cursor cache, Server registry, zombie reaper), plus the
// step-15 additions: the queue's receiver-drop semantics and wake pipe, the
// video_service state (latency/quality maps, display bookkeeping behind the
// scrap seam) and the input_service predicates.
// Hermetic: no network/files; the reaper test uses fork+_exit only.

#include <rustdesk/audio_service.hpp>
#include <rustdesk/clipboard_service.hpp>
#include <rustdesk/common.hpp>
#include <rustdesk/connection.hpp>
#include <rustdesk/input_service.hpp>
#include <rustdesk/server.hpp>
#include <rustdesk/service.hpp>
#include <rustdesk/video_service.hpp>

#include <poll.h>

#include <cstdint>
#include <cstdio>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

using rustdesk::ConnInner;
using rustdesk::MessagePtr;

// Fake subscriber: proves ServiceTmpl works for non-ConnInner T by wrapping
// one (all traffic observable through the shared queue, like GenericService).
struct FakeSub {
    ConnInner inner;
    explicit FakeSub(const ConnInner& c) : inner(c) {}
    int get_id() const { return inner.get_id(); }
    void send(const MessagePtr& msg) { inner.send(msg); }
};

using FakeService = rustdesk::ServiceTmpl<FakeSub>;

ConnInner make_conn(int32_t id) {
    ConnInner c;
    c.id = id;
    c.tx = std::make_shared<rustdesk::ConnQueue>();
    return c;
}

MessagePtr make_msg() {
    auto m = std::make_shared<hbb::Message>();
    m->mutable_misc()->set_refresh_video(true);
    return m;
}

void test_queue() {
    rustdesk::ConnQueue q;
    CHECK(q.size() == 0);
    auto m1 = make_msg();
    auto m2 = make_msg();
    q.send(m1);
    q.send(m2);
    CHECK(q.size() == 2);
    // Drain preserves order + shared identity (Arc parity).
    auto out = q.drain();
    CHECK(out.size() == 2);
    CHECK(out[0].get() == m1.get() && out[1].get() == m2.get());
    CHECK(q.size() == 0);
    // Empty queue: immediate nullopt (no wait).
    CHECK(!q.wait_receive(0).has_value());
    // Close unblocks waiters.
    q.close();
    CHECK(!q.wait_receive(50).has_value());

    // Default ConnInner (no queue): silent no-op, never throws.
    ConnInner bare;
    CHECK(bare.get_id() == 0);
    bare.send(m1);

    // ConnInner fan-out through the queue.
    const ConnInner c = make_conn(7);
    CHECK(c.get_id() == 7);
    c.send(m1);
    auto got = c.tx->drain();
    CHECK(got.size() == 1 && got[0].get() == m1.get());
}

void test_service_framework() {
    FakeService svc("fake", false);
    CHECK(svc.name() == "fake");
    CHECK(!svc.has_subscribes() && svc.ok() == false);  // ok() needs active+subs
    CHECK(svc.active());

    const ConnInner a = make_conn(1);
    const ConnInner b = make_conn(2);
    svc.on_subscribe(a);
    svc.on_subscribe(a);  // duplicate ignored
    svc.on_subscribe(b);
    CHECK(svc.is_subed(1) && svc.is_subed(2));
    CHECK(svc.has_subscribes() && svc.ok());

    // Fan-out shares one Arc (pointer identity, like upstream).
    auto m = make_msg();
    svc.send(*m);
    auto fa = a.tx->drain();
    auto fb = b.tx->drain();
    CHECK(fa.size() == 1 && fb.size() == 1);
    CHECK(fa[0].get() == fb[0].get());

    // send_without skips exactly one subscriber.
    svc.send_without(*m, 1);
    CHECK(a.tx->drain().empty());
    CHECK(b.tx->drain().size() == 1);

    svc.on_unsubscribe(1);
    CHECK(!svc.is_subed(1) && svc.is_subed(2));
    svc.on_unsubscribe(2);
    CHECK(!svc.has_subscribes());

    // Snapshot path (need_snapshot=true): newcomers wait in new_subscribes,
    // get snapshot sends, then promote on scope exit.
    FakeService snap("snap", true);
    snap.on_subscribe(a);
    CHECK(!snap.is_subed(1));  // not in the MAIN set yet
    bool saw_new = false;
    snap.snapshot([&](rustdesk::ServiceSwap<FakeSub> swap) {
        CHECK(!swap.has_subscribes());  // main set still empty (verbatim quirk)
        auto sm = make_msg();
        swap.send(*sm);
        CHECK(a.tx->drain().size() == 1);  // new_subscribes got it
        saw_new = true;
    });
    CHECK(saw_new);
    CHECK(snap.is_subed(1));  // promoted by ServiceSwap drop

    svc.join();
    snap.join();
    CHECK(!svc.active());
}

void test_cursor_cache() {
    const ConnInner c = make_conn(3);
    rustdesk::input_service::MouseCursorSub sub(c);

    hbb::Message full;
    hbb::CursorData* cd = full.mutable_cursor_data();
    cd->set_id(7);
    cd->set_width(32);
    auto full_ptr = std::make_shared<hbb::Message>(full);

    sub.send(full_ptr);  // first delivery: full message forwarded...
    auto got = c.tx->drain();
    CHECK(got.size() == 1);
    CHECK(got[0]->has_cursor_data() && got[0]->cursor_data().id() == 7);

    sub.send(full_ptr);  // ...second delivery: cached id-only message.
    got = c.tx->drain();
    CHECK(got.size() == 1);
    CHECK(!got[0]->has_cursor_data());
    CHECK(got[0]->has_cursor_id() && got[0]->cursor_id() == 7);

    // Non-cursor traffic passes through untouched.
    auto misc = make_msg();
    sub.send(misc);
    got = c.tx->drain();
    CHECK(got.size() == 1 && got[0].get() == misc.get());

    // Service shells carry the upstream names.
    CHECK(std::string(rustdesk::audio_service::kName) == "audio");
    CHECK(std::string(rustdesk::video_service::kName) == "video");
    CHECK(std::string(rustdesk::clipboard_service::kName) == "clipboard");
    CHECK(std::string(rustdesk::input_service::kNameCursor) == "mouse_cursor");
    CHECK(std::string(rustdesk::input_service::kNamePos) == "mouse_pos");
}

void test_server_registry() {
    auto server = rustdesk::Server::create();
    for (const char* name : {"audio", "video", "clipboard", "mouse_cursor", "mouse_pos"}) {
        CHECK(server->find_service(name) != nullptr);
    }
    CHECK(server->find_service("nope") == nullptr);

    const ConnInner c = make_conn(11);
    server->add_connection(c, {"audio"});  // audio skipped via noperms
    CHECK(server->has_connection(11));
    CHECK(!server->find_service("audio")->is_subed(11));
    // Snapshot services (video/mouse_cursor) stage into new_subscribes on
    // subscribe — is_subed stays false until a snapshot() promotes them
    // (mechanics covered in test_service_framework; here the observable part).
    CHECK(!server->find_service("video")->is_subed(11));
    CHECK(!server->find_service("mouse_cursor")->is_subed(11));
    // Non-snapshot services land in the main set immediately.
    CHECK(server->find_service("clipboard")->is_subed(11));
    CHECK(server->find_service("mouse_pos")->is_subed(11));

    // Server::subscribe toggle path on a directly-subscribed service.
    server->subscribe("clipboard", c, false);
    CHECK(!server->find_service("clipboard")->is_subed(11));
    server->subscribe("clipboard", c, true);
    CHECK(server->find_service("clipboard")->is_subed(11));
    server->subscribe("clipboard", c, true);  // idempotent (is_subed == sub)
    CHECK(server->find_service("clipboard")->is_subed(11));
    server->subscribe("missing", c, true);  // unknown service: silent no-op

    server->remove_connection(c);
    CHECK(!server->has_connection(11));
    CHECK(!server->find_service("clipboard")->is_subed(11));
    CHECK(!server->find_service("mouse_pos")->is_subed(11));

    CHECK(server->next_id() == 1);
    CHECK(server->next_id() == 2);
}

void test_zombie_reaper() {
#if defined(__unix__) || defined(__APPLE__)
    rustdesk::check_zombie();  // detached 100ms sweeper (idempotent start)
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        _exit(0);  // child exits immediately
    }
    rustdesk::track_child_pid(static_cast<intptr_t>(pid));
    bool reaped = false;
    for (int i = 0; i < 40; ++i) {  // up to ~4s
        {
            std::unique_lock l(rustdesk::child_lock());
            const auto& pids = rustdesk::child_processes();
            reaped = std::find(pids.begin(), pids.end(), static_cast<intptr_t>(pid)) == pids.end();
        }
        if (reaped) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(reaped);
#else
    CHECK(true);  // Windows reaper deferred to the platform step
#endif
}

bool fd_readable(int fd) {
    struct pollfd p {
        fd, POLLIN, 0
    };
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

void test_queue_receiver_side() {
    // try_recv keeps the send Instant (the loop measures latency from it).
    rustdesk::ConnQueue q;
    const auto before = std::chrono::steady_clock::now();
    auto m1 = make_msg();
    q.send(m1);
    // The wake pipe made after the send still reports the queued item.
    const int fd = q.wake_fd();
    CHECK(fd_readable(fd));
    q.clear_wake();
    CHECK(!fd_readable(fd));
    q.send(make_msg());
    CHECK(fd_readable(fd));
    const auto item = q.try_recv();
    CHECK(item && item->message.get() == m1.get());
    CHECK(item && item->instant >= before);
    CHECK(q.try_recv().has_value());
    CHECK(!q.try_recv().has_value());

    // close() is the receiver going away: queued items are dropped, and
    // later sends (ConnInner::send included) are discarded, not buffered.
    const ConnInner c = make_conn(9);
    c.send(make_msg());
    CHECK(c.tx->size() == 1);
    c.tx->close();
    CHECK(c.tx->size() == 0);
    c.send(make_msg());
    CHECK(c.tx->size() == 0);
    CHECK(!c.tx->try_recv().has_value());
}

void test_video_service_state() {
    namespace vs = rustdesk::video_service;
    // Latency maps: <= 0 removes; get_latency is the max internal one.
    CHECK(vs::get_latency() == 0);
    vs::update_internal_latency(1001, 50);
    vs::update_internal_latency(1002, 70);
    CHECK(vs::get_latency() == 70);
    vs::update_internal_latency(1002, 0);
    CHECK(vs::get_latency() == 50);
    vs::update_internal_latency(1001, -5);
    CHECK(vs::get_latency() == 0);
    vs::update_test_latency(1001, 30);  // separate map; no getter upstream
    vs::update_test_latency(1001, 0);

    // convert_quality: presets map to (bitrate << 8 | quantizer).
    CHECK(vs::convert_quality(hbb::Balanced) == (66 << 8 | 12));
    CHECK(vs::convert_quality(hbb::Low) == (50 << 8 | 18));
    CHECK(vs::convert_quality(hbb::Best) == (100 << 8 | 12));
    CHECK(vs::convert_quality(hbb::ImageQualityNotSet) == 0);  // bitrate 0
    // Custom: bitrate * 2, (100 - quantizer) * 36 / 100.
    CHECK(vs::convert_quality(50 << 8 | 50) == (100 << 8 | 18));
    // Quantizer above 100 goes negative and swallows the bitrate bits
    // (i32 `|` with a negative operand), verbatim.
    CHECK(vs::convert_quality(10 << 8 | 200) == -36);

    // Image quality: min over connections, Balanced when none.
    const int32_t balanced = vs::convert_quality(hbb::Balanced);
    CHECK(vs::get_image_quality() == balanced);
    vs::update_image_quality(2001, hbb::Best);
    CHECK(vs::get_image_quality() == (100 << 8 | 12));
    vs::update_image_quality(2002, hbb::Low);
    CHECK(vs::get_image_quality() == (50 << 8 | 18));
    vs::update_image_quality(2002, std::nullopt);
    CHECK(vs::get_image_quality() == (100 << 8 | 12));
    vs::update_image_quality(2001, hbb::ImageQualityNotSet);  // converts to 0: removed
    CHECK(vs::get_image_quality() == balanced);

    // refresh() raises SWITCH for the capture loop.
    CHECK(!vs::switch_pending());
    vs::refresh();
    CHECK(vs::switch_pending());

    // scrap seam: Display::all() fails like a host without an X server.
    CHECK(vs::current_display() == SIZE_MAX);
    std::string err;
    try {
        vs::get_displays();
    } catch (const std::exception& e) {
        err = e.what();
    }
    CHECK(err == "connection refused");
    vs::switch_display(0);  // nothing to switch to
    vs::switch_to_primary();
    CHECK(vs::current_display() == SIZE_MAX);
}

void test_input_service() {
    namespace is = rustdesk::input_service;
    hbb::MouseEvent me;
    me.set_mask(1 << 3 | 2);  // left button, up
    CHECK(is::is_left_up(me));
    me.set_mask(1 << 3 | 1);  // left button, down
    CHECK(!is::is_left_up(me));
    me.set_mask(2 << 3 | 2);  // right button, up
    CHECK(!is::is_left_up(me));

    hbb::KeyEvent ke;
    ke.set_control_key(hbb::Return);
    CHECK(is::is_enter(ke));
    ke.set_control_key(hbb::NumpadEnter);
    CHECK(is::is_enter(ke));
    ke.set_control_key(hbb::Space);
    CHECK(!is::is_enter(ke));
    ke.set_chr(13);
    CHECK(!is::is_enter(ke));

    // LATEST_INPUT: only mouse moves (type 0) record (conn, time).
    const int64_t t0 = rustdesk::get_time();
    hbb::MouseEvent move;
    move.set_mask(0);
    move.set_x(10);
    move.set_y(20);
    is::handle_mouse(move, 77);
    const auto [conn, time] = is::latest_input();
    CHECK(conn == 77);
    CHECK(time >= t0 && time <= rustdesk::get_time());
    hbb::MouseEvent click;
    click.set_mask(1 << 3 | 1);
    click.add_modifiers(hbb::Shift);
    is::handle_mouse(click, 78);
    CHECK(is::latest_input().first == 77);
    hbb::MouseEvent wheel;
    wheel.set_mask(3);
    wheel.set_x(-2);
    wheel.set_y(3);
    is::handle_mouse(wheel, 79);

    // handle_key over every union arm (null enigo: nothing is injected).
    hbb::KeyEvent k;
    k.set_down(true);
    k.add_modifiers(hbb::Control);
    k.add_modifiers(hbb::CapsLock);
    k.set_chr('a');
    is::handle_key(k);
    k.set_unicode(0x1F600);
    is::handle_key(k);
    k.set_unicode(0xD800);  // not a char: skipped
    is::handle_key(k);
    k.set_seq("abc");
    is::handle_key(k);
    k.set_control_key(hbb::F5);
    is::handle_key(k);
    // CtrlAltDel sends SAS to the "_service" ipc on a thread (nobody listens
    // here); LockScreen runs lock_screen() + switch_to_primary().
    k.set_control_key(hbb::CtrlAltDel);
    is::handle_key(k);
    k.set_control_key(hbb::LockScreen);
    is::handle_key(k);
    // Let those detached threads finish before statics are torn down.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

}  // namespace

int main() {
    test_queue();
    test_service_framework();
    test_cursor_cache();
    test_server_registry();
    test_zombie_reaper();
    test_queue_receiver_side();
    test_video_service_state();
    test_input_service();
    if (g_failures == 0) {
        std::puts("test_server: all tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
