// test_mpsc.cpp — hbb_common/mpsc.hpp: tokio::sync::mpsc unbounded channel
// semantics (order, sender counting, drop on either side) and the wake
// descriptor the poll() loops rely on.

#include <hbb_common/mpsc.hpp>

#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

using hbb_common::mpsc::unbounded_channel;
using hbb_common::mpsc::UnboundedReceiver;
using hbb_common::mpsc::UnboundedSender;

bool readable(int fd, int timeout_ms) {
    struct pollfd p {
        fd, POLLIN, 0
    };
    return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
}

void test_order_and_try_recv() {
    auto [tx, rx] = unbounded_channel<std::string>();
    CHECK(!rx.try_recv().has_value());
    CHECK(!rx.closed());
    CHECK(tx.send("a"));
    CHECK(tx.send("b"));
    const auto a = rx.try_recv();
    const auto b = rx.try_recv();
    CHECK(a && *a == "a");
    CHECK(b && *b == "b");
    CHECK(!rx.try_recv().has_value());
}

void test_sender_counting() {
    auto [tx, rx] = unbounded_channel<int>();
    {
        UnboundedSender<int> copy = tx;  // a clone is one more sender
        UnboundedSender<int> moved = std::move(copy);  // moving does not add one
        CHECK(moved.send(1));
    }
    CHECK(!rx.closed());  // `tx` is still alive
    {
        UnboundedSender<int> last = std::move(tx);
        CHECK(last.send(2));
    }
    // Every sender is gone, but queued values still come out first.
    CHECK(!rx.closed());
    const auto one = rx.try_recv();
    const auto two = rx.try_recv();
    CHECK(one && *one == 1);
    CHECK(two && *two == 2);
    CHECK(rx.closed());  // now recv() would yield None
    CHECK(!rx.recv(10).has_value());
}

void test_receiver_drop() {
    auto [tx, rx] = unbounded_channel<int>();
    CHECK(tx.send(1));
    { UnboundedReceiver<int> gone = std::move(rx); }
    CHECK(!tx.send(2));  // Err(SendError): the value is lost
    UnboundedSender<int> empty;
    CHECK(!empty.send(3));  // a default sender has no channel
}

void test_wake_fd() {
    auto [tx, rx] = unbounded_channel<int>();
    CHECK(!readable(rx.wake_fd(), 0));
    CHECK(tx.send(7));
    CHECK(readable(rx.wake_fd(), 0));
    rx.clear_wake();
    CHECK(!readable(rx.wake_fd(), 0));
    // The value is still there after clearing: wake is only a hint.
    const auto v = rx.try_recv();
    CHECK(v && *v == 7);
    // The last sender leaving wakes the receiver too.
    { UnboundedSender<int> last = std::move(tx); }
    CHECK(readable(rx.wake_fd(), 0));
    CHECK(rx.closed());
}

void test_blocking_recv() {
    auto [tx, rx] = unbounded_channel<int>();
    std::thread sender([tx = std::move(tx)] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        tx.send(42);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto v = rx.recv(5000);
    CHECK(v && *v == 42);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
    // Blocks until the sender thread exits and drops its sender: None.
    CHECK(!rx.recv(5000).has_value());
    sender.join();
    // Timeout path.
    auto [tx2, rx2] = unbounded_channel<int>();
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(!rx2.recv(50).has_value());
    CHECK(std::chrono::steady_clock::now() - t1 >= std::chrono::milliseconds(45));
    (void)tx2;
}

void test_many_sends_fill_pipe() {
    // More notifications than a pipe holds: send never blocks.
    auto [tx, rx] = unbounded_channel<int>();
    for (int i = 0; i < 200000; ++i) {
        CHECK(tx.send(i));
    }
    int n = 0;
    while (rx.try_recv()) {
        ++n;
    }
    CHECK(n == 200000);
}

}  // namespace

int main() {
    test_order_and_try_recv();
    test_sender_counting();
    test_receiver_drop();
    test_wake_fd();
    test_blocking_recv();
    test_many_sends_fill_pipe();
    if (g_failures == 0) {
        std::puts("test_mpsc: all tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
