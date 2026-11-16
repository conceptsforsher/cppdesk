// connection.cpp — see connection.hpp (src/server/connection.rs).
#include <rustdesk/connection.hpp>

#include <rustdesk/audio_service.hpp>
#include <rustdesk/clipboard_service.hpp>
#include <rustdesk/common.hpp>
#include <rustdesk/input_service.hpp>
#include <rustdesk/platform.hpp>
#include <rustdesk/server.hpp>
#include <rustdesk/video_service.hpp>
#include <rustdesk/whoami.hpp>

#include <cppdesk/version.hpp>

#include <hbb_common/config.hpp>

#include <openssl/sha.h>
#include <poll.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace rustdesk {
namespace {

using Clock = std::chrono::steady_clock;
using hbb_common::ResolvedAddr;
using hbb_common::TcpFramedStream;
using hbb_common::mpsc::UnboundedReceiver;
using hbb_common::mpsc::UnboundedSender;

constexpr auto kTestDelayTimeout = std::chrono::seconds(3);
constexpr auto kSec30 = std::chrono::seconds(30);
constexpr auto kH1 = std::chrono::seconds(3600);
constexpr auto kMilli1 = std::chrono::milliseconds(1);

// CLICK_TIME: Arc<Mutex<i64>>.
std::mutex& click_time_lock() {
    static std::mutex m;
    return m;
}
int64_t& click_time_value() {
    static int64_t v = 0;
    return v;
}

// LOGIN_FAILURES: Arc<Mutex<HashMap<String, (i32, i32, i32)>>>, keyed by peer
// ip: (minute of the last failure, failures in that minute, total failures).
using Failure = std::tuple<int32_t, int32_t, int32_t>;
std::mutex& login_failures_lock() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, Failure>& login_failures() {
    static std::unordered_map<std::string, Failure> m;
    return m;
}

void set_click_time_now() {
    std::lock_guard<std::mutex> l(click_time_lock());
    click_time_value() = get_time();
}

hbb::Message misc_message(hbb::Misc misc) {
    hbb::Message msg_out;
    *msg_out.mutable_misc() = std::move(misc);
    return msg_out;
}

// sleep(1.).await
void sleep_secs(double secs) {
    std::this_thread::sleep_for(std::chrono::duration<double>(secs));
}

// poll() timeout in ms until `deadline` (rounded up so the loop does not wake
// a hair early and spin), at least 0.
int ms_until(Clock::time_point deadline, Clock::time_point now) {
    if (deadline <= now) {
        return 0;
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
    const int64_t ms = (us + 999) / 1000;
    return static_cast<int>(std::min<int64_t>(ms, 1000LL * 3600));
}

// Waits until one of `fds` is readable or `timeout_ms` passes.
void wait_readable(std::vector<struct pollfd>& fds, int timeout_ms) {
    for (auto& p : fds) {
        p.revents = 0;
    }
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
}

}  // namespace

// --- ConnQueue / ConnInner ------------------------------------------------------

void ConnQueue::send(MessagePtr msg) {
    hbb_common::mpsc::WakePipe* wake = nullptr;
    {
        std::unique_lock l(mutex_);
        if (closed_) {
            return;  // the receiver is gone: tx.send() fails, the value is dropped
        }
        queue_.push_back(QueuedMessage{std::chrono::steady_clock::now(), std::move(msg)});
        wake = wake_.get();
    }
    cv_.notify_one();
    if (wake) {
        wake->notify();
    }
}

std::vector<MessagePtr> ConnQueue::drain() {
    std::vector<MessagePtr> out;
    std::unique_lock l(mutex_);
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front().message));
        queue_.pop_front();
    }
    return out;
}

std::optional<MessagePtr> ConnQueue::wait_receive(uint64_t timeout_ms) {
    std::unique_lock l(mutex_);
    if (queue_.empty() && !closed_) {
        cv_.wait_for(l, std::chrono::milliseconds(timeout_ms),
                     [&] { return !queue_.empty() || closed_; });
    }
    if (queue_.empty()) {
        return std::nullopt;  // timeout or close() parity (mpsc disconnect)
    }
    MessagePtr msg = std::move(queue_.front().message);
    queue_.pop_front();
    return msg;
}

std::optional<QueuedMessage> ConnQueue::try_recv() {
    std::unique_lock l(mutex_);
    if (queue_.empty()) {
        return std::nullopt;
    }
    QueuedMessage item = std::move(queue_.front());
    queue_.pop_front();
    return item;
}

int ConnQueue::wake_fd() {
    std::unique_lock l(mutex_);
    if (!wake_) {
        wake_ = std::make_unique<hbb_common::mpsc::WakePipe>();
        if (!queue_.empty()) {
            wake_->notify();  // items queued before the pipe existed
        }
    }
    return wake_->fd();
}

