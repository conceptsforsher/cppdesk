// util.hpp — translation of the free-function half of libs/hbb_common/src/lib.rs
// (everything except the async runtime pieces: sleep/timeout/Stream and the
// tcp/udp/quic modules, which arrive with the transport step).
//
// Error-model mapping: `anyhow::Result<T>` becomes "return T or throw"
// (`anyhow::bail!` becomes `throw std::runtime_error`).

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace hbb_common {

// allow_err! parity: run an expression, swallow any failure. Upstream logs at
// `log::debug!`, which is a no-op without an initialized logger, so this is
// deliberately silent too (a real logger arrives with the logging step).
#define HBB_ALLOW_ERR(expr) \
    do {                    \
        try {               \
            (void)(expr);   \
        } catch (...) {     \
        }                   \
    } while (0)

// Numeric socket address: `std::net::SocketAddr` parity for the resolved,
// first-result use in this crate (family kept so bind() picks the right one).
struct ResolvedAddr {
    int family;  // AF_INET / AF_INET6
    std::string ip;  // numeric host string
    uint16_t port = 0;
};

// `host.to_socket_addrs()[0]` parity. `host` must be "host:port" (a bare host
// fails resolution upstream too). Throws std::runtime_error
// ("Failed to solve {host}" parity) when nothing resolves.
ResolvedAddr to_socket_addr(const std::string& host);

// `impl Display for SocketAddr` parity: "1.2.3.4:21116" / "[::1]:21116".
std::string to_string(const ResolvedAddr& addr);

// `str::parse::<SocketAddr>()` parity: numeric "a.b.c.d:port" or
// "[v6]:port" only (no name resolution, no surrounding whitespace). The ip
// comes back in canonical inet_ntop form. nullopt on any syntax error.
std::optional<ResolvedAddr> parse_socket_addr(const std::string& s);

// `str::parse::<IpAddr>()` parity: a bare v4 dotted quad or v6 text (no
// brackets, port or zone). Port is 0; the ip comes back in canonical
// inet_ntop form, so two parses compare equal exactly when the IpAddrs do.
std::optional<ResolvedAddr> parse_ip_addr(const std::string& s);

// `new_socket(addr, tcp, reuse)` parity: create + (optionally)
// SO_REUSEADDR (+ SO_REUSEPORT on Linux, matching upstream's unix cfg) +
// bind. The socket is close-on-exec (socket2 parity). Returns the fd; throws
// std::system_error on failure. The caller owns the fd.
int new_socket(const ResolvedAddr& addr, bool tcp, bool reuse);

// Pure string helper used by the updater path. Byte-wise port: upstream
// iterates Unicode chars, but both delimiters ('-', '.') are ASCII, so the
// search is equivalent, and the slicing is byte-identical for ASCII URLs,
// which is all upstream ever feeds it.
std::string get_version_from_url(const std::string& url);

}  // namespace hbb_common
