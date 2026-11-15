// test_ipc.cpp — src/ipc.rs (step 14).
//
// Covers: Data/FS serde_json shapes (byte-exact) and decode rules; framing
// over a socketpair (coalesced frames, frames larger than the socket buffer,
// partial frames across timeouts, undecodable frames, EOF); the ipc server's
// handle() through a live in-process server; Data::Close (process exit) and
// the cross-process client helpers through a re-exec'd child daemon at the
// real postfix ""; check_pid / write_pid.
//
// Hermetic: HOME/XDG_CONFIG_HOME point at a temp dir before any Config access,
// and custom-rendezvous-server points at 127.0.0.1:1 so the daemon's
// test_nat_type stays on loopback. The postfix "" cases are skipped when a live
// IPC daemon already answers at /tmp/RustDesk/ipc (never disturb a real one).

#include <rustdesk/common.hpp>
#include <rustdesk/ipc.hpp>

#include <hbb_common/bytes_codec.hpp>
#include <hbb_common/config.hpp>
#include <hbb_common/util.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

namespace ipc = rustdesk::ipc;
using ipc::Data;
using ipc::FS;
using StrMap = std::unordered_map<std::string, std::string>;
using OptStr = std::optional<std::string>;

const char* g_self_exe = nullptr;  // argv[0] fallback for re-exec

// --- helpers ---------------------------------------------------------------------

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Poll `pred` until it holds or `ms` elapse (cross-connection effects are
// asynchronous: each IPC client is served by its own server thread).
bool eventually(const std::function<bool()>& pred, int ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (true) {
        try {
            if (pred()) {
                return true;
            }
        } catch (const std::exception&) {
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        sleep_ms(20);
    }
}

ipc::Connection connect_retry(const std::string& postfix, int ms = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (true) {
        try {
            return ipc::connect(1000, postfix);
        } catch (const std::exception&) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            sleep_ms(20);
        }
    }
}

// Config((name, None)) round trip on one connection (the server answers each
// connection's messages in order). Throws when the reply is not that Config.
OptStr ask_config(ipc::Connection& c, const std::string& name) {
    c.send(Data::Config{name, std::nullopt});
    const auto reply = c.next_timeout(3000);
    const Data::Config* cfg = reply ? reply->get_if<Data::Config>() : nullptr;
    if (!cfg || cfg->name != name) {
        throw std::runtime_error("unexpected reply to Config(" + name + ")");
    }
    return cfg->value;
}

template <class T>
T ask(ipc::Connection& c, const Data& request) {
    c.send(request);
    const auto reply = c.next_timeout(3000);
    const T* v = reply ? reply->get_if<T>() : nullptr;
    if (!v) {
        throw std::runtime_error("unexpected reply type");
    }
    return *v;
}

struct IpcPair {
    ipc::Connection left;
    ipc::Connection right;
};

struct RawPair {
    ipc::Connection ipc;
    parity_tokio_ipc::Connection raw;
};

void make_socketpair(int sv[2]) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        throw std::runtime_error("socketpair failed");
    }
}

IpcPair ipc_pair() {
    int sv[2];
    make_socketpair(sv);
    return IpcPair{ipc::Connection(parity_tokio_ipc::Connection(sv[0])),
                   ipc::Connection(parity_tokio_ipc::Connection(sv[1]))};
}

RawPair raw_pair() {
    int sv[2];
    make_socketpair(sv);
    return RawPair{ipc::Connection(parity_tokio_ipc::Connection(sv[0])),
                   parity_tokio_ipc::Connection(sv[1])};
}

std::vector<uint8_t> frame_bytes(const std::vector<std::string>& payloads) {
    hbb_common::BytesCodec codec;
    std::vector<uint8_t> wire;
    for (const std::string& p : payloads) {
        codec.encode(reinterpret_cast<const uint8_t*>(p.data()), p.size(), wire);
    }
    return wire;
}

void write_raw(parity_tokio_ipc::Connection& c, const std::vector<uint8_t>& bytes) {
    c.write_all(bytes.data(), bytes.size());
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

mode_t file_mode(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return st.st_mode & 0777;
}

// A child process running `ipc::start(postfix)`, SIGKILLed on scope exit.
struct Child {
    pid_t pid = -1;
    bool reaped = false;
    int status = 0;

    ~Child() { kill_and_reap(); }

    void kill_and_reap() {
        if (pid > 0 && !reaped) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            reaped = true;
        }
    }

    // True once the child exited within `ms` (status filled in).
    bool wait_exit(int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reaped = true;
                return true;
            }
            sleep_ms(20);
        }
        return false;
    }
};

std::string self_exe() {
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        return std::string(buf, static_cast<size_t>(n));
    }
    return g_self_exe ? g_self_exe : "";
}

