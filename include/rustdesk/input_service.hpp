// input_service.hpp — translation of src/server/input_service.rs.
//
// Step 13 landed MouseCursorSub (the cursor-id cache) and the two service
// shells. Step 15 (part 12) adds what the server connection calls:
// is_left_up, is_enter, handle_mouse, handle_key (+ KEY_MAP, LATEST_INPUT,
// modifier_sleep, send_sas) and, on Windows, mouse_move_relative.
//
// enigo seam: input is injected through enigo, whose Linux backend is libxdo
// (libs/enigo, enigo step). Until then the Enigo here is enigo's own Linux
// object in the state xdo_new(NULL) leaves it on a host without an X display:
// xdo is null, so every call returns at once and get_key_state() is false.
// Headless hosts therefore behave exactly like upstream; on a desktop host
// events are dropped until the enigo step. Everything around the injection
// (LATEST_INPUT, CtrlAltDel -> SAS over ipc, LockScreen -> lock_screen +
// switch_to_primary, the modifier/CapsLock bookkeeping) is ported as is.
//
// Deferred with their owners: run_pos/run_cursor and the StatePos/
// StateCursor loops (platform cursor capture, input step); the macOS branches
// (main-queue dispatch, flag modifiers) and the Windows ones (NumLock
// juggling, try_change_desktop) arrive with those platform steps.

#pragma once

#include <rustdesk/service.hpp>

#include <cstdint>
#include <unordered_map>
#include <utility>

#include "message.pb.h"

namespace rustdesk::input_service {

inline constexpr char kNameCursor[] = "mouse_cursor";
inline constexpr char kNamePos[] = "mouse_pos";

// MouseCursorSub parity: forwards cursor_data once in full, then id-only
// (lets the client cache cursors by id).
struct MouseCursorSub {
    ConnInner inner;
    std::unordered_map<uint64_t, MessagePtr> cached;

    explicit MouseCursorSub(const ConnInner& conn) : inner(conn) {}
    int get_id() const { return inner.get_id(); }
    void send(const MessagePtr& msg);
};

using MouseCursorService = ServiceTmpl<MouseCursorSub>;

MouseCursorService new_cursor();
GenericService new_pos();

// mask = buttons << 3 | type; a left-button release is buttons 1, type 2.
bool is_left_up(const hbb::MouseEvent& evt);
// control_key Return or NumpadEnter.
bool is_enter(const hbb::KeyEvent& evt);
void handle_mouse(const hbb::MouseEvent& evt, int32_t conn);
void handle_key(const hbb::KeyEvent& evt);
#if defined(_WIN32)
void mouse_move_relative(int32_t x, int32_t y);
#endif

// LATEST_INPUT (conn, time): the connection that last moved the mouse, so
// run_pos skips echoing the position back to it for 300 ms. Upstream keeps it
// in a `static mut`; exposed here for tests.
std::pair<int32_t, int64_t> latest_input();

}  // namespace rustdesk::input_service
