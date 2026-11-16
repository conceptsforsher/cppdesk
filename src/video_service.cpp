// video_service.cpp — see video_service.hpp.
#include <rustdesk/video_service.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rustdesk::video_service {
namespace {

using Clock = std::chrono::steady_clock;

// lazy_static! state. Function-local statics initialize on first use, like
// lazy_static (LAST_ACTIVE starts at the first access, not at startup).
std::mutex& current_display_lock() {
    static std::mutex m;
    return m;
}
size_t& current_display_value() {
    static size_t v = SIZE_MAX;  // usize::MAX
    return v;
}
std::mutex& last_active_lock() {
    static std::mutex m;
    return m;
}
Clock::time_point& last_active_value() {
    static Clock::time_point v = Clock::now();
    return v;
}
std::mutex& switch_lock() {
    static std::mutex m;
    return m;
}
bool& switch_value() {
    static bool v = false;
    return v;
}
std::mutex& internal_latencies_lock() {
    static std::mutex m;
    return m;
}
std::unordered_map<int32_t, int64_t>& internal_latencies() {
    static std::unordered_map<int32_t, int64_t> m;
    return m;
}
std::mutex& test_latencies_lock() {
    static std::mutex m;
    return m;
}
std::unordered_map<int32_t, int64_t>& test_latencies() {
    static std::unordered_map<int32_t, int64_t> m;
    return m;
}
std::mutex& image_qualities_lock() {
    static std::mutex m;
    return m;
}
std::unordered_map<int32_t, int32_t>& image_qualities() {
    static std::unordered_map<int32_t, int32_t> m;
    return m;
}

// scrap::Display (common/x11.rs surface used here).
struct Display {
    std::pair<int64_t, int64_t> origin;
    size_t width = 0;
    size_t height = 0;
    std::string name;
    bool online = true;
    bool primary = false;
};

// Display::all() — scrap seam (see the header): until libs/scrap lands,
// every host answers like one without an X server.
std::vector<Display> display_all() {
    throw std::runtime_error("connection refused");  // io::ErrorKind::ConnectionRefused
}

size_t get_primary() {
    try {
        const std::vector<Display> all = display_all();
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].primary) {
                return i;
            }
        }
    } catch (const std::exception&) {
    }
    return 0;
}

void update_latency(int32_t id, int64_t latency, std::unordered_map<int32_t, int64_t>& latencies) {
    if (latency <= 0) {
        latencies.erase(id);
    } else {
        latencies[id] = latency;
    }
}

}  // namespace

GenericService create() {
    // need_snapshot=true parity; run() body (scrap capture + vpx) deferred.
    return GenericService(kName, true);
}

std::pair<size_t, std::vector<hbb::DisplayInfo>> get_displays() {
    // switch to primary display if long time (30 seconds) no users
    {
        std::lock_guard<std::mutex> l(last_active_lock());
        if (Clock::now() - last_active_value() >= std::chrono::seconds(30)) {
            std::lock_guard<std::mutex> c(current_display_lock());
            current_display_value() = SIZE_MAX;
        }
    }
    std::vector<hbb::DisplayInfo> displays;
    size_t primary = 0;
    const std::vector<Display> all = display_all();  // `?`
    for (size_t i = 0; i < all.size(); ++i) {
        const Display& d = all[i];
        if (d.primary) {
            primary = i;
        }
        hbb::DisplayInfo info;
        info.set_x(static_cast<int32_t>(d.origin.first));
        info.set_y(static_cast<int32_t>(d.origin.second));
        info.set_width(static_cast<int32_t>(d.width));
        info.set_height(static_cast<int32_t>(d.height));
        info.set_name(d.name);
        info.set_online(d.online);
        displays.push_back(std::move(info));
    }
    std::lock_guard<std::mutex> l(current_display_lock());
    size_t& current = current_display_value();
    if (current >= displays.size()) {
        current = primary;
    }
    return {current, std::move(displays)};
}

void switch_display(int32_t i) {
    const auto index = static_cast<size_t>(static_cast<int64_t>(i));  // `i as usize`
    try {
        const auto displays = get_displays().second;
        if (index < displays.size()) {
            std::lock_guard<std::mutex> l(current_display_lock());
            current_display_value() = index;
        }
    } catch (const std::exception&) {
        // `if let Ok(..)`: nothing to switch to
    }
}

void refresh() {
    std::lock_guard<std::mutex> l(switch_lock());
    switch_value() = true;
}

void switch_to_primary() {
    switch_display(static_cast<int32_t>(get_primary()));
}

void update_test_latency(int32_t id, int64_t latency) {
    std::lock_guard<std::mutex> l(test_latencies_lock());
    update_latency(id, latency, test_latencies());
}

void update_internal_latency(int32_t id, int64_t latency) {
    std::lock_guard<std::mutex> l(internal_latencies_lock());
    update_latency(id, latency, internal_latencies());
}

int64_t get_latency() {
    std::lock_guard<std::mutex> l(internal_latencies_lock());
    int64_t max = 0;
    bool any = false;
    for (const auto& [id, latency] : internal_latencies()) {
        (void)id;
        if (!any || latency > max) {
            max = latency;
            any = true;
        }
    }
    return any ? max : 0;
}

int32_t convert_quality(int32_t q) {
    int32_t bitrate = 0;
    int32_t quantizer = 0;
    if (q == hbb::Balanced) {
        bitrate = 100 * 2 / 3;
        quantizer = 12;
    } else if (q == hbb::Low) {
        bitrate = 100 / 2;
        quantizer = 18;
    } else if (q == hbb::Best) {
        bitrate = 100;
        quantizer = 12;
    } else {
        const int32_t b = q >> 8 & 0xFF;
        const int32_t qz = q & 0xFF;
        bitrate = b * 2;
        quantizer = (100 - qz) * 36 / 100;
    }
    if (bitrate <= 0) {
        return 0;
    }
    return bitrate << 8 | quantizer;
}

void update_image_quality(int32_t id, std::optional<int32_t> q) {
    std::lock_guard<std::mutex> l(image_qualities_lock());
    if (q) {
        const int32_t converted = convert_quality(*q);
        if (converted > 0) {
            image_qualities()[id] = converted;
        } else {
            image_qualities().erase(id);
        }
    } else {
        image_qualities().erase(id);
    }
}

int32_t get_image_quality() {
    std::lock_guard<std::mutex> l(image_qualities_lock());
    if (image_qualities().empty()) {
        return convert_quality(hbb::Balanced);
    }
    int32_t min = INT32_MAX;
    for (const auto& [id, q] : image_qualities()) {
        (void)id;
        if (q < min) {
            min = q;
        }
    }
    return min;
}

size_t current_display() {
    std::lock_guard<std::mutex> l(current_display_lock());
    return current_display_value();
}

bool switch_pending() {
    std::lock_guard<std::mutex> l(switch_lock());
    return switch_value();
}

}  // namespace rustdesk::video_service
