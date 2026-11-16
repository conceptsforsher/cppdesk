// linux.cpp — src/platform/linux.rs, part 1 (see platform.hpp).
#include <rustdesk/platform.hpp>

#include <rustdesk/common.hpp>
#include <rustdesk/input_service.hpp>
#include <rustdesk/process.hpp>

#include <hbb_common/json.hpp>

#include <string>
#include <thread>
#include <vector>

namespace rustdesk::platform {
namespace {

bool is_rust_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\x0C' || c == '\r';
}

// str::split_whitespace().
std::vector<std::string> split_whitespace(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_rust_whitespace(s[i])) {
            ++i;
        }
        const size_t start = i;
        while (i < s.size() && !is_rust_whitespace(s[i])) {
            ++i;
        }
        if (i > start) {
            out.push_back(s.substr(start, i - start));
        }
    }
    return out;
}

// str::lines(): split on '\n', drop one trailing '\r' per line, no final
// empty line.
std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        const bool last = end == std::string::npos;
        if (last) {
            end = s.size();
        }
        std::string line = s.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(std::move(line));
        if (last) {
            break;
        }
        start = end + 1;
    }
    return out;
}

// `Command::new(program).args(args).output()` -> Ok(stdout, lossy) or Err.
std::optional<std::string> command_stdout(const std::string& program,
                                          const std::vector<std::string>& args) {
    try {
        const process::Output out = process::output(program, args);
        return hbb_common::json::from_utf8_lossy(out.stdout_data);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::string get_active_userid() {
    return get_value_of_seat0(1);
}

bool is_active(const std::string& sid) {
    if (const auto out = command_stdout("loginctl", {"show-session", "-p", "State", sid})) {
        // Substring test, verbatim: "State=inactive" contains "active" too.
        return out->find("active") != std::string::npos;
    }
    return false;
}

std::string get_value_of_seat0(size_t i) {
    if (const auto out = command_stdout("loginctl", {})) {
        for (const std::string& line : lines(*out)) {
            if (line.find("seat0") != std::string::npos) {
                const auto fields = split_whitespace(line);
                if (!fields.empty()) {
                    if (is_active(fields[0])) {
                        if (i < fields.size()) {
                            return fields[i];
                        }
                    }
                }
            }
        }
    }
    return "";
}

std::string get_display_server() {
    const std::string session = get_value_of_seat0(0);
    if (auto out = command_stdout("loginctl", {"show-session", "-p", "Type", session})) {
        std::string s = *out;
        // .replace("Type=", "") — every occurrence.
        size_t pos = 0;
        while ((pos = s.find("Type=", pos)) != std::string::npos) {
            s.erase(pos, 5);
        }
        while (!s.empty() && is_rust_whitespace(s.back())) {
            s.pop_back();  // trim_end()
        }
        return s;
    }
    return "";
}

std::string get_active_username() {
    return get_value_of_seat0(2);
}

bool is_prelogin() {
    const size_t n = get_active_userid().size();
    return n < 4 && n > 1;
}

bool is_root() {
    return rustdesk::username() == "root";
}

std::optional<intptr_t> run_as_user(const std::string& arg) {
    const std::string uid = get_active_userid();
    const std::string cmd = process::current_exe();  // `?`: throws
    // cmd.to_str().unwrap_or(""): a non-UTF-8 path becomes "".
    const std::string exe = hbb_common::json::is_valid_utf8(cmd) ? cmd : "";
    const intptr_t task = process::spawn(
        "sudo", {"XDG_RUNTIME_DIR=/run/user/" + uid, "-u", get_active_username(), exe, arg});
    return task;
}

void lock_screen() {
    std::thread([] {
        hbb::KeyEvent evt;
        evt.set_down(true);
        evt.add_modifiers(hbb::Meta);
        evt.set_chr('l');
        input_service::handle_key(evt);
        evt.set_down(false);
        input_service::handle_key(evt);
    }).detach();
}

void toggle_privacy_mode(bool /*v*/) {
    // https://unix.stackexchange.com/questions/17170/disable-keyboard-mouse-input-on-unix-under-x
}

void block_input(bool /*v*/) {
    //
}

bool is_installed() {
    return true;
}

}  // namespace rustdesk::platform
