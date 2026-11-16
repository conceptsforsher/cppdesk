// test_platform.cpp — step 15: the std::process / whoami stand-ins
// (process.hpp, whoami.hpp), run_me, and platform/linux.rs part 1.
//
// The seat0 session queries shell out to `loginctl`; the test puts a fake
// `loginctl` (and a fake `sudo` for run_as_user) first on PATH, driven by
// files in a temp dir, so every case is hermetic and the real session table
// of the host never matters.

#include <rustdesk/common.hpp>
#include <rustdesk/platform.hpp>
#include <rustdesk/process.hpp>
#include <rustdesk/whoami.hpp>

#include <hbb_common/tcp.hpp>
#include <hbb_common/util.hpp>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<int> g_failures{0};
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

namespace process = rustdesk::process;
namespace platform = rustdesk::platform;

fs::path g_dir;  // fake-tool directory (also holds their data files)

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_script(const fs::path& p, const std::string& body) {
    write_file(p, body);
    ::chmod(p.c_str(), 0755);
}

int wait_exit_code(intptr_t pid) {
    int status = 0;
    while (::waitpid(static_cast<pid_t>(pid), &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Fake loginctl: no args prints $DIR/list; `show-session -p PROP SID` prints
// $DIR/SID.PROP when it exists.
void install_fakes() {
    write_script(g_dir / "loginctl",
                 "#!/bin/sh\n"
                 "d=\"$CPPDESK_FAKE_DIR\"\n"
                 "if [ \"$#\" -eq 0 ]; then cat \"$d/list\"; exit 0; fi\n"
                 "[ -f \"$d/$4.$3\" ] && cat \"$d/$4.$3\"\n"
                 "exit 0\n");
    write_script(g_dir / "sudo",
                 "#!/bin/sh\n"
                 "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > \"$CPPDESK_FAKE_DIR/sudo_args\"\n");
}

void set_sessions(const std::string& list) {
    for (const auto& entry : fs::directory_iterator(g_dir)) {
        const std::string name = entry.path().filename().string();
        if (name.find(".State") != std::string::npos || name.find(".Type") != std::string::npos) {
            fs::remove(entry.path());
        }
    }
    write_file(g_dir / "list", list);
}

void test_output() {
    const auto out = process::output("/bin/sh", {"-c", "printf out; printf err >&2; exit 3"});
    CHECK(out.stdout_data == "out");
    CHECK(out.stderr_data == "err");
    CHECK(!out.success());
    CHECK(WIFEXITED(out.status) && WEXITSTATUS(out.status) == 3);
    CHECK(process::output("/bin/sh", {"-c", "exit 0"}).success());
    // stdin is /dev/null: `cat` sees EOF at once.
    const auto cat = process::output("/bin/sh", {"-c", "cat"});
    CHECK(cat.success() && cat.stdout_data.empty());
    // Both pipes drain together: no deadlock past the pipe buffer size.
    const auto big = process::output(
        "/bin/sh", {"-c", "head -c 200000 /dev/zero; head -c 300000 /dev/zero >&2"});
    CHECK(big.stdout_data.size() == 200000);
    CHECK(big.stderr_data.size() == 300000);
    // PATH lookup (execvp), like Command::new("loginctl").
    CHECK(process::output("true", {}).success());
    // Exec failure is an error from output() itself (io::Error), not a
    // child exiting 127.
    int err = 0;
    try {
        process::output("/nonexistent/cppdesk-no-such-program", {});
    } catch (const std::system_error& e) {
        err = e.code().value();
    }
    CHECK(err == ENOENT);
}

void test_spawn() {
    const intptr_t pid = process::spawn("/bin/sh", {"-c", "exit 7"});
    CHECK(pid > 0);
    CHECK(wait_exit_code(pid) == 7);
    int err = 0;
    try {
        process::spawn("cppdesk-no-such-program", {});
    } catch (const std::system_error& e) {
        err = e.code().value();
    }
    CHECK(err == ENOENT);
    // Close-on-exec sockets stay with the parent (the connection manager
    // must not inherit live connections).
    auto listener =
        hbb_common::TcpListener::bind(hbb_common::to_socket_addr("127.0.0.1:0"), true);
    CHECK(listener.has_value());
    if (listener) {
        const std::string probe = "test -e /proc/self/fd/" + std::to_string(listener->fd());
        const intptr_t child = process::spawn("/bin/sh", {"-c", probe});
        CHECK(wait_exit_code(child) == 1);  // not there
    }
}

void test_current_exe_and_run_me() {
    const std::string exe = process::current_exe();
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    CHECK(n > 0 && exe == std::string(buf, static_cast<size_t>(n)));
    CHECK(fs::path(exe).filename() == "test_platform");
    // run_me: Command::new(current_exe()).args(..).spawn(); the caller reaps.
    const intptr_t pid = rustdesk::run_me({"--child-exit", "5"});
    CHECK(pid > 0);
    CHECK(wait_exit_code(pid) == 5);
}

void test_whoami() {
    struct passwd* pw = ::getpwuid(::geteuid());
    CHECK(pw != nullptr);
    if (pw) {
        CHECK(rustdesk::whoami::username() == pw->pw_name);
    }
    char host[256] = {};
    ::gethostname(host, 255);
    CHECK(rustdesk::whoami::hostname() == host);
    CHECK(!rustdesk::whoami::hostname().empty());
    CHECK(rustdesk::whoami::platform() == "Linux");
    // crate::username() is whoami::username() with trailing NULs trimmed.
    CHECK(rustdesk::username() == rustdesk::whoami::username());
    CHECK(platform::is_root() == (rustdesk::username() == "root"));
}

void test_seat0() {
    set_sessions(
        "SESSION  UID USER    SEAT  TTY\n"
        "      5 1001 bob           pts/0\n"
        "      2 1000 alice   seat0 tty2\n"
        "\n"
        "2 sessions listed.\n");
    write_file(g_dir / "2.State", "State=active\n");
    write_file(g_dir / "2.Type", "Type=x11\n");
    CHECK(platform::get_value_of_seat0(0) == "2");
    CHECK(platform::get_active_userid() == "1000");
    CHECK(platform::get_active_username() == "alice");
    CHECK(platform::get_value_of_seat0(9).empty());  // no such column
    CHECK(platform::is_active("2"));
    CHECK(!platform::is_active("5"));  // no State file: empty output
    CHECK(platform::get_display_server() == "x11");
    CHECK(!platform::is_prelogin());  // uid "1000": 4 chars

    // The greeter (gdm runs with a low uid): 1 < len < 4 is pre-login.
    set_sessions("     c1   42 gdm     seat0 tty1\n");
    write_file(g_dir / "c1.State", "State=active\n");
    CHECK(platform::get_active_username() == "gdm");
    CHECK(platform::is_prelogin());
    set_sessions("     c1    7 x       seat0 tty1\n");
    write_file(g_dir / "c1.State", "State=active\n");
    CHECK(!platform::is_prelogin());  // len 1

    // Verbatim substring test: "inactive" contains "active".
    set_sessions("      3 1002 carol   seat0 tty3\n");
    write_file(g_dir / "3.State", "State=inactive\n");
    CHECK(platform::get_active_username() == "carol");
    write_file(g_dir / "3.State", "State=closing\n");
    CHECK(platform::get_active_username().empty());

    // CRLF lines, and "Type=" removed everywhere before trim_end.
    set_sessions("      4 1003 dave    seat0 tty4\r\n");
    write_file(g_dir / "4.State", "State=active\r\n");
    write_file(g_dir / "4.Type", "Type=wayland \r\n");
    CHECK(platform::get_active_username() == "dave");
    CHECK(platform::get_display_server() == "wayland");
    write_file(g_dir / "4.Type", "Type=Type=x11\n");
    CHECK(platform::get_display_server() == "x11");

    // No seat0 session: every query is empty (the CI runner's real case).
    set_sessions("No sessions.\n");
    CHECK(platform::get_active_userid().empty());
    CHECK(platform::get_display_server().empty());
    CHECK(!platform::is_prelogin());
}

void test_no_loginctl() {
    // Command::new("loginctl").output() failing (not installed) is the Err
    // arm: "" / false.
    const char* old_path = std::getenv("PATH");
    const std::string saved = old_path ? old_path : "";
    ::setenv("PATH", "/nonexistent-cppdesk-path", 1);
    CHECK(platform::get_active_username().empty());
    CHECK(platform::get_display_server().empty());
    CHECK(!platform::is_active("1"));
    ::setenv("PATH", saved.c_str(), 1);
}

void test_run_as_user() {
    set_sessions("      2 1000 alice   seat0 tty2\n");
    write_file(g_dir / "2.State", "State=active\n");
    fs::remove(g_dir / "sudo_args");
    const auto task = platform::run_as_user("--cm");
    CHECK(task.has_value());
    if (task) {
        CHECK(wait_exit_code(*task) == 0);
    }
    const std::string args = read_file(g_dir / "sudo_args");
    CHECK(args == "XDG_RUNTIME_DIR=/run/user/1000\n-u\nalice\n" + process::current_exe() +
                      "\n--cm\n");
}

void test_misc() {
    CHECK(platform::is_installed());
    platform::toggle_privacy_mode(true);  // no-ops on Linux
    platform::block_input(true);
    platform::lock_screen();  // Super+L through the (null) enigo, on a thread
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(platform::kServiceIntervalMs == 300);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--child-exit") == 0) {
        return std::atoi(argv[2]);  // run_me target
    }
    char tmpl[] = "/tmp/cppdesk_test_platform_XXXXXX";
    if (!::mkdtemp(tmpl)) {
        std::perror("mkdtemp");
        return 1;
    }
    g_dir = tmpl;
    install_fakes();
    ::setenv("CPPDESK_FAKE_DIR", g_dir.c_str(), 1);
    const char* path = std::getenv("PATH");
    const std::string new_path = g_dir.string() + ":" + (path ? path : "/usr/bin:/bin");
    ::setenv("PATH", new_path.c_str(), 1);

    test_output();
    test_spawn();
    test_current_exe_and_run_me();
    test_whoami();
    test_seat0();
    test_no_loginctl();
    test_run_as_user();
    test_misc();

    std::error_code ec;
    fs::remove_all(g_dir, ec);
    if (g_failures == 0) {
        std::puts("test_platform: all tests passed");
    }
    std::fflush(nullptr);
    std::_Exit(g_failures == 0 ? 0 : 1);  // detached helper threads may still run
}