// Re-exec this test binary as `--ipc-child <postfix|->` (exec gives the child
// a clean process: no inherited threads or locks from this one).
void spawn_child(Child& child, const std::string& postfix) {
    const std::string exe = self_exe();
    const std::string arg = postfix.empty() ? "-" : postfix;
    const char* argv0 = exe.c_str();
    const char* argv2 = arg.c_str();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(argv0, argv0, "--ipc-child", argv2, static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    child.pid = pid;
}

void start_server_thread(const std::string& postfix) {
    std::thread([postfix] {
        try {
            ipc::start(postfix);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "in-process ipc server (%s) stopped: %s\n", postfix.c_str(),
                         e.what());
        }
    }).detach();
}

// --- serde shapes --------------------------------------------------------------------

void check_shape(const Data& d, const std::string& expected) {
    const std::string got = ipc::to_json(d);
    if (got != expected) {
        ++g_failures;
        std::fprintf(stderr, "FAIL shape:\n  got      %s\n  expected %s\n", got.c_str(),
                     expected.c_str());
    }
    const auto back = ipc::from_json(got);
    if (!back || !(*back == d)) {
        ++g_failures;
        std::fprintf(stderr, "FAIL round trip: %s\n", expected.c_str());
    }
}

Data::Login sample_login() {
    Data::Login l;
    l.id = 7;
    l.is_file_transfer = true;
    l.peer_id = "123456789";
    l.name = "alice";
    l.authorized = false;
    l.port_forward = "localhost:22";
    l.keyboard = true;
    l.clipboard = false;
    l.audio = true;
    return l;
}

void test_shapes() {
    check_shape(sample_login(),
                "{\"t\":\"Login\",\"c\":{\"id\":7,\"is_file_transfer\":true,"
                "\"peer_id\":\"123456789\",\"name\":\"alice\",\"authorized\":false,"
                "\"port_forward\":\"localhost:22\",\"keyboard\":true,\"clipboard\":false,"
                "\"audio\":true}}");
    check_shape(Data::ChatMessage{"hi \"x\"\n\xc3\xa9"},
                "{\"t\":\"ChatMessage\",\"c\":{\"text\":\"hi \\\"x\\\"\\n\xc3\xa9\"}}");
    check_shape(Data::SwitchPermission{"keyboard", true},
                "{\"t\":\"SwitchPermission\",\"c\":{\"name\":\"keyboard\",\"enabled\":true}}");
    check_shape(Data::SystemInfo{}, "{\"t\":\"SystemInfo\",\"c\":null}");
    check_shape(Data::SystemInfo{"info"}, "{\"t\":\"SystemInfo\",\"c\":\"info\"}");
    check_shape(Data::ClickTime{-1}, "{\"t\":\"ClickTime\",\"c\":-1}");
    check_shape(Data::ClickTime{1616992000000}, "{\"t\":\"ClickTime\",\"c\":1616992000000}");
    check_shape(Data::Authorize{}, "{\"t\":\"Authorize\"}");
    check_shape(Data::Close{}, "{\"t\":\"Close\"}");
    check_shape(Data::SAS{}, "{\"t\":\"SAS\"}");
    check_shape(Data::OnlineStatus{}, "{\"t\":\"OnlineStatus\",\"c\":null}");
    check_shape(Data::OnlineStatus{std::make_pair(int64_t{42}, true)},
                "{\"t\":\"OnlineStatus\",\"c\":[42,true]}");
    check_shape(Data::Config{"id", std::nullopt}, "{\"t\":\"Config\",\"c\":[\"id\",null]}");
    check_shape(Data::Config{"password", std::string("secret")},
                "{\"t\":\"Config\",\"c\":[\"password\",\"secret\"]}");
    check_shape(Data::Options{}, "{\"t\":\"Options\",\"c\":null}");
    Data::Options one;
    one.value = StrMap{{"stop-service", "Y"}};
    check_shape(one, "{\"t\":\"Options\",\"c\":{\"stop-service\":\"Y\"}}");
    Data::Options none_set;
    none_set.value = StrMap{};
    check_shape(none_set, "{\"t\":\"Options\",\"c\":{}}");
    check_shape(Data::NatType{}, "{\"t\":\"NatType\",\"c\":null}");
    check_shape(Data::NatType{1}, "{\"t\":\"NatType\",\"c\":1}");
    check_shape(Data::ConfirmedKey{}, "{\"t\":\"ConfirmedKey\",\"c\":null}");
    Data::ConfirmedKey ck;
    ck.value = std::make_pair(std::vector<uint8_t>{1, 2}, std::vector<uint8_t>{255});
    check_shape(ck, "{\"t\":\"ConfirmedKey\",\"c\":[[1,2],[255]]}");
    check_shape(Data::RawMessage{{0, 127, 255}}, "{\"t\":\"RawMessage\",\"c\":[0,127,255]}");
    check_shape(Data::RawMessage{}, "{\"t\":\"RawMessage\",\"c\":[]}");
    check_shape(Data::Test{}, "{\"t\":\"Test\"}");

    check_shape(Data::FS{FS::ReadDir{"/home", true}},
                "{\"t\":\"FS\",\"c\":{\"t\":\"ReadDir\",\"c\":{\"dir\":\"/home\","
                "\"include_hidden\":true}}}");
    check_shape(Data::FS{FS::RemoveDir{"/tmp/x", 3, true}},
                "{\"t\":\"FS\",\"c\":{\"t\":\"RemoveDir\",\"c\":{\"path\":\"/tmp/x\",\"id\":3,"
                "\"recursive\":true}}}");
    check_shape(Data::FS{FS::RemoveFile{"/tmp/x/f", 3, 2}},
                "{\"t\":\"FS\",\"c\":{\"t\":\"RemoveFile\",\"c\":{\"path\":\"/tmp/x/f\",\"id\":3,"
                "\"file_num\":2}}}");
    check_shape(Data::FS{FS::CreateDir{"/tmp/y", 4}},
                "{\"t\":\"FS\",\"c\":{\"t\":\"CreateDir\",\"c\":{\"path\":\"/tmp/y\",\"id\":4}}}");
    FS::NewWrite nw;
    nw.path = "/tmp/z";
    nw.id = 5;
    nw.files = {{"a.txt", 10}, {"b/c.bin", 4294967296ull}};
    check_shape(Data::FS{nw},
                "{\"t\":\"FS\",\"c\":{\"t\":\"NewWrite\",\"c\":{\"path\":\"/tmp/z\",\"id\":5,"
                "\"files\":[[\"a.txt\",10],[\"b/c.bin\",4294967296]]}}}");
    check_shape(Data::FS{FS::CancelWrite{5}},
                "{\"t\":\"FS\",\"c\":{\"t\":\"CancelWrite\",\"c\":{\"id\":5}}}");
    FS::WriteBlock wb;
    wb.id = 5;
    wb.file_num = 1;
    wb.data = {104, 105};
    wb.compressed = false;
    check_shape(Data::FS{wb},
                "{\"t\":\"FS\",\"c\":{\"t\":\"WriteBlock\",\"c\":{\"id\":5,\"file_num\":1,"
                "\"data\":[104,105],\"compressed\":false}}}");
    check_shape(Data::FS{FS::WriteDone{5, 1}},
                "{\"t\":\"FS\",\"c\":{\"t\":\"WriteDone\",\"c\":{\"id\":5,\"file_num\":1}}}");
}

