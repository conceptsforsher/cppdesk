// parity_tokio_ipc.hpp — translation of libs/parity-tokio-ipc (lib.rs +
// unix.rs) at rustdesk d1013487: a vendored fork of parity-tokio-ipc 0.7.2.
//
// Endpoint / Incoming / Connection over Unix domain sockets. tokio's async
// UnixListener/UnixStream become blocking POSIX sockets, opened CLOEXEC like
// Rust's std, with SIGPIPE suppressed per call the way the Rust runtime
// ignores it. The framed layer (rustdesk ipc::Connection) adds deadlines
// through Connection::wait_readable. win.rs (named pipes) is deferred with the
// Windows platform step.
//
// Quirks kept verbatim from the vendored unix.rs:
// - Endpoint::incoming() applies the security attributes before bind(). A
//   set_mode()/allow_everyone_connect() endpoint on a fresh path therefore
//   fails with ENOENT (chmod of a missing file); the crate's own smoke_test
//   trips over this. rustdesk only uses allow_everyone_create() (mode None),
//   which never chmods.
// - Dropping Incoming does not unlink the socket file (there is no Drop impl).
//   rustdesk's ipc::check_pid removes stale sockets at the next start.
//
// Errors: io::Error maps to std::system_error (errno), and invalid paths to
// std::invalid_argument.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace parity_tokio_ipc {

// Socket permissions and ownership on UNIX.
class SecurityAttributes {
public:
    // New default security attributes.
    static SecurityAttributes empty();
    // New security attributes that allow everyone to connect (mode 0o777).
    SecurityAttributes allow_everyone_connect() const;
    // Set a custom permission on the socket.
    SecurityAttributes set_mode(uint16_t mode) const;
    // New security attributes that allow everyone to create (mode None on unix).
    static SecurityAttributes allow_everyone_create();
    // Called after the server socket has been created (upstream comment; it
    // actually runs before bind, see above). chmod()s the path when a mode is
    // set. Throws std::system_error on failure.
    void apply_permissions(const std::string& path) const;

    std::optional<uint16_t> mode() const { return mode_; }

private:
    std::optional<uint16_t> mode_;
};

// IPC connection (UnixStream). Move-only; closes the fd on destruction.
class Connection {
public:
    // wrap() parity. Public: the framed layer and tests adopt fds directly
    // (for example one end of a socketpair).
    explicit Connection(int fd);
    ~Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // AsyncRead::poll_read parity (blocking): bytes read, 0 at EOF.
    size_t read(uint8_t* buf, size_t len);
    // AsyncWriteExt::write_all parity (blocking; EPIPE instead of SIGPIPE).
    void write_all(const uint8_t* buf, size_t len);
    // AsyncWrite::poll_shutdown parity: shuts down the write half.
    void shutdown();
    // Readiness for the framed layer's deadlines: true when a read would not
    // block (data, EOF or error pending); false on timeout. timeout_ms < 0
    // waits forever.
    bool wait_readable(int timeout_ms) const;

    int fd() const { return fd_; }

private:
    int fd_ = -1;
};

// Stream of incoming connections (the bound UnixListener).
class Incoming {
public:
    Incoming(Incoming&& other) noexcept;
    Incoming& operator=(Incoming&& other) noexcept;
    Incoming(const Incoming&) = delete;
    Incoming& operator=(const Incoming&) = delete;
    ~Incoming();

    // `next().await` parity: blocks for the next client. Upstream yields
    // Some(Err(e)) on accept failure; here that throws std::system_error.
    Connection next();

    int fd() const { return fd_; }

private:
    friend class Endpoint;
    explicit Incoming(int fd) : fd_(fd) {}
    int fd_ = -1;
};

// Endpoint implementation for unix systems.
class Endpoint {
public:
    // New IPC endpoint at the given path.
    explicit Endpoint(std::string path);

    // Stream of incoming connections: apply_permissions, then bind + listen.
    Incoming incoming();
    // Set security attributes for the connection.
    void set_security_attributes(SecurityAttributes security_attributes);
    // Make new connection using the provided path. timeout_ms > 0 bounds the
    // connect (rustdesk wraps it in `timeout(ms, ...)`). On expiry it throws
    // std::system_error(errc::timed_out).
    static Connection connect(const std::string& path, uint64_t timeout_ms = 0);
    // Returns the path of the endpoint.
    const std::string& path() const { return path_; }

private:
    // Inner platform-dependent state: the bound + listening socket fd.
    int inner() const;

    std::string path_;
    SecurityAttributes security_attributes_;
};

// For testing/examples: "/tmp/my-uds-<random u64>".
std::string dummy_endpoint();

}  // namespace parity_tokio_ipc
