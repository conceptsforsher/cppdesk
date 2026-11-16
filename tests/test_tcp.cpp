// test_tcp.cpp — tcp.rs FramedStream over real loopback sockets, plus the
// SocketAddr Display / FromStr parity helpers from util.hpp.
//
// The framing cases pin step 14's transport fixes: frames larger than one
// recv() and several frames per recv() must decode (persistent read buffer),
// a frame cut by a timeout must complete later, next_timeout() must restore
// a blocking socket to blocking, connect/accept timeouts must not leak into
// the stream, and EOF must be distinguishable from a timeout.
//
// Step 15 adds: try_next() (the poll-loop read), empty frames on the wire,
// set_raw() dropping the secretbox key, peer_addr(), connect_host() and
// close-on-exec sockets.

#include <hbb_common/bytes_codec.hpp>
#include <hbb_common/crypto.hpp>
#include <hbb_common/tcp.hpp>
#include <hbb_common/util.hpp>

#include <fcntl.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

using hbb_common::BytesCodec;
using hbb_common::ResolvedAddr;
using hbb_common::TcpFramedStream;
using hbb_common::TcpListener;
using hbb_common::to_socket_addr;
using Bytes = std::vector<uint8_t>;

struct Conn {
    TcpFramedStream client;
    TcpFramedStream server;
};

std::optional<Conn> make_conn(uint64_t connect_timeout_ms = 2000, uint64_t accept_timeout_ms = 2000) {
    auto listener = TcpListener::bind(to_socket_addr("127.0.0.1:0"), true);
    if (!listener) {
        return std::nullopt;
    }
    const auto addr = listener->local_addr();
    if (!addr) {
        return std::nullopt;
    }
    std::optional<TcpFramedStream> accepted;
    std::thread acceptor([&] { accepted = listener->accept(accept_timeout_ms); });
    auto client =
        TcpFramedStream::connect(*addr, to_socket_addr("127.0.0.1:0"), connect_timeout_ms);
    acceptor.join();
    if (!client || !accepted) {
        return std::nullopt;
    }
    return Conn{std::move(*client), std::move(*accepted)};
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

Bytes pattern(size_t n) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        b[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    return b;
}

void test_tcp_listener_bind() {
    const auto addr = to_socket_addr("127.0.0.1:0");
    auto listener = TcpListener::bind(addr, true);
    CHECK(listener.has_value());
    CHECK(listener && listener->fd() >= 0);
    const auto local = listener ? listener->local_addr() : std::nullopt;
    CHECK(local && local->port != 0);
}

void test_small_roundtrip() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    const Bytes msg = {1, 2, 3};
    CHECK(c->client.send_raw(msg));
    const auto got = c->server.next_timeout(2000);
    CHECK(got && *got == msg);
    CHECK(c->server.send_raw(msg));
    const auto back = c->client.next_timeout(2000);
    CHECK(back && *back == msg);
}

void test_large_frame() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    const Bytes big = pattern(300 * 1024);  // far beyond one recv() / socket buffer
    std::thread writer([&] { CHECK(c->client.send_raw(big)); });
    const auto got = c->server.next_timeout(10000);
    writer.join();
    CHECK(got.has_value());
    CHECK(got && got->size() == big.size());
    CHECK(got && *got == big);
}

void test_coalesced_frames() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    BytesCodec codec;
    Bytes wire;
    const Bytes one = {'a'};
    const Bytes two = {'b', 'c'};
    const Bytes three = pattern(5000);
    codec.encode(one, wire);
    codec.encode(two, wire);
    codec.encode(three, wire);
    CHECK(c->client.send_bytes(wire));  // one write, three frames
    const auto f1 = c->server.next_timeout(2000);
    const auto f2 = c->server.next_timeout(2000);
    const auto f3 = c->server.next_timeout(2000);
    CHECK(f1 && *f1 == one);
    CHECK(f2 && *f2 == two);
    CHECK(f3 && *f3 == three);
}

void test_split_frame() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    BytesCodec codec;
    Bytes wire;
    const Bytes payload = pattern(100);  // 2-byte header: split inside the header
    codec.encode(payload, wire);
    const Bytes first(wire.begin(), wire.begin() + 1);
    const Bytes second(wire.begin() + 1, wire.end());
    std::thread writer([&] {
        CHECK(c->client.send_bytes(first));
        sleep_ms(150);
        CHECK(c->client.send_bytes(second));
    });
    const auto got = c->server.next_timeout(3000);
    writer.join();
    CHECK(got && *got == payload);
}