void test_decode_rules() {
    auto dec = [](const std::string& s) { return ipc::from_json(s); };
    // "c" may precede "t"; unknown keys are skipped.
    const auto a = dec("{\"c\":[\"id\",null],\"x\":1,\"t\":\"Config\"}");
    CHECK(a && a->is<Data::Config>() && a->get_if<Data::Config>()->name == "id");
    // Unit variants: absent or null content is fine, anything else is not.
    CHECK(dec("{\"t\":\"Close\",\"c\":null}").has_value());
    CHECK(!dec("{\"t\":\"Close\",\"c\":1}"));
    // Option newtype payload: a missing "c" is None.
    const auto ni = dec("{\"t\":\"NatType\"}");
    CHECK(ni && ni->is<Data::NatType>() && !ni->get_if<Data::NatType>()->value);
    const auto si = dec("{\"t\":\"SystemInfo\"}");
    CHECK(si && !si->get_if<Data::SystemInfo>()->value);
    // Required payloads: a missing "c" is an error.
    CHECK(!dec("{\"t\":\"ClickTime\"}"));
    CHECK(!dec("{\"t\":\"Login\"}"));
    CHECK(!dec("{\"t\":\"Config\"}"));
    CHECK(!dec("{\"t\":\"RawMessage\"}"));
    CHECK(!dec("{\"t\":\"FS\"}"));
    // Struct fields: missing / duplicate / mistyped rejected, unknown ignored.
    CHECK(!dec("{\"t\":\"ChatMessage\",\"c\":{}}"));
    CHECK(!dec("{\"t\":\"ChatMessage\",\"c\":{\"text\":\"a\",\"text\":\"b\"}}"));
    CHECK(!dec("{\"t\":\"ChatMessage\",\"c\":{\"text\":1}}"));
    CHECK(!dec("{\"t\":\"ChatMessage\",\"c\":[\"a\"]}"));
    const auto extra = dec("{\"t\":\"ChatMessage\",\"c\":{\"text\":\"a\",\"extra\":true}}");
    CHECK(extra && extra->get_if<Data::ChatMessage>() &&
          extra->get_if<Data::ChatMessage>()->text == "a");
    CHECK(!dec("{\"t\":\"SwitchPermission\",\"c\":{\"name\":\"x\",\"enabled\":1}}"));
    // Tag rules.
    CHECK(!dec("{\"c\":null}"));
    CHECK(!dec("{}"));
    CHECK(!dec("{\"t\":\"Nope\"}"));
    CHECK(!dec("{\"t\":\"close\"}"));  // variant names are case-sensitive
    CHECK(!dec("{\"t\":\"Close\",\"t\":\"Close\"}"));
    CHECK(!dec("{\"t\":\"NatType\",\"c\":1,\"c\":2}"));
    CHECK(!dec("{\"t\":1}"));
    CHECK(!dec("[\"Close\"]"));
    CHECK(!dec("\"Close\""));
    CHECK(!dec("null"));
    // Integer ranges and kinds.
    CHECK(!dec("{\"t\":\"NatType\",\"c\":2147483648}"));
    CHECK(dec("{\"t\":\"NatType\",\"c\":-2147483648}").has_value());
    CHECK(!dec("{\"t\":\"NatType\",\"c\":1.0}"));
    CHECK(!dec("{\"t\":\"NatType\",\"c\":\"1\"}"));
    CHECK(!dec("{\"t\":\"RawMessage\",\"c\":[256]}"));
    CHECK(!dec("{\"t\":\"RawMessage\",\"c\":[-1]}"));
    CHECK(!dec("{\"t\":\"RawMessage\",\"c\":\"AAE=\"}"));
    CHECK(!dec("{\"t\":\"ClickTime\",\"c\":9223372036854775808}"));
    CHECK(dec("{\"t\":\"ClickTime\",\"c\":-9223372036854775808}").has_value());
    CHECK(!dec("{\"t\":\"FS\",\"c\":{\"t\":\"NewWrite\",\"c\":{\"path\":\"p\",\"id\":1,"
               "\"files\":[[\"a\",-1]]}}}"));
    CHECK(!dec("{\"t\":\"FS\",\"c\":{\"t\":\"CancelWrite\",\"c\":{\"id\":2147483648}}}"));
    // Tuple arity.
    CHECK(!dec("{\"t\":\"Config\",\"c\":[\"id\"]}"));
    CHECK(!dec("{\"t\":\"Config\",\"c\":[\"id\",null,1]}"));
    CHECK(!dec("{\"t\":\"OnlineStatus\",\"c\":[1]}"));
    CHECK(!dec("{\"t\":\"ConfirmedKey\",\"c\":[[1]]}"));
    CHECK(!dec("{\"t\":\"FS\",\"c\":{\"t\":\"NewWrite\",\"c\":{\"path\":\"p\",\"id\":1,"
               "\"files\":[[\"a\"]]}}}"));
    // HashMap: a later duplicate key wins; values must be strings.
    const auto m = dec("{\"t\":\"Options\",\"c\":{\"k\":\"1\",\"k\":\"2\"}}");
    CHECK(m && m->get_if<Data::Options>()->value &&
          m->get_if<Data::Options>()->value->at("k") == "2");
    CHECK(!dec("{\"t\":\"Options\",\"c\":{\"k\":1}}"));
    CHECK(!dec("{\"t\":\"Options\",\"c\":[]}"));
    // Malformed JSON / trailing data.
    CHECK(!dec("{\"t\":\"Close\"} x"));
    CHECK(!dec("{\"t\":\"Close\""));
    CHECK(!dec(""));
    CHECK(dec(" {\"t\":\"Close\"} \n").has_value());
}

