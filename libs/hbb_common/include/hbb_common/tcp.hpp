// tcp.hpp — translation of libs/hbb_common/src/tcp.rs (transport layer).
//
// This provides a framed TCP stream with the same API surface as the Rust
// FramedStream. Key differences from Rust:
// - No async/await: uses blocking sockets with timeouts (SO_RCVTIMEO/SO_SNDTIMEO).
// - Encryption (sodiumoxide secretbox) is a stub until the crypto step lands.
// - `tokio_util::codec::Framed<BytesCodec>` → manual framing using our BytesCodec.
// - `super::timeout` → SO_RCVTIMEO/SO_SNDTIMEO + select() fallback.
// - `super::new_socket` already ported in util.cpp.
//
// The API mirrors the Rust methods so higher layers (client/server) can be
// ported with minimal changes when their turn comes.

#pragma once

#include <hbb_common/bytes_codec.hpp>
#include <hbb_common/util.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hbb_common {

class TcpFramedStream {
public:
    // Default backlog for listeners (parity with DEFAULT_BACKLOG = 128).
    static constexpr int kDefaultBacklog = 128;

    // Construct from an already-connected fd (takes ownership).
    explicit TcpFramedStream(int fd);

    // Destructor closes the fd.
    ~TcpFramedStream();

    TcpFramedStream(const TcpFramedStream&) = delete;
    TcpFramedStream& operator=(const TcpFramedStream&) = delete;
    TcpFramedStream(TcpFramedStream&& other) noexcept;
    TcpFramedStream& operator=(TcpFramedStream&& other) noexcept;

    // Connect to remote_addr with optional local bind and timeout (ms).
    // Mirrors `FramedStream::new(remote_addr, local_addr, ms_timeout)`.
    // Returns nullopt on timeout or connection failure. The timeout bounds
    // the connect only (`timeout(ms, connect)` parity), so the returned
    // stream has no socket timeouts.
    static std::optional<TcpFramedStream> connect(
        const ResolvedAddr& remote_addr,
        const ResolvedAddr& local_addr,
        uint64_t timeout_ms);

    // tokio::net::TcpStream::connect(host_port) parity: resolve every address
    // of "host:port" and try them in order until one connects (no local bind,
    // close-on-exec). timeout_ms > 0 bounds the whole attempt like
    // `timeout(ms, TcpStream::connect(..))`, except name resolution itself,
    // which blocks in getaddrinfo. nullopt when nothing connects.
    static std::optional<TcpFramedStream> connect_host(const std::string& host_port,
                                                       uint64_t timeout_ms);

    // Switch to raw mode (disable length-delimited framing). Like upstream's
    // set_raw() this also drops the secretbox key: raw bytes are never sealed.
    void set_raw();

    // Whether a secretbox key is active.
    bool is_secured() const;