void test_partial_frame_across_timeout() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    BytesCodec codec;
    Bytes wire;
    const Bytes payload = pattern(20000);
    codec.encode(payload, wire);
    const size_t half = wire.size() / 2;
    CHECK(c->client.send_bytes(Bytes(wire.begin(), wire.begin() + static_cast<long>(half))));
    const auto none = c->server.next_timeout(150);
    CHECK(!none.has_value());
    CHECK(!c->server.peer_closed());  // a timeout, not EOF
    CHECK(c->client.send_bytes(Bytes(wire.begin() + static_cast<long>(half), wire.end())));
    const auto got = c->server.next_timeout(3000);
    CHECK(got && *got == payload);
}

void test_timeout_restored_to_blocking() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    // The accepted stream is blocking. A short next_timeout() must not leave
    // its timeout behind (the old restore skipped the 0 case).
    CHECK(!c->server.next_timeout(50).has_value());
    const Bytes msg = {9, 9};
    std::thread writer([&] {
        sleep_ms(800);
        CHECK(c->client.send_raw(msg));
    });
    const auto got = c->server.next();  // must block past 50 ms
    writer.join();
    CHECK(got && *got == msg);
}

void test_accept_timeout_not_inherited() {
    // Linux copies the listener's SO_RCVTIMEO into accepted sockets.
    auto c = make_conn(2000, 300);
    CHECK(c.has_value());
    if (!c) return;
    const Bytes msg = {4};
    std::thread writer([&] {
        sleep_ms(800);
        CHECK(c->client.send_raw(msg));
    });
    const auto got = c->server.next();  // must outlive the 300 ms accept timeout
    writer.join();
    CHECK(got && *got == msg);
}

void test_connect_timeout_not_kept() {
    // `timeout(ms, connect)` bounds the connect only.
    auto c = make_conn(300, 2000);
    CHECK(c.has_value());
    if (!c) return;
    const Bytes msg = {5};
    std::thread writer([&] {
        sleep_ms(800);
        CHECK(c->server.send_raw(msg));
    });
    const auto got = c->client.next();  // must outlive the 300 ms connect timeout
    writer.join();
    CHECK(got && *got == msg);
}

void test_peer_closed() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    CHECK(!c->server.peer_closed());
    const Bytes last = {7, 7, 7};
    CHECK(c->client.send_raw(last));
    { TcpFramedStream gone = std::move(c->client); }  // close the client side
    const auto got = c->server.next_timeout(2000);  // buffered data still arrives
    CHECK(got && *got == last);
    CHECK(!c->server.peer_closed());
    const auto eof = c->server.next_timeout(2000);
    CHECK(!eof.has_value());
    CHECK(c->server.peer_closed());
}

void test_move_keeps_buffer() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    BytesCodec codec;
    Bytes wire;
    const Bytes a = {1};
    const Bytes b = {2};
    codec.encode(a, wire);
    codec.encode(b, wire);
    CHECK(c->client.send_bytes(wire));
    const auto f1 = c->server.next_timeout(2000);  // second frame now sits in rbuf
    CHECK(f1 && *f1 == a);
    sleep_ms(50);
    TcpFramedStream moved = std::move(c->server);
    const auto f2 = moved.next_timeout(2000);
    CHECK(f2 && *f2 == b);
}

void test_socket_addr_display() {
    using hbb_common::to_string;
    CHECK(to_string(ResolvedAddr{AF_INET, "1.2.3.4", 5}) == "1.2.3.4:5");
    CHECK(to_string(ResolvedAddr{AF_INET, "0.0.0.0", 0}) == "0.0.0.0:0");
    CHECK(to_string(ResolvedAddr{AF_INET6, "::1", 21116}) == "[::1]:21116");
    CHECK(to_string(ResolvedAddr{AF_INET6, "fe80::1", 80}) == "[fe80::1]:80");
}

