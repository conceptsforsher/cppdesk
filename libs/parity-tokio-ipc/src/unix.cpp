// unix.cpp — translation of libs/parity-tokio-ipc/src/unix.rs (+ lib.rs
// dummy_endpoint). See parity_tokio_ipc.hpp.
#include <parity_tokio_ipc/parity_tokio_ipc.hpp>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace parity_tokio_ipc {
namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;  // the Rust runtime ignores SIGPIPE
#else
constexpr int kSendFlags = 0;  // SO_NOSIGPIPE is set per socket instead
#endif

// std::os::unix::net::UnixListener::bind backlog.
constexpr int kBacklog = 128;

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

class FdGuard {
public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

// Rust's std opens every fd CLOEXEC; mirror that where SOCK_CLOEXEC/accept4
// are unavailable, and suppress SIGPIPE per socket where MSG_NOSIGNAL is.
void prepare_fd(int fd) {
#if !defined(SOCK_CLOEXEC)
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    (void)fd;
}

int new_unix_socket() {
#if defined(SOCK_CLOEXEC)
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (fd < 0) {
        throw_errno("socket");
    }
    prepare_fd(fd);
    return fd;
}

// std's sockaddr_un(): reject interior NULs and paths that do not fit
// sun_path (one byte is kept for the terminator).
socklen_t make_addr(const std::string& path, struct sockaddr_un& sa) {
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (path.find('\0') != std::string::npos) {
        throw std::invalid_argument("paths may not contain interior null bytes");
    }
    if (path.size() >= sizeof sa.sun_path) {
        throw std::invalid_argument("path must be shorter than SUN_LEN");
    }
    std::memcpy(sa.sun_path, path.data(), path.size());
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + path.size() + 1);
}

void set_send_timeout(int fd, uint64_t timeout_ms) {
    struct timeval tv {};
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

}  // namespace

// --- SecurityAttributes -------------------------------------------------------

SecurityAttributes SecurityAttributes::empty() {
    return SecurityAttributes();
}

SecurityAttributes SecurityAttributes::allow_everyone_connect() const {
    SecurityAttributes out = *this;
    out.mode_ = 0777;
    return out;
}

SecurityAttributes SecurityAttributes::set_mode(uint16_t mode) const {
    SecurityAttributes out = *this;
    out.mode_ = mode;
    return out;
}

SecurityAttributes SecurityAttributes::allow_everyone_create() {
    return SecurityAttributes();  // mode: None
}

void SecurityAttributes::apply_permissions(const std::string& path) const {
    // CString::new(path)? runs before the mode check, so a NUL fails even
    // without a mode (upstream order).
    if (path.find('\0') != std::string::npos) {
        throw std::invalid_argument("data provided contains a nul byte");
    }
    if (mode_) {
        if (::chmod(path.c_str(), static_cast<mode_t>(*mode_)) == -1) {
            throw_errno("chmod");
        }
    }
}

// --- Connection ---------------------------------------------------------------

Connection::Connection(int fd) : fd_(fd) {}

Connection::~Connection() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Connection::Connection(Connection&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

size_t Connection::read(uint8_t* buf, size_t len) {
    while (true) {
        const ssize_t n = ::recv(fd_, buf, len, 0);
        if (n >= 0) {
            return static_cast<size_t>(n);
        }
        if (errno == EINTR) {
            continue;
        }
        throw_errno("read");
    }
}

void Connection::write_all(const uint8_t* buf, size_t len) {
    while (len > 0) {
        const ssize_t n = ::send(fd_, buf, len, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("write");
        }
        if (n == 0) {
            // io::ErrorKind::WriteZero parity ("failed to write whole buffer").
            throw std::system_error(std::make_error_code(std::errc::broken_pipe),
                                    "failed to write whole buffer");
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
}

void Connection::shutdown() {
    if (::shutdown(fd_, SHUT_WR) != 0) {
        throw_errno("shutdown");
    }
}

bool Connection::wait_readable(int timeout_ms) const {
    struct pollfd p {};
    p.fd = fd_;
    p.events = POLLIN;
    const int r = ::poll(&p, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) {
            return false;  // caller re-checks its deadline
        }
        throw_errno("poll");
    }
    return r > 0;  // POLLIN, POLLHUP or POLLERR: read() reports which
}

// --- Incoming -----------------------------------------------------------------

Incoming::Incoming(Incoming&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Incoming& Incoming::operator=(Incoming&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Incoming::~Incoming() {
    if (fd_ >= 0) {
        ::close(fd_);  // no unlink: the vendored crate has no Drop impl
    }
}

Connection Incoming::next() {
    while (true) {
#if defined(__linux__)
        const int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
#else
        const int cfd = ::accept(fd_, nullptr, nullptr);
#endif
        if (cfd >= 0) {
#if !defined(__linux__)
            ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
#endif
            prepare_fd(cfd);
            return Connection(cfd);
        }
        if (errno == EINTR) {
            continue;
        }
        throw_errno("accept");
    }
}

// --- Endpoint -----------------------------------------------------------------

Endpoint::Endpoint(std::string path)
    : path_(std::move(path)), security_attributes_(SecurityAttributes::empty()) {}

Incoming Endpoint::incoming() {
    // the call to bind in `inner()` creates the file
    // `apply_permission()` will set the file permissions.
    // (Upstream comments; the order below is upstream's: permissions first.)
    security_attributes_.apply_permissions(path_);
    return Incoming(inner());
}

int Endpoint::inner() const {
    const int fd = new_unix_socket();
    FdGuard guard(fd);
    struct sockaddr_un sa;
    const socklen_t len = make_addr(path_, sa);
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&sa), len) != 0) {
        throw_errno("bind");
    }
    if (::listen(fd, kBacklog) != 0) {
        throw_errno("listen");
    }
    return guard.release();
}

void Endpoint::set_security_attributes(SecurityAttributes security_attributes) {
    security_attributes_ = security_attributes;
}

Connection Endpoint::connect(const std::string& path, uint64_t timeout_ms) {
    const int fd = new_unix_socket();
    FdGuard guard(fd);
    struct sockaddr_un sa;
    const socklen_t len = make_addr(path, sa);
    if (timeout_ms > 0) {
        // A blocking AF_UNIX connect waits for listener backlog room and
        // honors SO_SNDTIMEO (EAGAIN on expiry).
        set_send_timeout(fd, timeout_ms);
    }
    if (::connect(fd, reinterpret_cast<const struct sockaddr*>(&sa), len) != 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) {
            throw std::system_error(std::make_error_code(std::errc::timed_out), "connect");
        }
        throw_errno("connect");
    }
    if (timeout_ms > 0) {
        set_send_timeout(fd, 0);  // the timeout bounds the connect only
    }
    return Connection(guard.release());
}

// --- lib.rs -------------------------------------------------------------------

std::string dummy_endpoint() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const uint64_t num = rng();  // rand::Rng::gen::<u64>(&mut thread_rng())
#if defined(_WIN32)
    return "\\\\.\\pipe\\my-pipe-" + std::to_string(num);
#else
    return "/tmp/my-uds-" + std::to_string(num);
#endif
}

}  // namespace parity_tokio_ipc
