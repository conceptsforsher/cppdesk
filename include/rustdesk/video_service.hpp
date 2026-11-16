// video_service.hpp — translation of src/server/video_service.rs.
//
// Step 13 landed the shell (NAME + constructor). Step 15 (part 12) adds the
// state the server connection drives: the per-connection latency and image
// quality maps, the display bookkeeping (CURRENT_DISPLAY, LAST_ACTIVE,
// SWITCH) and get_displays/switch_display/refresh/switch_to_primary.
//
// scrap seam: every display query goes through `Display::all()`, which needs
// libs/scrap (scrap step). Until then it fails the way scrap's X11 backend
// does on a host without an X server: x11::Server::default() errors and
// Display::all() returns io::ErrorKind::ConnectionRefused, whose Display
// text is "connection refused". So get_displays() throws that, a login for
// remote control gets it back as its LoginResponse error (exactly what a
// headless upstream server answers), and switch_display/switch_to_primary
// change nothing.
//
// Deferred with the scrap step: run() (capture + VP9 encode), create_msg,
// create_frame, handle_one_frame, get_current_display and get_quality.

#pragma once

#include <rustdesk/service.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "message.pb.h"

namespace rustdesk::video_service {

inline constexpr char kName[] = "video";

GenericService create();

// (current display index, displays). Throws when Display::all() fails.
std::pair<size_t, std::vector<hbb::DisplayInfo>> get_displays();
void switch_display(int32_t i);
void refresh();  // SWITCH = true: the capture loop restarts and re-announces
void switch_to_primary();

// latency <= 0 removes the connection's entry.
void update_test_latency(int32_t id, int64_t latency);
void update_internal_latency(int32_t id, int64_t latency);
int64_t get_latency();  // max internal latency, 0 when none
void update_image_quality(int32_t id, std::optional<int32_t> q);

// Module-private upstream; exposed for tests.
int32_t convert_quality(int32_t q);
int32_t get_image_quality();  // min over connections, else Balanced converted
size_t current_display();     // *CURRENT_DISPLAY (usize::MAX = "pick primary")
bool switch_pending();        // *SWITCH

}  // namespace rustdesk::video_service
