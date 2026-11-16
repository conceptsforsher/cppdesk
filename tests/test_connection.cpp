// test_connection.cpp — server.rs + server/connection.rs (step 15).
//
// A real client talks to the server's entry points over loopback:
// accept_connection (the hole-punch path: it re-listens on a socket's local
// address) and create_relay_connection (through a fake relay). The client
// half of the secure handshake is Client::secure_connection, so both halves
// of the port are checked against each other. A fake connection manager
// listens on the "_cm" ipc socket and reacts to what start_ipc relays (by the
// Login's peer name or a chat text), which drives the cm -> connection paths
// (Authorize, Close, ChatMessage, SwitchPermission, RawMessage, ClickTime).
//
// The first case runs with no cm listening, so start_ipc spawns `<this
// test> --cm`; main() turns into that child cm, which logs what it receives.
//
// Hermetic: temp HOME/XDG_CONFIG_HOME, a fake `loginctl` first on PATH
// (seat0 user alice, uid 1000, x11), fixed id/password. Like test_ipc it
// skips when a live connection manager owns /tmp/RustDesk/ipc_cm, except
// under GitHub Actions, where that is a failure.

#include <rustdesk/client.hpp>
#include <rustdesk/common.hpp>
#include <rustdesk/connection.hpp>
#include <rustdesk/ipc.hpp>
#include <rustdesk/platform.hpp>
#include <rustdesk/process.hpp>
#include <rustdesk/server.hpp>
#include <rustdesk/whoami.hpp>

#include <hbb_common/compress.hpp>
#include <hbb_common/config.hpp>
#include <hbb_common/tcp.hpp>
#include <hbb_common/util.hpp>

#include <openssl/sha.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

using hbb_common::ResolvedAddr;
using hbb_common::TcpFramedStream;
using hbb_common::TcpListener;
namespace ipc = rustdesk::ipc;

constexpr char kId[] = "123456789";
constexpr char kPassword[] = "test-Passw0rd";

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string sha256(const std::string& data) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
    return std::string(reinterpret_cast<const char*>(md), sizeof md);
}

// Sha256(Sha256(password + salt) + challenge), what the client sends.
std::string password_hash(const std::string& password, const hbb::Hash& h) {
    return sha256(sha256(password + h.salt()) + h.challenge());
}

// --- fake connection manager (in-process) ---------------------------------------

class FakeCm {
public:
    bool start() {
        try {
            incoming_.emplace(ipc::new_listener("_cm"));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "fake cm: %s\n", e.what());
            return false;
        }
        std::thread([this] {
            int index = 0;
            while (true) {
                try {
                    parity_tokio_ipc::Connection conn = incoming_->next();
                    std::thread([this, i = index++, c = ipc::Connection(std::move(conn))]() mutable {
                        serve(i, std::move(c));
                    }).detach();
                } catch (const std::exception&) {
                    sleep_ms(10);
                }
            }
        }).detach();
        return true;
    }

    // All Data the cm got on the ipc link whose Login names `name`.
    std::vector<ipc::Data> of(const std::string& name) {
        std::lock_guard<std::mutex> l(m_);
        const int index = index_of_locked(name);
        std::vector<ipc::Data> out;
        for (const auto& [i, d] : records_) {
            if (i == index) {
                out.push_back(d);
            }
        }
        return out;
    }

    bool wait(const std::function<bool()>& pred, int timeout_ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            sleep_ms(10);
        }
        return pred();
    }

    bool closed(const std::string& name) {
        std::lock_guard<std::mutex> l(m_);
        const int index = index_of_locked(name);
        return index >= 0 && closed_.count(index) > 0;
    }