void ConnQueue::clear_wake() {
    std::unique_lock l(mutex_);
    if (wake_) {
        wake_->drain();
    }
}

void ConnQueue::close() {
    std::deque<QueuedMessage> dropped;
    {
        std::unique_lock l(mutex_);
        closed_ = true;
        dropped.swap(queue_);
    }
    cv_.notify_all();
}

size_t ConnQueue::size() const {
    std::unique_lock l(mutex_);
    return queue_.size();
}

void ConnInner::send(const MessagePtr& msg) const {
    if (tx) {
        tx->send(msg);  // unbounded: never blocks; a closed queue drops (allow_err!)
    }
    // tx: None parity: silent no-op.
}

int64_t click_time() {
    std::lock_guard<std::mutex> l(click_time_lock());
    return click_time_value();
}

// --- Connection -------------------------------------------------------------------

Connection::Connection(ConnInner inner, TcpFramedStream stream, std::weak_ptr<Server> server,
                       hbb::Hash hash, UnboundedSender<ipc::Data> tx_to_cm)
    : inner_(std::move(inner)),
      stream_(std::move(stream)),
      server_(std::move(server)),
      hash_(std::move(hash)),
      timer_(Interval::every(kSec30)),
      tx_to_cm_(std::move(tx_to_cm)),
      keyboard_(hbb_common::get_option("enable-keyboard").empty()),
      clipboard_(hbb_common::get_option("enable-clipboard").empty()),
      audio_(hbb_common::get_option("audio-input") != "Mute"),
      image_quality_(hbb::Balanced) {}

