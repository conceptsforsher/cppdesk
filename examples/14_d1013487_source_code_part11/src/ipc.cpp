// ipc.cpp — see ipc.hpp.
//
// NOTE: most client helpers here share names with hbb_common's Config API
// (get_id, get_password, get_options, ...). Inside this namespace an
// unqualified call resolves to the ipc:: version, so every Config access
// below is spelled hbb_common:: explicitly.
#include <rustdesk/ipc.hpp>

#include <rustdesk/common.hpp>

#include <hbb_common/config.hpp>
#include <hbb_common/json.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace rustdesk::ipc {
namespace {

namespace json = hbb_common::json;
using json::Array;
using json::JsonError;
using json::Member;
using json::Object;
using json::Value;

// --- serde_json encoding (tag = "t", content = "c") ----------------------------

Value tagged(const char* tag, Value content) {
    Object o;
    o.push_back(Member{"t", Value(tag)});
    o.push_back(Member{"c", std::move(content)});
    return Value(std::move(o));
}

// Adjacently tagged unit variant: serde emits the tag alone.
Value tagged_unit(const char* tag) {
    Object o;
    o.push_back(Member{"t", Value(tag)});
    return Value(std::move(o));
}

Value opt_string(const std::optional<std::string>& v) {
    return v ? Value(*v) : Value(nullptr);
}

// Vec<u8>: a JSON array of numbers (serde_json has no byte-string form).
Value bytes_value(const std::vector<uint8_t>& v) {
    Array a;
    a.reserve(v.size());
    for (const uint8_t b : v) {
        a.emplace_back(static_cast<uint64_t>(b));
    }
    return Value(std::move(a));
}

Value fs_to_json(const FS& fs) {
    return std::visit(
        [](const auto& x) -> Value {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, FS::ReadDir>) {
                return tagged("ReadDir",
                              Object{{"dir", x.dir}, {"include_hidden", x.include_hidden}});
            } else if constexpr (std::is_same_v<T, FS::RemoveDir>) {
                return tagged("RemoveDir",
                              Object{{"path", x.path}, {"id", x.id}, {"recursive", x.recursive}});
            } else if constexpr (std::is_same_v<T, FS::RemoveFile>) {
                return tagged("RemoveFile",
                              Object{{"path", x.path}, {"id", x.id}, {"file_num", x.file_num}});
            } else if constexpr (std::is_same_v<T, FS::CreateDir>) {
                return tagged("CreateDir", Object{{"path", x.path}, {"id", x.id}});
            } else if constexpr (std::is_same_v<T, FS::NewWrite>) {
                Array files;
                files.reserve(x.files.size());
                for (const auto& [name, size] : x.files) {
                    files.emplace_back(Array{Value(name), Value(size)});  // (String, u64) tuple
                }
                return tagged("NewWrite", Object{{"path", x.path},
                                                 {"id", x.id},
                                                 {"files", Value(std::move(files))}});
            } else if constexpr (std::is_same_v<T, FS::CancelWrite>) {
                return tagged("CancelWrite", Object{{"id", x.id}});
            } else if constexpr (std::is_same_v<T, FS::WriteBlock>) {
                return tagged("WriteBlock", Object{{"id", x.id},
                                                   {"file_num", x.file_num},
                                                   {"data", bytes_value(x.data)},
                                                   {"compressed", x.compressed}});
            } else {
                static_assert(std::is_same_v<T, FS::WriteDone>);
                return tagged("WriteDone", Object{{"id", x.id}, {"file_num", x.file_num}});
            }
        },
        fs.value);
}

