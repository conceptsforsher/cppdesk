// process.hpp — std::process::Command / std::env::current_exe stand-ins.
//
// The C++ standard library has no process API; these mirror what Rust's std
// does on unix for the two Command forms rustdesk uses at this commit:
//
// - spawn(): `Command::new(program).args(args).spawn()`. The child inherits
//   stdin/stdout/stderr and nothing else: every descriptor Rust opens is
//   close-on-exec, and the C++ port opens its sockets and pipes the same way.
//   The child gets the default SIGPIPE disposition and an empty signal mask,
//   then execvp()s. An exec failure travels back over a close-on-exec pipe,
//   so spawn() throws (io::Error) instead of returning a child that exits 127.
//   Returns the child's pid: the caller owns reaping it, like a Child handle
//   (server.rs keeps them in CHILD_PROCESS for check_zombie).
// - output(): `Command::new(program).args(args).output()`. stdin is
//   /dev/null, stdout and stderr are captured, and the call waits for exit.
//
// Errors throw std::system_error (io::Error parity).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rustdesk::process {

// std::process::Output.
struct Output {
    int status = 0;  // raw waitpid() status
    std::string stdout_data;
    std::string stderr_data;

    // ExitStatus::success(): exited normally with code 0.
    bool success() const;
};

intptr_t spawn(const std::string& program, const std::vector<std::string>& args);
Output output(const std::string& program, const std::vector<std::string>& args);

// std::env::current_exe(): the /proc/self/exe link on Linux (a replaced
// binary reads "<path> (deleted)", verbatim like Rust). Throws on failure.
std::string current_exe();

}  // namespace rustdesk::process
