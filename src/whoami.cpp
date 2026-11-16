// whoami.cpp — see whoami.hpp.
#include <rustdesk/whoami.hpp>

#include <hbb_common/json.hpp>

#if defined(_WIN32)
#error "whoami: the Windows side arrives with the Windows platform step"
#endif

#include <pwd.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace rustdesk::whoami {

std::string username() {
    std::vector<char> buffer(16384);  // BUF_SIZE "from the man page"
    struct passwd pwd {};
    struct passwd* result = nullptr;
    ::getpwuid_r(::geteuid(), &pwd, buffer.data(), buffer.size(), &result);
    if (!result || !result->pw_name) {
        return "";
    }
    return hbb_common::json::from_utf8_lossy(result->pw_name);
}

std::string hostname() {
    char buf[256] = {};
    ::gethostname(buf, 255);
    return hbb_common::json::from_utf8_lossy(std::string(buf, ::strnlen(buf, 255)));
}

std::string platform() {
#if defined(__APPLE__)
    return "Mac OS";
#else
    return "Linux";
#endif
}

}  // namespace rustdesk::whoami
