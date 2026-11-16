// input_service.cpp — see input_service.hpp.
#include <rustdesk/input_service.hpp>

#include <rustdesk/common.hpp>
#include <rustdesk/ipc.hpp>
#include <rustdesk/platform.hpp>
#include <rustdesk/video_service.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rustdesk::input_service {
namespace {

// --- enigo (libs/enigo/src/lib.rs + linux.rs surface) -------------------------
// See the header: xdo stays null until the enigo step, the upstream state on
// a host without an X display. Each method keeps upstream's null check, which
// is where the libxdo calls go.
namespace enigo {

enum class Kind {
    Alt, Backspace, CapsLock, Control, Delete, DownArrow, End, Escape,
    F1, F10, F11, F12, F2, F3, F4, F5, F6, F7, F8, F9,
    Home, LeftArrow, Meta, Option, PageDown, PageUp, Return, RightArrow, Shift, Space, Tab,
    UpArrow, Numpad0, Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8,
    Numpad9, Cancel, Clear, Menu, Pause, Kana, Hangul, Junja, Final, Hanja, Kanji, Convert,
    Select, Print, Execute, Snapshot, Insert, Help, Sleep, Separator, Scroll, NumLock, RWin,
    Apps, Multiply, Add, Subtract, Decimal, Divide, Equals, NumpadEnter,
    Layout,  // Layout(char)
    Raw,     // Raw(u16)
};

// enigo::Key: a named key, or Layout/Raw with their payload.
struct Key {
    Kind kind;
    uint32_t payload = 0;
    bool operator==(const Key& o) const { return kind == o.kind && payload == o.payload; }
    bool operator!=(const Key& o) const { return !(*this == o); }
};

enum class MouseButton { Left, Middle, Right, ScrollUp, ScrollDown, ScrollLeft, ScrollRight };

class Enigo {
public:
    void mouse_move_to(int32_t /*x*/, int32_t /*y*/) {
        if (!xdo_) {
            return;
        }
    }
    void mouse_move_relative(int32_t /*x*/, int32_t /*y*/) {
        if (!xdo_) {
            return;
        }
    }
    // ResultType: Ok(()) on the null path.
    void mouse_down(MouseButton /*button*/) {
        if (!xdo_) {
            return;
        }
    }
    void mouse_up(MouseButton /*button*/) {
        if (!xdo_) {
            return;
        }
    }
    void mouse_click(MouseButton /*button*/) {
        if (!xdo_) {
            return;
        }
    }
    void mouse_scroll_x(int32_t length) {
        const MouseButton button = length < 0 ? MouseButton::ScrollLeft : MouseButton::ScrollRight;
        if (length < 0) {
            length = -length;
        }
        for (int32_t i = 0; i < length; ++i) {
            mouse_click(button);
        }
    }
    void mouse_scroll_y(int32_t length) {
        const MouseButton button = length < 0 ? MouseButton::ScrollUp : MouseButton::ScrollDown;
        if (length < 0) {
            length = -length;
        }
        for (int32_t i = 0; i < length; ++i) {
            mouse_click(button);
        }
    }
    bool get_key_state(Key /*key*/) {
        if (!xdo_) {
            return false;
        }
        return false;
    }
    void key_sequence(const std::string& /*sequence*/) {
        if (!xdo_) {
            return;
        }
    }
    void key_down(Key /*key*/) {
        if (!xdo_) {
            return;
        }
    }
    void key_up(Key /*key*/) {
        if (!xdo_) {
            return;
        }
    }

private:
    const void* xdo_ = nullptr;  // xdo_new(ptr::null()) -> null without a display
};

}  // namespace enigo

// ENIGO: Arc<Mutex<Enigo>>.
std::mutex& enigo_lock() {
    static std::mutex m;
    return m;
}
enigo::Enigo& enigo_instance() {
    static enigo::Enigo e;
    return e;
}

// LATEST_INPUT (static mut Input { conn, time }).
struct Input {
    int32_t conn = 0;
    int64_t time = 0;
};
std::mutex& latest_input_lock() {
    static std::mutex m;
    return m;
}
Input& latest_input_value() {
    static Input v;
    return v;
}

const std::unordered_map<int32_t, enigo::Key>& key_map() {
    using enigo::Kind;
    static const std::unordered_map<int32_t, enigo::Key> map = [] {
        const std::pair<hbb::ControlKey, Kind> table[] = {
            {hbb::Alt, Kind::Alt},
            {hbb::Backspace, Kind::Backspace},
            {hbb::CapsLock, Kind::CapsLock},
            {hbb::Control, Kind::Control},
            {hbb::Delete, Kind::Delete},
            {hbb::DownArrow, Kind::DownArrow},
            {hbb::End, Kind::End},
            {hbb::Escape, Kind::Escape},
            {hbb::F1, Kind::F1},
            {hbb::F10, Kind::F10},
            {hbb::F11, Kind::F11},
            {hbb::F12, Kind::F12},
            {hbb::F2, Kind::F2},
            {hbb::F3, Kind::F3},
            {hbb::F4, Kind::F4},
            {hbb::F5, Kind::F5},
            {hbb::F6, Kind::F6},
            {hbb::F7, Kind::F7},
            {hbb::F8, Kind::F8},
            {hbb::F9, Kind::F9},
            {hbb::Home, Kind::Home},
            {hbb::LeftArrow, Kind::LeftArrow},
            {hbb::Meta, Kind::Meta},
            {hbb::Option, Kind::Option},
            {hbb::PageDown, Kind::PageDown},
            {hbb::PageUp, Kind::PageUp},
            {hbb::Return, Kind::Return},
            {hbb::RightArrow, Kind::RightArrow},
            {hbb::Shift, Kind::Shift},
            {hbb::Space, Kind::Space},
            {hbb::Tab, Kind::Tab},
            {hbb::UpArrow, Kind::UpArrow},
            {hbb::Numpad0, Kind::Numpad0},
            {hbb::Numpad1, Kind::Numpad1},
            {hbb::Numpad2, Kind::Numpad2},
            {hbb::Numpad3, Kind::Numpad3},
            {hbb::Numpad4, Kind::Numpad4},
            {hbb::Numpad5, Kind::Numpad5},
            {hbb::Numpad6, Kind::Numpad6},
            {hbb::Numpad7, Kind::Numpad7},
            {hbb::Numpad8, Kind::Numpad8},
            {hbb::Numpad9, Kind::Numpad9},
            {hbb::Cancel, Kind::Cancel},
            {hbb::Clear, Kind::Clear},
            {hbb::Menu, Kind::Menu},
            {hbb::Pause, Kind::Pause},
            {hbb::Kana, Kind::Kana},
            {hbb::Hangul, Kind::Hangul},
            {hbb::Junja, Kind::Junja},
            {hbb::Final, Kind::Final},
            {hbb::Hanja, Kind::Hanja},
            {hbb::Kanji, Kind::Kanji},
            {hbb::Convert, Kind::Convert},
            {hbb::Select, Kind::Select},
            {hbb::Print, Kind::Print},
            {hbb::Execute, Kind::Execute},
            {hbb::Snapshot, Kind::Snapshot},
            {hbb::Insert, Kind::Insert},
            {hbb::Help, Kind::Help},
            {hbb::Sleep, Kind::Sleep},
            {hbb::Separator, Kind::Separator},
            {hbb::Scroll, Kind::Scroll},
            {hbb::NumLock, Kind::NumLock},
            {hbb::RWin, Kind::RWin},
            {hbb::Apps, Kind::Apps},
            {hbb::Multiply, Kind::Multiply},
            {hbb::Add, Kind::Add},
            {hbb::Subtract, Kind::Subtract},
            {hbb::Decimal, Kind::Decimal},
            {hbb::Divide, Kind::Divide},
            {hbb::Equals, Kind::Equals},
            {hbb::NumpadEnter, Kind::NumpadEnter},
        };
        std::unordered_map<int32_t, enigo::Key> m;
        for (const auto& [ck, kind] : table) {
            m.emplace(static_cast<int32_t>(ck), enigo::Key{kind});
        }
        return m;
    }();
    return map;
}

const enigo::Key* key_map_get(int32_t value) {
    const auto& map = key_map();
    const auto it = map.find(value);
    return it == map.end() ? nullptr : &it->second;
}

void modifier_sleep() {
    // sleep for a while, this is only for keying in rdp in peer so far
#if defined(_WIN32)
    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
#endif
}

// #[tokio::main(basic_scheduler)] async fn send_sas().
void send_sas() {
    ipc::Connection stream = ipc::connect(1000, kPostfixService);
    stream.send(ipc::Data::SAS{});  // timeout(1000, ..)??: a local socket write
}

void handle_key_(const hbb::KeyEvent& evt) {
    std::lock_guard<std::mutex> lock(enigo_lock());
    enigo::Enigo& en = enigo_instance();
    std::vector<enigo::Key> to_release;
    bool has_cap = false;
    for (int i = 0; i < evt.modifiers_size(); ++i) {
        if (const enigo::Key* key = key_map_get(evt.modifiers(i))) {
            if (*key == enigo::Key{enigo::Kind::CapsLock}) {
                has_cap = true;
            } else if (*key == enigo::Key{enigo::Kind::NumLock}) {
                // Windows: has_numlock = true
            } else {
                if (!en.get_key_state(*key)) {
                    en.key_down(*key);
                    modifier_sleep();
                    to_release.push_back(*key);
                }
            }
        }
    }
    if (valid_for_capslock(evt)) {
        if (has_cap != en.get_key_state(enigo::Key{enigo::Kind::CapsLock})) {
            en.key_down(enigo::Key{enigo::Kind::CapsLock});
            en.key_up(enigo::Key{enigo::Kind::CapsLock});
        }
    }
    switch (evt.union_case()) {
        case hbb::KeyEvent::kControlKey: {
            const int32_t ck = evt.control_key();
            if (const enigo::Key* key = key_map_get(ck)) {
                if (evt.down()) {
                    en.key_down(*key);
                } else {
                    en.key_up(*key);
                }
            } else if (ck == hbb::CtrlAltDel) {
                // have to spawn new thread because send_sas is tokio_main, the
                // caller can not be tokio_main.
                std::thread([] { HBB_ALLOW_ERR(send_sas()); }).detach();
            } else if (ck == hbb::LockScreen) {
                platform::lock_screen();
                video_service::switch_to_primary();
            }
            break;
        }
        case hbb::KeyEvent::kChr: {
            // Layout(chr as u8 as char): the low byte, as a Latin-1 char.
            const enigo::Key key{enigo::Kind::Layout, evt.chr() & 0xFFu};
            if (evt.down()) {
                en.key_down(key);
            } else {
                en.key_up(key);
            }
            break;
        }
        case hbb::KeyEvent::kUnicode: {
            // char::try_from(u32): scalar values only (no surrogates, <= 0x10FFFF).
            const uint32_t chr = evt.unicode();
            if (chr <= 0x10FFFF && !(chr >= 0xD800 && chr <= 0xDFFF)) {
                std::string s;
                if (chr < 0x80) {
                    s.push_back(static_cast<char>(chr));
                } else if (chr < 0x800) {
                    s.push_back(static_cast<char>(0xC0 | (chr >> 6)));
                    s.push_back(static_cast<char>(0x80 | (chr & 0x3F)));
                } else if (chr < 0x10000) {
                    s.push_back(static_cast<char>(0xE0 | (chr >> 12)));
                    s.push_back(static_cast<char>(0x80 | ((chr >> 6) & 0x3F)));
                    s.push_back(static_cast<char>(0x80 | (chr & 0x3F)));
                } else {
                    s.push_back(static_cast<char>(0xF0 | (chr >> 18)));
                    s.push_back(static_cast<char>(0x80 | ((chr >> 12) & 0x3F)));
                    s.push_back(static_cast<char>(0x80 | ((chr >> 6) & 0x3F)));
                    s.push_back(static_cast<char>(0x80 | (chr & 0x3F)));
                }
                en.key_sequence(s);
            }
            break;
        }
        case hbb::KeyEvent::kSeq:
            en.key_sequence(evt.seq());
            break;
        default:
            break;
    }
    for (const enigo::Key& key : to_release) {
        en.key_up(key);
    }
}

}  // namespace