void test_socket_addr_parse() {
    using hbb_common::parse_socket_addr;
    const auto v4 = parse_socket_addr("127.0.0.1:21116");
    CHECK(v4 && v4->family == AF_INET && v4->ip == "127.0.0.1" && v4->port == 21116);
    const auto v6 = parse_socket_addr("[::1]:80");
    CHECK(v6 && v6->family == AF_INET6 && v6->ip == "::1" && v6->port == 80);
    const auto canon = parse_socket_addr("[0:0:0:0:0:0:0:1]:1");
    CHECK(canon && canon->ip == "::1");  // inet_ntop canonical form
    const auto max_port = parse_socket_addr("1.2.3.4:65535");
    CHECK(max_port && max_port->port == 65535);
    CHECK(parse_socket_addr("1.2.3.4:0080").has_value());  // Rust allows leading zeros in ports
    // Round trip with Display.
    CHECK(v4 && hbb_common::to_string(*v4) == "127.0.0.1:21116");
    CHECK(v6 && hbb_common::to_string(*v6) == "[::1]:80");
    // Not SocketAddrs.
    CHECK(!parse_socket_addr("localhost:80"));  // no name resolution
    CHECK(!parse_socket_addr("1.2.3.4"));
    CHECK(!parse_socket_addr("1.2.3.4:"));
    CHECK(!parse_socket_addr("1.2.3.4:65536"));
    CHECK(!parse_socket_addr("1.2.3.4:-1"));
    CHECK(!parse_socket_addr("1.2.3.4:80x"));
    CHECK(!parse_socket_addr("::1:80"));  // IPv6 needs brackets
    CHECK(!parse_socket_addr("[::1]"));
    CHECK(!parse_socket_addr("[::1]80"));
    CHECK(!parse_socket_addr("[1.2.3.4]:80"));
    CHECK(!parse_socket_addr(" 1.2.3.4:80"));
    CHECK(!parse_socket_addr("1.2.3.4:80 "));
    CHECK(!parse_socket_addr("1.2.3:80"));
    CHECK(!parse_socket_addr("256.1.1.1:80"));
    CHECK(!parse_socket_addr(""));
}

void test_try_next() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    // Nothing sent: no frame, not closed, and no blocking.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!c->server.try_next().has_value());
    CHECK(!c->server.peer_closed());
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    // Two frames in one write: both come out of successive try_next() calls,
    // the second from the buffer alone.
    BytesCodec codec;
    Bytes wire;
    const Bytes a = {1, 2};
    const Bytes b = pattern(3000);
    codec.encode(a, wire);
    codec.encode(b, wire);
    CHECK(c->client.send_bytes(wire));
    std::optional<Bytes> f1;
    for (int i = 0; i < 200 && !f1; ++i) {
        f1 = c->server.try_next();
        if (!f1) sleep_ms(5);
    }
    CHECK(f1 && *f1 == a);
    const auto f2 = c->server.try_next();
    CHECK(f2 && *f2 == b);
    CHECK(!c->server.try_next().has_value());
    // A partial frame stays buffered until the rest arrives.
    Bytes wire2;
    const Bytes big = pattern(20000);
    codec.encode(big, wire2);
    CHECK(c->client.send_bytes(Bytes(wire2.begin(), wire2.begin() + 100)));
    sleep_ms(50);
    CHECK(!c->server.try_next().has_value());
    CHECK(c->client.send_bytes(Bytes(wire2.begin() + 100, wire2.end())));
    std::optional<Bytes> f3;
    for (int i = 0; i < 200 && !f3; ++i) {
        f3 = c->server.try_next();
        if (!f3) sleep_ms(5);
    }
    CHECK(f3 && *f3 == big);
    // EOF: nullopt with peer_closed().
    { TcpFramedStream gone = std::move(c->client); }
    bool closed = false;
    for (int i = 0; i < 200 && !closed; ++i) {
        CHECK(!c->server.try_next().has_value());
        closed = c->server.peer_closed();
        if (!closed) sleep_ms(5);
    }
    CHECK(closed);
}

void test_empty_frame() {
    // An all-default Message serializes to zero bytes; upstream still sends
    // it as a frame (the server handshake waits for exactly that).
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    CHECK(c->client.send_raw({}));
    const auto got = c->server.next_timeout(2000);
    CHECK(got.has_value() && got->empty());
    // Encrypted: an empty payload seals to the 16-byte MAC and opens empty.
    const auto key = hbb_common::crypto::secretbox_gen_key();
    c->client.set_key(key);
    c->server.set_key(key);
    CHECK(c->client.send_raw({}));
    const auto sealed = c->server.next_timeout(2000);
    CHECK(sealed.has_value() && sealed->empty());
}

void test_set_raw_drops_key() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    const auto key = hbb_common::crypto::secretbox_gen_key();
    c->client.set_key(key);
    c->server.set_key(key);
    CHECK(c->client.is_secured() && c->server.is_secured());
    c->client.set_raw();
    c->server.set_raw();
    CHECK(!c->client.is_secured());  // `self.1 = None`
    CHECK(!c->server.is_secured());
    const Bytes payload = {'r', 'a', 'w'};
    CHECK(c->client.send_raw(payload));  // raw codec, no seal: bytes as-is
    const auto got = c->server.next_timeout(2000);
    CHECK(got && *got == payload);
}

void test_peer_addr() {
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    const auto client_local = c->client.local_addr();
    const auto server_peer = c->server.peer_addr();
    CHECK(client_local && server_peer);
    CHECK(client_local && server_peer && server_peer->ip == "127.0.0.1" &&
          server_peer->port == client_local->port);
    const auto client_peer = c->client.peer_addr();
    const auto server_local = c->server.local_addr();
    CHECK(client_peer && server_local && client_peer->port == server_local->port);
}