Value data_to_json(const Data& data) {
    return std::visit(
        [](const auto& x) -> Value {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Data::Login>) {
                return tagged("Login", Object{{"id", x.id},
                                              {"is_file_transfer", x.is_file_transfer},
                                              {"peer_id", x.peer_id},
                                              {"name", x.name},
                                              {"authorized", x.authorized},
                                              {"port_forward", x.port_forward},
                                              {"keyboard", x.keyboard},
                                              {"clipboard", x.clipboard},
                                              {"audio", x.audio}});
            } else if constexpr (std::is_same_v<T, Data::ChatMessage>) {
                return tagged("ChatMessage", Object{{"text", x.text}});
            } else if constexpr (std::is_same_v<T, Data::SwitchPermission>) {
                return tagged("SwitchPermission", Object{{"name", x.name}, {"enabled", x.enabled}});
            } else if constexpr (std::is_same_v<T, Data::SystemInfo>) {
                return tagged("SystemInfo", opt_string(x.value));
            } else if constexpr (std::is_same_v<T, Data::ClickTime>) {
                return tagged("ClickTime", Value(x.value));
            } else if constexpr (std::is_same_v<T, Data::Authorize>) {
                return tagged_unit("Authorize");
            } else if constexpr (std::is_same_v<T, Data::Close>) {
                return tagged_unit("Close");
            } else if constexpr (std::is_same_v<T, Data::SAS>) {
                return tagged_unit("SAS");
            } else if constexpr (std::is_same_v<T, Data::OnlineStatus>) {
                if (!x.value) {
                    return tagged("OnlineStatus", Value(nullptr));
                }
                return tagged("OnlineStatus", Array{Value(x.value->first), Value(x.value->second)});
            } else if constexpr (std::is_same_v<T, Data::Config>) {
                return tagged("Config", Array{Value(x.name), opt_string(x.value)});
            } else if constexpr (std::is_same_v<T, Data::Options>) {
                if (!x.value) {
                    return tagged("Options", Value(nullptr));
                }
                Object o;
                o.reserve(x.value->size());
                for (const auto& [k, v] : *x.value) {
                    o.push_back(Member{k, Value(v)});  // HashMap order is unspecified upstream too
                }
                return tagged("Options", Value(std::move(o)));
            } else if constexpr (std::is_same_v<T, Data::NatType>) {
                return tagged("NatType", x.value ? Value(*x.value) : Value(nullptr));
            } else if constexpr (std::is_same_v<T, Data::ConfirmedKey>) {
                if (!x.value) {
                    return tagged("ConfirmedKey", Value(nullptr));
                }
                return tagged("ConfirmedKey",
                              Array{bytes_value(x.value->first), bytes_value(x.value->second)});
            } else if constexpr (std::is_same_v<T, Data::RawMessage>) {
                return tagged("RawMessage", bytes_value(x.value));
            } else if constexpr (std::is_same_v<T, Data::FS>) {
                return tagged("FS", fs_to_json(x.value));
            } else {
                static_assert(std::is_same_v<T, Data::Test>);
                return tagged_unit("Test");
            }
        },
        data.value);
}

// --- serde decoding rules -------------------------------------------------------

[[noreturn]] void bad(const std::string& what) {
    throw JsonError(what);
}

const Object& need_object(const Value& v, const char* what) {
    const Object* o = v.as_object();
    if (!o) {
        bad(std::string("invalid type: expected ") + what);
    }
    return *o;
}

// serde_derive struct visitor: unknown keys skipped, duplicates and missing
// fields rejected.
const Value& field(const Object& o, const char* key) {
    const Value* found = nullptr;
    for (const Member& m : o) {
        if (m.key == key) {
            if (found) {
                bad(std::string("duplicate field `") + key + "`");
            }
            found = &m.value;
        }
    }
    if (!found) {
        bad(std::string("missing field `") + key + "`");
    }
    return *found;
}

bool get_bool(const Value& v) {
    const bool* b = v.as_bool();
    if (!b) {
        bad("invalid type: expected a boolean");
    }
    return *b;
}

int64_t get_i64(const Value& v) {
    const auto i = v.as_i64();
    if (!i) {
        bad("invalid value: expected i64");
    }
    return *i;
}

int32_t get_i32(const Value& v) {
    const int64_t i = get_i64(v);
    if (i < INT32_MIN || i > INT32_MAX) {
        bad("invalid value: expected i32");
    }
    return static_cast<int32_t>(i);
}

