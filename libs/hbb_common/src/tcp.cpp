// tcp.cpp — implementation of tcp.hpp.
#include <hbb_common/tcp.hpp>

#include <hbb_common/crypto.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace hbb_common {
namespace {

// Set one socket timeout option (SO_RCVTIMEO / SO_SNDTIMEO) in ms; 0 = blocking.
void sock_set_one_timeout(int fd, int opt, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof tv);
}

int sock_get_one_timeout(int fd, int opt) {
    struct timeval tv {};
    socklen_t len = sizeof tv;
    if (::getsockopt(fd, SOL_SOCKET, opt, &tv, &len) != 0) {
        return 0;
    }
    return static_cast<int>(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

// Set socket send/receive timeout (ms). 0 = infinite (blocking).
void sock_set_timeout(int fd, int timeout_ms) {
    sock_set_one_timeout(fd, SO_RCVTIMEO, timeout_ms);
    sock_set_one_timeout(fd, SO_SNDTIMEO, timeout_ms);
}

// sockaddr_storage -> ResolvedAddr (SocketAddr parity for v4/v6).
std::optional<ResolvedAddr> to_resolved(const struct sockaddr_storage& sa) {
    ResolvedAddr out{};
    char ip[INET6_ADDRSTRLEN] = {};
    if (sa.ss_family == AF_INET) {
        const auto* sa4 = reinterpret_cast<const struct sockaddr_in*>(&sa);
        if (!::inet_ntop(AF_INET, &sa4->sin_addr, ip, sizeof ip)) {
            return std::nullopt;
        }
        out.family = AF_INET;
        out.port = ntohs(sa4->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        const auto* sa6 = reinterpret_cast<const struct sockaddr_in6*>(&sa);
        if (!::inet_ntop(AF_INET6, &sa6->sin6_addr, ip, sizeof ip)) {
            return std::nullopt;
        }
        out.family = AF_INET6;
        out.port = ntohs(sa6->sin6_port);
    } else {
        return std::nullopt;
    }
    out.ip = ip;
    return out;
}

// Non-blocking connect to one resolved address, waiting until `deadline`
// (nullopt: no limit). Returns the connected, blocking fd or -1.
int connect_one(const struct addrinfo* ai,
                const std::optional<std::chrono::steady_clock::time_point>& deadline) {
#if defined(SOCK_CLOEXEC)
    const int fd = ::socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (fd < 0) {
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        while (true) {
            int wait_ms = -1;
            if (deadline) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      *deadline - std::chrono::steady_clock::now())
                                      .count();
                if (left <= 0) {
                    ::close(fd);
                    return -1;
                }
                wait_ms = static_cast<int>(left);
            }
            struct pollfd p {
                fd, POLLOUT, 0
            };
            const int r = ::poll(&p, 1, wait_ms);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            if (r <= 0) {
                ::close(fd);
                return -1;
            }
            break;
        }
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, flags);  // back to blocking, like a tokio-owned socket's I/O
    return fd;
}

}  // namespace

TcpFramedStream::TcpFramedStream(int fd) : fd_(fd) {}

TcpFramedStream::~TcpFramedStream() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::optional<ResolvedAddr> TcpFramedStream::local_addr() const {
    struct sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&sa), &len) != 0) {
        return std::nullopt;
    }
    return to_resolved(sa);
}

std::optional<ResolvedAddr> TcpFramedStream::peer_addr() const {
    struct sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    if (::getpeername(fd_, reinterpret_cast<struct sockaddr*>(&sa), &len) != 0) {
        return std::nullopt;
    }
    return to_resolved(sa);
}

TcpFramedStream::TcpFramedStream(TcpFramedStream&& other) noexcept
    : crypto_(std::move(other.crypto_)),
      fd_(other.fd_),
      codec_(std::move(other.codec_)),
      rbuf_(std::move(other.rbuf_)),
      peer_closed_(other.peer_closed_),
      saved_rcv_timeout_ms_(other.saved_rcv_timeout_ms_) {
    other.fd_ = -1;
}

TcpFramedStream& TcpFramedStream::operator=(TcpFramedStream&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        crypto_ = std::move(other.crypto_);
        fd_ = other.fd_;
        codec_ = std::move(other.codec_);
        rbuf_ = std::move(other.rbuf_);
        peer_closed_ = other.peer_closed_;
        saved_rcv_timeout_ms_ = other.saved_rcv_timeout_ms_;
        other.fd_ = -1;
    }
    return *this;
}