private:
    int index_of_locked(const std::string& name) const {
        for (const auto& [i, d] : records_) {
            if (const auto* login = d.get_if<ipc::Data::Login>()) {
                if (login->name == name) {
                    return i;
                }
            }
        }
        return -1;
    }

    void serve(int index, ipc::Connection conn) {
        while (true) {
            std::optional<ipc::Data> data;
            try {
                data = conn.next();
            } catch (const std::exception&) {
                std::lock_guard<std::mutex> l(m_);
                closed_.insert(index);
                return;
            }
            if (!data) {
                continue;
            }
            {
                std::lock_guard<std::mutex> l(m_);
                records_.emplace_back(index, *data);
            }
            try {
                react(*data, conn);
            } catch (const std::exception&) {
            }
        }
    }

    // What a user clicking in the cm window would do, keyed by test names.
    static void react(const ipc::Data& data, ipc::Connection& conn) {
        if (const auto* login = data.get_if<ipc::Data::Login>()) {
            if (login->name == "authorize-me" && !login->authorized) {
                conn.send(ipc::Data::Authorize{});
            } else if (login->name == "perm-me" && login->authorized) {
                conn.send(ipc::Data::SwitchPermission{"clipboard", false});
                hbb::Message raw;
                raw.mutable_misc()->mutable_chat_message()->set_text("raw!");
                const std::string bytes = raw.SerializeAsString();
                conn.send(ipc::Data::RawMessage{std::vector<uint8_t>(bytes.begin(), bytes.end())});
            }
        } else if (const auto* chat = data.get_if<ipc::Data::ChatMessage>()) {
            if (chat->text == "chat-me") {
                conn.send(ipc::Data::ChatMessage{"hello from cm"});
            } else if (chat->text == "query-click") {
                conn.send(ipc::Data::ClickTime{0});
            } else if (chat->text == "close-me") {
                conn.send(ipc::Data::Close{});
            }
        }
    }

    std::optional<parity_tokio_ipc::Incoming> incoming_;
    std::mutex m_;
    std::vector<std::pair<int, ipc::Data>> records_;
    std::set<int> closed_;
};

FakeCm g_cm;

// --- client side ----------------------------------------------------------------

// Next message from the server, skipping its periodic TestDelay probes.
std::optional<hbb::Message> recv_msg(TcpFramedStream& s, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return std::nullopt;
        }
        const auto frame = s.next_timeout(static_cast<uint64_t>(left));
        if (!frame) {
            return std::nullopt;
        }
        hbb::Message msg;
        if (!msg.ParseFromArray(frame->data(), static_cast<int>(frame->size()))) {
            continue;
        }
        if (msg.has_test_delay() && !msg.test_delay().from_client()) {
            continue;
        }
        return msg;
    }
}

// The connection ended: no more messages, EOF seen.
bool recv_eof(TcpFramedStream& s, int timeout_ms = 5000) {
    while (true) {
        const auto m = recv_msg(s, timeout_ms);
        if (!m) {
            return s.peer_closed();
        }
    }
}

std::optional<std::string> login_error(const std::optional<hbb::Message>& m) {
    if (m && m->has_login_response() &&
        m->login_response().union_case() == hbb::LoginResponse::kError) {
        return m->login_response().error();
    }
    return std::nullopt;
}

const hbb::PeerInfo* peer_info(const std::optional<hbb::Message>& m) {
    if (m && m->has_login_response() && m->login_response().has_peer_info()) {
        return &m->login_response().peer_info();
    }
    return nullptr;
}

hbb::Message login_msg(const std::string& password, const std::string& name,
                       const std::string& username = kId) {
    hbb::Message m;
    hbb::LoginRequest* lr = m.mutable_login_request();
    lr->set_username(username);
    lr->set_password(password);
    lr->set_my_id("client-" + name);
    lr->set_my_name(name);
    return m;
}

hbb::Message misc_chat(const std::string& text) {
    hbb::Message m;
    m.mutable_misc()->mutable_chat_message()->set_text(text);
    return m;
}

// One accept_connection run: the server re-listens on `sock`'s local address
// (a bound, unconnected socket stands in for the rendezvous connection).
struct Session {
    std::thread server;
    std::optional<TcpFramedStream> client;
    hbb::Hash hash;
    bool ok = false;

    void close() {
        client.reset();
        if (server.joinable()) {
            server.join();
        }
    }
    ~Session() { close(); }
};

std::optional<TcpFramedStream> connect_client(uint16_t port, const std::string& local_ip) {
    for (int i = 0; i < 200; ++i) {
        auto s = TcpFramedStream::connect(ResolvedAddr{AF_INET, "127.0.0.1", port},
                                          ResolvedAddr{AF_INET, local_ip, 0}, 1000);
        if (s) {
            return s;
        }
        sleep_ms(20);
    }
    return std::nullopt;
}

void open_session(Session& s, const rustdesk::ServerPtr& server, bool secure,
                  const std::string& local_ip = "127.0.0.1", bool expect_hash = true) {
    const int fd = hbb_common::new_socket(ResolvedAddr{AF_INET, "127.0.0.1", 0}, true, true);
    TcpFramedStream sock(fd);
    const auto local = sock.local_addr();
    if (!local) {
        return;
    }
    const uint16_t port = local->port;
    s.server = std::thread([server, sock = std::move(sock), secure]() mutable {
        rustdesk::accept_connection(server, std::move(sock), ResolvedAddr{AF_INET, "127.0.0.1", 0},
                                    secure);
    });
    s.client = connect_client(port, local_ip);
    if (!s.client) {
        return;
    }
    if (secure) {
        try {
            rustdesk::Client::secure_connection(kId, hbb_common::get_key_pair()[1], *s.client);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "secure_connection: %s\n", e.what());
            return;
        }
    }
    if (!expect_hash) {
        s.ok = true;
        return;
    }
    const auto m = recv_msg(*s.client);
    if (m && m->has_hash()) {
        s.hash = m->hash();
        s.ok = true;
    }
}