bool is_left_up(const hbb::MouseEvent& evt) {
    const int32_t buttons = evt.mask() >> 3;
    const int32_t evt_type = evt.mask() & 0x7;
    return buttons == 1 && evt_type == 2;
}

#if defined(_WIN32)
void mouse_move_relative(int32_t x, int32_t y) {
    // crate::platform::windows::try_change_desktop() arrives with the Windows step.
    std::lock_guard<std::mutex> lock(enigo_lock());
    enigo_instance().mouse_move_relative(x, y);
}
#endif

void handle_mouse(const hbb::MouseEvent& evt, int32_t conn) {
    const int32_t buttons = evt.mask() >> 3;
    const int32_t evt_type = evt.mask() & 0x7;
    if (evt_type == 0) {
        std::lock_guard<std::mutex> l(latest_input_lock());
        latest_input_value() = Input{conn, get_time()};
    }
    std::lock_guard<std::mutex> lock(enigo_lock());
    enigo::Enigo& en = enigo_instance();
    std::vector<enigo::Key> to_release;
    for (int i = 0; i < evt.modifiers_size(); ++i) {
        if (const enigo::Key* key = key_map_get(evt.modifiers(i))) {
            if (evt_type == 1 || evt_type == 2) {
                if (*key != enigo::Key{enigo::Kind::CapsLock} &&
                    *key != enigo::Key{enigo::Kind::NumLock}) {
                    if (!en.get_key_state(*key)) {
                        en.key_down(*key);
                        modifier_sleep();
                        to_release.push_back(*key);
                    }
                }
            }
        }
    }
    switch (evt_type) {
        case 0:
            en.mouse_move_to(evt.x(), evt.y());
            break;
        case 1:
            switch (buttons) {
                case 1:
                    en.mouse_down(enigo::MouseButton::Left);
                    break;
                case 2:
                    en.mouse_down(enigo::MouseButton::Right);
                    break;
                case 4:
                    en.mouse_down(enigo::MouseButton::Middle);
                    break;
                default:
                    break;
            }
            break;
        case 2:
            switch (buttons) {
                case 1:
                    en.mouse_up(enigo::MouseButton::Left);
                    break;
                case 2:
                    en.mouse_up(enigo::MouseButton::Right);
                    break;
                case 4:
                    en.mouse_up(enigo::MouseButton::Middle);
                    break;
                default:
                    break;
            }
            break;
        case 3: {
            int32_t x = evt.x();
            int32_t y = evt.y();
#if !defined(_WIN32)
            x = -x;
            y = -y;
#endif
            if (x != 0) {
                en.mouse_scroll_x(x);
            }
            if (y != 0) {
                en.mouse_scroll_y(y);
            }
            break;
        }
        default:
            break;
    }
    for (const enigo::Key& key : to_release) {
        en.key_up(key);
    }
}