std::optional<TcpFramedStream> TcpFramedStream::connect(
    const ResolvedAddr& remote_addr,
    const ResolvedAddr& local_addr,
    uint64_t timeout_ms) {
    try {
        int fd = new_socket(local_addr, true, true);
        // Set connect timeout using select() on the socket.
        if (timeout_ms > 0) {
            sock_set_timeout(fd, static_cast<int>(timeout_ms));
        }
        if (local_addr.family == AF_INET) {
            struct sockaddr_in sa {};
            sa.sin_family = AF_INET;
            sa.sin_port = htons(remote_addr.port);
            ::inet_pton(AF_INET, remote_addr.ip.c_str(), &sa.sin_addr);
            if (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
                if (errno != EINPROGRESS) {
                    ::close(fd);
                    return std::nullopt;
                }
            }
        } else {
            struct sockaddr_in6 sa {};
            sa.sin6_family = AF_INET6;
            sa.sin6_port = htons(remote_addr.port);
            ::inet_pton(AF_INET6, remote_addr.ip.c_str(), &sa.sin6_addr);
            if (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
                if (errno != EINPROGRESS) {
                    ::close(fd);
                    return std::nullopt;
                }
            }
        }
        // For blocking connect, the timeout was set via SO_RCVTIMEO/SO_SNDTIMEO.
        // Check if connection succeeded.
        if (timeout_ms > 0) {
            // poll(), not select(): FD_SET on a descriptor >= FD_SETSIZE
            // writes out of bounds, and a busy server has many sockets.
            struct pollfd p {
                fd, POLLOUT, 0
            };
            if (::poll(&p, 1, static_cast<int>(timeout_ms)) <= 0) {
                ::close(fd);
                return std::nullopt;
            }
            int err = 0;
            socklen_t len = sizeof err;
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                ::close(fd);
                return std::nullopt;
            }
            // `timeout(ms, connect)` bounds the connect only: clear the
            // temporary socket timeouts so later I/O blocks like tokio's.
            sock_set_timeout(fd, 0);
        }
        return TcpFramedStream(fd);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<TcpFramedStream> TcpFramedStream::connect_host(const std::string& host_port,
                                                              uint64_t timeout_ms) {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (timeout_ms > 0) {
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    }
    // ToSocketAddrs for &str: a literal SocketAddr first, else "host:port"
    // split at the last ':' and resolved.
    std::string node;
    std::string service;
    if (const auto lit = parse_socket_addr(host_port)) {
        node = lit->ip;
        service = std::to_string(lit->port);
    } else {
        const size_t colon = host_port.rfind(':');
        if (colon == std::string::npos) {
            return std::nullopt;  // "invalid socket address"
        }
        node = host_port.substr(0, colon);
        service = host_port.substr(colon + 1);
        if (service.empty() || service.size() > 5 ||
            service.find_first_not_of("0123456789") != std::string::npos ||
            std::stoul(service) > 65535) {
            return std::nullopt;  // "invalid port value"
        }
    }
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* list = nullptr;
    if (::getaddrinfo(node.c_str(), service.c_str(), &hints, &list) != 0 || !list) {
        if (list) {
            ::freeaddrinfo(list);
        }
        return std::nullopt;
    }
    int fd = -1;
    for (const struct addrinfo* ai = list; ai && fd < 0; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        fd = connect_one(ai, deadline);
        if (fd < 0 && deadline && std::chrono::steady_clock::now() >= *deadline) {
            break;  // Elapsed: the timeout covers every attempt
        }
    }
    ::freeaddrinfo(list);
    if (fd < 0) {
        return std::nullopt;
    }
    return TcpFramedStream(fd);
}

void TcpFramedStream::set_raw() {
    codec_.set_raw();
    crypto_.reset();  // `self.1 = None` (upstream set_raw drops the key too)
}

bool TcpFramedStream::is_secured() const {
    return crypto_.has_value();
}

void TcpFramedStream::set_key(const std::vector<uint8_t>& key) {
    if (key.size() != crypto::kSecretBoxKeyBytes) {
        throw std::invalid_argument("set_key: secretbox key must be 32 bytes");
    }
    crypto_ = std::make_tuple(key, 0, 0);
}

bool TcpFramedStream::send_all(const uint8_t* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout on send.
                return false;
            }
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool TcpFramedStream::send_bytes(const std::vector<uint8_t>& msg) {
    return send_all(msg.data(), msg.size());
}

bool TcpFramedStream::send_raw(const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> payload = msg;
    if (crypto_) {
        // key.1 += 1; seal with get_nonce(key.1) parity.
        auto& [key, seq_out, seq_in] = *crypto_;
        (void)seq_in;
        ++seq_out;
        payload = crypto::secretbox_seal(msg.data(), msg.size(), crypto::seq_nonce(seq_out), key);
    }
    std::vector<uint8_t> framed;
    codec_.encode(payload, framed);
    return send_all(framed.data(), framed.size());
}

std::optional<std::vector<uint8_t>> TcpFramedStream::recv_frame(int recv_flags) {
    // Framed parity: decode from the persistent buffer first (an earlier
    // recv() may already hold a whole frame), read more only when the
    // buffer has no complete frame, and keep any surplus for the next call.
    // (The previous version decoded from a fresh 4 KiB buffer per recv(),
    // which dropped coalesced frames and corrupted frames > 4 KiB.)
    while (true) {
        auto decoded = codec_.decode(rbuf_);
        if (!decoded) {
            uint8_t chunk[16384];
            const ssize_t n = ::recv(fd_, chunk, sizeof chunk, recv_flags);
            if (n == 0) {
                peer_closed_ = true;
                return std::nullopt;  // EOF
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Timeout, or nothing more right now under MSG_DONTWAIT
                    // (a partial frame stays buffered).
                    return std::nullopt;
                }
                peer_closed_ = true;
                return std::nullopt;  // Error (reset)
            }
            rbuf_.insert(rbuf_.end(), chunk, chunk + n);
            continue;
        }
        if (crypto_) {
            // key.2 += 1; open with get_nonce(key.2) parity. A failed open
            // surfaces as Some(Err("decryption error")) upstream; here it
            // throws (loud and distinct from EOF/timeout nullopt).
            auto& [key, seq_out, seq_in] = *crypto_;
            (void)seq_out;
            ++seq_in;
            try {
                *decoded = crypto::secretbox_open(decoded->data(), decoded->size(),
                                                  crypto::seq_nonce(seq_in), key);
            } catch (const std::exception&) {
                throw std::runtime_error("decryption error");
            }
        }
        return decoded;
    }
}