    // Send a protobuf message (serializes via SerializeAsString; the Rust
    // side calls Message::write_to_bytes — same wire bytes).
    // Template for any protobuf-generated message type.
    template <typename Msg>
    bool send(const Msg& msg) {
        const std::string bytes = msg.SerializeAsString();
        return send_raw(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }

    // Send raw bytes (length-delimited unless raw mode). An empty payload
    // still goes out as an empty frame, as upstream: an all-default Message
    // serializes to zero bytes, and the server's handshake waits for it.
    bool send_raw(const std::vector<uint8_t>& msg);

    // Send pre-constructed bytes (for already-encoded frames).
    bool send_bytes(const std::vector<uint8_t>& msg);

    // Receive next frame. Returns nullopt on timeout/EOF (peer_closed() tells
    // them apart), or the decoded payload. Bytes past the frame stay buffered
    // for the next call, so frames larger than one recv() and several frames
    // in one recv() both decode (tokio_util Framed parity).
    std::optional<std::vector<uint8_t>> next();

    // Receive with explicit timeout (ms). Overrides the socket receive timeout
    // for this call only and restores the previous value afterwards (including
    // 0 = blocking). A frame cut short by the timeout stays buffered.
    std::optional<std::vector<uint8_t>> next_timeout(uint64_t timeout_ms);

    // Non-blocking next() for poll() loops (no upstream counterpart: tokio's
    // select! polls next() without blocking). Returns a frame when one is
    // complete in the buffer or completes from what the socket holds right
    // now; nullopt otherwise (peer_closed() then tells EOF/error from "not
    // yet"). Throws like next() on a decryption or framing error.
    std::optional<std::vector<uint8_t>> try_next();

    // True once the peer closed the stream or the socket failed. Upstream's
    // `stream.next()` yields None there ("Reset by the peer"), while a
    // timeout is a separate Elapsed error. nullopt alone cannot tell them apart.
    bool peer_closed() const { return peer_closed_; }

    // Install encryption key (parity with `set_key(Key)`).
    // Throws std::invalid_argument unless the key is 32 bytes
    // (secretbox::KEYBYTES; upstream enforces this by type).
    void set_key(const std::vector<uint8_t>& key);

    // Set send+receive timeouts in ms (0 = blocking). Used by multi-step
    // handshakes to bound the whole exchange (upstream `timeout()` parity).
    void set_timeout_ms(int timeout_ms);

    // Get the underlying fd (for advanced use / platform layer).
    int fd() const { return fd_; }

    // Local address of the socket (getsockname). Used to re-bind the same
    // local port across successive rendezvous connections
    // (`socket.get_ref().local_addr()` parity in test_nat_type_).
    // Returns nullopt if the address cannot be determined.
    std::optional<ResolvedAddr> local_addr() const;

    // Remote address of the socket (getpeername; the `addr` half of
    // `listener.accept()`'s (stream, addr)).
    std::optional<ResolvedAddr> peer_addr() const;

private:
    // Encryption state: (key, outbound_seq, inbound_seq), seqs start at 0.
    // Key is 32 bytes (secretbox::KEYBYTES); nonces come from
    // crypto::seq_nonce (24 zero bytes, first 8 = LE seqnum).
    std::optional<std::tuple<std::vector<uint8_t>, uint64_t, uint64_t>> crypto_;

    int fd_ = -1;
    BytesCodec codec_;
    // Persistent read buffer (Framed's BytesMut): bytes received but not yet
    // decoded into a frame.
    std::vector<uint8_t> rbuf_;
    bool peer_closed_ = false;

    // Internal send/receive helpers. recv_flags is 0 (blocking, bounded by
    // SO_RCVTIMEO) or MSG_DONTWAIT (try_next).
    bool send_all(const uint8_t* data, size_t len);
    std::optional<std::vector<uint8_t>> recv_frame(int recv_flags = 0);

    // Per-call receive timeout (SO_RCVTIMEO) save/restore for next_timeout().
    void set_timeout_ms_for_call(int timeout_ms);
    void restore_timeout();
    int saved_rcv_timeout_ms_ = 0;
};

// TCP listener wrapper (parity with `new_listener`).
class TcpListener {
public:
    // Bind to address. If `reuse` is true, uses new_socket with reuse opts.
    static std::optional<TcpListener> bind(const ResolvedAddr& addr, bool reuse = false);

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    // Accept next connection (blocking with timeout). The accepted stream
    // starts with no socket timeouts (Linux would otherwise copy the
    // listener's accept timeout into it).
    std::optional<TcpFramedStream> accept(uint64_t timeout_ms = 0);

    int fd() const { return fd_; }

    // Local address of the listener (getsockname; needed to learn the
    // OS-picked port after binding :0).
    std::optional<ResolvedAddr> local_addr() const;

    ~TcpListener();

private:
    explicit TcpListener(int fd) : fd_(fd) {}
    int fd_ = -1;
};

}  // namespace hbb_common