// --- framing --------------------------------------------------------------------------

void test_framing_roundtrip() {
    IpcPair p = ipc_pair();
    p.left.send(sample_login());
    const auto d = p.right.next();
    CHECK(d && *d == Data(sample_login()));
    p.right.send(Data::Config{"id", std::string("42")});
    const auto e = p.left.next_timeout(2000);
    CHECK(e && e->get_if<Data::Config>() && e->get_if<Data::Config>()->value == OptStr("42"));

    // Idle: next_timeout raises Elapsed, next_timeout2 yields None.
    bool timed_out = false;
    try {
        (void)p.left.next_timeout(50);
    } catch (const ipc::TimeoutError&) {
        timed_out = true;
    }
    CHECK(timed_out);
    CHECK(!p.left.next_timeout2(50).has_value());

    // next_timeout2 with data: Some(Ok(Some(data))).
    p.right.send(Data::Test{});
    const auto f = p.left.next_timeout2(2000);
    CHECK(f.has_value() && f->has_value() && (*f)->is<Data::Test>());

    // A frame far larger than the socket buffer (writer must run concurrently).
    Data::RawMessage big;
    big.value.resize(300000);
    for (size_t i = 0; i < big.value.size(); ++i) {
        big.value[i] = static_cast<uint8_t>(i * 31 + 7);
    }
    std::thread writer([&] {
        try {
            p.left.send(big);
        } catch (const std::exception& ex) {
            ++g_failures;
            std::fprintf(stderr, "FAIL big send: %s\n", ex.what());
        }
    });
    const auto got = p.right.next_timeout(20000);
    writer.join();
    CHECK(got && got->get_if<Data::RawMessage>() && got->get_if<Data::RawMessage>()->value == big.value);
}