uint64_t get_u64(const Value& v) {
    const auto u = v.as_u64();
    if (!u) {
        bad("invalid value: expected u64");
    }
    return *u;
}

std::string get_string(const Value& v) {
    const std::string* s = v.as_string();
    if (!s) {
        bad("invalid type: expected a string");
    }
    return *s;
}

std::optional<std::string> get_opt_string(const Value& v) {
    if (v.is_null()) {
        return std::nullopt;
    }
    return get_string(v);
}

const Array& get_tuple(const Value& v, size_t n) {
    const Array* a = v.as_array();
    if (!a) {
        bad("invalid type: expected a tuple");
    }
    if (a->size() != n) {
        bad("invalid length " + std::to_string(a->size()) + ", expected a tuple of size " +
            std::to_string(n));
    }
    return *a;
}

std::vector<uint8_t> get_bytes(const Value& v) {
    const Array* a = v.as_array();
    if (!a) {
        bad("invalid type: expected a sequence");
    }
    std::vector<uint8_t> out;
    out.reserve(a->size());
    for (const Value& e : *a) {
        const auto u = e.as_u64();
        if (!u || *u > 255) {
            bad("invalid value: expected u8");
        }
        out.push_back(static_cast<uint8_t>(*u));
    }
    return out;
}

// Adjacently tagged enum: keys other than "t"/"c" are skipped, a duplicate
// "t"/"c" is an error, and "c" may come before "t".
struct Tagged {
    std::string tag;
    const Value* content = nullptr;  // nullptr: no "c" key
};

Tagged split_tagged(const Value& v) {
    const Object& o = need_object(v, "an adjacently tagged enum");
    const Value* t = nullptr;
    const Value* c = nullptr;
    for (const Member& m : o) {
        if (m.key == "t") {
            if (t) {
                bad("duplicate field `t`");
            }
            t = &m.value;
        } else if (m.key == "c") {
            if (c) {
                bad("duplicate field `c`");
            }
            c = &m.value;
        }
    }
    if (!t) {
        bad("missing field `t`");
    }
    return Tagged{get_string(*t), c};
}

const Value& need_content(const Tagged& t) {
    if (!t.content) {
        bad("missing field `c`");
    }
    return *t.content;
}

// Unit variant: "c" may be absent or null.
void unit_content(const Tagged& t) {
    if (t.content && !t.content->is_null()) {
        bad("invalid type: expected unit");
    }
}

// Option<T> newtype payload: absent "c" (serde's missing_field) or null is None.
bool none_content(const Tagged& t) {
    return !t.content || t.content->is_null();
}

FS fs_from_json(const Value& v) {
    const Tagged t = split_tagged(v);
    if (t.tag == "ReadDir") {
        const Object& o = need_object(need_content(t), "struct variant FS::ReadDir");
        FS::ReadDir x;
        x.dir = get_string(field(o, "dir"));
        x.include_hidden = get_bool(field(o, "include_hidden"));
        return x;
    }
    if (t.tag == "RemoveDir") {
        const Object& o = need_object(need_content(t), "struct variant FS::RemoveDir");
        FS::RemoveDir x;
        x.path = get_string(field(o, "path"));
        x.id = get_i32(field(o, "id"));
        x.recursive = get_bool(field(o, "recursive"));
        return x;
    }
    if (t.tag == "RemoveFile") {
        const Object& o = need_object(need_content(t), "struct variant FS::RemoveFile");
        FS::RemoveFile x;
        x.path = get_string(field(o, "path"));
        x.id = get_i32(field(o, "id"));
        x.file_num = get_i32(field(o, "file_num"));
        return x;
    }
    if (t.tag == "CreateDir") {
        const Object& o = need_object(need_content(t), "struct variant FS::CreateDir");
        FS::CreateDir x;
        x.path = get_string(field(o, "path"));
        x.id = get_i32(field(o, "id"));
        return x;
    }
    if (t.tag == "NewWrite") {
        const Object& o = need_object(need_content(t), "struct variant FS::NewWrite");
        FS::NewWrite x;
        x.path = get_string(field(o, "path"));
        x.id = get_i32(field(o, "id"));
        const Array* files = field(o, "files").as_array();
        if (!files) {
            bad("invalid type: expected a sequence");
        }
        for (const Value& f : *files) {
            const Array& pair = get_tuple(f, 2);
            x.files.emplace_back(get_string(pair[0]), get_u64(pair[1]));
        }
        return x;
    }
    if (t.tag == "CancelWrite") {
        const Object& o = need_object(need_content(t), "struct variant FS::CancelWrite");
        FS::CancelWrite x;
        x.id = get_i32(field(o, "id"));
        return x;
    }
    if (t.tag == "WriteBlock") {
        const Object& o = need_object(need_content(t), "struct variant FS::WriteBlock");
        FS::WriteBlock x;
        x.id = get_i32(field(o, "id"));
        x.file_num = get_i32(field(o, "file_num"));
        x.data = get_bytes(field(o, "data"));
        x.compressed = get_bool(field(o, "compressed"));
        return x;
    }
    if (t.tag == "WriteDone") {
        const Object& o = need_object(need_content(t), "struct variant FS::WriteDone");
        FS::WriteDone x;
        x.id = get_i32(field(o, "id"));
        x.file_num = get_i32(field(o, "file_num"));
        return x;
    }
    bad("unknown variant `" + t.tag + "`");
}

