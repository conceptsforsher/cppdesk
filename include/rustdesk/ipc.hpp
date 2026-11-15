// ipc.hpp — translation of src/ipc.rs (step 14, part 11 of d1013487).
//
// The local IPC channel between RustDesk processes: the server daemon (ipc
// server at postfix ""), the UI, the connection manager ("_cm"), the
// PulseAudio helper ("_pa") and the Windows service ("_service"). Wire
// format: BytesCodec length-delimited frames carrying serde_json-encoded
// `Data`, over parity-tokio-ipc endpoints (Unix socket at
// Config::ipc_path(postfix); Windows named pipes come with the platform step).
//
// Mapping:
// - `enum FS` / `enum Data` (#[serde(tag = "t", content = "c")]) become
//   structs whose nested types are the variants (ipc::Data::Login,
//   ipc::FS::ReadDir, ...), held in a std::variant. Every shape encodes
//   byte-identically to serde_json (see to_json), and decoding applies
//   serde's rules (unknown keys ignored, duplicates/missing/mistyped rejected,
//   a missing "c" means None for Option payloads).
// - ResultType<T> means return T or throw. `Ok(None)` from next() (an
//   undecodable frame) becomes std::nullopt.
// - `#[tokio::main] async fn` becomes a blocking call. `tokio::spawn` per IPC
//   client becomes a detached std::thread. `timeout(ms, fut)` becomes a poll()
//   deadline that throws TimeoutError.
// - log::* stays silent (no logger yet; HBB_ALLOW_ERR parity). Upstream's log
//   lines survive as comments at their call sites.
// - Module-private helpers (get_config, set_config, check_pid, write_pid,
//   get_pid_file, get_options_, get_nat_type_) are declared here too: C++ has
//   no module privacy, and the tests exercise them directly.

#pragma once

#include <parity_tokio_ipc/parity_tokio_ipc.hpp>

#include <hbb_common/bytes_codec.hpp>
#include <hbb_common/util.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace rustdesk::ipc {

// `pub enum FS` (file-transfer requests the server forwards to the cm).
struct FS {
    struct ReadDir {
        std::string dir;
        bool include_hidden = false;
        bool operator==(const ReadDir&) const = default;
    };
    struct RemoveDir {
        std::string path;
        int32_t id = 0;
        bool recursive = false;
        bool operator==(const RemoveDir&) const = default;
    };
    struct RemoveFile {
        std::string path;
        int32_t id = 0;
        int32_t file_num = 0;
        bool operator==(const RemoveFile&) const = default;
    };
    struct CreateDir {
        std::string path;
        int32_t id = 0;
        bool operator==(const CreateDir&) const = default;
    };
    struct NewWrite {
        std::string path;
        int32_t id = 0;
        std::vector<std::pair<std::string, uint64_t>> files;  // Vec<(String, u64)>
        bool operator==(const NewWrite&) const = default;
    };
    struct CancelWrite {
        int32_t id = 0;
        bool operator==(const CancelWrite&) const = default;
    };
    struct WriteBlock {
        int32_t id = 0;
        int32_t file_num = 0;
        std::vector<uint8_t> data;
        bool compressed = false;
        bool operator==(const WriteBlock&) const = default;
    };
    struct WriteDone {
        int32_t id = 0;
        int32_t file_num = 0;
        bool operator==(const WriteDone&) const = default;
    };

    using Variant = std::variant<ReadDir, RemoveDir, RemoveFile, CreateDir, NewWrite, CancelWrite,
                                 WriteBlock, WriteDone>;
    Variant value;

    // Implicit from any variant (`FS::ReadDir { .. }` -> FS).
    template <class T,
              std::enable_if_t<!std::is_same_v<std::decay_t<T>, FS> &&
                                   std::is_constructible_v<Variant, T&&>,
                               int> = 0>
    FS(T&& v) : value(std::forward<T>(v)) {}

    template <class T>
    const T* get_if() const {
        return std::get_if<T>(&value);
    }
    template <class T>
    bool is() const {
        return std::holds_alternative<T>(value);
    }
    bool operator==(const FS& other) const { return value == other.value; }
};

// `pub enum Data`.
struct Data {
    struct Login {
        int32_t id = 0;
        bool is_file_transfer = false;
        std::string peer_id;
        std::string name;
        bool authorized = false;
        std::string port_forward;
        bool keyboard = false;
        bool clipboard = false;
        bool audio = false;
        bool operator==(const Login&) const = default;
    };
    struct ChatMessage {
        std::string text;
        bool operator==(const ChatMessage&) const = default;
    };
    struct SwitchPermission {
        std::string name;
        bool enabled = false;
        bool operator==(const SwitchPermission&) const = default;
    };
    struct SystemInfo {  // SystemInfo(Option<String>)
        std::optional<std::string> value;
        bool operator==(const SystemInfo&) const = default;
    };
    struct ClickTime {  // ClickTime(i64)
        int64_t value = 0;
        bool operator==(const ClickTime&) const = default;
    };
    struct Authorize {
        bool operator==(const Authorize&) const = default;
    };
    struct Close {
        bool operator==(const Close&) const = default;
    };
    struct SAS {
        bool operator==(const SAS&) const = default;
    };
    struct OnlineStatus {  // OnlineStatus(Option<(i64, bool)>)
        std::optional<std::pair<int64_t, bool>> value;
        bool operator==(const OnlineStatus&) const = default;
    };
    struct Config {  // Config((String, Option<String>))
        std::string name;
        std::optional<std::string> value;
        bool operator==(const Config&) const = default;
    };
    struct Options {  // Options(Option<HashMap<String, String>>)
        std::optional<std::unordered_map<std::string, std::string>> value;
        bool operator==(const Options&) const = default;
    };
    struct NatType {  // NatType(Option<i32>)
        std::optional<int32_t> value;
        bool operator==(const NatType&) const = default;
    };
    struct ConfirmedKey {  // ConfirmedKey(Option<(Vec<u8>, Vec<u8>)>)
        std::optional<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> value;
        bool operator==(const ConfirmedKey&) const = default;
    };
    struct RawMessage {  // RawMessage(Vec<u8>)
        std::vector<uint8_t> value;
        bool operator==(const RawMessage&) const = default;
    };
    struct FS {  // FS(FS)
        ::rustdesk::ipc::FS value;
        bool operator==(const FS&) const = default;
    };
    struct Test {
        bool operator==(const Test&) const = default;
    };

