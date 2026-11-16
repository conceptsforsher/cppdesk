// process.cpp — see process.hpp.
#include <rustdesk/process.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <climits>
#include <string>
#include <system_error>
#include <vector>

namespace rustdesk::process {
namespace {

void cloexec_pipe(int fds[2]) {
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
#else
    if (::pipe(fds) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// Child side after fork(): only async-signal-safe calls from here on.
[[noreturn]] void exec_child(const char* program, char* const* argv, int err_fd) {
    ::signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    ::execvp(program, argv);
    const int e = errno;
    ssize_t n;
    do {
        n = ::write(err_fd, &e, sizeof e);
    } while (n < 0 && errno == EINTR);
    ::_exit(1);
}

// Parent side: EOF on the error pipe means exec succeeded. Otherwise reap
// the failed child and throw its errno.
void check_exec(pid_t pid, int& err_rd, const std::string& program) {
    int e = 0;
    ssize_t n;
    do {
        n = ::read(err_rd, &e, sizeof e);
    } while (n < 0 && errno == EINTR);
    close_fd(err_rd);
    if (n == static_cast<ssize_t>(sizeof e)) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        throw std::system_error(e, std::generic_category(), program);
    }
}

std::vector<char*> make_argv(const std::string& program, std::vector<std::string>& owned) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (auto& a : owned) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    return argv;
}

}  // namespace

bool Output::success() const {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

intptr_t spawn(const std::string& program, const std::vector<std::string>& args) {
    std::vector<std::string> owned = args;
    std::vector<char*> argv = make_argv(program, owned);  // built before fork()
    int err[2];
    cloexec_pipe(err);
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(err[0]);
        ::close(err[1]);
        throw std::system_error(e, std::generic_category(), "fork");
    }
    if (pid == 0) {
        ::close(err[0]);
        exec_child(program.c_str(), argv.data(), err[1]);
    }
    ::close(err[1]);
    check_exec(pid, err[0], program);
    return static_cast<intptr_t>(pid);
}

Output output(const std::string& program, const std::vector<std::string>& args) {
    std::vector<std::string> owned = args;
    std::vector<char*> argv = make_argv(program, owned);
    int out[2];
    int errp[2];
    int exec_err[2];
    cloexec_pipe(out);
    try {
        cloexec_pipe(errp);
    } catch (...) {
        ::close(out[0]);
        ::close(out[1]);
        throw;
    }
    try {
        cloexec_pipe(exec_err);
    } catch (...) {
        ::close(out[0]);
        ::close(out[1]);
        ::close(errp[0]);
        ::close(errp[1]);
        throw;
    }
#if defined(O_CLOEXEC)
    int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
#else
    int null_fd = ::open("/dev/null", O_RDONLY);
#endif
    const pid_t pid = null_fd < 0 ? -1 : ::fork();
    if (pid < 0) {
        const int e = errno;
        for (int fd : {out[0], out[1], errp[0], errp[1], exec_err[0], exec_err[1], null_fd}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        throw std::system_error(e, std::generic_category(), null_fd < 0 ? "/dev/null" : "fork");
    }
    if (pid == 0) {
        // dup2() clears close-on-exec on the targets only.
        ::dup2(null_fd, 0);
        ::dup2(out[1], 1);
        ::dup2(errp[1], 2);
        exec_child(program.c_str(), argv.data(), exec_err[1]);
    }
    ::close(null_fd);
    ::close(out[1]);
    ::close(errp[1]);
    ::close(exec_err[1]);
    int out_rd = out[0];
    int err_rd = errp[0];
    try {
        check_exec(pid, exec_err[0], program);
    } catch (...) {
        close_fd(out_rd);
        close_fd(err_rd);
        throw;
    }
    // read2() parity: drain both pipes together so neither can fill and stall
    // the child.
    Output result;
    while (out_rd >= 0 || err_rd >= 0) {
        struct pollfd fds[2];
        nfds_t n = 0;
        if (out_rd >= 0) {
            fds[n++] = {out_rd, POLLIN, 0};
        }
        if (err_rd >= 0) {
            fds[n++] = {err_rd, POLLIN, 0};
        }
        if (::poll(fds, n, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (nfds_t i = 0; i < n; ++i) {
            if (fds[i].revents == 0) {
                continue;
            }
            char buf[4096];
            const ssize_t r = ::read(fds[i].fd, buf, sizeof buf);
            if (r < 0 && errno == EINTR) {
                continue;
            }
            const bool is_out = fds[i].fd == out_rd;
            if (r <= 0) {
                close_fd(is_out ? out_rd : err_rd);
                continue;
            }
            (is_out ? result.stdout_data : result.stderr_data).append(buf, static_cast<size_t>(r));
        }
    }
    close_fd(out_rd);
    close_fd(err_rd);
    while (::waitpid(pid, &result.status, 0) < 0 && errno == EINTR) {
    }
    return result;
}

std::string current_exe() {
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (::_NSGetExecutablePath(buf, &size) != 0) {
        throw std::system_error(ENAMETOOLONG, std::generic_category(), "current_exe");
    }
    return buf;
#else
    char buf[PATH_MAX + 1];
    const ssize_t n = ::readlink("/proc/self/exe", buf, PATH_MAX);
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), "readlink /proc/self/exe");
    }
    return std::string(buf, static_cast<size_t>(n));
#endif
}

}  // namespace rustdesk::process
