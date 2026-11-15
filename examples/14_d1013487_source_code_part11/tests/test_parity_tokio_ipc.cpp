// test_parity_tokio_ipc.cpp — libs/parity-tokio-ipc (lib.rs #[cfg(test)]
// smoke_test parity + the vendored unix.rs behavior rustdesk relies on).
//
// Upstream's smoke_test cannot pass against the vendored unix.rs as shipped:
// it builds its endpoint with SecurityAttributes::empty().set_mode(0o777),
// and incoming() chmods BEFORE bind, so on a fresh path the chmod fails
// (ENOENT) and `.expect("failed to open up a new socket")` panics. Its final
// assertion (socket file gone after the server drops) also contradicts the
// crate, which has no Drop impl that unlinks. Both behaviors are pinned below
// as the shipped semantics, and the echo flow itself runs with empty()
// attributes.

#include <parity_tokio_ipc/parity_tokio_ipc.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

using parity_tokio_ipc::Connection;
using parity_tokio_ipc::Endpoint;
using parity_tokio_ipc::Incoming;
using parity_tokio_ipc::SecurityAttributes;

bool read_exact(Connection& c, uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        const size_t n = c.read(buf + off, len - off);
        if (n == 0) {
            return false;
        }
        off += n;
    }
    return true;
}

mode_t file_mode(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return st.st_mode & 07777;
}

void test_dummy_endpoint() {
    const std::string a = parity_tokio_ipc::dummy_endpoint();
    const std::string b = parity_tokio_ipc::dummy_endpoint();
    const std::string prefix = "/tmp/my-uds-";
    CHECK(a.rfind(prefix, 0) == 0);
    CHECK(a.size() > prefix.size());
    bool digits = true;
    for (size_t i = prefix.size(); i < a.size(); ++i) {
        digits = digits && a[i] >= '0' && a[i] <= '9';
    }
    CHECK(digits);
    CHECK(a != b);
}