bool send(Session& s, const hbb::Message& m) {
    return s.client && s.client->send(m);
}

// --- cases -------------------------------------------------------------------------

// start_ipc with no cm listening: run_me(["--cm"]) spawns this binary as the
// cm, the connect-retry loop reaches it, and the child is tracked in
// CHILD_PROCESS for the reaper.
void test_spawned_cm(const rustdesk::ServerPtr& server, const fs::path& home) {
    if (rustdesk::platform::is_root()) {
        std::puts("test_connection: root runs run_as_user instead; spawn case skipped");
        return;
    }
    const fs::path log = home / "child_cm.log";
    ::setenv("CPPDESK_TEST_CM_LOG", log.c_str(), 1);
    Session s;
    open_session(s, server, false);
    CHECK(s.ok);
    if (!s.ok) {
        return;
    }
    hbb::Message m = login_msg(password_hash(kPassword, s.hash), "spawned");
    m.mutable_login_request()->mutable_file_transfer()->set_dir("");
    CHECK(send(s, m));
    const auto res = recv_msg(*s.client);
    CHECK(peer_info(res) != nullptr);
    // Give start_ipc time to spawn and reach the child before closing.
    intptr_t child = 0;
    for (int i = 0; i < 300 && child == 0; ++i) {
        {
            std::lock_guard<std::mutex> l(rustdesk::child_lock());
            if (!rustdesk::child_processes().empty()) {
                child = rustdesk::child_processes().front();
            }
        }
        if (child == 0) {
            sleep_ms(10);
        }
    }
    CHECK(child > 0);
    sleep_ms(1500);  // the retry loop polls every 300 ms
    s.close();       // connection ends -> start_ipc exits -> the child sees EOF
    if (child > 0) {
        int status = 0;
        pid_t r = -1;
        for (int i = 0; i < 1000; ++i) {
            r = ::waitpid(static_cast<pid_t>(child), &status, WNOHANG);
            if (r != 0) {
                break;
            }
            sleep_ms(10);
        }
        CHECK(r == static_cast<pid_t>(child));
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (r == 0) {
            ::kill(static_cast<pid_t>(child), SIGKILL);
            ::waitpid(static_cast<pid_t>(child), &status, 0);
        }
        std::lock_guard<std::mutex> l(rustdesk::child_lock());
        rustdesk::child_processes().clear();
    }
    const std::string got = read_file(log);
    CHECK(got.find("{\"t\":\"Test\"}") != std::string::npos);
    CHECK(got.find("\"t\":\"Login\"") != std::string::npos);
    CHECK(got.find("\"name\":\"spawned\"") != std::string::npos);
    CHECK(got.find("\"is_file_transfer\":true") != std::string::npos);
    CHECK(got.find("\"authorized\":true") != std::string::npos);
    CHECK(got.find("{\"t\":\"FS\",\"c\":{\"t\":\"ReadDir\"") != std::string::npos);
}