void Connection::start(const ResolvedAddr& addr, TcpFramedStream stream, int32_t id,
                       std::weak_ptr<Server> server) {
    hbb::Hash hash;
    hash.set_salt(hbb_common::get_salt());
    hash.set_challenge(hbb_common::get_auto_password());
    auto [tx_from_cm, rx_from_cm] = hbb_common::mpsc::unbounded_channel<ipc::Data>();
    auto [tx_to_cm, rx_to_cm] = hbb_common::mpsc::unbounded_channel<ipc::Data>();
    // (tx, rx): the services' channel. Closing it on every exit is rx's drop.
    const ConnQueuePtr rx = std::make_shared<ConnQueue>();
    struct CloseOnExit {
        ConnQueuePtr q;
        ~CloseOnExit() { q->close(); }
    } close_rx{rx};
    Connection conn(ConnInner{id, rx}, std::move(stream), std::move(server), std::move(hash),
                    std::move(tx_to_cm));
    std::thread([rx_to_cm = std::move(rx_to_cm), tx_from_cm = std::move(tx_from_cm)]() mutable {
        try {
            start_ipc(std::move(rx_to_cm), std::move(tx_from_cm));
        } catch (const std::exception&) {
            // log::error!("ipc to connection manager exit: {}", err);
        }
    }).detach();
    if (!conn.on_open(addr)) {
        return;
    }
    if (!conn.keyboard_) {
        conn.send_permisssion(hbb::Keyboard, false);
    }
    if (!conn.clipboard_) {
        conn.send_permisssion(hbb::PermissionClipboard, false);
    }
    if (!conn.audio_) {
        conn.send_permisssion(hbb::Audio, false);
    }
    Interval test_delay_timer = Interval::at(Clock::now() + kTestDelayTimeout, kTestDelayTimeout);
    Clock::time_point last_recv_time = Clock::now();
    std::vector<struct pollfd> fds = {
        {conn.stream_.fd(), POLLIN, 0},
        {rx->wake_fd(), POLLIN, 0},
        {rx_from_cm.wake_fd(), POLLIN, 0},
    };
    while (true) {
        bool progressed = false;
        // Some(data) = rx_from_cm.recv()
        if (auto data = rx_from_cm.try_recv()) {
            progressed = true;
            if (data->is<ipc::Data::Authorize>()) {
                conn.send_logon_response();
                if (conn.port_forward_socket_) {
                    break;
                }
            } else if (data->is<ipc::Data::Close>()) {
                hbb::Misc misc;
                misc.set_close_reason("Closed manually by the peer");
                conn.send(misc_message(std::move(misc)));
                conn.on_close("Close requested from connection manager", false);
                break;
            } else if (const auto* chat = data->get_if<ipc::Data::ChatMessage>()) {
                hbb::Misc misc;
                misc.mutable_chat_message()->set_text(chat->text);
                conn.send(misc_message(std::move(misc)));
            } else if (const auto* sp = data->get_if<ipc::Data::SwitchPermission>()) {
                // log::info!("Change permission {} -> {}", name, enabled);
                const std::string& name = sp->name;
                const bool enabled = sp->enabled;
                if (name == "keyboard") {
                    conn.keyboard_ = enabled;
                    conn.send_permisssion(hbb::Keyboard, enabled);
                    if (auto s = conn.server_.lock()) {
                        s->subscribe(input_service::kNameCursor, conn.inner_,
                                     enabled || conn.show_remote_cursor_);
                    }
                } else if (name == "clipboard") {
                    conn.clipboard_ = enabled;
                    conn.send_permisssion(hbb::PermissionClipboard, enabled);
                    if (auto s = conn.server_.lock()) {
                        s->subscribe(clipboard_service::kName, conn.inner_,
                                     conn.clipboard_enabled() && conn.keyboard_);
                    }
                } else if (name == "audio") {
                    conn.audio_ = enabled;
                    conn.send_permisssion(hbb::Audio, enabled);
                    if (auto s = conn.server_.lock()) {
                        s->subscribe(audio_service::kName, conn.inner_, conn.audio_enabled());
                    }
                }
            } else if (const auto* raw = data->get_if<ipc::Data::RawMessage>()) {
                HBB_ALLOW_ERR(conn.stream_.send_raw(raw->value));
            }
        }
        // Some((instant, value)) = rx.recv()
        if (auto item = rx->try_recv()) {
            progressed = true;
            const int64_t latency =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - item->instant)
                    .count();
            video_service::update_internal_latency(id, latency);
            const hbb::Message& msg = *item->message;
            // Stale frames are skipped (upstream `continue`s to the next select!).
            const bool stale =
                latency > 1000 && (msg.union_case() == hbb::Message::kVideoFrame ||
                                   msg.union_case() == hbb::Message::kAudioFrame);
            if (!stale && !conn.stream_.send(msg)) {
                conn.on_close("send failed", false);  // err.to_string()
                break;
            }
        }
        // res = conn.stream.next()
        bool closed = false;
        try {
            if (auto bytes = conn.stream_.try_next()) {
                progressed = true;
                last_recv_time = Clock::now();
                hbb::Message msg_in;
                if (msg_in.ParseFromArray(bytes->data(), static_cast<int>(bytes->size()))) {
                    if (!conn.on_message(msg_in)) {
                        break;
                    }
                }
            } else if (conn.stream_.peer_closed()) {
                conn.on_close("Reset by the peer", true);
                closed = true;
            }
        } catch (const std::exception& err) {
            conn.on_close(err.what(), true);  // Some(Err(err))
            closed = true;
        }
        if (closed) {
            break;
        }
        const auto now = Clock::now();
        // _ = conn.timer.tick()
        if (conn.timer_.ready(now)) {
            progressed = true;
            conn.timer_.tick();
            if (!conn.read_jobs_.empty()) {
                try {
                    hbb_common::handle_read_jobs(conn.read_jobs_, conn.stream_);
                } catch (const std::exception& err) {
                    conn.on_close(err.what(), false);
                    break;
                }
            } else {
                conn.timer_ = Interval::at(Clock::now() + kSec30, kSec30);
            }
        }
        // _ = test_delay_timer.tick()
        if (test_delay_timer.ready(now)) {
            progressed = true;
            test_delay_timer.tick();
            if (Clock::now() - last_recv_time >= kSec30) {
                conn.on_close("Timeout", true);
                break;
            }
            const int64_t time = get_time();
            if (time > 0 && conn.last_test_delay_ == 0) {
                conn.last_test_delay_ = time;
                hbb::Message msg_out;
                msg_out.mutable_test_delay()->set_time(time);
                conn.inner_.send(std::make_shared<hbb::Message>(std::move(msg_out)));
            }
        }
        if (!progressed) {
            const auto wait_now = Clock::now();
            const auto deadline = std::min(conn.timer_.next, test_delay_timer.next);
            wait_readable(fds, ms_until(deadline, wait_now));
            if (fds[1].revents) {
                rx->clear_wake();
            }
            if (fds[2].revents) {
                rx_from_cm.clear_wake();
            }
        }
    }
    video_service::update_internal_latency(id, 0);
    video_service::update_test_latency(id, 0);
    video_service::update_image_quality(id, std::nullopt);
    if (conn.port_forward_socket_) {
        TcpFramedStream& forward = *conn.port_forward_socket_;
        // log::info!("Running port forwarding loop");
        conn.stream_.set_raw();
        std::vector<struct pollfd> pf_fds = {
            {forward.fd(), POLLIN, 0},
            {conn.stream_.fd(), POLLIN, 0},
            {rx_from_cm.wake_fd(), POLLIN, 0},
        };
        while (true) {
            bool progressed = false;
            // Some(data) = rx_from_cm.recv()
            if (auto data = rx_from_cm.try_recv()) {
                progressed = true;
                if (data->is<ipc::Data::Close>()) {
                    conn.on_close("Close requested from connection manager", false);
                    break;
                }
            }
            // res = forward.next()
            bool done = false;
            try {
                if (auto bytes = forward.try_next()) {
                    progressed = true;
                    last_recv_time = Clock::now();
                    if (!conn.stream_.send_bytes(*bytes)) {
                        conn.on_close("send failed", false);
                        done = true;
                    }
                } else if (forward.peer_closed()) {
                    conn.on_close("Forward reset by the peer", false);
                    done = true;
                }
            } catch (const std::exception& err) {
                conn.on_close(err.what(), false);
                done = true;
            }
            if (done) {
                break;
            }
            // res = conn.stream.next()
            try {
                if (auto bytes = conn.stream_.try_next()) {
                    progressed = true;
                    last_recv_time = Clock::now();
                    if (!forward.send_bytes(*bytes)) {
                        conn.on_close("send failed", false);
                        done = true;
                    }
                } else if (conn.stream_.peer_closed()) {
                    conn.on_close("Stream reset by the peer", false);
                    done = true;
                }
            } catch (const std::exception& err) {
                conn.on_close(err.what(), false);
                done = true;
            }
            if (done) {
                break;
            }
            // _ = conn.timer.tick()
            const auto now = Clock::now();
            if (conn.timer_.ready(now)) {
                progressed = true;
                conn.timer_.tick();
                if (now - last_recv_time >= kH1) {
                    conn.on_close("Timeout", false);
                    break;
                }
            }
            if (!progressed) {
                wait_readable(pf_fds, ms_until(conn.timer_.next, Clock::now()));
                if (pf_fds[2].revents) {
                    rx_from_cm.clear_wake();
                }
            }
        }
    }
}