Data data_from_json(const Value& v) {
    const Tagged t = split_tagged(v);
    if (t.tag == "Login") {
        const Object& o = need_object(need_content(t), "struct variant Data::Login");
        Data::Login x;
        x.id = get_i32(field(o, "id"));
        x.is_file_transfer = get_bool(field(o, "is_file_transfer"));
        x.peer_id = get_string(field(o, "peer_id"));
        x.name = get_string(field(o, "name"));
        x.authorized = get_bool(field(o, "authorized"));
        x.port_forward = get_string(field(o, "port_forward"));
        x.keyboard = get_bool(field(o, "keyboard"));
        x.clipboard = get_bool(field(o, "clipboard"));
        x.audio = get_bool(field(o, "audio"));
        return x;
    }
    if (t.tag == "ChatMessage") {
        const Object& o = need_object(need_content(t), "struct variant Data::ChatMessage");
        Data::ChatMessage x;
        x.text = get_string(field(o, "text"));
        return x;
    }
    if (t.tag == "SwitchPermission") {
        const Object& o = need_object(need_content(t), "struct variant Data::SwitchPermission");
        Data::SwitchPermission x;
        x.name = get_string(field(o, "name"));
        x.enabled = get_bool(field(o, "enabled"));
        return x;
    }
    if (t.tag == "SystemInfo") {
        Data::SystemInfo x;
        if (!none_content(t)) {
            x.value = get_string(*t.content);
        }
        return x;
    }
    if (t.tag == "ClickTime") {
        Data::ClickTime x;
        x.value = get_i64(need_content(t));
        return x;
    }
    if (t.tag == "Authorize") {
        unit_content(t);
        return Data::Authorize{};
    }
    if (t.tag == "Close") {
        unit_content(t);
        return Data::Close{};
    }
    if (t.tag == "SAS") {
        unit_content(t);
        return Data::SAS{};
    }
    if (t.tag == "OnlineStatus") {
        Data::OnlineStatus x;
        if (!none_content(t)) {
            const Array& pair = get_tuple(*t.content, 2);
            x.value = std::make_pair(get_i64(pair[0]), get_bool(pair[1]));
        }
        return x;
    }
    if (t.tag == "Config") {
        const Array& pair = get_tuple(need_content(t), 2);
        Data::Config x;
        x.name = get_string(pair[0]);
        x.value = get_opt_string(pair[1]);
        return x;
    }
    if (t.tag == "Options") {
        Data::Options x;
        if (!none_content(t)) {
            const Object& o = need_object(*t.content, "a map");
            std::unordered_map<std::string, std::string> m;
            for (const Member& kv : o) {
                m[kv.key] = get_string(kv.value);  // HashMap: a later duplicate wins
            }
            x.value = std::move(m);
        }
        return x;
    }
    if (t.tag == "NatType") {
        Data::NatType x;
        if (!none_content(t)) {
            x.value = get_i32(*t.content);
        }
        return x;
    }
    if (t.tag == "ConfirmedKey") {
        Data::ConfirmedKey x;
        if (!none_content(t)) {
            const Array& pair = get_tuple(*t.content, 2);
            x.value = std::make_pair(get_bytes(pair[0]), get_bytes(pair[1]));
        }
        return x;
    }
    if (t.tag == "RawMessage") {
        Data::RawMessage x;
        x.value = get_bytes(need_content(t));
        return x;
    }
    if (t.tag == "FS") {
        return Data::FS{fs_from_json(need_content(t))};
    }
    if (t.tag == "Test") {
        unit_content(t);
        return Data::Test{};
    }
    bad("unknown variant `" + t.tag + "`");
}