void test_framing_raw() {
    RawPair p = raw_pair();
    // Two frames in one write: both decode (persistent read buffer).
    write_raw(p.raw, frame_bytes({ipc::to_json(Data::Close{}), ipc::to_json(Data::SAS{})}));
    const auto a = p.ipc.next_timeout(2000);
    const auto b = p.ipc.next_timeout(2000);
    CHECK(a && a->is<Data::Close>());
    CHECK(b && b->is<Data::SAS>());

    // A frame cut by a timeout stays buffered and completes later.
    const std::vector<uint8_t> wire = frame_bytes({ipc::to_json(Data::ChatMessage{"split me"})});
    const size_t half = wire.size() / 2;
    write_raw(p.raw, std::vector<uint8_t>(wire.begin(), wire.begin() + half));
    CHECK(!p.ipc.next_timeout2(100).has_value());
    write_raw(p.raw, std::vector<uint8_t>(wire.begin() + half, wire.end()));
    const auto c = p.ipc.next_timeout(2000);
    CHECK(c && c->get_if<Data::ChatMessage>() && c->get_if<Data::ChatMessage>()->text == "split me");

    // Undecodable frames are Ok(None) and leave the connection usable.
    write_raw(p.raw, frame_bytes({"not json"}));
    const auto n1 = p.ipc.next_timeout(2000);
    CHECK(!n1.has_value());
    write_raw(p.raw, frame_bytes({std::string("\xff\xfe", 2)}));  // invalid UTF-8
    CHECK(!p.ipc.next_timeout(2000).has_value());
    write_raw(p.raw, frame_bytes({"{\"t\":\"Unknown\"}"}));
    CHECK(!p.ipc.next_timeout(2000).has_value());
    write_raw(p.raw, frame_bytes({""}));  // zero-length frame
    CHECK(!p.ipc.next_timeout(2000).has_value());
    // next_timeout2 on an undecodable frame: Some(Ok(None)).
    write_raw(p.raw, frame_bytes({"[1,2"}));
    const auto n2 = p.ipc.next_timeout2(2000);
    CHECK(n2.has_value() && !n2->has_value());
    // ...and the stream is still in sync for the next good frame.
    write_raw(p.raw, frame_bytes({ipc::to_json(Data::Test{})}));
    const auto ok = p.ipc.next_timeout(2000);
    CHECK(ok && ok->is<Data::Test>());
}