void test_secure_login(const rustdesk::ServerPtr& server) {
    Session s;
    open_session(s, server, true);
    CHECK(s.ok);
    if (!s.ok) {
        return;
    }
    CHECK(s.client->is_secured());
    CHECK(s.hash.salt() == hbb_common::get_salt());
    CHECK(s.hash.challenge().size() == 6);  // get_auto_password()

    // Wrong password: an error, and the cm hears about the attempt.
    CHECK(send(s, login_msg(password_hash("nope", s.hash), "clicker")));
    CHECK(login_error(recv_msg(*s.client)) == std::string("Wrong Password"));
    // Right password, remote control: authorized, but the scrap seam answers
    // like a host without an X server.
    CHECK(send(s, login_msg(password_hash(kPassword, s.hash), "clicker")));
    CHECK(login_error(recv_msg(*s.client)) == std::string("connection refused"));
    CHECK(g_cm.wait([] {
        int authorized = 0;
        int refused = 0;
        for (const auto& d : g_cm.of("clicker")) {
            if (const auto* l = d.get_if<ipc::Data::Login>()) {
                (l->authorized ? authorized : refused) += 1;
                CHECK(l->peer_id == "client-clicker");
                CHECK(!l->is_file_transfer && l->port_forward.empty());
                CHECK(l->keyboard && l->clipboard && l->audio);
            }
        }
        return authorized == 1 && refused == 1;
    }));

    // TestDelay from the client is echoed through the service queue.
    hbb::Message td;
    td.mutable_test_delay()->set_time(12345);
    td.mutable_test_delay()->set_from_client(true);
    CHECK(send(s, td));
    const auto echo = recv_msg(*s.client);
    CHECK(echo && echo->has_test_delay() && echo->test_delay().time() == 12345 &&
          echo->test_delay().from_client());

    // Left-button release stamps CLICK_TIME; the cm reads it back.
    const int64_t before = rustdesk::get_time();
    hbb::Message up;
    up.mutable_mouse_event()->set_mask(1 << 3 | 2);
    CHECK(send(s, up));
    CHECK(send(s, misc_chat("query-click")));
    CHECK(g_cm.wait([&] {
        for (const auto& d : g_cm.of("clicker")) {
            if (const auto* ct = d.get_if<ipc::Data::ClickTime>()) {
                if (ct->value >= before && ct->value <= rustdesk::get_time()) {
                    return true;
                }
            }
        }
        return false;
    }));
    CHECK(rustdesk::click_time() >= before);

    // cm ChatMessage -> Misc chat to the peer.
    CHECK(send(s, misc_chat("chat-me")));
    const auto chat = recv_msg(*s.client);
    CHECK(chat && chat->has_misc() && chat->misc().has_chat_message() &&
          chat->misc().chat_message().text() == "hello from cm");

    // cm Close -> close_reason to the peer, then the connection ends and the
    // ipc link to the cm goes with it.
    CHECK(send(s, misc_chat("close-me")));
    const auto bye = recv_msg(*s.client);
    CHECK(bye && bye->has_misc() &&
          bye->misc().close_reason() == "Closed manually by the peer");
    CHECK(recv_eof(*s.client));
    s.close();
    CHECK(g_cm.wait([] { return g_cm.closed("clicker"); }));
}