    using Variant =
        std::variant<Login, ChatMessage, SwitchPermission, SystemInfo, ClickTime, Authorize, Close,
                     SAS, OnlineStatus, Config, Options, NatType, ConfirmedKey, RawMessage, FS, Test>;
    Variant value;

    // Implicit from any variant (`Data::Close {}` -> Data).
    template <class T,
              std::enable_if_t<!std::is_same_v<std::decay_t<T>, Data> &&
                                   std::is_constructible_v<Variant, T&&>,
                               int> = 0>
    Data(T&& v) : value(std::forward<T>(v)) {}

    template <class T>
    const T* get_if() const {
        return std::get_if<T>(&value);
    }
    template <class T>
    bool is() const {
        return std::holds_alternative<T>(value);
    }
    bool operator==(const Data& other) const { return value == other.value; }
};

// serde_json::to_vec(&data) parity (compact JSON text).
std::string to_json(const Data& data);
// serde_json::from_str::<Data>(s) parity; nullopt on any error.
std::optional<Data> from_json(std::string_view s);

// tokio::time::Elapsed parity: what `timeout(ms, ...)?` raises.
struct TimeoutError : std::runtime_error {
    TimeoutError() : std::runtime_error("deadline has elapsed") {}
};

// `pub struct Connection { inner: Framed<Conn, BytesCodec> }`.
class Connection {
public:
    explicit Connection(parity_tokio_ipc::Connection conn);

    // serde_json::to_vec + framed send. Throws on I/O failure.
    void send(const Data& data);
    // Module-private upstream (`async fn send_config`).
    void send_config(const std::string& name, const std::string& value);
    // timeout(ms, next())?? : TimeoutError on expiry, else as next().
    std::optional<Data> next_timeout(uint64_t ms_timeout);
    // Option<ResultType<Option<Data>>>: nullopt on expiry. Some(Err) (peer
    // reset or I/O failure) is thrown, like next(); otherwise the inner value
    // is next()'s Ok.
    std::optional<std::optional<Data>> next_timeout2(uint64_t ms_timeout);
    // Ok(Some(data)) is a value. Ok(None) means the frame was not UTF-8 / not
    // a Data, which is nullopt. bail!("reset by the peer") at EOF throws
    // std::runtime_error.
    std::optional<Data> next();

    // Send one raw frame of arbitrary bytes. No upstream counterpart; tests use
    // it to inject undecodable frames.
    void send_raw_frame(const std::vector<uint8_t>& bytes);

    const parity_tokio_ipc::Connection& raw() const { return conn_; }

private:
    // One decoded frame. nullopt only when `deadline` passes first; EOF or
    // I/O failure throws. A partial frame stays buffered across calls (Framed
    // keeps its BytesMut when a timeout drops the future).
    std::optional<std::vector<uint8_t>> recv_frame(
        const std::optional<std::chrono::steady_clock::time_point>& deadline);

    parity_tokio_ipc::Connection conn_;
    hbb_common::BytesCodec codec_;
    std::vector<uint8_t> rbuf_;
};

// `#[tokio::main] pub async fn start(postfix)`: serves forever, with one
// detached thread per client. Throws when the listener cannot start.
void start(const std::string& postfix);
// Binds Config::ipc_path(postfix) after check_pid; chmod 0777 + write_pid on
// success. Throws on failure.
parity_tokio_ipc::Incoming new_listener(const std::string& postfix);
// Connects to Config::ipc_path(postfix) within ms_timeout. Throws on failure.
Connection connect(uint64_t ms_timeout, const std::string& postfix);

#if !defined(_WIN32)
std::string get_pid_file(const std::string& postfix);
// Removes a stale socket unless the pid file names a live process with our
// executable's name that still answers a connect.
void check_pid(const std::string& postfix);
void write_pid(const std::string& postfix);
#endif

// Client-side helpers (talk to the server at postfix ""). The throwing ones
// mirror ResultType; the others swallow errors exactly where upstream
// unwraps with a fallback.
std::optional<std::string> get_config(const std::string& name);  // 1000 ms
std::optional<std::string> get_config_async(const std::string& name, uint64_t ms_timeout);
void set_config(const std::string& name, const std::string& value);
void set_password(const std::string& v);  // throws when the server is unreachable
std::string get_id();
std::string get_password();
hbb_common::ResolvedAddr get_rendezvous_server(uint64_t ms_timeout);
std::unordered_map<std::string, std::string> get_options_(uint64_t ms_timeout);
std::unordered_map<std::string, std::string> get_options();
std::string get_option(const std::string& key);
void set_option(const std::string& key, const std::string& value);
void set_options(const std::unordered_map<std::string, std::string>& value);  // throws
int32_t get_nat_type_(uint64_t ms_timeout);
int32_t get_nat_type(uint64_t ms_timeout);

}  // namespace rustdesk::ipc