// `std::str::from_utf8(&bytes)` then `serde_json::from_str::<Data>(s)`;
// either failing yields Ok(None).
std::optional<Data> decode_frame(const std::vector<uint8_t>& bytes) {
    const std::string_view s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!json::is_valid_utf8(s)) {
        return std::nullopt;
    }
    return from_json(s);
}

std::chrono::steady_clock::time_point deadline_after(uint64_t ms_timeout) {
    // Clamp absurd timeouts so the time_point arithmetic cannot overflow.
    constexpr uint64_t kMaxMs = 1000ull * 60 * 60 * 24 * 365;
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(std::min(ms_timeout, kMaxMs));
}

// `async fn handle(data, stream)`: server-side dispatch.
void handle(const Data& data, Connection& stream) {
    if (data.is<Data::SystemInfo>()) {
        const std::string info = "log_path: " + hbb_common::log_path().string() +
                                 ", config: " + hbb_common::config_file("").string() +
                                 ", username: " + rustdesk::username();
        HBB_ALLOW_ERR(stream.send(Data::SystemInfo{info}));
    } else if (data.is<Data::Close>()) {
        // log::info!("Receive close message");
        // std::process::exit(0): terminate without running destructors (Rust
        // has none for statics), after flushing C stdio like libc exit() does.
        std::fflush(nullptr);
        std::_Exit(0);
    } else if (data.is<Data::OnlineStatus>()) {
        int64_t x = 0;  // ONLINE.values().max().unwrap_or(&0)
        {
            std::unique_lock l(hbb_common::online_lock());
            bool any = false;
            for (const auto& [host, latency] : hbb_common::online_map()) {
                (void)host;
                if (!any || latency > x) {
                    x = latency;
                }
                any = true;
            }
        }
        const bool confirmed = hbb_common::get_key_confirmed();
        HBB_ALLOW_ERR(stream.send(Data::OnlineStatus{std::make_pair(x, confirmed)}));
    } else if (const auto* ck = data.get_if<Data::ConfirmedKey>(); ck && !ck->value) {
        Data::ConfirmedKey out;
        if (hbb_common::get_key_confirmed()) {
            const auto pair = hbb_common::get_key_pair();  // (sk, pk)
            out.value = std::make_pair(pair[0], pair[1]);
        }
        HBB_ALLOW_ERR(stream.send(out));
    } else if (const auto* c = data.get_if<Data::Config>()) {
        if (!c->value) {
            Data::Config out;
            out.name = c->name;
            if (c->name == "id") {
                out.value = hbb_common::get_id();
            } else if (c->name == "password") {
                out.value = hbb_common::get_password();
            } else if (c->name == "salt") {
                out.value = hbb_common::get_salt();
            } else if (c->name == "rendezvous_server") {
                out.value = hbb_common::to_string(hbb_common::get_rendezvous_server());
            }
            HBB_ALLOW_ERR(stream.send(out));
        } else {
            const std::string& value = *c->value;
            if (c->name == "id") {
                hbb_common::set_id(value);
            } else if (c->name == "password") {
                hbb_common::set_password(value);
            } else if (c->name == "salt") {
                hbb_common::set_salt(value);
            } else {
                return;
            }
            // log::info!("{} updated", name);
        }
    } else if (const auto* o = data.get_if<Data::Options>()) {
        if (!o->value) {
            Data::Options out;
            out.value = hbb_common::get_options();
            HBB_ALLOW_ERR(stream.send(out));
        } else {
            hbb_common::set_options(*o->value);
        }
    } else if (data.is<Data::NatType>()) {
        Data::NatType out;
        out.value = hbb_common::get_nat_type();
        HBB_ALLOW_ERR(stream.send(out));
    }
    // _ => {}
}