void test_framing_eof() {
    {
        RawPair p = raw_pair();
        { parity_tokio_ipc::Connection gone = std::move(p.raw); }
        bool reset = false;
        try {
            (void)p.ipc.next();
        } catch (const ipc::TimeoutError&) {
        } catch (const std::runtime_error& e) {
            reset = std::string(e.what()) == "reset by the peer";
        }
        CHECK(reset);
    }
    {
        // next_timeout2: Some(Err(..)) on reset surfaces as an exception.
        RawPair p = raw_pair();
        { parity_tokio_ipc::Connection gone = std::move(p.raw); }
        bool threw = false;
        try {
            (void)p.ipc.next_timeout2(1000);
        } catch (const ipc::TimeoutError&) {
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        // Peer closes mid-frame: an error, not a silent None.
        RawPair p = raw_pair();
        const std::vector<uint8_t> wire = frame_bytes({ipc::to_json(Data::Test{})});
        write_raw(p.raw, std::vector<uint8_t>(wire.begin(), wire.begin() + 3));
        { parity_tokio_ipc::Connection gone = std::move(p.raw); }
        bool threw = false;
        try {
            (void)p.ipc.next_timeout(1000);
        } catch (const ipc::TimeoutError&) {
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
}

// --- server handle() (in-process daemon on a private postfix) ----------------------

void test_server_handle() {
    const std::string postfix = "_cppdesk_test_" + std::to_string(::getpid());
    start_server_thread(postfix);
    ipc::Connection c = connect_retry(postfix);

    // Config get / set.
    hbb_common::set_id("111222333");
    CHECK(ask_config(c, "id") == OptStr("111222333"));

    // write_pid + chmod 0777 after a successful bind. connect() already
    // succeeds at listen(), but the first reply means new_listener returned.
    const std::string sock = hbb_common::ipc_path(postfix);
    CHECK(read_file(ipc::get_pid_file(postfix)) == std::to_string(::getpid()));
    CHECK(file_mode(sock) == 0777);
    CHECK(file_mode(ipc::get_pid_file(postfix)) == 0777);
    c.send(Data::Config{"id", std::string("444555666")});
    CHECK(ask_config(c, "id") == OptStr("444555666"));
    CHECK(hbb_common::get_id() == "444555666");
    c.send(Data::Config{"password", std::string("pw-1")});
    CHECK(ask_config(c, "password") == OptStr("pw-1"));
    CHECK(hbb_common::get_password() == "pw-1");
    const OptStr salt = ask_config(c, "salt");
    CHECK(salt && !salt->empty() && *salt == hbb_common::get_salt());
    c.send(Data::Config{"salt", std::string("salty")});
    CHECK(ask_config(c, "salt") == OptStr("salty"));
    CHECK(ask_config(c, "rendezvous_server") == OptStr("127.0.0.1:1"));
    CHECK(ask_config(c, "rendezvous_server") ==
          OptStr(hbb_common::to_string(hbb_common::get_rendezvous_server())));
    CHECK(!ask_config(c, "no-such-key").has_value());
    // Setting an unknown name is ignored and produces no reply: the next
    // reply on this connection still answers the following request.
    c.send(Data::Config{"no-such-key", std::string("x")});
    CHECK(ask_config(c, "id") == OptStr("444555666"));

    // Options.
    StrMap opts = hbb_common::get_options();
    const auto got_opts = ask<Data::Options>(c, Data::Options{});
    CHECK(got_opts.value && *got_opts.value == opts);
    opts["test-key"] = "test-value";
    Data::Options set_msg;
    set_msg.value = opts;
    c.send(set_msg);
    const auto got_opts2 = ask<Data::Options>(c, Data::Options{});
    CHECK(got_opts2.value && *got_opts2.value == opts);
    CHECK(hbb_common::get_option("test-key") == "test-value");

    // NatType.
    hbb_common::set_nat_type(2);
    CHECK(ask<Data::NatType>(c, Data::NatType{}).value == std::optional<int32_t>(2));

    // OnlineStatus: (max latency or 0, key_confirmed).
    hbb_common::reset_online();
    hbb_common::set_key_confirmed(false);
    auto os = ask<Data::OnlineStatus>(c, Data::OnlineStatus{});
    CHECK(os.value && os.value->first == 0 && !os.value->second);
    hbb_common::update_latency("rs-a", 5);
    hbb_common::update_latency("rs-b", 9);
    hbb_common::set_key_confirmed(true);
    os = ask<Data::OnlineStatus>(c, Data::OnlineStatus{});
    CHECK(os.value && os.value->first == 9 && os.value->second);
    hbb_common::reset_online();
    hbb_common::update_latency("rs-c", -1);
    os = ask<Data::OnlineStatus>(c, Data::OnlineStatus{});
    CHECK(os.value && os.value->first == -1);

    // ConfirmedKey(None): the key pair only once confirmed.
    hbb_common::set_key_confirmed(false);
    CHECK(!ask<Data::ConfirmedKey>(c, Data::ConfirmedKey{}).value.has_value());
    hbb_common::set_key_confirmed(true);
    const auto key = ask<Data::ConfirmedKey>(c, Data::ConfirmedKey{});
    const auto pair = hbb_common::get_key_pair();
    CHECK(key.value && key.value->first == pair[0] && key.value->second == pair[1]);
    CHECK(key.value && key.value->first.size() == 64 && key.value->second.size() == 32);

    // SystemInfo.
    const auto info = ask<Data::SystemInfo>(c, Data::SystemInfo{});
    const std::string expected = "log_path: " + hbb_common::log_path().string() +
                                 ", config: " + hbb_common::config_file("").string() +
                                 ", username: " + rustdesk::username();
    CHECK(info.value == OptStr(expected));
    CHECK(info.value && info.value->find("RustDesk.toml") != std::string::npos);

    // Everything else is ignored without a reply (`_ => {}`), including
    // ConfirmedKey(Some) and undecodable frames.
    Data::ConfirmedKey some_key;
    some_key.value = std::make_pair(std::vector<uint8_t>{1}, std::vector<uint8_t>{2});
    c.send(some_key);
    c.send(Data::Test{});
    c.send(sample_login());
    c.send(Data::ChatMessage{"hello"});
    c.send(Data::SwitchPermission{"audio", false});
    c.send(Data::ClickTime{1});
    c.send(Data::Authorize{});
    c.send(Data::SAS{});
    c.send(Data::RawMessage{{1, 2, 3}});
    c.send(Data::FS{FS::ReadDir{"/", false}});
    c.send_raw_frame({'?', '!'});
    CHECK(ask_config(c, "id") == OptStr("444555666"));

    // Several clients at once, each on its own server thread.
    ipc::Connection c2 = connect_retry(postfix);
    CHECK(ask_config(c2, "password") == OptStr("pw-1"));
    CHECK(ask_config(c, "salt") == OptStr("salty"));

    // A second listener on a live postfix: check_pid sees our own pid with our
    // own process name, the connect double-check succeeds, so the socket is
    // kept, and bind then fails (address in use).
    bool threw = false;
    try {
        (void)ipc::new_listener(postfix);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(std::filesystem::exists(sock));
    CHECK(ask_config(c, "id") == OptStr("444555666"));  // still serving
}

// --- check_pid on stale state ----------------------------------------------------------

void make_stale_socket(const std::string& path) {
    std::filesystem::remove(path);
    parity_tokio_ipc::Endpoint e(path);
    parity_tokio_ipc::Incoming dropped = e.incoming();  // bound, then dropped: no unlink
}

void test_check_pid() {
    const std::string postfix = "_cppdesk_pid_" + std::to_string(::getpid());
    const std::string sock = hbb_common::ipc_path(postfix);
    const std::string pid_file = ipc::get_pid_file(postfix);
    CHECK(pid_file == sock + ".pid");

    // No pid file: the stale socket goes.
    std::filesystem::remove(pid_file);
    make_stale_socket(sock);
    CHECK(std::filesystem::exists(sock));
    ipc::check_pid(postfix);
    CHECK(!std::filesystem::exists(sock));

    // Unparsable pid files count as pid 0 (`parse::<i32>().unwrap_or(0)`).
    for (const std::string content : {"abc", "123\n", "", "-5", "99999999999"}) {
        make_stale_socket(sock);
        write_file(pid_file, content);
        ipc::check_pid(postfix);
        CHECK(!std::filesystem::exists(sock));
    }

    // A live process with another name (pid 1) does not protect the socket.
    make_stale_socket(sock);
    write_file(pid_file, "1");
    ipc::check_pid(postfix);
    CHECK(!std::filesystem::exists(sock));

    // Our own pid (same name) but nobody listening: the connect double-check
    // fails, so the socket goes.
    make_stale_socket(sock);
    write_file(pid_file, std::to_string(::getpid()));
    ipc::check_pid(postfix);
    CHECK(!std::filesystem::exists(sock));

    // Our own pid and a live listener: kept.
    std::filesystem::remove(sock);
    parity_tokio_ipc::Endpoint e(sock);
    parity_tokio_ipc::Incoming live = e.incoming();
    write_file(pid_file, std::to_string(::getpid()));
    ipc::check_pid(postfix);
    CHECK(std::filesystem::exists(sock));

    // new_listener over a stale socket succeeds and rewrites the pid file.
    { parity_tokio_ipc::Incoming gone = std::move(live); }
    write_file(pid_file, "1");
    {
        parity_tokio_ipc::Incoming fresh = ipc::new_listener(postfix);
        CHECK(read_file(pid_file) == std::to_string(::getpid()));
        CHECK(file_mode(sock) == 0777);
    }
    std::filesystem::remove(sock);
    std::filesystem::remove(pid_file);
}

// --- Close: the daemon exits with status 0 -------------------------------------------

void test_close_exits() {
    const std::string postfix = "_cppdesk_close_" + std::to_string(::getpid());
    Child child;
    spawn_child(child, postfix);
    ipc::Connection c = connect_retry(postfix);
    CHECK(ask_config(c, "id").has_value());  // the child really serves
    c.send(Data::Close{});
    CHECK(child.wait_exit(5000));
    CHECK(child.reaped && WIFEXITED(child.status) && WEXITSTATUS(child.status) == 0);
    std::filesystem::remove(hbb_common::ipc_path(postfix));
    std::filesystem::remove(ipc::get_pid_file(postfix));
}

// --- client helpers against a cross-process daemon at postfix "" ---------------------

bool live_default_daemon() {
    try {
        (void)ipc::connect(200, "");
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void test_client_helpers() {
    if (live_default_daemon()) {
        std::puts("SKIP test_client_helpers: a live IPC daemon answers at postfix \"\"");
        return;
    }

    // No daemon: every helper falls back to (or fails like) upstream.
    hbb_common::set_id("100000001");
    CHECK(ipc::get_id() == "100000001");
    CHECK(ipc::get_option("custom-rendezvous-server") == "127.0.0.1:1");
    CHECK(ipc::get_nat_type(200) == hbb_common::get_nat_type());
    CHECK(hbb_common::to_string(ipc::get_rendezvous_server(200)) == "127.0.0.1:1");
    CHECK(hbb_common::to_string(rustdesk::get_rendezvous_server(200)) == "127.0.0.1:1");
    bool threw = false;
    try {
        (void)ipc::get_config("id");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        ipc::set_password("pw-no-daemon");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(hbb_common::get_password() == "pw-no-daemon");  // Config first, then Err

    // Seed what the daemon will load from the shared config files.
    hbb_common::set_nat_type(2);
    hbb_common::set_salt("salt-A");

    Child child;
    spawn_child(child, "");
    ipc::Connection c = connect_retry("");
    // Force the daemon to load Config and Config2 before we diverge locally.
    CHECK(ask_config(c, "id") == OptStr("100000001"));
    CHECK(ask<Data::NatType>(c, Data::NatType{}).value == std::optional<int32_t>(2));

    // write_pid wrote the daemon's pid; socket + pid file are 0777.
    CHECK(read_file(ipc::get_pid_file("")) == std::to_string(child.pid));
    CHECK(file_mode(hbb_common::ipc_path("")) == 0777);

    // get_nat_type: the daemon's value wins and is cached locally.
    hbb_common::set_nat_type(1);
    CHECK(ipc::get_nat_type(1000) == 2);
    CHECK(hbb_common::get_nat_type() == 2);
    CHECK(rustdesk::get_nat_type(1000) == 2);

    // get_rendezvous_server: the daemon's address, not the local one.
    hbb_common::set_option("custom-rendezvous-server", "127.0.0.1:2");
    CHECK(hbb_common::to_string(hbb_common::get_rendezvous_server()) == "127.0.0.1:2");
    CHECK(hbb_common::to_string(ipc::get_rendezvous_server(1000)) == "127.0.0.1:1");
    CHECK(hbb_common::to_string(rustdesk::get_rendezvous_server(1000)) == "127.0.0.1:1");

    // get_id: a different daemon id resets key_confirmed and is adopted;
    // the daemon's salt is copied too.
    c.send(Data::Config{"id", std::string("100000002")});
    CHECK(ask_config(c, "id") == OptStr("100000002"));
    hbb_common::set_salt("salt-B");
    hbb_common::set_key_confirmed(true);
    CHECK(ipc::get_id() == "100000002");
    CHECK(hbb_common::get_id() == "100000002");
    CHECK(!hbb_common::get_key_confirmed());
    CHECK(hbb_common::get_salt() == "salt-A");

    // get_password: the daemon's password is adopted.
    hbb_common::set_password("pw-local");
    CHECK(ipc::get_password() == "pw-no-daemon");
    CHECK(hbb_common::get_password() == "pw-no-daemon");

    // set_password: local first, then the daemon (separate connection).
    ipc::set_password("pw-new");
    CHECK(hbb_common::get_password() == "pw-new");
    CHECK(eventually([&] { return ask_config(c, "password") == OptStr("pw-new"); }));

    // get_options adopts the daemon's map (our local-only key disappears).
    hbb_common::set_option("local-only", "1");
    const StrMap daemon_opts = ipc::get_options();
    CHECK(daemon_opts.count("local-only") == 0);
    CHECK(daemon_opts.count("custom-rendezvous-server") == 1 &&
          daemon_opts.at("custom-rendezvous-server") == "127.0.0.1:1");
    CHECK(hbb_common::get_option("local-only").empty());
    CHECK(hbb_common::get_option("custom-rendezvous-server") == "127.0.0.1:1");

    // set_option / get_option round-trip through the daemon.
    ipc::set_option("k1", "v1");
    CHECK(hbb_common::get_option("k1") == "v1");
    CHECK(eventually([&] {
        const auto o = ask<Data::Options>(c, Data::Options{});
        return o.value && o.value->count("k1") == 1 && o.value->at("k1") == "v1";
    }));
    CHECK(eventually([] { return ipc::get_option("k1") == "v1"; }));
    ipc::set_option("k1", "");
    CHECK(eventually([&] {
        const auto o = ask<Data::Options>(c, Data::Options{});
        return o.value && o.value->count("k1") == 0;
    }));
    CHECK(hbb_common::get_option("k1").empty());

    // check_pid against the live daemon: same executable name, connect
    // succeeds, so its socket is kept and our bind fails.
    threw = false;
    try {
        (void)ipc::new_listener("");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(std::filesystem::exists(hbb_common::ipc_path("")));
    CHECK(ask_config(c, "id") == OptStr("100000002"));

    // Daemon gone: helpers fall back to local Config again, and a new
    // listener replaces the stale socket.
    child.kill_and_reap();
    CHECK(ipc::get_id() == "100000002");
    CHECK(ipc::get_nat_type(200) == 2);
    {
        parity_tokio_ipc::Incoming fresh = ipc::new_listener("");
        CHECK(read_file(ipc::get_pid_file("")) == std::to_string(::getpid()));
    }
    std::filesystem::remove(hbb_common::ipc_path(""));
    std::filesystem::remove(ipc::get_pid_file(""));
}

}  // namespace

int main(int argc, char** argv) {
    g_self_exe = argc > 0 ? argv[0] : nullptr;
    if (argc == 3 && std::string(argv[1]) == "--ipc-child") {
        // Child daemon mode (spawn_child): serve until killed or Data::Close.
        const std::string postfix = std::string(argv[2]) == "-" ? "" : argv[2];
        try {
            ipc::start(postfix);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "child ipc daemon failed: %s\n", e.what());
        }
        return 1;
    }

    char tmpl[] = "/tmp/cppdesk-test-ipc-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir) {
        std::perror("mkdtemp");
        return 1;
    }
    ::setenv("XDG_CONFIG_HOME", dir, 1);
    ::setenv("HOME", dir, 1);
    // Keep every rendezvous lookup (including the daemon's test_nat_type) on
    // loopback; port 1 refuses immediately.
    hbb_common::set_option("custom-rendezvous-server", "127.0.0.1:1");

    try {
        test_shapes();
        test_decode_rules();
        test_framing_roundtrip();
        test_framing_raw();
        test_framing_eof();
        test_server_handle();
        test_check_pid();
        test_close_exits();
        test_client_helpers();
    } catch (const std::exception& e) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const std::string own = "_cppdesk_test_" + std::to_string(::getpid());
    std::filesystem::remove(hbb_common::ipc_path(own), ec);
    std::filesystem::remove(ipc::get_pid_file(own), ec);
    if (g_failures == 0) {
        std::puts("test_ipc: all tests passed");
    }
    // Detached in-process server threads are still blocked in accept/recv:
    // leave without running static destructors under them.
    std::fflush(nullptr);
    std::_Exit(g_failures == 0 ? 0 : 1);
}
