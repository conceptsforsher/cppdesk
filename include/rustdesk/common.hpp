// common.hpp — translation of src/common.rs (entry-layer portion, step 08).
//
// What's here: pure helpers (key predicates, time, version, port strings),
// process-global CONTENT/SOFTWARE_UPDATE_URL, username/run_me, and the
// rendezvous/NAT/software-update background tasks (std::thread + blocking
// streams instead of #[tokio::main]).
//
// Deferred with their owner steps (declared nowhere until then):
// - check_clipboard → clipboard step (needs ClipboardContext)
// - update_clipboard(.., Some(old)) → client UI step (ui/remote.rs)
// - resample_channels → audio step (needs dasp resampling)
// - cli.rs Session/start_one_port_forward → client step (needs client::Interface)
// - main.rs argv dispatch → entry step (needs server/ui/platform)

#pragma once

#include <hbb_common/config.hpp>
#include <hbb_common/util.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Generated protobuf (message.proto): KeyEvent oneof predicates below.
#include "message.pb.h"

namespace rustdesk {

inline constexpr char kClipboardName[] = "clipboard";
inline constexpr uint64_t kClipboardIntervalMs = 333;
inline constexpr char kPostfixService[] = "_service";

// CONTENT / SOFTWARE_UPDATE_URL parity (Arc<Mutex<String>> globals).
std::mutex& content_lock();
std::string& shared_content();
std::mutex& update_url_lock();
std::string& software_update_url();

// KeyEvent predicates (control_key()/chr() + union_case() parity).
bool valid_for_numlock(const hbb::KeyEvent& evt);
bool valid_for_capslock(const hbb::KeyEvent& evt);
bool is_control_key(const hbb::KeyEvent& evt, hbb::ControlKey key);
bool is_modifier(const hbb::KeyEvent& evt);

// Misc helpers.
int64_t get_time();  // ms since UNIX epoch
std::string check_port(const std::string& host, int32_t port);
std::string test_if_valid_server(std::string host);
int64_t get_version_number(const std::string& v);
// whoami::username() with trailing NULs trimmed ("fix bug of whoami"): the
// effective user's passwd name, so `sudo cppdesk` is "root".
std::string username();
// run_me parity: `Command::new(current_exe()?).args(args).spawn()`. Returns
// the child's pid (the Child handle: the caller reaps it, e.g. through
// CHILD_PROCESS); throws std::system_error (io::Error) on failure.
intptr_t run_me(const std::vector<std::string>& args);

// update_clipboard(clipboard, None): the server side, CONTENT as `old`.
// Decompresses when flagged and requires UTF-8 (String::from_utf8), then
// opens a ClipboardContext. Clipboard seam: copypasta (clipboard step)
// arrives later; until then ClipboardContext::new() fails like on a host
// without a display, so the content is dropped and CONTENT is left alone
// (upstream logs "Failed to create clipboard context").
void update_clipboard(const hbb::Clipboard& clipboard);

// Rendezvous / NAT / update tasks (each spawns a detached worker thread).
// hbb_common timeouts in ms flow straight through.
void test_nat_type();
void test_rendezvous_server();
void check_software_update();

// Desktop: ask the IPC daemon (ipc::get_rendezvous_server / ipc::get_nat_type,
// with a Config fallback when it is unreachable). Android/iOS: read Config
// directly (upstream's mobile variants).
hbb_common::ResolvedAddr get_rendezvous_server(uint64_t timeout_ms);
int32_t get_nat_type(uint64_t timeout_ms);

}  // namespace rustdesk