void test_file_transfer(const rustdesk::ServerPtr& server, const fs::path& home) {
    const fs::path tree = home / "tree";
    fs::create_directories(tree / "sub");
    write_file(tree / "a.txt", "alpha");
    write_file(tree / "sub" / "b.txt", "bravo");
    const fs::path send_dir = home / "send";
    fs::create_directories(send_dir);
    std::string payload;
    for (int i = 0; i < 5000; ++i) {
        payload += "line " + std::to_string(i) + "\n";
    }
    write_file(send_dir / "hello.txt", payload);

    Session s;
    open_session(s, server, false);
    CHECK(s.ok);
    if (!s.ok) {
        return;
    }
    hbb::Message m = login_msg(password_hash(kPassword, s.hash), "ft");
    m.mutable_login_request()->mutable_file_transfer()->set_dir(tree.string());
    CHECK(send(s, m));
    const auto res = recv_msg(*s.client);
    const hbb::PeerInfo* pi = peer_info(res);
    CHECK(pi != nullptr);
    if (pi) {
        CHECK(pi->username() == "alice");  // the seat0 user (fake loginctl)
        CHECK(pi->hostname() == rustdesk::whoami::hostname());
        CHECK(pi->platform() == "Linux");
        CHECK(pi->version() == "1.1.2");
        CHECK(pi->displays_size() == 0);
        CHECK(!pi->sas_enabled());
    }

    // Requests the cm executes arrive there as FS data.
    hbb::Message rd;
    rd.mutable_file_action()->mutable_read_dir()->set_path("/x");
    rd.mutable_file_action()->mutable_read_dir()->set_include_hidden(true);
    CHECK(send(s, rd));
    hbb::Message recv_req;
    auto* r = recv_req.mutable_file_action()->mutable_receive();
    r->set_id(7);
    r->set_path("/dest");
    auto* f = r->add_files();
    f->set_name("a");
    f->set_modified_time(123);
    CHECK(send(s, recv_req));
    hbb::Message rmdir;
    rmdir.mutable_file_action()->mutable_remove_dir()->set_id(8);
    rmdir.mutable_file_action()->mutable_remove_dir()->set_path("/d");
    rmdir.mutable_file_action()->mutable_remove_dir()->set_recursive(true);
    CHECK(send(s, rmdir));
    hbb::Message rmfile;
    rmfile.mutable_file_action()->mutable_remove_file()->set_id(9);
    rmfile.mutable_file_action()->mutable_remove_file()->set_path("/f");
    rmfile.mutable_file_action()->mutable_remove_file()->set_file_num(2);
    CHECK(send(s, rmfile));
    hbb::Message mk;
    mk.mutable_file_action()->mutable_create()->set_id(10);
    mk.mutable_file_action()->mutable_create()->set_path("/new");
    CHECK(send(s, mk));
    hbb::Message cancel;
    cancel.mutable_file_action()->mutable_cancel()->set_id(11);
    CHECK(send(s, cancel));
    hbb::Message block;
    block.mutable_file_response()->mutable_block()->set_id(12);
    block.mutable_file_response()->mutable_block()->set_file_num(1);
    block.mutable_file_response()->mutable_block()->set_data("xyz");
    CHECK(send(s, block));
    hbb::Message done;
    done.mutable_file_response()->mutable_done()->set_id(12);
    done.mutable_file_response()->mutable_done()->set_file_num(1);
    CHECK(send(s, done));
    CHECK(g_cm.wait([&] {
        int seen = 0;
        for (const auto& d : g_cm.of("ft")) {
            const auto* fsd = d.get_if<ipc::Data::FS>();
            if (!fsd) {
                continue;
            }
            const ipc::FS& v = fsd->value;
            if (const auto* x = v.get_if<ipc::FS::ReadDir>()) {
                seen |= (x->dir == tree.string() && !x->include_hidden) ? 1 : 0;
                seen |= (x->dir == "/x" && x->include_hidden) ? 2 : 0;
            } else if (const auto* x = v.get_if<ipc::FS::NewWrite>()) {
                seen |= (x->id == 7 && x->path == "/dest" && x->files.size() == 1 &&
                         x->files[0] == std::make_pair(std::string("a"), uint64_t{123}))
                            ? 4
                            : 0;
            } else if (const auto* x = v.get_if<ipc::FS::RemoveDir>()) {
                seen |= (x->id == 8 && x->path == "/d" && x->recursive) ? 8 : 0;
            } else if (const auto* x = v.get_if<ipc::FS::RemoveFile>()) {
                seen |= (x->id == 9 && x->path == "/f" && x->file_num == 2) ? 16 : 0;
            } else if (const auto* x = v.get_if<ipc::FS::CreateDir>()) {
                seen |= (x->id == 10 && x->path == "/new") ? 32 : 0;
            } else if (const auto* x = v.get_if<ipc::FS::CancelWrite>()) {
                seen |= x->id == 11 ? 64 : 0;
            } else if (const auto* x = v.get_if<ipc::FS::WriteBlock>()) {
                seen |= (x->id == 12 && x->file_num == 1 &&
                         x->data == std::vector<uint8_t>{'x', 'y', 'z'} && !x->compressed)
                            ? 128
                            : 0;
            } else if (const auto* x = v.get_if<ipc::FS::WriteDone>()) {
                seen |= (x->id == 12 && x->file_num == 1) ? 256 : 0;
            }
        }
        bool test_seen = false;
        for (const auto& d : g_cm.of("ft")) {
            test_seen |= d.is<ipc::Data::Test>();
        }
        return seen == 511 && test_seen;
    }));

    // all_files is answered by the server itself.
    hbb::Message all;
    all.mutable_file_action()->mutable_all_files()->set_id(5);
    all.mutable_file_action()->mutable_all_files()->set_path(tree.string());
    CHECK(send(s, all));
    const auto listing = recv_msg(*s.client);
    CHECK(listing && listing->has_file_response() && listing->file_response().has_dir() &&
          listing->file_response().dir().id() == 5 &&
          listing->file_response().dir().entries_size() == 2);
    hbb::Message missing;
    missing.mutable_file_action()->mutable_all_files()->set_id(6);
    missing.mutable_file_action()->mutable_all_files()->set_path((home / "none").string());
    CHECK(send(s, missing));
    const auto err = recv_msg(*s.client);
    CHECK(err && err->has_file_response() && err->file_response().has_error() &&
          err->file_response().error().id() == 6 &&
          err->file_response().error().file_num() == -1 &&
          !err->file_response().error().error().empty());

    // send: a read job pumped by the 1 ms timer until done.
    hbb::Message req;
    req.mutable_file_action()->mutable_send()->set_id(20);
    req.mutable_file_action()->mutable_send()->set_path(send_dir.string());
    CHECK(send(s, req));
    const auto dir = recv_msg(*s.client);
    CHECK(dir && dir->has_file_response() && dir->file_response().has_dir() &&
          dir->file_response().dir().id() == 20 &&
          dir->file_response().dir().entries_size() == 1);
    std::string received;
    bool finished = false;
    for (int i = 0; i < 1000 && !finished; ++i) {
        const auto m2 = recv_msg(*s.client);
        if (!m2 || !m2->has_file_response()) {
            break;
        }
        const hbb::FileResponse& fr = m2->file_response();
        if (fr.has_block()) {
            CHECK(fr.block().id() == 20);
            const std::string& data = fr.block().data();
            if (fr.block().compressed()) {
                const auto plain = hbb_common::decompress(
                    reinterpret_cast<const uint8_t*>(data.data()), data.size());
                received.append(plain.begin(), plain.end());
            } else {
                received += data;
            }
        } else if (fr.has_done()) {
            CHECK(fr.done().id() == 20);
            finished = true;
        } else {
            break;
        }
    }
    CHECK(finished);
    CHECK(received == payload);
    s.close();
}