#if !defined(_WIN32)
// `str::parse::<i32>()` parity: optional sign + ASCII digits, nothing else
// (a trailing newline already fails), range-checked.
std::optional<int32_t> parse_i32(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = s[0] == '-';
        if (++i == s.size()) {
            return std::nullopt;
        }
    }
    int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return std::nullopt;
        }
        v = v * 10 + (s[i] - '0');
        if (v > static_cast<int64_t>(INT32_MAX) + 1) {
            return std::nullopt;
        }
    }
    v = neg ? -v : v;
    if (v < INT32_MIN || v > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(v);
}

// psutil Process::new(pid) succeeds: the process exists.
bool process_exists(pid_t pid) {
#if defined(__linux__)
    std::error_code ec;
    return std::filesystem::exists("/proc/" + std::to_string(pid), ec);
#else
    return ::kill(pid, 0) == 0 || errno == EPERM;
#endif
}

// psutil Process::name() parity (Linux: the kernel comm name, as psutil reads
// it from /proc; macOS: proc_name). Failures are "" (`unwrap_or("")`).
std::string process_name(pid_t pid) {
#if defined(__linux__)
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (f) {
        std::getline(f, name);
    }
    return name;
#elif defined(__APPLE__)
    char buf[256] = {};
    if (::proc_name(pid, buf, sizeof buf) <= 0) {
        return "";
    }
    return std::string(buf);
#else
    (void)pid;
    return "";
#endif
}
#endif

}  // namespace

// --- Data <-> JSON --------------------------------------------------------------

std::string to_json(const Data& data) {
    return json::dump(data_to_json(data));
}