void test_connect_host() {
    auto listener = TcpListener::bind(to_socket_addr("127.0.0.1:0"), true);
    CHECK(listener.has_value());
    if (!listener) return;
    const auto addr = listener->local_addr();
    CHECK(addr.has_value());
    if (!addr) return;
    // Literal SocketAddr and "host:port" by name both connect.
    for (const std::string target :
         {"127.0.0.1:" + std::to_string(addr->port), "localhost:" + std::to_string(addr->port)}) {
        std::optional<TcpFramedStream> accepted;
        std::thread acceptor([&] { accepted = listener->accept(3000); });
        auto s = TcpFramedStream::connect_host(target, 3000);
        acceptor.join();
        if (target.rfind("localhost", 0) == 0 && !s) {
            // localhost may resolve to ::1 only on some hosts; the listener
            // is v4. Not a failure of the resolver loop.
            continue;
        }
        CHECK(s.has_value());
        CHECK(accepted.has_value());
        if (s && accepted) {
            const Bytes msg = {7};
            CHECK(s->send_raw(msg));
            const auto got = accepted->next_timeout(2000);
            CHECK(got && *got == msg);
        }
    }
    // Invalid port values and refused connects come back empty.
    CHECK(!TcpFramedStream::connect_host("127.0.0.1", 500).has_value());
    CHECK(!TcpFramedStream::connect_host("localhost:99999", 500).has_value());
    CHECK(!TcpFramedStream::connect_host("localhost:x", 500).has_value());
    auto closed_port = TcpListener::bind(to_socket_addr("127.0.0.1:0"), true);
    CHECK(closed_port.has_value());
    if (closed_port) {
        const auto cp = closed_port->local_addr();
        closed_port.reset();  // nobody listens there now
        if (cp) {
            CHECK(!TcpFramedStream::connect_host("127.0.0.1:" + std::to_string(cp->port), 1000)
                       .has_value());
        }
    }
}

bool is_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

void test_cloexec() {
    // Rust's std/socket2 open every socket close-on-exec; a spawned child
    // (the connection manager) must not inherit live connections.
    auto c = make_conn();
    CHECK(c.has_value());
    if (!c) return;
    CHECK(is_cloexec(c->client.fd()));
    CHECK(is_cloexec(c->server.fd()));
    auto listener = TcpListener::bind(to_socket_addr("127.0.0.1:0"), true);
    CHECK(listener && is_cloexec(listener->fd()));
    const auto addr = listener ? listener->local_addr() : std::nullopt;
    if (addr) {
        auto s = TcpFramedStream::connect_host("127.0.0.1:" + std::to_string(addr->port), 2000);
        auto accepted = listener->accept(2000);
        CHECK(s && is_cloexec(s->fd()));
        CHECK(accepted && is_cloexec(accepted->fd()));
    }
}

void test_parse_ip_addr() {
    using hbb_common::parse_ip_addr;
    const auto v4 = parse_ip_addr("10.0.0.1");
    CHECK(v4 && v4->family == AF_INET && v4->ip == "10.0.0.1" && v4->port == 0);
    const auto v6 = parse_ip_addr("0:0:0:0:0:0:0:1");
    CHECK(v6 && v6->family == AF_INET6 && v6->ip == "::1");
    CHECK(parse_ip_addr("::ffff:1.2.3.4").has_value());
    CHECK(!parse_ip_addr("10.0.0.1:80"));  // a SocketAddr, not an IpAddr
    CHECK(!parse_ip_addr("[::1]"));
    CHECK(!parse_ip_addr("01.2.3.4"));  // leading zeros are rejected
    CHECK(!parse_ip_addr(" 1.2.3.4"));
    CHECK(!parse_ip_addr("localhost"));
    CHECK(!parse_ip_addr(""));
    CHECK(!parse_ip_addr(std::string("1.2.3.4\0", 8)));
}

}  // namespace

int main() {
    test_tcp_listener_bind();
    test_small_roundtrip();
    test_large_frame();
    test_coalesced_frames();
    test_split_frame();
    test_partial_frame_across_timeout();
    test_timeout_restored_to_blocking();
    test_accept_timeout_not_inherited();
    test_connect_timeout_not_kept();
    test_peer_closed();
    test_move_keeps_buffer();
    test_socket_addr_display();
    test_socket_addr_parse();
    test_try_next();
    test_empty_frame();
    test_set_raw_drops_key();
    test_peer_addr();
    test_connect_host();
    test_cloexec();
    test_parse_ip_addr();
    if (g_failures == 0) {
        std::puts("test_tcp: all tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