void test_port_forward(const rustdesk::ServerPtr& server) {
    auto echo_listener = TcpListener::bind(ResolvedAddr{AF_INET, "127.0.0.1", 0}, true);
    CHECK(echo_listener.has_value());
    if (!echo_listener) {
        return;
    }
    const uint16_t echo_port = echo_listener->local_addr()->port;
    std::atomic<bool> echo_eof{false};
    std::thread echo([&] {
        auto c = echo_listener->accept(10000);
        if (!c) {
            return;
        }
        c->set_raw();
        while (true) {
            const auto bytes = c->next_timeout(10000);
            if (!bytes) {
                echo_eof = c->peer_closed();
                return;
            }
            c->send_bytes(*bytes);
        }
    });

    Session s;
    open_session(s, server, true);
    CHECK(s.ok);
    if (s.ok) {
        hbb::Message m = login_msg(password_hash(kPassword, s.hash), "pf");
        m.mutable_login_request()->mutable_port_forward()->set_host("127.0.0.1");
        m.mutable_login_request()->mutable_port_forward()->set_port(echo_port);
        CHECK(send(s, m));
        const auto res = recv_msg(*s.client);
        const hbb::PeerInfo* pi = peer_info(res);
        CHECK(pi != nullptr);
        if (pi) {
            CHECK(pi->username() == "alice");
            CHECK(pi->platform() == "Linux" && pi->version() == "1.1.2");
            CHECK(pi->displays_size() == 0);
        }
        // Both ends go raw; set_raw() drops the key on each side.
        s.client->set_raw();
        CHECK(!s.client->is_secured());
        const std::vector<uint8_t> ping = {'p', 'i', 'n', 'g', '!'};
        CHECK(s.client->send_bytes(ping));
        std::vector<uint8_t> got;
        for (int i = 0; i < 50 && got.size() < ping.size(); ++i) {
            const auto chunk = s.client->next_timeout(200);
            if (chunk) {
                got.insert(got.end(), chunk->begin(), chunk->end());
            }
        }
        CHECK(got == ping);
        CHECK(g_cm.wait([&] {
            for (const auto& d : g_cm.of("pf")) {
                if (const auto* l = d.get_if<ipc::Data::Login>()) {
                    return l->authorized &&
                           l->port_forward == "127.0.0.1:" + std::to_string(echo_port);
                }
            }
            return false;
        }));
    }
    s.close();  // "Stream reset by the peer": the forward socket closes too
    echo.join();
    CHECK(echo_eof);

    // RDP with port 0 means localhost:3389; the error names it "RDP".
    Session rdp;
    open_session(rdp, server, false);
    CHECK(rdp.ok);
    if (rdp.ok) {
        hbb::Message m = login_msg("", "rdp");
        m.mutable_login_request()->mutable_port_forward()->set_host("RDP");
        m.mutable_login_request()->mutable_port_forward()->set_port(0);
        CHECK(send(rdp, m));
        const auto e = login_error(recv_msg(*rdp.client));
        // Unless something really serves RDP on this host.
        CHECK(!e || *e == "Failed to access remote RDP, please make sure if it is open");
    }
    rdp.close();
}

void test_cm_paths(const rustdesk::ServerPtr& server) {
    // Password-less login: the cm's Authorize answers for the user.
    {
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);
        if (s.ok) {
            hbb::Message m = login_msg("", "authorize-me");
            m.mutable_login_request()->mutable_file_transfer()->set_dir("");
            CHECK(send(s, m));
            const auto res = recv_msg(*s.client);
            CHECK(peer_info(res) != nullptr);
            CHECK(g_cm.wait([] {
                for (const auto& d : g_cm.of("authorize-me")) {
                    if (const auto* fsd = d.get_if<ipc::Data::FS>()) {
                        if (const auto* rd = fsd->value.get_if<ipc::FS::ReadDir>()) {
                            return rd->dir.empty() && !rd->include_hidden;
                        }
                    }
                }
                return false;
            }));
        }
    }
    // SwitchPermission and RawMessage from the cm.
    {
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);
        if (s.ok) {
            CHECK(send(s, login_msg(password_hash(kPassword, s.hash), "perm-me")));
            CHECK(login_error(recv_msg(*s.client)) == std::string("connection refused"));
            const auto perm = recv_msg(*s.client);
            CHECK(perm && perm->has_misc() && perm->misc().has_permission_info() &&
                  perm->misc().permission_info().permission() == hbb::PermissionClipboard &&
                  !perm->misc().permission_info().enabled());
            const auto raw = recv_msg(*s.client);
            CHECK(raw && raw->has_misc() && raw->misc().chat_message().text() == "raw!");
        }
    }
}

