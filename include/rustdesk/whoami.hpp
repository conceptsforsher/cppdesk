// whoami.hpp — the whoami 0.9.0 crate surface rustdesk uses at this commit
// (Cargo.lock pins whoami 0.9.0).
//
// - username(): getpwuid_r(geteuid()) -> pw_name (unix). The effective user,
//   so `sudo cppdesk` reports "root". Rust reads the passwd struct even when
//   no entry exists (undefined behavior); here that case returns "".
// - hostname(): gethostname() into a 256-byte buffer, length by strlen.
// - platform(): `Platform` Display text. whoami 0.9 maps macOS to "Mac OS",
//   Windows to "Windows", and every other unix (BSDs included) to "Linux".
// Non-UTF-8 bytes are replaced like String::from_utf8_lossy (string_from_os).

#pragma once

#include <string>

namespace rustdesk::whoami {

std::string username();
std::string hostname();
std::string platform();

}  // namespace rustdesk::whoami