std::optional<Data> from_json(std::string_view s) {
    try {
        return data_from_json(json::parse(s));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// --- Connection -----------------------------------------------------------------

Connection::Connection(parity_tokio_ipc::Connection conn) : conn_(std::move(conn)) {}

void Connection::send(const Data& data) {
    const std::string v = to_json(data);
    send_raw_frame(std::vector<uint8_t>(v.begin(), v.end()));
}

void Connection::send_config(const std::string& name, const std::string& value) {
    Data::Config c;
    c.name = name;
    c.value = value;
    send(c);
}

void Connection::send_raw_frame(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> framed;
    codec_.encode(bytes, framed);
    conn_.write_all(framed.data(), framed.size());
}

std::optional<std::vector<uint8_t>> Connection::recv_frame(
    const std::optional<std::chrono::steady_clock::time_point>& deadline) {
    while (true) {
        if (auto frame = codec_.decode(rbuf_)) {
            return frame;
        }
        if (deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= *deadline) {
                return std::nullopt;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count() + 1;
            const int wait_ms = static_cast<int>(std::min<long long>(left, INT_MAX));
            if (!conn_.wait_readable(wait_ms)) {
                continue;  // timed out or interrupted: re-check the deadline
            }
        }
        uint8_t chunk[16384];
        const size_t n = conn_.read(chunk, sizeof chunk);  // throws on I/O error
        if (n == 0) {
            throw std::runtime_error("reset by the peer");
        }
        rbuf_.insert(rbuf_.end(), chunk, chunk + n);
    }
}

std::optional<Data> Connection::next() {
    const auto bytes = recv_frame(std::nullopt);
    return decode_frame(*bytes);  // no deadline: recv_frame returns or throws
}

std::optional<Data> Connection::next_timeout(uint64_t ms_timeout) {
    const auto bytes = recv_frame(deadline_after(ms_timeout));
    if (!bytes) {
        throw TimeoutError();
    }
    return decode_frame(*bytes);
}

std::optional<std::optional<Data>> Connection::next_timeout2(uint64_t ms_timeout) {
    const auto bytes = recv_frame(deadline_after(ms_timeout));
    if (!bytes) {
        return std::nullopt;  // None: elapsed
    }
    return std::optional<std::optional<Data>>(std::in_place, decode_frame(*bytes));
}

// --- server ---------------------------------------------------------------------

void start(const std::string& postfix) {
    if (postfix.empty()) {
        rustdesk::test_nat_type();
    }
    parity_tokio_ipc::Incoming incoming = new_listener(postfix);
    while (true) {
        try {
            parity_tokio_ipc::Connection conn = incoming.next();
            std::thread([stream = Connection(std::move(conn)), postfix]() mutable {
                while (true) {
                    try {
                        if (auto data = stream.next()) {
                            handle(*data, stream);
                        }
                    } catch (const std::exception&) {
                        // log::trace!("ipc{} connection closed: {}", postfix, err);
                        (void)postfix;
                        break;
                    }
                }
            }).detach();
        } catch (const std::exception&) {
            // log::error!("Couldn't get client: {:?}", err);
        }
    }
}

parity_tokio_ipc::Incoming new_listener(const std::string& postfix) {
    const std::string path = hbb_common::ipc_path(postfix);
#if !defined(_WIN32)
    check_pid(postfix);
#endif
    parity_tokio_ipc::Endpoint endpoint(path);
    // SecurityAttributes::allow_everyone_create() cannot fail on unix
    // (upstream logs "Failed to set ipc{} security: {}" otherwise).
    endpoint.set_security_attributes(parity_tokio_ipc::SecurityAttributes::allow_everyone_create());
    // On failure upstream logs "Faild to start ipc{} server at path {}: {}"
    // and returns the error: incoming() throws straight through here.
    parity_tokio_ipc::Incoming incoming = endpoint.incoming();
    // log::info!("Started ipc{} server at path: {}", postfix, &path);
#if !defined(_WIN32)
    ::chmod(path.c_str(), 0777);  // set_permissions(0o0777).ok()
    write_pid(postfix);
#endif
    return incoming;
}

Connection connect(uint64_t ms_timeout, const std::string& postfix) {
    const std::string path = hbb_common::ipc_path(postfix);
    return Connection(parity_tokio_ipc::Endpoint::connect(path, ms_timeout));
}

#if !defined(_WIN32)
std::string get_pid_file(const std::string& postfix) {
    const std::string path = hbb_common::ipc_path(postfix);
    return path + ".pid";
}

void check_pid(const std::string& postfix) {
    const std::string pid_file = get_pid_file(postfix);
    std::ifstream file(pid_file, std::ios::binary);
    if (file) {
        const std::string content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        const int32_t pid = parse_i32(content).value_or(0);
        if (pid > 0 && process_exists(static_cast<pid_t>(pid))) {
            if (process_name(::getpid()) == process_name(static_cast<pid_t>(pid))) {
                // double check with connect
                try {
                    (void)connect(1000, postfix);
                    return;
                } catch (const std::exception&) {
                }
            }
        }
    }
    // allow_err!(std::fs::remove_file(&Config::ipc_path(postfix)))
    std::error_code ec;
    std::filesystem::remove(hbb_common::ipc_path(postfix), ec);
}

void write_pid(const std::string& postfix) {
    const std::string path = get_pid_file(postfix);
    // File::create parity: O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, mode 0o666.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        return;
    }
    ::chmod(path.c_str(), 0777);  // set_permissions(0o0777).ok()
    const std::string pid = std::to_string(::getpid());
    size_t off = 0;
    while (off < pid.size()) {  // write_all(...).ok()
        const ssize_t n = ::write(fd, pid.data() + off, pid.size() - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        off += static_cast<size_t>(n);
    }
    ::close(fd);
}
#endif

// --- client helpers ---------------------------------------------------------------

std::optional<std::string> get_config(const std::string& name) {
    return get_config_async(name, 1'000);
}

std::optional<std::string> get_config_async(const std::string& name, uint64_t ms_timeout) {
    Connection c = connect(ms_timeout, "");
    Data::Config req;
    req.name = name;
    c.send(req);
    if (const auto reply = c.next_timeout(ms_timeout)) {
        if (const auto* cfg = reply->get_if<Data::Config>()) {
            if (cfg->name == name) {
                return cfg->value;
            }
        }
    }
    return std::nullopt;
}

void set_config(const std::string& name, const std::string& value) {
    Connection c = connect(1000, "");
    c.send_config(name, value);
}

void set_password(const std::string& v) {
    hbb_common::set_password(v);
    set_config("password", v);
}

std::string get_id() {
    std::optional<std::string> v;
    try {
        v = get_config("id");
    } catch (const std::exception&) {
    }
    if (v) {
        // update salt also, so that nexttime reinstallation not causing first-time auto-login failure
        std::optional<std::string> v2;
        try {
            v2 = get_config("salt");
        } catch (const std::exception&) {
        }
        if (v2) {
            hbb_common::set_salt(*v2);
        }
        if (*v != hbb_common::get_id()) {
            hbb_common::set_key_confirmed(false);
            hbb_common::set_id(*v);
        }
        return *v;
    }
    return hbb_common::get_id();
}

std::string get_password() {
    std::optional<std::string> v;
    try {
        v = get_config("password");
    } catch (const std::exception&) {
    }
    if (v) {
        hbb_common::set_password(*v);
        return *v;
    }
    return hbb_common::get_password();
}

hbb_common::ResolvedAddr get_rendezvous_server(uint64_t ms_timeout) {
    try {
        if (const auto v = get_config_async("rendezvous_server", ms_timeout)) {
            if (const auto addr = hbb_common::parse_socket_addr(*v)) {
                return *addr;
            }
        }
    } catch (const std::exception&) {
    }
    return hbb_common::get_rendezvous_server();
}

std::unordered_map<std::string, std::string> get_options_(uint64_t ms_timeout) {
    Connection c = connect(ms_timeout, "");
    c.send(Data::Options{});
    if (const auto reply = c.next_timeout(ms_timeout)) {
        if (const auto* o = reply->get_if<Data::Options>(); o && o->value) {
            hbb_common::set_options(*o->value);
            return *o->value;
        }
    }
    return hbb_common::get_options();
}

std::unordered_map<std::string, std::string> get_options() {
    try {
        return get_options_(1000);
    } catch (const std::exception&) {
        return hbb_common::get_options();
    }
}

std::string get_option(const std::string& key) {
    const auto options = get_options();
    const auto it = options.find(key);
    return it != options.end() ? it->second : "";
}

void set_option(const std::string& key, const std::string& value) {
    auto options = get_options();
    if (value.empty()) {
        options.erase(key);
    } else {
        options[key] = value;
    }
    HBB_ALLOW_ERR(set_options(options));  // set_options(options).ok()
}

void set_options(const std::unordered_map<std::string, std::string>& value) {
    hbb_common::set_options(value);
    Data::Options msg;
    msg.value = value;
    connect(1000, "").send(msg);
}

int32_t get_nat_type_(uint64_t ms_timeout) {
    Connection c = connect(ms_timeout, "");
    c.send(Data::NatType{});
    if (const auto reply = c.next_timeout(ms_timeout)) {
        if (const auto* n = reply->get_if<Data::NatType>(); n && n->value) {
            hbb_common::set_nat_type(*n->value);
            return *n->value;
        }
    }
    return hbb_common::get_nat_type();
}

int32_t get_nat_type(uint64_t ms_timeout) {
    try {
        return get_nat_type_(ms_timeout);
    } catch (const std::exception&) {
        return hbb_common::get_nat_type();
    }
}

}  // namespace rustdesk::ipc