void test_login_errors(const rustdesk::ServerPtr& server) {
    // Unknown id.
    {
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);
        if (s.ok) {
            CHECK(send(s, login_msg(password_hash(kPassword, s.hash), "offline", "987654321")));
            CHECK(login_error(recv_msg(*s.client)) == std::string("Offline"));
        }
    }
    // Disabled features answer, wait a second and close.
    hbb_common::set_option("enable-file-transfer", "N");
    {
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);
        if (s.ok) {
            hbb::Message m = login_msg(password_hash(kPassword, s.hash), "noft");
            m.mutable_login_request()->mutable_file_transfer()->set_dir("");
            CHECK(send(s, m));
            CHECK(login_error(recv_msg(*s.client)) ==
                  std::string("No permission of file transfer"));
            CHECK(recv_eof(*s.client));
        }
    }
    hbb_common::set_option("enable-file-transfer", "");
    hbb_common::set_option("enable-tunnel", "N");
    {
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);
        if (s.ok) {
            hbb::Message m = login_msg(password_hash(kPassword, s.hash), "nopf");
            m.mutable_login_request()->mutable_port_forward()->set_port(1);
            CHECK(send(s, m));
            CHECK(login_error(recv_msg(*s.client)) ==
                  std::string("No permission of IP tunneling"));
            CHECK(recv_eof(*s.client));
        }
    }
    hbb_common::set_option("enable-tunnel", "");
    // A disabled keyboard is announced right after the Hash.
    hbb_common::set_option("enable-keyboard", "N");
    {
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);
        if (s.ok) {
            const auto perm = recv_msg(*s.client);
            CHECK(perm && perm->has_misc() &&
                  perm->misc().permission_info().permission() == hbb::Keyboard &&
                  !perm->misc().permission_info().enabled());
        }
    }
    hbb_common::set_option("enable-keyboard", "");
}

void test_whitelist(const rustdesk::ServerPtr& server) {
    hbb_common::set_option("whitelist", "10.0.0.1,,");
    {
        Session s;
        open_session(s, server, false, "127.0.0.1", false);
        CHECK(s.ok);
        if (s.ok) {
            CHECK(login_error(recv_msg(*s.client)) ==
                  std::string("Your ip is blocked by the peer"));
            CHECK(recv_eof(*s.client));
        }
    }
    for (const char* list : {"10.0.0.1,127.0.0.1", "0.0.0.0"}) {
        hbb_common::set_option("whitelist", list);
        Session s;
        open_session(s, server, false);
        CHECK(s.ok);  // got the Hash
    }
    hbb_common::set_option("whitelist", "");
}

