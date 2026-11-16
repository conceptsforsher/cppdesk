// platform.hpp — translation of src/platform/mod.rs + linux.rs, part 1
// (step 15, part 12 of d1013487).
//
// mod.rs re-exports the per-OS module (`pub use linux::*`), so callers write
// platform::is_root() and the like. Part 1 ports the Linux functions the
// server connection calls: the seat0 session queries (loginctl), is_root,
// run_as_user (sudo), lock_screen and the privacy/block-input switches
// (empty on Linux upstream too). src/platform/linux.cpp holds them and is
// compiled on Linux only; macOS and Windows arrive with the platform step,
// and until then a non-Linux build fails to link instead of misbehaving.
//
// Deferred with their owners:
// - get_cursor_pos/get_cursor/get_cursor_data/reset_input_cache (libxdo,
//   Xfixes) -> input step, with the cursor services that call them.
// - start_os_service, get_cm, get_display (the --service supervisor) ->
//   entry step.
// - is_login_wayland/fix_login_wayland -> ui step.
// - get_pa_monitor/get_pa_source_name/get_pa_sources (pulsectl) -> audio step.
//
// Mapping notes:
// - `std::process::Command` goes through process.hpp. A failed spawn is the
//   `Err` arm, which these functions turn into "" / false like upstream.
// - String::from_utf8_lossy, str::lines and str::split_whitespace keep their
//   Rust meaning (whitespace is the ASCII set; loginctl prints ASCII).
// - linux.rs's `pub fn get_display_server` is reached upstream as
//   `crate::platform::linux::get_display_server()`; it sits directly in
//   rustdesk::platform here (a namespace named `linux` collides with the
//   GNU `linux` macro).

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rustdesk::platform {

// SERVICE_INTERVAL (mod.rs), the --service supervisor's poll period in ms.
inline constexpr uint64_t kServiceIntervalMs = 300;

// --- linux.rs -----------------------------------------------------------------

// `loginctl show-session -p Type <seat0 session>` with "Type=" removed and
// trailing whitespace trimmed ("x11", "wayland", ... or "").
std::string get_display_server();
std::string get_active_username();  // seat0 column 3 ("" when none is active)
bool is_prelogin();                 // 1 < len(seat0 uid) < 4 (gdm's low uids)
bool is_root();                     // crate::username() == "root"
// `sudo XDG_RUNTIME_DIR=/run/user/<uid> -u <user> <current_exe> <arg>`,
// spawned. Returns Some(child pid) (Linux never returns None); throws when
// current_exe or the spawn fails.
std::optional<intptr_t> run_as_user(const std::string& arg);
void lock_screen();                 // Super+L through input_service::handle_key, on a thread
void toggle_privacy_mode(bool v);   // no-op on Linux (upstream leaves a link only)
void block_input(bool v);           // no-op on Linux
bool is_installed();                // true on Linux

// Module-private in linux.rs; exposed for tests (C++ has no module privacy).
std::string get_active_userid();    // seat0 column 2
bool is_active(const std::string& sid);
std::string get_value_of_seat0(size_t i);

}  // namespace rustdesk::platform