std::optional<std::vector<uint8_t>> TcpFramedStream::next() {
    return recv_frame();
}

std::optional<std::vector<uint8_t>> TcpFramedStream::try_next() {
    if (peer_closed_) {
        return std::nullopt;
    }
    return recv_frame(MSG_DONTWAIT);
}

std::optional<std::vector<uint8_t>> TcpFramedStream::next_timeout(uint64_t timeout_ms) {
    if (timeout_ms > 0) {
        set_timeout_ms_for_call(static_cast<int>(timeout_ms));
        auto res = recv_frame();
        restore_timeout();
        return res;
    }
    return recv_frame();
}

void TcpFramedStream::set_timeout_ms(int timeout_ms) {
    sock_set_timeout(fd_, timeout_ms);
}

void TcpFramedStream::set_timeout_ms_for_call(int timeout_ms) {
    // Only the receive side: next_timeout() bounds one read, not sends.
    saved_rcv_timeout_ms_ = sock_get_one_timeout(fd_, SO_RCVTIMEO);
    sock_set_one_timeout(fd_, SO_RCVTIMEO, timeout_ms);
}

void TcpFramedStream::restore_timeout() {
    // Restore exactly what the socket had, 0 (blocking) included. The old
    // version skipped the restore when the saved value was 0, which left the
    // per-call timeout on blocking streams for every later next().
    sock_set_one_timeout(fd_, SO_RCVTIMEO, saved_rcv_timeout_ms_);
    saved_rcv_timeout_ms_ = 0;
}

// --- TcpListener ------------------------------------------------------------

std::optional<TcpListener> TcpListener::bind(const ResolvedAddr& addr, bool reuse) {
    try {
        int fd = new_socket(addr, true, reuse);
        if (::listen(fd, TcpFramedStream::kDefaultBacklog) != 0) {
            ::close(fd);
            return std::nullopt;
        }
        return TcpListener(fd);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<TcpFramedStream> TcpListener::accept(uint64_t timeout_ms) {
    if (timeout_ms > 0) {
        sock_set_timeout(fd_, static_cast<int>(timeout_ms));
    }
    struct sockaddr_storage sa{};
    socklen_t salen = sizeof sa;
    // Rust's std accepts with SOCK_CLOEXEC: spawned children (the
    // connection manager) must not inherit live connections.
#if defined(__linux__)
    int cfd = ::accept4(fd_, reinterpret_cast<struct sockaddr*>(&sa), &salen, SOCK_CLOEXEC);
#else
    int cfd = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&sa), &salen);
    if (cfd >= 0) {
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (timeout_ms > 0) {
        sock_set_timeout(fd_, 0);  // restore blocking
    }
    if (cfd < 0) {
        return std::nullopt;
    }
    if (timeout_ms > 0) {
        // Linux copies SO_RCVTIMEO/SO_SNDTIMEO from the listener into the
        // accepted socket: the accept timeout must not leak into the stream.
        sock_set_timeout(cfd, 0);
    }
    return TcpFramedStream(cfd);
}

TcpListener::~TcpListener() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

TcpListener::TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

std::optional<ResolvedAddr> TcpListener::local_addr() const {
    struct sockaddr_storage sa {};
    socklen_t len = sizeof sa;
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&sa), &len) != 0) {
        return std::nullopt;
    }
    ResolvedAddr out{};
    char ip[INET6_ADDRSTRLEN] = {};
    if (sa.ss_family == AF_INET) {
        const auto* sa4 = reinterpret_cast<const struct sockaddr_in*>(&sa);
        if (!::inet_ntop(AF_INET, &sa4->sin_addr, ip, sizeof ip)) {
            return std::nullopt;
        }
        out.family = AF_INET;
        out.port = ntohs(sa4->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        const auto* sa6 = reinterpret_cast<const struct sockaddr_in6*>(&sa);
        if (!::inet_ntop(AF_INET6, &sa6->sin6_addr, ip, sizeof ip)) {
            return std::nullopt;
        }
        out.family = AF_INET6;
        out.port = ntohs(sa6->sin6_port);
    } else {
        return std::nullopt;
    }
    out.ip = ip;
    return out;
}

}  // namespace hbb_common