void test_security_attributes() {
    CHECK(!SecurityAttributes::empty().mode().has_value());
    CHECK(!SecurityAttributes::allow_everyone_create().mode().has_value());
    CHECK(SecurityAttributes::empty().allow_everyone_connect().mode() == 0777);
    CHECK(SecurityAttributes::empty().set_mode(0600).mode() == 0600);

    // apply_permissions chmods an existing path when a mode is set...
    const std::string path = parity_tokio_ipc::dummy_endpoint() + ".perm";
    { FILE* f = std::fopen(path.c_str(), "w"); if (f) std::fclose(f); }
    ::chmod(path.c_str(), 0600);
    SecurityAttributes::empty().set_mode(0640).apply_permissions(path);
    CHECK(file_mode(path) == 0640);
    // ...is a no-op without a mode, and fails on a missing path with one.
    SecurityAttributes::empty().apply_permissions(path);
    CHECK(file_mode(path) == 0640);
    std::filesystem::remove(path);
    bool threw = false;
    try {
        SecurityAttributes::empty().set_mode(0777).apply_permissions(path);
    } catch (const std::system_error&) {
        threw = true;
    }
    CHECK(threw);
    // CString::new parity: an interior NUL is rejected even without a mode.
    threw = false;
    try {
        SecurityAttributes::empty().apply_permissions(std::string("/tmp/a\0b", 8));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// The upstream smoke_test's server: echo 5 bytes per client.
void run_server(Incoming& incoming, int clients) {
    for (int i = 0; i < clients; ++i) {
        Connection stream = incoming.next();
        uint8_t buf[5];
        if (!read_exact(stream, buf, sizeof buf)) {
            ++g_failures;
            std::fprintf(stderr, "FAIL: unable to read from socket\n");
            return;
        }
        stream.write_all(buf, sizeof buf);
    }
}

void test_smoke() {
    const std::string path = parity_tokio_ipc::dummy_endpoint();
    Endpoint endpoint(path);
    endpoint.set_security_attributes(SecurityAttributes::empty());
    CHECK(endpoint.path() == path);
    Incoming incoming = endpoint.incoming();
    CHECK(std::filesystem::exists(path));  // bind created the socket file
    std::thread server([&] {
        try {
            run_server(incoming, 2);
        } catch (const std::exception& e) {
            ++g_failures;
            std::fprintf(stderr, "FAIL: server: %s\n", e.what());
        }
    });

    Connection client_0 = Endpoint::connect(path);
    Connection client_1 = Endpoint::connect(path, 1000);
    const uint8_t msg[] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t rx_buf[5] = {};
    client_0.write_all(msg, sizeof msg);
    CHECK(read_exact(client_0, rx_buf, sizeof rx_buf));
    uint8_t rx_buf2[5] = {};
    client_1.write_all(msg, sizeof msg);
    CHECK(read_exact(client_1, rx_buf2, sizeof rx_buf2));
    CHECK(std::memcmp(rx_buf, msg, sizeof msg) == 0);
    CHECK(std::memcmp(rx_buf2, msg, sizeof msg) == 0);
    server.join();

    // Shut the server down (drop Incoming). The vendored crate never unlinks:
    // the socket file outlives the listener (upstream's last assert expects
    // the opposite; see the file comment).
    { Incoming dropped = std::move(incoming); }
    CHECK(std::filesystem::exists(path));
    bool refused = false;
    try {
        (void)Endpoint::connect(path, 200);
    } catch (const std::system_error&) {
        refused = true;  // nobody listens on the stale file
    }
    CHECK(refused);
    std::filesystem::remove(path);
}

void test_mode_before_bind_quirk() {
    // set_mode on a fresh path: apply_permissions runs before bind -> ENOENT.
    const std::string path = parity_tokio_ipc::dummy_endpoint();
    Endpoint endpoint(path);
    endpoint.set_security_attributes(SecurityAttributes::empty().set_mode(0777));
    bool threw = false;
    try {
        (void)endpoint.incoming();
    } catch (const std::system_error& e) {
        threw = e.code() == std::errc::no_such_file_or_directory;
    }
    CHECK(threw);
    CHECK(!std::filesystem::exists(path));
    // allow_everyone_create (what rustdesk uses) binds fine.
    Endpoint ok(path);
    ok.set_security_attributes(SecurityAttributes::allow_everyone_create());
    Incoming incoming = ok.incoming();
    CHECK(std::filesystem::exists(path));
    // A second bind on a live path fails (EADDRINUSE).
    bool in_use = false;
    try {
        Endpoint again(path);
        (void)again.incoming();
    } catch (const std::system_error& e) {
        in_use = e.code() == std::errc::address_in_use;
    }
    CHECK(in_use);
    std::filesystem::remove(path);
}

void test_connect_errors() {
    bool threw = false;
    try {
        (void)Endpoint::connect("/tmp/definitely-missing-" + std::to_string(::getpid()));
    } catch (const std::system_error& e) {
        threw = e.code() == std::errc::no_such_file_or_directory;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)Endpoint::connect("/tmp/" + std::string(200, 'x'));  // > sun_path
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_eof_and_shutdown() {
    const std::string path = parity_tokio_ipc::dummy_endpoint();
    Endpoint endpoint(path);
    Incoming incoming = endpoint.incoming();
    Connection client = Endpoint::connect(path);
    Connection server = incoming.next();
    client.shutdown();  // write half closed: the server reads EOF
    uint8_t b = 0;
    CHECK(server.read(&b, 1) == 0);
    CHECK(server.wait_readable(0));  // EOF counts as readable
    const uint8_t x = 7;
    server.write_all(&x, 1);  // the other direction still works
    CHECK(client.read(&b, 1) == 1 && b == 7);
    CHECK(!client.wait_readable(50));  // nothing pending
    {
        Connection gone = std::move(server);  // close the server side
    }
    // Peer fully closed: write_all fails with EPIPE instead of SIGPIPE.
    bool threw = false;
    try {
        for (int i = 0; i < 64; ++i) {
            const std::vector<uint8_t> chunk(65536, 1);
            client.write_all(chunk.data(), chunk.size());
        }
    } catch (const std::system_error&) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    try {
        test_dummy_endpoint();
        test_security_attributes();
        test_smoke();
        test_mode_before_bind_quirk();
        test_connect_errors();
        test_eof_and_shutdown();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        return 1;
    }
    if (g_failures == 0) {
        std::puts("test_parity_tokio_ipc: all tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