void test_login_failures(const rustdesk::ServerPtr& server) {
    // LOGIN_FAILURES counts per minute; keep all attempts in one minute.
    const int64_t into_minute = rustdesk::get_time() % 60'000;
    if (into_minute > 50'000) {
        sleep_ms(static_cast<int>(60'000 - into_minute + 200));
    }
    Session s;
    open_session(s, server, false, "127.0.0.2");  // its own LOGIN_FAILURES entry
    CHECK(s.ok);
    if (!s.ok) {
        return;
    }
    for (int i = 0; i < 7; ++i) {
        CHECK(send(s, login_msg(password_hash("wrong", s.hash), "brute")));
        CHECK(login_error(recv_msg(*s.client)) == std::string("Wrong Password"));
    }
    // More than 6 in this minute: even the right password waits.
    CHECK(send(s, login_msg(password_hash(kPassword, s.hash), "brute")));
    CHECK(login_error(recv_msg(*s.client)) == std::string("Please try 1 minute later"));
}

void test_relay(const rustdesk::ServerPtr& server) {
    auto relay = TcpListener::bind(ResolvedAddr{AF_INET, "127.0.0.1", 0}, true);
    CHECK(relay.has_value());
    if (!relay) {
        return;
    }
    const uint16_t port = relay->local_addr()->port;
    std::thread server_thread([server, port] {
        rustdesk::create_relay_connection(server, "127.0.0.1:" + std::to_string(port), "uuid-77",
                                          ResolvedAddr{AF_INET, "127.0.0.1", 0}, true);
    });
    auto side = relay->accept(5000);
    CHECK(side.has_value());
    if (side) {
        const auto frame = side->next_timeout(5000);
        hbb::RendezvousMessage rm;
        CHECK(frame && rm.ParseFromArray(frame->data(), static_cast<int>(frame->size())));
        CHECK(rm.has_request_relay() && rm.request_relay().uuid() == "uuid-77");
        // The relay now just pipes; play the client on the same socket.
        bool secured = false;
        try {
            rustdesk::Client::secure_connection(kId, hbb_common::get_key_pair()[1], *side);
            secured = true;
        } catch (const std::exception&) {
        }
        CHECK(secured && side->is_secured());
        const auto hash = recv_msg(*side);
        CHECK(hash && hash->has_hash());
        if (hash && hash->has_hash()) {
            hbb::Message m = login_msg(password_hash(kPassword, hash->hash()), "relayed");
            m.mutable_login_request()->mutable_file_transfer()->set_dir("");
            CHECK(side->send(m));
            CHECK(peer_info(recv_msg(*side)) != nullptr);
        }
        side.reset();
    }
    server_thread.join();
}

// --- `--cm` child -------------------------------------------------------------------

int run_child_cm() {
    ::alarm(30);  // never outlive the test
    const char* log = std::getenv("CPPDESK_TEST_CM_LOG");
    try {
        parity_tokio_ipc::Incoming incoming = ipc::new_listener("_cm");
        ipc::Connection conn(incoming.next());
        std::ofstream out(log ? log : "/dev/null", std::ios::trunc);
        while (true) {
            try {
                if (const auto data = conn.next()) {
                    out << ipc::to_json(*data) << "\n";
                    out.flush();
                }
            } catch (const std::exception&) {
                break;  // the server side went away
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "child cm: %s\n", e.what());
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--cm") == 0) {
        std::fflush(nullptr);
        std::_Exit(run_child_cm());
    }
    char tmpl[] = "/tmp/cppdesk_test_connection_XXXXXX";
    if (!::mkdtemp(tmpl)) {
        std::perror("mkdtemp");
        return 1;
    }
    const fs::path home = tmpl;
    ::setenv("HOME", home.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", home.c_str(), 1);
    // Fake loginctl: seat0 is alice (uid 1000) on x11.
    const fs::path bin = home / "bin";
    fs::create_directories(bin);
    write_file(bin / "loginctl",
               "#!/bin/sh\n"
               "if [ \"$#\" -eq 0 ]; then\n"
               "  printf '%s\\n' '   SESSION  UID USER  SEAT  TTY' '         2 1000 alice seat0 tty2'\n"
               "  exit 0\n"
               "fi\n"
               "case \"$3\" in State) echo State=active ;; Type) echo Type=x11 ;; esac\n");
    ::chmod((bin / "loginctl").c_str(), 0755);
    const char* path = std::getenv("PATH");
    const std::string new_path = bin.string() + ":" + (path ? path : "/usr/bin:/bin");
    ::setenv("PATH", new_path.c_str(), 1);

    hbb_common::set_id(kId);
    hbb_common::set_password(kPassword);
    (void)hbb_common::get_key_pair();  // generate the sign key pair up front

    // A live connection manager on this host would steal our connections.
    bool cm_busy = false;
    try {
        ipc::connect(300, "_cm");
        cm_busy = true;
    } catch (const std::exception&) {
    }
    if (cm_busy) {
        const char* gha = std::getenv("GITHUB_ACTIONS");
        if (gha && std::string(gha) == "true") {
            std::fputs("FAIL: a connection manager already owns ipc_cm on the CI runner\n", stderr);
            return 1;
        }
        std::puts("test_connection: live connection manager present; skipped");
        return 0;
    }

    const rustdesk::ServerPtr server = rustdesk::Server::create();
    test_spawned_cm(server, home);
    CHECK(g_cm.start());
    test_secure_login(server);
    test_file_transfer(server, home);
    test_port_forward(server);
    test_cm_paths(server);
    test_login_errors(server);
    test_whitelist(server);
    test_relay(server);
    test_login_failures(server);

    std::error_code ec;
    fs::remove_all(home, ec);
    if (g_failures == 0) {
        std::puts("test_connection: all tests passed");
    }
    std::fflush(nullptr);
    std::_Exit(g_failures == 0 ? 0 : 1);  // detached cm/start_ipc threads may still run
}
