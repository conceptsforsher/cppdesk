// mpsc.hpp — tokio::sync::mpsc::unbounded_channel stand-in, plus the wakeup
// descriptor a poll() loop needs to wait on a channel next to sockets.
//
// Upstream's server Connection and its start_ipc task each sit in one
// tokio::select! over socket reads, channel receives and timers. The C++
// loops wait in poll() instead, so a channel exposes a file descriptor that
// becomes readable when the receiver has something to look at: a value was
// sent, or the last sender went away (recv() would now yield None). WakePipe
// is that descriptor: a non-blocking, close-on-exec self-pipe.
//
// tokio 0.2 semantics kept:
// - UnboundedSender::send never blocks. Once the receiver is dropped it fails
//   (returns false, the Err(SendError) arm) and the value is lost.
// - The receiver sees every value sent before the last sender dropped; then
//   recv() yields None (here: try_recv() is empty and closed() is true).
// - Dropping the receiver drops the queued values.
//
// Wakeup discipline for poll() loops: clear_wake() first, then try_recv()
// until empty, then poll() on wake_fd(). A send that lands after the
// try_recv() leaves a byte in the pipe, so poll() returns at once; no wakeup
// is lost. Spurious wakeups are possible and harmless.

#pragma once

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

namespace hbb_common::mpsc {

class WakePipe {
public:
    WakePipe() {
        int fds[2];
#if defined(__linux__)
        if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe2");
        }
#else
        if (::pipe(fds) != 0) {
            throw std::system_error(errno, std::generic_category(), "pipe");
        }
        for (const int fd : fds) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
#endif
        rd_ = fds[0];
        wr_ = fds[1];
    }
    ~WakePipe() {
        ::close(rd_);
        ::close(wr_);
    }
    WakePipe(const WakePipe&) = delete;
    WakePipe& operator=(const WakePipe&) = delete;

    // Readable while a notification is pending.
    int fd() const { return rd_; }

    void notify() const {
        const char byte = 1;
        ssize_t n;
        do {
            n = ::write(wr_, &byte, 1);
        } while (n < 0 && errno == EINTR);
        // EAGAIN: the pipe is full, so it is readable already.
    }

    void drain() const {
        char buf[256];
        while (true) {
            const ssize_t n = ::read(rd_, buf, sizeof buf);
            if (n > 0 || (n < 0 && errno == EINTR)) {
                continue;
            }
            break;  // EAGAIN: empty
        }
    }

private:
    int rd_ = -1;
    int wr_ = -1;
};

template <class T>
struct Chan {
    std::mutex mutex;
    std::deque<T> queue;
    size_t senders = 0;
    bool receiver_alive = true;
    WakePipe wake;
};

template <class T>
class UnboundedSender {
public:
    UnboundedSender() = default;
    explicit UnboundedSender(std::shared_ptr<Chan<T>> chan) : chan_(std::move(chan)) { acquire(); }
    UnboundedSender(const UnboundedSender& other) : chan_(other.chan_) { acquire(); }
    UnboundedSender(UnboundedSender&& other) noexcept = default;
    UnboundedSender& operator=(const UnboundedSender& other) {
        if (this != &other) {
            release();
            chan_ = other.chan_;
            acquire();
        }
        return *this;
    }
    UnboundedSender& operator=(UnboundedSender&& other) noexcept {
        if (this != &other) {
            release();
            chan_ = std::move(other.chan_);
        }
        return *this;
    }
    ~UnboundedSender() { release(); }

    // `tx.send(v).is_ok()`: false once the receiver is gone (value dropped).
    bool send(T value) const {
        if (!chan_) {
            return false;
        }
        {
            std::lock_guard<std::mutex> l(chan_->mutex);
            if (!chan_->receiver_alive) {
                return false;
            }
            chan_->queue.push_back(std::move(value));
        }
        chan_->wake.notify();
        return true;
    }

private:
    void acquire() {
        if (chan_) {
            std::lock_guard<std::mutex> l(chan_->mutex);
            ++chan_->senders;
        }
    }
    void release() {
        if (!chan_) {
            return;
        }
        bool last = false;
        {
            std::lock_guard<std::mutex> l(chan_->mutex);
            last = --chan_->senders == 0;
        }
        if (last) {
            chan_->wake.notify();  // the receiver's recv() now yields None
        }
        chan_.reset();
    }

    std::shared_ptr<Chan<T>> chan_;
};

template <class T>
class UnboundedReceiver {
public:
    explicit UnboundedReceiver(std::shared_ptr<Chan<T>> chan) : chan_(std::move(chan)) {}
    UnboundedReceiver(UnboundedReceiver&& other) noexcept = default;
    UnboundedReceiver& operator=(UnboundedReceiver&& other) noexcept {
        if (this != &other) {
            close();
            chan_ = std::move(other.chan_);
        }
        return *this;
    }
    UnboundedReceiver(const UnboundedReceiver&) = delete;
    UnboundedReceiver& operator=(const UnboundedReceiver&) = delete;
    ~UnboundedReceiver() { close(); }

    // Non-blocking recv(): the next queued value, if any.
    std::optional<T> try_recv() {
        std::lock_guard<std::mutex> l(chan_->mutex);
        if (chan_->queue.empty()) {
            return std::nullopt;
        }
        std::optional<T> value(std::move(chan_->queue.front()));
        chan_->queue.pop_front();
        return value;
    }

    // recv() would yield None: every sender is gone and nothing is queued.
    bool closed() const {
        std::lock_guard<std::mutex> l(chan_->mutex);
        return chan_->senders == 0 && chan_->queue.empty();
    }

    int wake_fd() const { return chan_->wake.fd(); }
    void clear_wake() const { chan_->wake.drain(); }

    // Blocking recv() (timeout_ms < 0: forever). nullopt on None or timeout.
    std::optional<T> recv(int timeout_ms = -1) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
        while (true) {
            clear_wake();
            if (auto value = try_recv()) {
                return value;
            }
            if (closed()) {
                return std::nullopt;
            }
            int wait = -1;
            if (timeout_ms >= 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now())
                                      .count();
                if (left <= 0) {
                    return std::nullopt;
                }
                wait = static_cast<int>(left);
            }
            struct pollfd p {
                wake_fd(), POLLIN, 0
            };
            ::poll(&p, 1, wait);
        }
    }

private:
    void close() {
        if (!chan_) {
            return;
        }
        std::deque<T> dropped;
        {
            std::lock_guard<std::mutex> l(chan_->mutex);
            chan_->receiver_alive = false;
            dropped.swap(chan_->queue);
        }
        chan_.reset();
    }

    std::shared_ptr<Chan<T>> chan_;
};

// `mpsc::unbounded_channel::<T>()`.
template <class T>
std::pair<UnboundedSender<T>, UnboundedReceiver<T>> unbounded_channel() {
    auto chan = std::make_shared<Chan<T>>();
    return {UnboundedSender<T>(chan), UnboundedReceiver<T>(chan)};
}

}  // namespace hbb_common::mpsc