bool is_enter(const hbb::KeyEvent& evt) {
    if (evt.union_case() == hbb::KeyEvent::kControlKey) {
        const int32_t ck = evt.control_key();
        if (ck == hbb::Return || ck == hbb::NumpadEnter) {
            return true;
        }
    }
    return false;
}

void handle_key(const hbb::KeyEvent& evt) {
    // macOS: `if !*IS_SERVER` hops to the main queue (macOS platform step).
    handle_key_(evt);
}

std::pair<int32_t, int64_t> latest_input() {
    std::lock_guard<std::mutex> l(latest_input_lock());
    return {latest_input_value().conn, latest_input_value().time};
}

void MouseCursorSub::send(const MessagePtr& msg) {
    if (msg->union_case() == hbb::Message::kCursorData) {
        const uint64_t id = msg->cursor_data().id();
        const auto it = cached.find(id);
        if (it != cached.end()) {
            inner.send(it->second);
        } else {
            inner.send(msg);
            hbb::Message tmp;
            // Only send id out; require client side cache also (upstream comment).
            tmp.set_cursor_id(id);
            cached.emplace(id, std::make_shared<hbb::Message>(std::move(tmp)));
        }
    } else {
        inner.send(msg);
    }
}

MouseCursorService new_cursor() {
    // need_snapshot=true parity; repeat(StateCursor, run_cursor) deferred
    // (needs platform cursor capture from the input step).
    return MouseCursorService(kNameCursor, true);
}

GenericService new_pos() {
    // need_snapshot=false parity; repeat(StatePos, run_pos) deferred.
    return GenericService(kNamePos, false);
}

}  // namespace rustdesk::input_service