void Connection::send_permisssion(hbb::Permission permission, bool enabled) {
    hbb::Misc misc;
    hbb::PermissionInfo* info = misc.mutable_permission_info();
    info->set_permission(permission);
    info->set_enabled(enabled);
    send(misc_message(std::move(misc)));
}

bool Connection::on_open(const ResolvedAddr& addr) {
    // log::debug!("#{} Connection opened from {}.", self.inner.id, addr);
    std::vector<std::string> whitelist;
    {
        const std::string option = hbb_common::get_option("whitelist");
        size_t start = 0;
        while (true) {
            const size_t comma = option.find(',', start);
            std::string item = option.substr(start, comma == std::string::npos ? std::string::npos
                                                                                : comma - start);
            if (!item.empty()) {
                whitelist.push_back(std::move(item));
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    const bool any_ip =
        std::find(whitelist.begin(), whitelist.end(), "0.0.0.0") != whitelist.end();
    const bool listed = std::any_of(whitelist.begin(), whitelist.end(), [&](const std::string& x) {
        const auto ip = hbb_common::parse_ip_addr(x);  // x.parse() == Ok(addr.ip())
        return ip && ip->family == addr.family && ip->ip == addr.ip;
    });
    if (!whitelist.empty() && !any_ip && !listed) {
        send_login_error("Your ip is blocked by the peer");
        sleep_secs(1.);
        return false;
    }
    ip_ = addr.ip;
    hbb::Message msg_out;
    *msg_out.mutable_hash() = hash_;
    send(msg_out);
    return true;
}

void Connection::send_logon_response() {
    if (authorized_) {
        return;
    }
    std::string username = platform::get_active_username();
    hbb::LoginResponse res;
    if (port_forward_socket_) {
        hbb::Message msg_out;
        hbb::PeerInfo* pi = res.mutable_peer_info();
        pi->set_hostname(whoami::hostname());
        pi->set_username(username);
        pi->set_platform(whoami::platform());
        pi->set_version(cppdesk::kVersion);
        *msg_out.mutable_login_response() = res;
        send(msg_out);
        return;
    }
#if defined(__linux__)
    if (!file_transfer_) {
        const std::string dtype = platform::get_display_server();
        if (dtype != "x11") {
            res.set_error("Unsupported display server type " + dtype + ", x11 expected");
            hbb::Message msg_out;
            *msg_out.mutable_login_response() = res;
            send(msg_out);
            return;
        }
    }
#endif
    bool sas_enabled = false;
#if defined(_WIN32)
    if (platform::is_root()) {
        sas_enabled = true;
    }
#endif
    if (file_transfer_) {
        if (platform::is_prelogin() || !tx_to_cm_.send(ipc::Data::Test{})) {
            username = "";
        }
    }
    authorized_ = true;
    hbb::PeerInfo pi;
    pi.set_hostname(whoami::hostname());
    pi.set_username(username);
    pi.set_platform(whoami::platform());
    pi.set_version(cppdesk::kVersion);
    pi.set_sas_enabled(sas_enabled);
    bool sub_service = false;
    if (file_transfer_) {
        *res.mutable_peer_info() = pi;
    } else {
        try_activate_screen();
        try {
            auto [current, displays] = video_service::get_displays();
            for (auto& d : displays) {
                *pi.add_displays() = std::move(d);
            }
            pi.set_current_display(static_cast<int32_t>(current));
            *res.mutable_peer_info() = pi;
            sub_service = true;
        } catch (const std::exception& err) {
            res.set_error(err.what());
        }
    }
    hbb::Message msg_out;
    *msg_out.mutable_login_response() = res;
    send(msg_out);
    if (file_transfer_) {
        const auto [dir, show_hidden] = *file_transfer_;
        std::error_code ec;
        const bool is_dir = !dir.empty() && std::filesystem::is_directory(dir, ec);
        read_dir(is_dir ? dir : "", show_hidden);
    } else if (sub_service) {
        if (auto s = server_.lock()) {
            std::vector<std::string> noperms;
            if (!keyboard_ && !show_remote_cursor_) {
                noperms.push_back(input_service::kNameCursor);
            }
            if (!show_remote_cursor_) {
                noperms.push_back(input_service::kNamePos);
            }
            if (!clipboard_enabled() || !keyboard_) {
                noperms.push_back(clipboard_service::kName);
            }
            if (!audio_enabled()) {
                noperms.push_back(audio_service::kName);
            }
            s->add_connection(inner_, noperms);
        }
    }
}

bool Connection::clipboard_enabled() const {
    return clipboard_ && !disable_clipboard_;
}

bool Connection::audio_enabled() const {
    return audio_ && !disable_audio_;
}

void Connection::try_start_cm(const std::string& peer_id, const std::string& name,
                              bool authorized) {
    ipc::Data::Login login;
    login.id = inner_.get_id();
    login.is_file_transfer = file_transfer_.has_value();
    login.port_forward = port_forward_address_;
    login.peer_id = peer_id;
    login.name = name;
    login.authorized = authorized;
    login.keyboard = keyboard_;
    login.clipboard = clipboard_;
    login.audio = audio_;
    send_to_cm(std::move(login));
}

void Connection::send_to_cm(ipc::Data data) {
    tx_to_cm_.send(std::move(data));  // .ok()
}

void Connection::send_fs(ipc::FS data) {
    send_to_cm(ipc::Data::FS{std::move(data)});
}

void Connection::send_login_error(const std::string& err) {
    hbb::Message msg_out;
    msg_out.mutable_login_response()->set_error(err);
    send(msg_out);
}

bool Connection::on_message(const hbb::Message& msg) {
    if (msg.union_case() == hbb::Message::kLoginRequest) {
        const hbb::LoginRequest& lr = msg.login_request();
        if (lr.has_option()) {
            update_option(lr.option());
        }
        if (authorized_) {
            return true;
        }
        switch (lr.union_case()) {
            case hbb::LoginRequest::kFileTransfer: {
                if (!hbb_common::get_option("enable-file-transfer").empty()) {
                    send_login_error("No permission of file transfer");
                    sleep_secs(1.);
                    return false;
                }
                file_transfer_ = std::make_pair(lr.file_transfer().dir(),
                                                lr.file_transfer().show_hidden());
                break;
            }
            case hbb::LoginRequest::kPortForward: {
                if (!hbb_common::get_option("enable-tunnel").empty()) {
                    send_login_error("No permission of IP tunneling");
                    sleep_secs(1.);
                    return false;
                }
                hbb::PortForward pf = lr.port_forward();
                bool is_rdp = false;
                if (pf.host() == "RDP" && pf.port() == 0) {
                    pf.set_host("localhost");
                    pf.set_port(3389);
                    is_rdp = true;
                }
                if (pf.host().empty()) {
                    pf.set_host("localhost");
                }
                std::string addr = pf.host() + ":" + std::to_string(pf.port());
                port_forward_address_ = addr;
                // timeout(3000, TcpStream::connect(&addr))
                if (auto sock = TcpFramedStream::connect_host(addr, 3000)) {
                    sock->set_raw();  // Framed::new(sock, tokio_util BytesCodec::new())
                    port_forward_socket_ = std::move(*sock);
                } else {
                    if (is_rdp) {
                        addr = "RDP";
                    }
                    send_login_error("Failed to access remote " + addr +
                                     ", please make sure if it is open");
                }
                break;
            }
            default:
                break;
        }
        if (lr.username() != hbb_common::get_id()) {
            send_login_error("Offline");
        } else if (lr.password().empty()) {
            try_start_cm(lr.my_id(), lr.my_name(), false);
        } else {
            // Sha256(Sha256(password + salt) + challenge)
            const std::string password = hbb_common::get_password();
            unsigned char first[SHA256_DIGEST_LENGTH];
            {
                SHA256_CTX ctx;
                SHA256_Init(&ctx);
                SHA256_Update(&ctx, password.data(), password.size());
                SHA256_Update(&ctx, hash_.salt().data(), hash_.salt().size());
                SHA256_Final(first, &ctx);
            }
            unsigned char second[SHA256_DIGEST_LENGTH];
            {
                SHA256_CTX ctx;
                SHA256_Init(&ctx);
                SHA256_Update(&ctx, first, sizeof first);
                SHA256_Update(&ctx, hash_.challenge().data(), hash_.challenge().size());
                SHA256_Final(second, &ctx);
            }
            Failure failure{0, 0, 0};
            {
                std::lock_guard<std::mutex> l(login_failures_lock());
                const auto it = login_failures().find(ip_);
                if (it != login_failures().end()) {
                    failure = it->second;
                }
            }
            const auto time = static_cast<int32_t>(get_time() / 60'000);
            auto& [minute, in_minute, total] = failure;
            if (total > 30) {
                send_login_error("Too many wrong password attempts");
            } else if (time == minute && in_minute > 6) {
                send_login_error("Please try 1 minute later");
            } else if (lr.password() !=
                       std::string(reinterpret_cast<const char*>(second), sizeof second)) {
                if (minute == time) {
                    in_minute += 1;
                    total += 1;
                } else {
                    minute = time;
                    in_minute = 1;
                    total += 1;
                }
                {
                    std::lock_guard<std::mutex> l(login_failures_lock());
                    login_failures()[ip_] = failure;
                }
                send_login_error("Wrong Password");
                try_start_cm(lr.my_id(), lr.my_name(), false);
            } else {
                if (minute != 0) {
                    std::lock_guard<std::mutex> l(login_failures_lock());
                    login_failures().erase(ip_);
                }
                send_logon_response();
                try_start_cm(lr.my_id(), lr.my_name(), true);
                if (port_forward_socket_) {
                    return false;
                }
            }
        }
    } else if (msg.union_case() == hbb::Message::kTestDelay) {
        const hbb::TestDelay& t = msg.test_delay();
        if (t.from_client()) {
            hbb::Message msg_out;
            *msg_out.mutable_test_delay() = t;
            inner_.send(std::make_shared<hbb::Message>(std::move(msg_out)));
        } else {
            last_test_delay_ = 0;
            const int64_t latency = get_time() - t.time();
            if (latency > 0) {
                video_service::update_test_latency(inner_.get_id(), latency);
            }
        }
    } else if (authorized_) {
        switch (msg.union_case()) {
            case hbb::Message::kMouseEvent: {
                const hbb::MouseEvent& me = msg.mouse_event();
                if (keyboard_) {
                    input_service::handle_mouse(me, inner_.get_id());
                    if (input_service::is_left_up(me)) {
                        set_click_time_now();
                    }
                }
                break;
            }
            case hbb::Message::kKeyEvent: {
                hbb::KeyEvent me = msg.key_event();
                if (keyboard_) {
                    if (me.press()) {
                        if (me.union_case() == hbb::KeyEvent::kUnicode) {
                            input_service::handle_key(me);
                        } else if (me.union_case() == hbb::KeyEvent::kSeq) {
                            input_service::handle_key(me);
                        } else {
                            me.set_down(true);
                            input_service::handle_key(me);
                            me.set_down(false);
                            input_service::handle_key(me);
                        }
                    } else {
                        input_service::handle_key(me);
                    }
                    if (input_service::is_enter(me)) {
                        set_click_time_now();
                    }
                }
                break;
            }
            case hbb::Message::kClipboard:
                if (clipboard_) {
                    update_clipboard(msg.clipboard());
                }
                break;
            case hbb::Message::kFileAction: {
                if (file_transfer_) {
                    const hbb::FileAction& fa = msg.file_action();
                    switch (fa.union_case()) {
                        case hbb::FileAction::kReadDir:
                            read_dir(fa.read_dir().path(), fa.read_dir().include_hidden());
                            break;
                        case hbb::FileAction::kAllFiles: {
                            const hbb::ReadAllFiles& f = fa.all_files();
                            try {
                                auto files = hbb_common::get_recursive_files(f.path(),
                                                                             f.include_hidden());
                                send(hbb_common::new_dir(f.id(), std::move(files)));
                            } catch (const std::exception& err) {
                                send(hbb_common::new_error(f.id(), err.what(), -1));
                            }
                            break;
                        }
                        case hbb::FileAction::kSend: {
                            const hbb::FileTransferSendRequest& s = fa.send();
                            const int32_t id = s.id();
                            try {
                                hbb_common::TransferJob job = hbb_common::TransferJob::new_read(
                                    id, s.path(), s.include_hidden());
                                send(hbb_common::new_dir(id, job.files()));
                                read_jobs_.push_back(std::move(job));
                                timer_ = Interval::every(kMilli1);
                            } catch (const std::exception& err) {
                                send(hbb_common::new_error(id, err.what(), 0));
                            }
                            break;
                        }
                        case hbb::FileAction::kReceive: {
                            const hbb::FileTransferReceiveRequest& r = fa.receive();
                            ipc::FS::NewWrite nw;
                            nw.path = r.path();
                            nw.id = r.id();
                            for (const hbb::FileEntry& f : r.files()) {
                                nw.files.emplace_back(f.name(), f.modified_time());
                            }
                            send_fs(std::move(nw));
                            break;
                        }
                        case hbb::FileAction::kRemoveDir: {
                            const hbb::FileRemoveDir& d = fa.remove_dir();
                            send_fs(ipc::FS::RemoveDir{d.path(), d.id(), d.recursive()});
                            break;
                        }
                        case hbb::FileAction::kRemoveFile: {
                            const hbb::FileRemoveFile& f = fa.remove_file();
                            send_fs(ipc::FS::RemoveFile{f.path(), f.id(), f.file_num()});
                            break;
                        }
                        case hbb::FileAction::kCreate: {
                            const hbb::FileDirCreate& c = fa.create();
                            send_fs(ipc::FS::CreateDir{c.path(), c.id()});
                            break;
                        }
                        case hbb::FileAction::kCancel: {
                            const int32_t id = fa.cancel().id();
                            send_fs(ipc::FS::CancelWrite{id});
                            hbb_common::remove_job(id, read_jobs_);
                            break;
                        }
                        default:
                            break;
                    }
                }
                break;
            }
            case hbb::Message::kFileResponse: {
                const hbb::FileResponse& fr = msg.file_response();
                switch (fr.union_case()) {
                    case hbb::FileResponse::kBlock: {
                        const hbb::FileTransferBlock& block = fr.block();
                        send_fs(ipc::FS::WriteBlock{
                            block.id(), block.file_num(),
                            std::vector<uint8_t>(block.data().begin(), block.data().end()),
                            block.compressed()});
                        break;
                    }
                    case hbb::FileResponse::kDone: {
                        const hbb::FileTransferDone& d = fr.done();
                        send_fs(ipc::FS::WriteDone{d.id(), d.file_num()});
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case hbb::Message::kMisc: {
                const hbb::Misc& misc = msg.misc();
                switch (misc.union_case()) {
                    case hbb::Misc::kSwitchDisplay:
                        video_service::switch_display(misc.switch_display().display());
                        break;
                    case hbb::Misc::kChatMessage:
                        send_to_cm(ipc::Data::ChatMessage{misc.chat_message().text()});
                        break;
                    case hbb::Misc::kOption:
                        update_option(misc.option());
                        break;
                    case hbb::Misc::kRefreshVideo:
                        if (misc.refresh_video()) {
                            video_service::refresh();
                        }
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }
    return true;
}

void Connection::update_option(const hbb::OptionMessage& o) {
    // log::info!("Option update: {:?}", o);
    // ProtobufEnumOrUnknown::enum_value() is Ok for every known value,
    // NotSet included (so an unset image_quality clears this connection's
    // entry: convert_quality(0) is 0).
    if (hbb::ImageQuality_IsValid(o.image_quality())) {
        image_quality_ = o.image_quality();
        video_service::update_image_quality(inner_.get_id(), image_quality_);
    }
    const int32_t q = o.custom_image_quality();
    if (q > 0) {
        image_quality_ = q;
        video_service::update_image_quality(inner_.get_id(), q);
    }
    if (hbb::BoolOption_IsValid(o.lock_after_session_end())) {
        const auto v = o.lock_after_session_end();
        if (v != hbb::BoolOptionNotSet) {
            lock_after_session_end_ = v == hbb::Yes;
        }
    }
    if (hbb::BoolOption_IsValid(o.show_remote_cursor())) {
        const auto v = o.show_remote_cursor();
        if (v != hbb::BoolOptionNotSet) {
            show_remote_cursor_ = v == hbb::Yes;
            if (auto s = server_.lock()) {
                s->subscribe(input_service::kNameCursor, inner_,
                             keyboard_ || show_remote_cursor_);
                s->subscribe(input_service::kNamePos, inner_, show_remote_cursor_);
            }
        }
    }
    if (hbb::BoolOption_IsValid(o.disable_audio())) {
        const auto v = o.disable_audio();
        if (v != hbb::BoolOptionNotSet) {
            disable_audio_ = v == hbb::Yes;
            if (auto s = server_.lock()) {
                // Verbatim: upstream (re)subscribes NAME_CURSOR here, not the
                // audio service.
                s->subscribe(input_service::kNameCursor, inner_, audio_enabled());
            }
        }
    }
    if (hbb::BoolOption_IsValid(o.disable_clipboard())) {
        const auto v = o.disable_clipboard();
        if (v != hbb::BoolOptionNotSet) {
            disable_clipboard_ = v == hbb::Yes;
            if (auto s = server_.lock()) {
                // Verbatim: NAME_CURSOR again, not the clipboard service.
                s->subscribe(input_service::kNameCursor, inner_,
                             clipboard_enabled() && keyboard_);
            }
        }
    }
    if (hbb::BoolOption_IsValid(o.privacy_mode())) {
        const auto v = o.privacy_mode();
        if (v != hbb::BoolOptionNotSet) {
            privacy_mode_ = v == hbb::Yes;
            if (privacy_mode_ && keyboard_) {
                platform::toggle_privacy_mode(true);
            }
        }
    }
    if (keyboard_) {
        if (hbb::BoolOption_IsValid(o.block_input())) {
            const auto v = o.block_input();
            if (v != hbb::BoolOptionNotSet) {
                platform::block_input(v == hbb::Yes);
            }
        }
    }
}

void Connection::on_close(const std::string& reason, bool lock) {
    if (auto s = server_.lock()) {
        s->remove_connection(inner_);
    }
    // log::info!("#{} Connection closed: {}", self.inner.id(), reason);
    (void)reason;
    if (lock && lock_after_session_end_ && keyboard_) {
        platform::lock_screen();
        video_service::switch_to_primary();
    }
    if (privacy_mode_) {
        platform::toggle_privacy_mode(false);
    }
    port_forward_socket_.reset();
}

void Connection::read_dir(const std::string& dir, bool include_hidden) {
    send_fs(ipc::FS::ReadDir{dir, include_hidden});
}

void Connection::send(const hbb::Message& msg) {
    HBB_ALLOW_ERR(stream_.send(msg));
}

// --- start_ipc / try_activate_screen ------------------------------------------

void start_ipc(UnboundedReceiver<ipc::Data> rx_to_cm, UnboundedSender<ipc::Data> tx_from_cm) {
    while (true) {
        if (!platform::is_prelogin()) {
            break;
        }
        sleep_secs(1.);
    }
    std::optional<ipc::Connection> stream;
    try {
        stream.emplace(ipc::connect(1000, "_cm"));
    } catch (const std::exception&) {
    }
    if (!stream) {
        bool run_done = false;
        if (platform::is_root()) {
            // let mut res = Ok(None); up to 10 tries, 1 s apart.
            std::optional<intptr_t> task;
            std::exception_ptr err;
            for (int i = 0; i < 10; ++i) {
                try {
                    task = platform::run_as_user("--cm");
                    err = nullptr;
                    break;
                } catch (...) {
                    err = std::current_exception();
                }
                sleep_secs(1.);
            }
            if (err) {
                std::rethrow_exception(err);  // res?
            }
            if (task) {
                track_child_pid(*task);  // CHILD_PROCESS.push(task)
            }
            run_done = true;
        }
        if (!run_done) {
            track_child_pid(run_me({"--cm"}));
        }
        for (int i = 0; i < 10; ++i) {
            sleep_secs(0.3);
            try {
                stream.emplace(ipc::connect(1000, "_cm"));
                break;
            } catch (const std::exception&) {
            }
        }
        if (!stream) {
            throw std::runtime_error("Failed to connect to connection manager");
        }
    }
    ipc::Connection& s = *stream;
    std::vector<struct pollfd> fds = {
        {s.fd(), POLLIN, 0},
        {rx_to_cm.wake_fd(), POLLIN, 0},
    };
    while (true) {
        bool progressed = false;
        // res = stream.next(): Err returns (try_next throws).
        if (auto res = s.try_next()) {
            progressed = true;
            if (*res) {
                ipc::Data data = std::move(**res);
                if (data.is<ipc::Data::ClickTime>()) {
                    const int64_t ct = click_time();
                    s.send(ipc::Data::ClickTime{ct});
                } else if (!tx_from_cm.send(std::move(data))) {
                    throw std::runtime_error("channel closed");  // SendError
                }
            }
            // Ok(None): `_ => {}`
        }
        // res = rx_to_cm.recv()
        if (auto data = rx_to_cm.try_recv()) {
            progressed = true;
            s.send(*data);
        } else if (rx_to_cm.closed()) {
            throw std::runtime_error("expected");  // bail!("expected")
        }
        if (!progressed) {
            wait_readable(fds, -1);
            if (fds[1].revents) {
                rx_to_cm.clear_wake();
            }
        }
    }
}

void try_activate_screen() {
#if defined(_WIN32)
    std::thread([] {
        input_service::mouse_move_relative(-6, -6);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        input_service::mouse_move_relative(6, 6);
    }).detach();
#endif
}

}  // namespace rustdesk
