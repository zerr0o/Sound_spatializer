#include "sound_spatializer/window_audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <dwmapi.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Avrt.lib")

#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <vector>

namespace sound_spatializer {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

constexpr std::size_t kPerApplicationFifoFrames = 32'768;
constexpr std::size_t kCaptureScratchFrames = 1'024;
constexpr float kDefaultPixelsPerMeter = 2'400.0F;
constexpr std::uint32_t kSessionMissesBeforeRemoval = 3;

class ScopedComApartment {
public:
    explicit ScopedComApartment(DWORD concurrency_model) noexcept
        : result_(CoInitializeEx(nullptr, concurrency_model)),
          uninitialize_(SUCCEEDED(result_)) {}

    ~ScopedComApartment() {
        if (uninitialize_) CoUninitialize();
    }

    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{};
    bool uninitialize_{};
};

template <std::size_t Size>
void copy_text(std::array<char, Size>& destination, std::string_view source) noexcept {
    static_assert(Size > 0);
    const std::size_t count = std::min(source.size(), Size - 1);
    if (count != 0) std::memcpy(destination.data(), source.data(), count);
    destination[count] = '\0';
    if (count + 1 < Size) {
        std::memset(destination.data() + count + 1, 0, Size - count - 1);
    }
}

[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

[[nodiscard]] std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required) != required) {
        return {};
    }
    return result;
}

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        const char folded_a = a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : static_cast<char>(a);
        const char folded_b = b >= 'A' && b <= 'Z' ? static_cast<char>(b + ('a' - 'A')) : static_cast<char>(b);
        if (folded_a != folded_b) return false;
    }
    return true;
}

[[nodiscard]] std::string hresult_text(std::string_view operation, HRESULT result) {
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer), "%.*s failed (HRESULT 0x%08lX)",
                  static_cast<int>(std::min<std::size_t>(operation.size(), 160)), operation.data(),
                  static_cast<unsigned long>(result));
    return buffer;
}

[[nodiscard]] bool finite_vec3(const Vec3f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finite_quaternion(const Quaternionf& value) noexcept {
    return std::isfinite(value.w) && std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] RECT to_rect(const WindowAudioPixelBounds& bounds) noexcept {
    return {bounds.left, bounds.top, bounds.right, bounds.bottom};
}

[[nodiscard]] WindowAudioPixelBounds from_rect(const RECT& bounds) noexcept {
    return {bounds.left, bounds.top, bounds.right, bounds.bottom};
}

[[nodiscard]] std::int64_t rect_area(const RECT& bounds) noexcept {
    const auto width = std::max<LONG>(0, bounds.right - bounds.left);
    const auto height = std::max<LONG>(0, bounds.bottom - bounds.top);
    return static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
}

[[nodiscard]] std::string executable_path(std::uint32_t process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) return {};
    std::array<wchar_t, 32'768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    const bool success = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    return success ? wide_to_utf8(std::wstring_view(path.data(), length)) : std::string{};
}

[[nodiscard]] std::string executable_name(std::string_view path) {
    const std::size_t separator = path.find_last_of("\\/");
    return std::string(separator == std::string_view::npos ? path : path.substr(separator + 1));
}

struct ProcessTree {
    std::unordered_map<std::uint32_t, std::uint32_t> parents;

    [[nodiscard]] bool is_ancestor(std::uint32_t possible_parent, std::uint32_t process) const noexcept {
        if (possible_parent == 0 || process == 0 || possible_parent == process) return false;
        for (std::size_t depth = 0; depth < 64; ++depth) {
            const auto found = parents.find(process);
            if (found == parents.end() || found->second == 0 || found->second == process) return false;
            process = found->second;
            if (process == possible_parent) return true;
        }
        return false;
    }
};

[[nodiscard]] bool capture_tree_intersects_excluded_process(
    const ProcessTree& process_tree, std::uint32_t excluded_process_id,
    std::uint32_t candidate_process_id) noexcept {
    if (excluded_process_id == 0 || candidate_process_id == 0) return false;
    return candidate_process_id == excluded_process_id ||
           process_tree.is_ancestor(excluded_process_id,
                                    candidate_process_id) ||
           process_tree.is_ancestor(candidate_process_id,
                                    excluded_process_id);
}

[[nodiscard]] ProcessTree read_process_tree() {
    ProcessTree result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            result.parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID);
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return result;
}

struct DisplayPathName {
    std::wstring gdi_name;
    std::string id;
    std::string friendly_name;
};

[[nodiscard]] std::vector<DisplayPathName> query_display_path_names() {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return {};

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    LONG query_result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                            modes.data(), nullptr);
    if (query_result != ERROR_SUCCESS) return {};
    paths.resize(path_count);

    std::vector<DisplayPathName> result;
    result.reserve(paths.size());
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        const bool target_available = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS;

        DisplayPathName names{};
        names.gdi_name = source.viewGdiDeviceName;
        if (target_available) {
            names.id = wide_to_utf8(target.monitorDevicePath);
            names.friendly_name = wide_to_utf8(target.monitorFriendlyDeviceName);
        }
        if (names.id.empty()) names.id = wide_to_utf8(names.gdi_name);
        if (names.friendly_name.empty()) names.friendly_name = names.id;
        result.push_back(std::move(names));
    }
    return result;
}

struct DisplayInfo {
    HMONITOR monitor{};
    std::wstring gdi_name;
    std::string id;
    std::string name;
    RECT bounds{};
    bool primary{};
    Vec3f center_m{};
    float width_m{};
    float height_m{};
    Quaternionf orientation{};
};

struct MonitorEnumerationContext {
    std::vector<DisplayInfo>* displays{};
    const std::vector<DisplayPathName>* path_names{};
};

BOOL CALLBACK enumerate_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* context = reinterpret_cast<MonitorEnumerationContext*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (context == nullptr || context->displays == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) return TRUE;

    DisplayInfo display{};
    display.monitor = monitor;
    display.gdi_name = info.szDevice;
    display.bounds = info.rcMonitor;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (context->path_names != nullptr) {
        const auto found = std::find_if(context->path_names->begin(), context->path_names->end(),
                                        [&](const DisplayPathName& candidate) {
                                            return _wcsicmp(candidate.gdi_name.c_str(), display.gdi_name.c_str()) == 0;
                                        });
        if (found != context->path_names->end()) {
            display.id = found->id;
            display.name = found->friendly_name;
        }
    }
    if (display.id.empty()) display.id = wide_to_utf8(display.gdi_name);
    if (display.id.size() >= kWindowAudioDisplayIdBytes)
        display.id.resize(kWindowAudioDisplayIdBytes - 1);
    if (display.name.empty()) display.name = display.id;
    context->displays->push_back(std::move(display));
    return TRUE;
}

[[nodiscard]] const WindowAudioDisplayCalibration* find_calibration(const WindowAudioConfig& config,
                                                                     const DisplayInfo& display) noexcept {
    const std::size_t count = std::min(config.display_calibration_count, config.display_calibrations.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& calibration = config.display_calibrations[index];
        if (ascii_iequals(calibration.display_id, display.id)) {
            return &calibration;
        }
    }
    return nullptr;
}

[[nodiscard]] std::vector<DisplayInfo> enumerate_displays(const WindowAudioConfig& config) {
    const std::vector<DisplayPathName> path_names = query_display_path_names();
    std::vector<DisplayInfo> displays;
    MonitorEnumerationContext context{&displays, &path_names};
    EnumDisplayMonitors(nullptr, nullptr, enumerate_monitor, reinterpret_cast<LPARAM>(&context));
    if (displays.empty()) return displays;

    RECT desktop = displays.front().bounds;
    for (const auto& display : displays) {
        desktop.left = std::min(desktop.left, display.bounds.left);
        desktop.top = std::min(desktop.top, display.bounds.top);
        desktop.right = std::max(desktop.right, display.bounds.right);
        desktop.bottom = std::max(desktop.bottom, display.bounds.bottom);
    }
    const float desktop_center_x = 0.5F * static_cast<float>(desktop.left + desktop.right);
    const float desktop_center_y = 0.5F * static_cast<float>(desktop.top + desktop.bottom);

    for (auto& display : displays) {
        if (const auto* calibration = find_calibration(config, display);
            calibration != nullptr && finite_vec3(calibration->center_m) &&
            finite_quaternion(calibration->orientation) && std::isfinite(calibration->width_m) &&
            std::isfinite(calibration->height_m) && calibration->width_m > 0.1F &&
            calibration->height_m > 0.1F) {
            display.center_m = calibration->center_m;
            display.width_m = calibration->width_m;
            display.height_m = calibration->height_m;
            display.orientation = calibration->orientation.normalized_value();
            continue;
        }
        const float center_x = 0.5F * static_cast<float>(display.bounds.left + display.bounds.right);
        const float center_y = 0.5F * static_cast<float>(display.bounds.top + display.bounds.bottom);
        display.center_m = {
            config.listener_position_m.x + (center_x - desktop_center_x) / kDefaultPixelsPerMeter,
            config.listener_position_m.y - (center_y - desktop_center_y) / kDefaultPixelsPerMeter,
            config.listener_position_m.z + 0.85F,
        };
        display.width_m =
            static_cast<float>(std::max<LONG>(1, display.bounds.right - display.bounds.left)) /
            kDefaultPixelsPerMeter;
        display.height_m =
            static_cast<float>(std::max<LONG>(1, display.bounds.bottom - display.bounds.top)) /
            kDefaultPixelsPerMeter;
        display.orientation = {};
    }
    return displays;
}

struct WindowMatch {
    HWND handle{};
    RECT bounds{};
    std::wstring title;
    std::int64_t area{};
};

struct WindowEnumerationContext {
    std::uint32_t process_id{};
    WindowMatch best{};
};

BOOL CALLBACK enumerate_process_window(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowEnumerationContext*>(parameter);
    if (context == nullptr || IsWindowVisible(window) == FALSE) return TRUE;

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != context->process_id) return TRUE;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != FALSE) {
        return TRUE;
    }
    const LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended_style & WS_EX_TOOLWINDOW) != 0) return TRUE;

    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
        GetWindowRect(window, &bounds) == FALSE) {
        return TRUE;
    }
    const std::int64_t area = rect_area(bounds);
    if (area <= context->best.area) return TRUE;

    const int title_length = GetWindowTextLengthW(window);
    std::wstring title(static_cast<std::size_t>(std::max(0, title_length)) + 1, L'\0');
    const int copied_title_length =
        title_length > 0 ? GetWindowTextW(window, title.data(), title_length + 1) : 0;
    title.resize(static_cast<std::size_t>(std::max(0, copied_title_length)));
    context->best = {window, bounds, std::move(title), area};
    return TRUE;
}

[[nodiscard]] WindowMatch find_process_window(std::uint32_t process_id, const ProcessTree& tree) {
    for (std::size_t depth = 0; depth < 32 && process_id != 0; ++depth) {
        WindowEnumerationContext context{process_id, {}};
        EnumWindows(enumerate_process_window, reinterpret_cast<LPARAM>(&context));
        if (context.best.handle != nullptr) return context.best;
        const auto parent = tree.parents.find(process_id);
        if (parent == tree.parents.end() || parent->second == process_id) break;
        process_id = parent->second;
    }
    return {};
}

[[nodiscard]] const DisplayInfo* primary_display(const std::vector<DisplayInfo>& displays) noexcept {
    if (displays.empty()) return nullptr;
    const auto primary = std::find_if(displays.begin(), displays.end(),
                                      [](const DisplayInfo& display) { return display.primary; });
    return primary != displays.end() ? &*primary : &displays.front();
}

[[nodiscard]] const DisplayInfo* find_display(const std::vector<DisplayInfo>& displays,
                                               std::string_view id) noexcept {
    if (id.empty()) return nullptr;
    const auto found = std::find_if(displays.begin(), displays.end(),
                                    [&](const DisplayInfo& display) { return ascii_iequals(display.id, id); });
    return found != displays.end() ? &*found : nullptr;
}

[[nodiscard]] const DisplayInfo* display_for_window(const std::vector<DisplayInfo>& displays,
                                                     const WindowMatch& window,
                                                     std::string_view fallback_id) noexcept {
    if (window.handle != nullptr) {
        HMONITOR monitor = MonitorFromRect(&window.bounds, MONITOR_DEFAULTTONEAREST);
        const auto found = std::find_if(displays.begin(), displays.end(),
                                        [&](const DisplayInfo& display) { return display.monitor == monitor; });
        if (found != displays.end()) return &*found;
    }
    if (const auto* fallback = find_display(displays, fallback_id)) return fallback;
    return primary_display(displays);
}

struct StereoPlacement {
    Vec3f left{};
    Vec3f right{};
};

[[nodiscard]] const DisplayInfo* display_for_point(
    const std::vector<DisplayInfo>& displays, float x, float y,
    const DisplayInfo& fallback) noexcept {
    for (const auto& display : displays) {
        if (x >= static_cast<float>(display.bounds.left) &&
            x < static_cast<float>(display.bounds.right) &&
            y >= static_cast<float>(display.bounds.top) &&
            y < static_cast<float>(display.bounds.bottom)) {
            return &display;
        }
    }

    const DisplayInfo* nearest = &fallback;
    float nearest_distance = std::numeric_limits<float>::max();
    for (const auto& display : displays) {
        const float closest_x = std::clamp(
            x, static_cast<float>(display.bounds.left),
            static_cast<float>(display.bounds.right));
        const float closest_y = std::clamp(
            y, static_cast<float>(display.bounds.top),
            static_cast<float>(display.bounds.bottom));
        const float delta_x = x - closest_x;
        const float delta_y = y - closest_y;
        const float distance =
            delta_x * delta_x + delta_y * delta_y;
        if (distance < nearest_distance) {
            nearest = &display;
            nearest_distance = distance;
        }
    }
    return nearest;
}

[[nodiscard]] Vec3f map_pixel_to_world(const DisplayInfo& display, float x,
                                       float y) noexcept {
    const float screen_width_px =
        static_cast<float>(std::max<LONG>(1, display.bounds.right - display.bounds.left));
    const float screen_height_px =
        static_cast<float>(std::max<LONG>(1, display.bounds.bottom - display.bounds.top));
    const float u = std::clamp(
        (x - static_cast<float>(display.bounds.left)) / screen_width_px,
        0.0F, 1.0F);
    const float v = std::clamp(
        (y - static_cast<float>(display.bounds.top)) / screen_height_px,
        0.0F, 1.0F);
    const Vec3f screen_right = rotate(display.orientation, {1.0F, 0.0F, 0.0F});
    const Vec3f screen_up = rotate(display.orientation, {0.0F, 1.0F, 0.0F});
    return display.center_m +
           screen_right * ((u - 0.5F) * display.width_m) +
           screen_up * ((0.5F - v) * display.height_m);
}

[[nodiscard]] StereoPlacement calculate_placement(
    const std::vector<DisplayInfo>& displays, const DisplayInfo& display,
    const WindowMatch& window, float stereo_spread, bool follow_window,
    WindowAudioPlacementMode placement_mode) noexcept {
    const float effective_spread =
        placement_mode == WindowAudioPlacementMode::window_edges
            ? 1.0F
            : std::clamp(stereo_spread, 0.0F, 1.0F);

    if (!follow_window || window.handle == nullptr) {
        const float center_x =
            0.5F * static_cast<float>(display.bounds.left +
                                      display.bounds.right);
        const float center_y =
            0.5F * static_cast<float>(display.bounds.top +
                                      display.bounds.bottom);
        const float half_width =
            0.5F * static_cast<float>(display.bounds.right -
                                      display.bounds.left);
        return {
            map_pixel_to_world(display,
                               center_x - half_width * effective_spread,
                               center_y),
            map_pixel_to_world(display,
                               center_x + half_width * effective_spread,
                               center_y)};
    }

    const float left_bound = static_cast<float>(window.bounds.left);
    const float right_bound = static_cast<float>(
        std::max<LONG>(window.bounds.left + 1, window.bounds.right));
    const float center_x = 0.5F * (left_bound + right_bound);
    const float center_y =
        0.5F * static_cast<float>(window.bounds.top +
                                  window.bounds.bottom);
    const float half_width = 0.5F * (right_bound - left_bound);
    const float left_x = center_x - half_width * effective_spread;
    float right_x = center_x + half_width * effective_spread;
    if (right_x >= right_bound) {
        // RECT.right is exclusive. Keep an emitter on the monitor actually
        // occupied by the rightmost window pixel when the border lands exactly
        // on an adjacent display boundary.
        right_x = std::nextafter(right_bound, left_bound);
    }

    const DisplayInfo* left_display =
        display_for_point(displays, left_x, center_y, display);
    const DisplayInfo* right_display =
        display_for_point(displays, right_x, center_y, display);
    return {
        map_pixel_to_world(*left_display, left_x, center_y),
        map_pixel_to_world(*right_display, right_x, center_y)};
}

struct SessionCandidate {
    std::uint32_t process_id{};
    std::string session_id;
    std::string application_id;
    std::string application_name;
    bool session_active{};
};

struct SessionEnumeration {
    std::vector<SessionCandidate> candidates;
    bool complete{true};
    std::size_t uncapturable_active_sessions{};
    std::string coverage_detail;
};

[[nodiscard]] constexpr bool is_discoverable_session_state(AudioSessionState state) noexcept {
    return state == AudioSessionStateActive || state == AudioSessionStateInactive;
}

static_assert(is_discoverable_session_state(AudioSessionStateActive));
static_assert(is_discoverable_session_state(AudioSessionStateInactive));
static_assert(!is_discoverable_session_state(AudioSessionStateExpired));

[[nodiscard]] SessionEnumeration enumerate_sessions(
    const WindowAudioConfig& config, std::string& error) {
    SessionEnumeration enumeration{};
    ComPtr<IMMDeviceEnumerator> device_enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&device_enumerator));
    if (FAILED(result)) {
        error = hresult_text("create MMDevice enumerator", result);
        enumeration.complete = false;
        return enumeration;
    }

    ComPtr<IMMDevice> device;
    if (config.discovery_endpoint_id.empty()) {
        result = device_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    } else {
        const std::wstring endpoint_id = utf8_to_wide(config.discovery_endpoint_id);
        result = endpoint_id.empty() ? E_INVALIDARG : device_enumerator->GetDevice(endpoint_id.c_str(), &device);
    }
    if (FAILED(result)) {
        error = hresult_text("open session-discovery endpoint", result);
        enumeration.complete = false;
        return enumeration;
    }

    ComPtr<IAudioSessionManager2> session_manager;
    result = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(session_manager.GetAddressOf()));
    if (FAILED(result)) {
        error = hresult_text("activate audio session manager", result);
        enumeration.complete = false;
        return enumeration;
    }

    ComPtr<IAudioSessionEnumerator> session_enumerator;
    result = session_manager->GetSessionEnumerator(&session_enumerator);
    if (FAILED(result)) {
        error = hresult_text("enumerate audio sessions", result);
        enumeration.complete = false;
        return enumeration;
    }

    int session_count = 0;
    result = session_enumerator->GetCount(&session_count);
    if (FAILED(result)) {
        error = hresult_text("read audio session count", result);
        enumeration.complete = false;
        return enumeration;
    }
    enumeration.candidates.reserve(
        static_cast<std::size_t>(std::max(0, session_count)));
    for (int index = 0; index < session_count; ++index) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(session_enumerator->GetSession(index, &control))) {
            // A skipped entry could be the only active system sound or an
            // application that has not been armed. Conservatively retain the
            // endpoint mix until a complete pass succeeds.
            enumeration.complete = false;
            if (enumeration.coverage_detail.empty())
                enumeration.coverage_detail =
                    "session-enumeration-entry-unavailable";
            continue;
        }
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(control->GetState(&state))) {
            enumeration.complete = false;
            if (enumeration.coverage_detail.empty())
                enumeration.coverage_detail =
                    "session-state-unavailable";
            continue;
        }
        if (!is_discoverable_session_state(state)) continue;
        const bool session_active = state == AudioSessionStateActive;

        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(&control2))) {
            enumeration.complete = false;
            if (session_active)
                ++enumeration.uncapturable_active_sessions;
            if (session_active && enumeration.coverage_detail.empty())
                enumeration.coverage_detail =
                    "active-session-control2-unavailable";
            continue;
        }
        const HRESULT system_session = control2->IsSystemSoundsSession();
        if (FAILED(system_session)) {
            enumeration.complete = false;
            if (session_active)
                ++enumeration.uncapturable_active_sessions;
            if (session_active && enumeration.coverage_detail.empty())
                enumeration.coverage_detail =
                    "active-session-type-unavailable";
            continue;
        }
        if (system_session == S_OK) {
            // System sounds have no application window and process-loopback
            // cannot target their PID 0 session. They are outside the
            // per-window source set; counting the session as uncovered would
            // keep this mode in endpoint fallback permanently on machines
            // where Windows leaves that session active.
            continue;
        }
        DWORD process_id = 0;
        if (FAILED(control2->GetProcessId(&process_id)) || process_id == 0) {
            enumeration.complete = false;
            if (session_active)
                ++enumeration.uncapturable_active_sessions;
            if (session_active && enumeration.coverage_detail.empty())
                enumeration.coverage_detail =
                    "active-session-process-unavailable";
            continue;
        }

        LPWSTR session_identifier = nullptr;
        std::string session_id;
        if (SUCCEEDED(control2->GetSessionIdentifier(&session_identifier)) && session_identifier != nullptr) {
            session_id = wide_to_utf8(session_identifier);
            CoTaskMemFree(session_identifier);
        }

        LPWSTR display_name = nullptr;
        std::string application_name;
        if (SUCCEEDED(control->GetDisplayName(&display_name)) && display_name != nullptr) {
            application_name = wide_to_utf8(display_name);
            CoTaskMemFree(display_name);
        }
        const std::string path = executable_path(process_id);
        if (application_name.empty()) application_name = executable_name(path);
        if (application_name.empty()) application_name = "Process " + std::to_string(process_id);
        std::string application_id =
            !path.empty() ? path
                          : (!session_id.empty() ? session_id
                                                 : application_name);
        if (application_id.size() >= kWindowAudioApplicationIdBytes)
            application_id.resize(kWindowAudioApplicationIdBytes - 1);
        enumeration.candidates.push_back(
            {process_id, session_id, application_id, application_name,
             session_active});
    }
    return enumeration;
}

void deduplicate_process_trees(std::vector<SessionCandidate>& candidates, const ProcessTree& process_tree) {
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const SessionCandidate& left, const SessionCandidate& right) {
                         if (left.process_id != right.process_id)
                             return left.process_id < right.process_id;
                         return left.session_active && !right.session_active;
                     });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const SessionCandidate& left, const SessionCandidate& right) {
                                     return left.process_id == right.process_id;
                                 }),
                     candidates.end());

    std::vector<bool> remove(candidates.size(), false);
    for (std::size_t child = 0; child < candidates.size(); ++child) {
        for (std::size_t parent = 0; parent < candidates.size(); ++parent) {
            if (child != parent &&
                process_tree.is_ancestor(candidates[parent].process_id, candidates[child].process_id)) {
                // The parent process-loopback stream includes the child's
                // audio. Preserve the child's active priority so an inactive
                // parent session does not get crowded out of the eight armed
                // slots while one of its children is already sounding.
                candidates[parent].session_active =
                    candidates[parent].session_active ||
                    candidates[child].session_active;
                remove[child] = true;
            }
        }
    }
    std::size_t write = 0;
    for (std::size_t read = 0; read < candidates.size(); ++read) {
        if (!remove[read]) candidates[write++] = std::move(candidates[read]);
    }
    candidates.resize(write);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const SessionCandidate& left,
                        const SessionCandidate& right) {
                         return left.session_active &&
                                !right.session_active;
                     });
}

class ActivationCompletion final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IActivateAudioInterfaceCompletionHandler, FtmBase> {
public:
    ActivationCompletion() : completed_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationCompletion() override {
        if (completed_ != nullptr) CloseHandle(completed_);
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activated;
        HRESULT activation_result = E_FAIL;
        HRESULT result = operation != nullptr
                             ? operation->GetActivateResult(&activation_result, &activated)
                             : E_POINTER;
        result_ = FAILED(result) ? result : activation_result;
        if (SUCCEEDED(result_) && activated != nullptr) {
            result_ = activated.As(&audio_client_);
        }
        SetEvent(completed_);
        return S_OK;
    }

    [[nodiscard]] HANDLE completed_event() const noexcept { return completed_; }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }
    [[nodiscard]] ComPtr<IAudioClient> audio_client() const noexcept { return audio_client_; }

private:
    HANDLE completed_{};
    HRESULT result_{E_PENDING};
    ComPtr<IAudioClient> audio_client_;
};

[[nodiscard]] ComPtr<IAudioClient> activate_process_loopback(std::uint32_t process_id,
                                                             const std::stop_token& stop_token,
                                                             HRESULT& result) {
    AUDIOCLIENT_ACTIVATION_PARAMS activation{};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.TargetProcessId = process_id;
    activation.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT parameters{};
    parameters.vt = VT_BLOB;
    parameters.blob.cbSize = sizeof(activation);
    parameters.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

    ComPtr<ActivationCompletion> completion = Make<ActivationCompletion>();
    if (completion == nullptr || completion->completed_event() == nullptr) {
        result = E_OUTOFMEMORY;
        return {};
    }
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                         &parameters, completion.Get(), &operation);
    if (FAILED(result)) return {};

    while (!stop_token.stop_requested()) {
        const DWORD wait = WaitForSingleObject(completion->completed_event(), 50);
        if (wait == WAIT_OBJECT_0) {
            result = completion->result();
            return SUCCEEDED(result) ? completion->audio_client() : ComPtr<IAudioClient>{};
        }
        if (wait == WAIT_FAILED) {
            result = HRESULT_FROM_WIN32(GetLastError());
            return {};
        }
    }
    result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return {};
}

[[nodiscard]] bool is_float_format(const WAVEFORMATEX* format) noexcept {
    if (format == nullptr || format->nChannels == 0 || format->wBitsPerSample != 32 ||
        format->nBlockAlign != format->nChannels * sizeof(float)) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    return false;
}

[[nodiscard]] WAVEFORMATEXTENSIBLE canonical_stereo_float_48k() noexcept {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = 48'000;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = 2 * sizeof(float);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

class ScopedMmcssRegistration {
public:
    ScopedMmcssRegistration() noexcept {
        handle_ =
            AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index_);
        if (handle_ == nullptr) {
            task_index_ = 0;
            handle_ =
                AvSetMmThreadCharacteristicsW(L"Audio", &task_index_);
        }
        if (handle_ != nullptr)
            AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
    }

    ~ScopedMmcssRegistration() {
        if (handle_ != nullptr)
            AvRevertMmThreadCharacteristics(handle_);
    }

    ScopedMmcssRegistration(const ScopedMmcssRegistration&) = delete;
    ScopedMmcssRegistration& operator=(const ScopedMmcssRegistration&) =
        delete;

    [[nodiscard]] bool registered() const noexcept {
        return handle_ != nullptr;
    }

private:
    HANDLE handle_{};
    DWORD task_index_{};
};

struct AtomicPlacement {
    static_assert(std::atomic<float>::is_always_lock_free,
                  "RT placement publication requires lock-free float atomics");

    alignas(64) std::atomic<std::uint64_t> sequence{};
    std::atomic<float> left_x{};
    std::atomic<float> left_y{};
    std::atomic<float> left_z{};
    std::atomic<float> right_x{};
    std::atomic<float> right_y{};
    std::atomic<float> right_z{};
    std::atomic<float> gain_linear{1.0F};

    void store(const StereoPlacement& placement, float gain) noexcept {
        sequence.fetch_add(1, std::memory_order_acq_rel);
        left_x.store(placement.left.x, std::memory_order_relaxed);
        left_y.store(placement.left.y, std::memory_order_relaxed);
        left_z.store(placement.left.z, std::memory_order_relaxed);
        right_x.store(placement.right.x, std::memory_order_relaxed);
        right_y.store(placement.right.y, std::memory_order_relaxed);
        right_z.store(placement.right.z, std::memory_order_relaxed);
        gain_linear.store(gain, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] bool load(
        WindowAudioRealtimePlacement& result) const noexcept {
        // Bounded seqlock: all payload members remain atomic, so a writer
        // preempted while the sequence is odd cannot create a data race or
        // make the RT reader spin indefinitely. The caller skips this source
        // for the exceptionally contended callback instead.
        for (std::size_t attempt = 0; attempt < 4; ++attempt) {
            const std::uint64_t before =
                sequence.load(std::memory_order_acquire);
            if ((before & 1U) != 0) continue;
            WindowAudioRealtimePlacement candidate{};
            candidate.left_position_m = {
                left_x.load(std::memory_order_relaxed),
                left_y.load(std::memory_order_relaxed),
                left_z.load(std::memory_order_relaxed),
            };
            candidate.right_position_m = {
                right_x.load(std::memory_order_relaxed),
                right_y.load(std::memory_order_relaxed),
                right_z.load(std::memory_order_relaxed),
            };
            candidate.gain_linear =
                gain_linear.load(std::memory_order_relaxed);
            const std::uint64_t after =
                sequence.load(std::memory_order_acquire);
            if (before == after && (after & 1U) == 0) {
                result = candidate;
                return true;
            }
        }
        result = {};
        return false;
    }
};

struct SlotMetadata {
    SessionCandidate session;
    WindowMatch window;
    std::string display_id;
    StereoPlacement placement;
    float gain_db{};
};

struct CaptureSlot {
    AsyncStereoResampler resampler{kPerApplicationFifoFrames};
    alignas(64) std::atomic<std::uint32_t> generation{};
    std::atomic<std::uint32_t> process_id{};
    std::atomic<bool> active{};
    // Stream activation is not audio readiness: Windows process-loopback can
    // successfully pre-arm an inactive session and deliver only silent
    // buffers. The endpoint fallback remains authoritative until a real PCM
    // sample has entered this generation.
    std::atomic<bool> pcm_ready{};
    std::atomic<bool> session_active{};
    // A pre-armed inactive capture can receive process-tree PCM before session
    // discovery observes the source-endpoint activation. Epoch equality is a
    // fail-safe latch: discovery can acknowledge a stable epoch without ever
    // overwriting a concurrent capture-thread revocation.
    std::atomic<std::uint64_t> inactive_pcm_epoch{};
    std::atomic<std::uint64_t> validated_inactive_pcm_epoch{};
    // Once discovery has confirmed that the source-endpoint session is still
    // inactive, process-loopback PCM belongs to another endpoint. Keep
    // discarding it without repeatedly poisoning otherwise complete coverage.
    // A source-session transition clears this latch.
    std::atomic<bool> inactive_pcm_confirmed_external{};
    std::atomic<std::uint32_t> realtime_readers{};
    std::atomic<WindowAudioCaptureState> capture_state{WindowAudioCaptureState::inactive};
    std::atomic<std::uint32_t> sample_rate{};
    std::atomic<std::uint32_t> channel_count{};
    AtomicPlacement placement;
    std::jthread capture_thread;
    SlotMetadata metadata;
    std::uint32_t missed_discovery_passes{};

    CaptureSlot() {
        // A single process-loopback packet is commonly 480 frames. Waiting for
        // half that amount avoids declaring a source ready from the FIR's nine
        // look-ahead samples, while keeping startup latency bounded.
        resampler.set_startup_fill(256);
        resampler.set_target_fill(512);
        // More than ~21 ms at 48 kHz indicates an output outage or scheduling
        // discontinuity, not useful latency reserve. The RT consumer rebases
        // to the newest target-sized tail in O(1).
        resampler.set_rebase_threshold(1'024);
    }
};

class WindowAudioCaptureWindows final : public IWindowAudioCapture {
public:
    ~WindowAudioCaptureWindows() override { stop(); }

    bool start(const WindowAudioConfig& requested_config) override {
        stop();
        const WindowAudioConfig sanitized_config = sanitize_config(requested_config);
        {
            std::lock_guard lock(error_mutex_);
            last_error_.clear();
        }
        {
            std::lock_guard lock(lifecycle_mutex_);
            {
                std::lock_guard config_lock(config_mutex_);
                config_ = sanitized_config;
                refresh_revision_.fetch_add(1, std::memory_order_release);
                coverage_policy_revision_.fetch_add(
                    1, std::memory_order_release);
            }
            stop_requested_.store(false, std::memory_order_release);
            running_.store(true, std::memory_order_release);
            discovery_thread_ = std::thread([this] { discovery_loop(); });
        }
        return true;
    }

    bool reconfigure(const WindowAudioConfig& requested_config) override {
        const WindowAudioConfig sanitized_config = sanitize_config(requested_config);
        bool invalidate_coverage = false;
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            if (!running_.load(std::memory_order_acquire) ||
                stop_requested_.load(std::memory_order_acquire)) {
                return false;
            }
            std::lock_guard config_lock(config_mutex_);
            if (requires_restart(config_, sanitized_config)) return false;
            invalidate_coverage =
                coverage_policy_changed(config_, sanitized_config);
            config_ = sanitized_config;
            refresh_revision_.fetch_add(1, std::memory_order_release);
            if (invalidate_coverage) {
                coverage_policy_revision_.fetch_add(
                    1, std::memory_order_release);
            }
        }
        refresh_cv_.notify_all();
        return true;
    }

    void stop() noexcept override {
        stop_requested_.store(true, std::memory_order_release);
        refresh_cv_.notify_all();
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (discovery_thread_.joinable()) discovery_thread_.join();
            // Wake every capture first. Their event waits then elapse in
            // parallel instead of making shutdown pay up to eight sequential
            // 100 ms timeouts.
            for (auto& slot : slots_) request_slot_stop(slot);
            for (auto& slot : slots_) finish_slot_deactivation(slot);
            running_.store(false, std::memory_order_release);
            publish_coverage_state(false, 0);
        }
        publish_empty_snapshot();
    }

    [[nodiscard]] bool running() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] WindowAudioPullResult pull(WindowAudioSlotHandle handle, StereoFrame* output,
                                              std::size_t frame_count) noexcept override {
        WindowAudioPullResult result{};
        if (output == nullptr || frame_count == 0) return result;
        std::fill_n(output, frame_count, StereoFrame{});
        if (!handle.valid()) return result;

        CaptureSlot& slot = slots_[handle.slot];
        slot.realtime_readers.fetch_add(1, std::memory_order_acq_rel);
        const auto release_reader = [&slot] {
            slot.realtime_readers.fetch_sub(1, std::memory_order_release);
        };
        const std::uint32_t generation = slot.generation.load(std::memory_order_acquire);
        if (generation != handle.generation ||
            !slot.active.load(std::memory_order_acquire) ||
            slot.capture_state.load(std::memory_order_acquire) !=
                WindowAudioCaptureState::capturing) {
            release_reader();
            return result;
        }
        if (!slot.placement.load(result.placement)) {
            release_reader();
            return result;
        }
        const std::uint64_t underruns_before = slot.resampler.underruns();
        result.received_frames =
            slot.resampler.render(output, frame_count, 48'000.0F);
        const std::uint64_t underruns_after = slot.resampler.underruns();
        if (underruns_after > underruns_before) {
            fifo_underruns_.fetch_add(underruns_after - underruns_before,
                                     std::memory_order_relaxed);
        }
        result.active = slot.generation.load(std::memory_order_acquire) == handle.generation &&
                        window_audio_slot_is_renderable(
                            slot.active.load(std::memory_order_acquire),
                            slot.capture_state.load(std::memory_order_acquire),
                            slot.pcm_ready.load(std::memory_order_acquire),
                            slot.session_active.load(std::memory_order_acquire));
        if (!result.active) {
            std::fill_n(output, frame_count, StereoFrame{});
            result.received_frames = 0;
            release_reader();
            return result;
        }
        release_reader();
        return result;
    }

    [[nodiscard]] WindowAudioRealtimeSnapshot realtime_snapshot() const noexcept override {
        WindowAudioRealtimeSnapshot result{};
        for (std::size_t attempt = 0; attempt < 4; ++attempt) {
            result = {};
            const std::uint64_t before = realtime_sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0) continue;
            std::size_t ready_active_captures = 0;
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                const auto& slot = slots_[index];
                if (!slot.active.load(std::memory_order_acquire)) continue;
                if (window_audio_inactive_pcm_requires_endpoint_fallback(
                        slot.inactive_pcm_epoch.load(
                            std::memory_order_acquire),
                        slot.validated_inactive_pcm_epoch.load(
                            std::memory_order_acquire))) {
                    result.endpoint_fallback_requested = true;
                }
                const WindowAudioCaptureState capture_state =
                    slot.capture_state.load(std::memory_order_acquire);
                const bool session_active =
                    slot.session_active.load(std::memory_order_acquire);
                if (session_active &&
                    window_audio_slot_is_renderable(
                        true, capture_state,
                        slot.pcm_ready.load(std::memory_order_acquire),
                        true)) {
                    ++ready_active_captures;
                }
                // Include an open-but-not-yet-audible stream so pull() keeps
                // draining its FIFO. pull().active remains false until real
                // PCM arrives, so the engine keeps rendering the endpoint.
                if (capture_state != WindowAudioCaptureState::capturing)
                    continue;
                const std::uint32_t generation = slot.generation.load(std::memory_order_acquire);
                if (generation == 0 || result.count >= result.handles.size()) continue;
                result.handles[result.count++] = {static_cast<std::uint32_t>(index), generation};
            }
            result.required_active_captures =
                required_active_capture_count_.load(
                    std::memory_order_relaxed);
            result.ready_active_captures = ready_active_captures;
            result.coverage_complete = window_audio_coverage_is_complete(
                structural_coverage_complete_.load(
                    std::memory_order_relaxed) &&
                    !stop_requested_.load(std::memory_order_relaxed) &&
                    !result.endpoint_fallback_requested &&
                    validated_coverage_policy_revision_.load(
                        std::memory_order_relaxed) ==
                        coverage_policy_revision_.load(
                            std::memory_order_relaxed),
                result.required_active_captures,
                result.ready_active_captures);
            const std::uint64_t after = realtime_sequence_.load(std::memory_order_acquire);
            if (before == after && (after & 1U) == 0) {
                result.sequence = after;
                return result;
            }
        }
        // Slot fields are individually atomic, so even a highly contended
        // fallback remains memory-safe. Returning no handles prevents mixing a
        // half-applied source-set transition in the audio callback.
        return {};
    }

    [[nodiscard]] WindowAudioSnapshot snapshot() const override {
        std::lock_guard lock(snapshot_mutex_);
        return snapshot_;
    }

    [[nodiscard]] WindowAudioDiagnostics diagnostics() const override {
        WindowAudioDiagnostics result{};
        result.supported = true;
        result.running = running();
        result.discovery_passes = discovery_passes_.load(std::memory_order_relaxed);
        result.sessions_seen = sessions_seen_.load(std::memory_order_relaxed);
        result.candidates_seen = candidates_seen_.load(std::memory_order_relaxed);
        result.capture_start_failures = capture_start_failures_.load(std::memory_order_relaxed);
        result.unsupported_formats = unsupported_formats_.load(std::memory_order_relaxed);
        result.uncovered_active_sessions =
            uncovered_active_sessions_.load(std::memory_order_relaxed);
        result.fifo_overruns = fifo_overruns_.load(std::memory_order_relaxed);
        result.fifo_underruns = fifo_underruns_.load(std::memory_order_relaxed);
        result.captured_frames = captured_frames_.load(std::memory_order_relaxed);
        result.duplicated_mono_frames = duplicated_mono_frames_.load(std::memory_order_relaxed);
        result.mmcss_registration_failures =
            mmcss_registration_failures_.load(std::memory_order_relaxed);
        const WindowAudioRealtimeSnapshot realtime = realtime_snapshot();
        result.required_active_captures =
            realtime.required_active_captures;
        result.ready_active_captures = realtime.ready_active_captures;
        result.coverage_complete = realtime.coverage_complete;
        result.endpoint_fallback_requested =
            realtime.endpoint_fallback_requested;
        for (const auto& slot : slots_) {
            if (window_audio_slot_is_renderable(
                    slot.active.load(std::memory_order_acquire),
                    slot.capture_state.load(std::memory_order_acquire),
                    slot.pcm_ready.load(std::memory_order_acquire),
                    slot.session_active.load(std::memory_order_acquire))) {
                ++result.active_slots;
            }
        }
        {
            std::lock_guard lock(error_mutex_);
            copy_text(result.coverage_detail, coverage_detail_);
            copy_text(result.last_error, last_error_);
        }
        return result;
    }

private:
    [[nodiscard]] static WindowAudioConfig sanitize_config(
        const WindowAudioConfig& requested_config) {
        WindowAudioConfig result = requested_config;
        result.max_applications =
            std::clamp<std::size_t>(result.max_applications, 1,
                                    kMaximumWindowAudioApplications);
        result.display_calibration_count =
            std::min(result.display_calibration_count,
                     result.display_calibrations.size());
        result.source_rule_count =
            std::min(result.source_rule_count, result.source_rules.size());
        result.refresh_interval_ms =
            std::clamp<std::uint32_t>(result.refresh_interval_ms, 10, 5'000);
        result.stereo_spread = std::clamp(result.stereo_spread, 0.0F, 1.0F);
        if (result.excluded_process_id == 0)
            result.excluded_process_id = GetCurrentProcessId();
        return result;
    }

    [[nodiscard]] static bool requires_restart(
        const WindowAudioConfig& current,
        const WindowAudioConfig& requested) noexcept {
        return current.discovery_endpoint_id !=
                   requested.discovery_endpoint_id ||
               current.max_applications != requested.max_applications ||
               current.excluded_process_id != requested.excluded_process_id;
    }

    [[nodiscard]] static bool coverage_policy_changed(
        const WindowAudioConfig& current,
        const WindowAudioConfig& requested) noexcept {
        if (current.source_rule_count != requested.source_rule_count)
            return true;
        for (std::size_t index = 0;
             index < current.source_rule_count; ++index) {
            const auto& current_rule = current.source_rules[index];
            const auto& requested_rule = requested.source_rules[index];
            if (current_rule.enabled != requested_rule.enabled ||
                !ascii_iequals(current_rule.application_id,
                               requested_rule.application_id)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static const WindowAudioSourceRule* rule_for(
        const WindowAudioConfig& config,
        std::string_view application_id) noexcept {
        for (std::size_t index = 0; index < config.source_rule_count; ++index) {
            if (ascii_iequals(config.source_rules[index].application_id,
                              application_id)) {
                return &config.source_rules[index];
            }
        }
        return nullptr;
    }

    void set_error(std::string error) {
        std::lock_guard lock(error_mutex_);
        last_error_ = std::move(error);
    }

    void clear_error() {
        std::lock_guard lock(error_mutex_);
        last_error_.clear();
    }

    void set_coverage_detail(std::string detail) {
        std::lock_guard lock(error_mutex_);
        coverage_detail_ = std::move(detail);
    }

    void publish_coverage_state(
        bool structurally_complete,
        std::size_t required_active_captures) noexcept {
        if (structural_coverage_complete_.load(
                std::memory_order_acquire) == structurally_complete &&
            required_active_capture_count_.load(
                std::memory_order_acquire) ==
                required_active_captures) {
            return;
        }
        realtime_sequence_.fetch_add(1, std::memory_order_acq_rel);
        required_active_capture_count_.store(
            required_active_captures, std::memory_order_relaxed);
        structural_coverage_complete_.store(
            structurally_complete, std::memory_order_relaxed);
        realtime_sequence_.fetch_add(1, std::memory_order_release);
    }

    void request_slot_stop(CaptureSlot& slot) noexcept {
        realtime_sequence_.fetch_add(1, std::memory_order_acq_rel);
        slot.active.store(false, std::memory_order_release);
        realtime_sequence_.fetch_add(1, std::memory_order_release);
        if (slot.capture_thread.joinable())
            slot.capture_thread.request_stop();
    }

    void finish_slot_deactivation(CaptureSlot& slot) noexcept {
        if (slot.capture_thread.joinable())
            slot.capture_thread.join();
        // The audio callback never waits. Slot recycling is rare and happens
        // on the discovery thread, which waits for any in-flight RT reader
        // before resetting the resampler's consumer-side phase/history.
        while (slot.realtime_readers.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
        slot.capture_state.store(WindowAudioCaptureState::inactive, std::memory_order_release);
        slot.pcm_ready.store(false, std::memory_order_release);
        slot.session_active.store(false, std::memory_order_release);
        const std::uint64_t inactive_pcm_epoch =
            slot.inactive_pcm_epoch.load(std::memory_order_acquire);
        slot.validated_inactive_pcm_epoch.store(
            inactive_pcm_epoch, std::memory_order_release);
        slot.inactive_pcm_confirmed_external.store(
            false, std::memory_order_release);
        slot.process_id.store(0, std::memory_order_release);
        slot.sample_rate.store(0, std::memory_order_relaxed);
        slot.channel_count.store(0, std::memory_order_relaxed);
        slot.resampler.reset();
        slot.resampler.set_nominal_ratio(1.0F);
        slot.metadata = {};
        slot.missed_discovery_passes = 0;
    }

    void deactivate_slot(CaptureSlot& slot) noexcept {
        request_slot_stop(slot);
        finish_slot_deactivation(slot);
    }

    void assign_slot(CaptureSlot& slot, const SessionCandidate& candidate,
                     const std::vector<DisplayInfo>& displays,
                     const ProcessTree& process_tree,
                     const WindowAudioConfig& config) {
        deactivate_slot(slot);
        std::uint32_t generation = slot.generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (generation == 0) {
            slot.generation.store(1, std::memory_order_release);
            generation = 1;
        }
        slot.metadata.session = candidate;
        slot.process_id.store(candidate.process_id, std::memory_order_release);
        slot.pcm_ready.store(false, std::memory_order_release);
        slot.session_active.store(false, std::memory_order_release);
        // Placement belongs to the capture generation. Publish it before the
        // handle becomes visible to the audio callback so a fast first packet
        // can never reuse coordinates from the previous slot occupant.
        update_slot_metadata(slot, candidate, displays, process_tree, config);
        slot.capture_state.store(WindowAudioCaptureState::activating, std::memory_order_release);
        realtime_sequence_.fetch_add(1, std::memory_order_acq_rel);
        slot.active.store(true, std::memory_order_release);
        realtime_sequence_.fetch_add(1, std::memory_order_release);
        slot.capture_thread = std::jthread(
            [this, &slot, process_id = candidate.process_id, generation](std::stop_token stop_token) {
                capture_loop(slot, process_id, generation, stop_token);
            });
    }

    void restart_failed_capture(CaptureSlot& slot) {
        if (slot.capture_thread.joinable()) slot.capture_thread.join();
        const std::uint32_t process_id = slot.process_id.load(std::memory_order_acquire);
        const std::uint32_t generation = slot.generation.load(std::memory_order_acquire);
        if (process_id == 0 || !slot.active.load(std::memory_order_acquire)) return;
        slot.pcm_ready.store(false, std::memory_order_release);
        slot.capture_state.store(WindowAudioCaptureState::activating, std::memory_order_release);
        slot.capture_thread = std::jthread(
            [this, &slot, process_id, generation](std::stop_token stop_token) {
                capture_loop(slot, process_id, generation, stop_token);
            });
    }

    void reconcile_inactive_pcm_fallback(CaptureSlot& slot) noexcept {
        const std::uint64_t observed =
            slot.inactive_pcm_epoch.load(std::memory_order_acquire);
        const std::uint64_t validated =
            slot.validated_inactive_pcm_epoch.load(
                std::memory_order_acquire);
        if (observed == validated) return;

        if (slot.session_active.load(std::memory_order_acquire)) {
            // Coverage was invalidated before reconciliation. Fresh PCM was
            // also required by update_slot_metadata(), so acknowledging this
            // epoch cannot authorize the pre-discovery packet.
            slot.validated_inactive_pcm_epoch.store(
                observed, std::memory_order_release);
            slot.inactive_pcm_confirmed_external.store(
                false, std::memory_order_release);
            return;
        }

        // One complete discovery pass has now confirmed that the application
        // is not rendering into the selected source endpoint. Its
        // process-loopback stream may legitimately contain audio from another
        // endpoint; discard that audio until the endpoint session changes,
        // rather than forcing the whole renderer into a permanent stereo
        // fallback.
        slot.validated_inactive_pcm_epoch.store(
            observed, std::memory_order_release);
        slot.inactive_pcm_confirmed_external.store(
            true, std::memory_order_release);
    }

    void capture_loop(CaptureSlot& slot, std::uint32_t process_id, std::uint32_t generation,
                      const std::stop_token& stop_token) {
        // Declared before every COM smart pointer so their Release calls run
        // before the matching CoUninitialize on every early-return path.
        ScopedComApartment apartment(COINIT_MULTITHREADED);
        if (!apartment.usable()) {
            capture_start_failures_.fetch_add(1, std::memory_order_relaxed);
            slot.capture_state.store(WindowAudioCaptureState::failed, std::memory_order_release);
            set_error(hresult_text("initialize process-loopback COM apartment",
                                   apartment.result()));
            return;
        }

        ScopedMmcssRegistration mmcss_registration;
        if (!mmcss_registration.registered()) {
            mmcss_registration_failures_.fetch_add(
                1, std::memory_order_relaxed);
        }

        HRESULT result = S_OK;
        ComPtr<IAudioClient> audio_client = activate_process_loopback(process_id, stop_token, result);
        if (audio_client == nullptr) {
            if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                capture_start_failures_.fetch_add(1, std::memory_order_relaxed);
                slot.capture_state.store(WindowAudioCaptureState::failed, std::memory_order_release);
                set_error(hresult_text("activate process-loopback audio client", result));
            }
            return;
        }

        WAVEFORMATEX* allocated_mix_format = nullptr;
        result = audio_client->GetMixFormat(&allocated_mix_format);
        const bool native_float_48k =
            SUCCEEDED(result) && is_float_format(allocated_mix_format) &&
            allocated_mix_format->nSamplesPerSec == 48'000 &&
            allocated_mix_format->nChannels <= 2;
        const WAVEFORMATEXTENSIBLE requested_format = canonical_stereo_float_48k();
        const WAVEFORMATEX* capture_format =
            native_float_48k ? allocated_mix_format : &requested_format.Format;

        constexpr DWORD stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        result = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, capture_format, nullptr);
        const bool capture_is_float = is_float_format(capture_format);
        const std::uint32_t capture_sample_rate = capture_format->nSamplesPerSec;
        const std::uint32_t capture_channels = capture_format->nChannels;
        if (allocated_mix_format != nullptr) CoTaskMemFree(allocated_mix_format);
        if (FAILED(result) || !capture_is_float) {
            unsupported_formats_.fetch_add(1, std::memory_order_relaxed);
            slot.capture_state.store(WindowAudioCaptureState::unsupported_format, std::memory_order_release);
            set_error(FAILED(result)
                          ? hresult_text("initialize 48 kHz float process-loopback stream", result)
                          : "process-loopback returned a non-float format");
            return;
        }

        HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (audio_event == nullptr) {
            capture_start_failures_.fetch_add(1, std::memory_order_relaxed);
            slot.capture_state.store(WindowAudioCaptureState::failed, std::memory_order_release);
            set_error("create process-loopback event failed");
            return;
        }
        result = audio_client->SetEventHandle(audio_event);
        ComPtr<IAudioCaptureClient> capture_client;
        if (SUCCEEDED(result)) result = audio_client->GetService(IID_PPV_ARGS(&capture_client));
        if (SUCCEEDED(result)) result = audio_client->Start();
        if (FAILED(result)) {
            CloseHandle(audio_event);
            capture_start_failures_.fetch_add(1, std::memory_order_relaxed);
            slot.capture_state.store(WindowAudioCaptureState::failed, std::memory_order_release);
            set_error(hresult_text("start process-loopback stream", result));
            return;
        }

        const std::uint32_t channels = capture_channels;
        slot.sample_rate.store(capture_sample_rate, std::memory_order_relaxed);
        slot.channel_count.store(channels, std::memory_order_relaxed);
        slot.capture_state.store(WindowAudioCaptureState::capturing, std::memory_order_release);
        std::array<StereoFrame, kCaptureScratchFrames> scratch{};

        while (!stop_token.stop_requested() &&
               slot.generation.load(std::memory_order_acquire) == generation &&
               slot.active.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(audio_event, 100);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
                set_error("wait for process-loopback audio failed");
                slot.capture_state.store(WindowAudioCaptureState::failed, std::memory_order_release);
                break;
            }

            UINT32 packet_frames = 0;
            result = capture_client->GetNextPacketSize(&packet_frames);
            while (SUCCEEDED(result) && packet_frames != 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                result = capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(result)) break;
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    slot.pcm_ready.store(false, std::memory_order_release);
                    slot.resampler.request_rebase();
                }
                std::size_t consumed = 0;
                while (consumed < frames) {
                    const std::size_t block =
                        std::min<std::size_t>(scratch.size(), static_cast<std::size_t>(frames) - consumed);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                        std::fill_n(scratch.data(), block, StereoFrame{});
                    } else {
                        const auto* samples = reinterpret_cast<const float*>(data);
                        for (std::size_t frame = 0; frame < block; ++frame) {
                            const std::size_t source_frame = consumed + frame;
                            const float left = samples[source_frame * channels];
                            const float right = channels == 1 ? left : samples[source_frame * channels + 1];
                            scratch[frame] = {left, right};
                        }
                        if (channels == 1) {
                            duplicated_mono_frames_.fetch_add(block, std::memory_order_relaxed);
                        }
                    }
                    const bool session_active_before =
                        slot.session_active.load(std::memory_order_acquire);
                    const bool block_is_non_silent =
                        (flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 &&
                        std::any_of(
                            scratch.begin(),
                            scratch.begin() +
                                static_cast<std::ptrdiff_t>(block),
                            [](const StereoFrame& frame) {
                                return frame.left != 0.0F ||
                                       frame.right != 0.0F;
                            });
                    std::size_t pushed = 0;
                    if (block_is_non_silent) {
                        const bool session_active_after =
                            slot.session_active.load(std::memory_order_acquire);
                        const bool packet_crossed_inactive_state =
                            !session_active_before || !session_active_after;
                        if (packet_crossed_inactive_state) {
                            // A capture pre-armed from an inactive source
                            // session may receive process-tree PCM before the
                            // next endpoint discovery pass. Revoke the whole
                            // process set immediately, but never authorize this
                            // packet itself: only discovery may set
                            // session_active.
                            slot.pcm_ready.store(false,
                                                 std::memory_order_release);
                            // The transition can race update_slot_metadata().
                            // Rebase after push so even a packet that crossed
                            // inactive -> active cannot survive the discovery
                            // thread's earlier FIFO boundary and become audible
                            // after the veto is acknowledged.
                            slot.resampler.request_rebase();
                            if (!slot.inactive_pcm_confirmed_external.load(
                                    std::memory_order_acquire)) {
                                std::uint64_t observed =
                                    slot.inactive_pcm_epoch.load(
                                        std::memory_order_acquire);
                                const std::uint64_t validated =
                                    slot.validated_inactive_pcm_epoch.load(
                                        std::memory_order_acquire);
                                if (observed == validated &&
                                    slot.inactive_pcm_epoch
                                        .compare_exchange_strong(
                                            observed, observed + 1,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                                    refresh_revision_.fetch_add(
                                        1, std::memory_order_release);
                                    refresh_cv_.notify_one();
                                }
                            }
                        } else {
                            pushed =
                                slot.resampler.push(scratch.data(), block);
                            if (pushed != 0) {
                            // Process-loopback follows the process tree across
                            // all render endpoints. Only a block bracketed by
                            // source-session authority may prove readiness.
                                slot.pcm_ready.store(
                                    true, std::memory_order_release);
                            }
                        }
                    } else {
                        pushed = slot.resampler.push(scratch.data(), block);
                    }
                    if (pushed < block) {
                        fifo_overruns_.fetch_add(block - pushed, std::memory_order_relaxed);
                    }
                    captured_frames_.fetch_add(pushed, std::memory_order_relaxed);
                    consumed += block;
                }
                const HRESULT release_result =
                    capture_client->ReleaseBuffer(frames);
                if (FAILED(release_result)) {
                    result = release_result;
                    break;
                }
                result = capture_client->GetNextPacketSize(&packet_frames);
            }
            if (FAILED(result)) {
                slot.capture_state.store(WindowAudioCaptureState::failed, std::memory_order_release);
                set_error(hresult_text("read process-loopback packet", result));
                break;
            }
        }
        audio_client->Stop();
        CloseHandle(audio_event);
        if (slot.generation.load(std::memory_order_acquire) == generation &&
            slot.capture_state.load(std::memory_order_acquire) == WindowAudioCaptureState::capturing) {
            slot.capture_state.store(WindowAudioCaptureState::inactive, std::memory_order_release);
            slot.pcm_ready.store(false, std::memory_order_release);
        }
    }

    void update_slot_metadata(CaptureSlot& slot,
                              const SessionCandidate& candidate,
                              const std::vector<DisplayInfo>& displays,
                              const ProcessTree& process_tree,
                              const WindowAudioConfig& config) {
        slot.metadata.session = candidate;
        const bool was_session_active =
            slot.session_active.load(std::memory_order_acquire);
        if (candidate.session_active != was_session_active) {
            slot.inactive_pcm_confirmed_external.store(
                false, std::memory_order_release);
        }
        if (candidate.session_active && !was_session_active) {
            // Discard the low-latency tail that may have been captured while
            // the process was rendering on another endpoint. A fresh
            // non-silent source-endpoint packet must prime this activation.
            slot.pcm_ready.store(false, std::memory_order_release);
            slot.resampler.request_rebase();
        }
        slot.session_active.store(candidate.session_active,
                                  std::memory_order_release);
        if (!candidate.session_active) {
            slot.pcm_ready.store(false, std::memory_order_release);
        }
        slot.metadata.window = find_process_window(candidate.process_id, process_tree);
        const WindowAudioSourceRule* rule =
            rule_for(config, candidate.application_id);
        const float spread =
            rule != nullptr ? rule->stereo_spread : config.stereo_spread;
        const float gain_db = rule != nullptr ? std::clamp(rule->gain_db, -60.0F, 12.0F) : 0.0F;
        const std::string_view fallback_id = rule != nullptr ? rule->fallback_display_id : std::string_view{};
        const DisplayInfo* display =
            display_for_window(displays, slot.metadata.window, fallback_id);
        if (display == nullptr) {
            slot.metadata.display_id.clear();
            slot.metadata.placement = {
                {-0.15F, config.listener_position_m.y,
                 config.listener_position_m.z + 0.85F},
                {0.15F, config.listener_position_m.y,
                 config.listener_position_m.z + 0.85F}};
        } else {
            slot.metadata.display_id = display->id;
            slot.metadata.placement = calculate_placement(
                displays, *display, slot.metadata.window, spread,
                config.follow_window_position, config.placement_mode);
        }
        slot.metadata.gain_db = gain_db;
        slot.placement.store(slot.metadata.placement, std::pow(10.0F, gain_db / 20.0F));
    }

    void reconcile_sessions(std::vector<SessionCandidate>& candidates,
                            const std::vector<DisplayInfo>& displays,
                            const ProcessTree& process_tree,
                            const WindowAudioConfig& config) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const SessionCandidate& candidate) {
                             if (capture_tree_intersects_excluded_process(
                                     process_tree,
                                     config.excluded_process_id,
                                     candidate.process_id)) {
                                 return true;
                             }
                             const WindowAudioSourceRule* rule =
                                 rule_for(config, candidate.application_id);
                             return rule != nullptr && !rule->enabled;
                         }),
                         candidates.end());

        std::vector<bool> claimed(candidates.size(), false);
        for (auto& slot : slots_) {
            if (!slot.active.load(std::memory_order_acquire)) continue;
            const std::uint32_t process_id = slot.process_id.load(std::memory_order_acquire);
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const SessionCandidate& candidate) {
                                                return candidate.process_id == process_id;
                                            });
            if (found == candidates.end()) {
                // Keep the armed capture around for the bounded removal grace,
                // but revoke its authority immediately. Otherwise a ready A
                // can satisfy the ready-count of a newly required, unprimed B
                // and make a partial process set look complete.
                slot.metadata.session.session_active = false;
                slot.session_active.store(false, std::memory_order_release);
                slot.pcm_ready.store(false, std::memory_order_release);
                slot.resampler.request_rebase();
                if (++slot.missed_discovery_passes >= kSessionMissesBeforeRemoval) deactivate_slot(slot);
                continue;
            }
            slot.missed_discovery_passes = 0;
            claimed[static_cast<std::size_t>(found - candidates.begin())] = true;
            update_slot_metadata(slot, *found, displays, process_tree, config);

            const auto state = slot.capture_state.load(std::memory_order_acquire);
            if (state == WindowAudioCaptureState::failed &&
                discovery_passes_.load(std::memory_order_relaxed) % 5 == 0) {
                restart_failed_capture(slot);
            }
        }

        std::size_t active_count = 0;
        for (const auto& slot : slots_) {
            if (slot.active.load(std::memory_order_acquire)) ++active_count;
        }
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (claimed[index]) continue;
            auto destination = slots_.end();
            if (active_count < config.max_applications) {
                destination = std::find_if(
                    slots_.begin(), slots_.end(), [](const CaptureSlot& slot) {
                        return !slot.active.load(std::memory_order_acquire);
                    });
            } else if (candidates[index].session_active) {
                // Inactive, non-expired sessions are deliberately pre-armed,
                // but they must not starve a newly active ninth application.
                destination = std::find_if(
                    slots_.begin(), slots_.end(), [](const CaptureSlot& slot) {
                        return slot.active.load(std::memory_order_acquire) &&
                               !slot.metadata.session.session_active;
                    });
            }
            if (destination == slots_.end()) continue;
            const bool replacing =
                destination->active.load(std::memory_order_acquire);
            assign_slot(*destination, candidates[index], displays,
                        process_tree, config);
            if (!replacing) ++active_count;
        }
    }

    void publish_snapshot(const std::vector<DisplayInfo>& displays) {
        WindowAudioSnapshot next{};
        next.sequence = snapshot_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        next.display_count = std::min(displays.size(), next.displays.size());
        for (std::size_t index = 0; index < next.display_count; ++index) {
            const auto& source = displays[index];
            auto& destination = next.displays[index];
            copy_text(destination.id, source.id);
            copy_text(destination.name, source.name);
            destination.primary = source.primary;
            destination.bounds_px = from_rect(source.bounds);
            destination.center_m = source.center_m;
            destination.width_m = source.width_m;
            destination.height_m = source.height_m;
            destination.orientation = source.orientation;
        }

        for (std::size_t slot_index = 0; slot_index < slots_.size(); ++slot_index) {
            const auto& slot = slots_[slot_index];
            if (!slot.active.load(std::memory_order_acquire) ||
                next.window_source_count >= next.window_sources.size()) {
                continue;
            }
            auto& destination = next.window_sources[next.window_source_count++];
            destination.handle = {
                static_cast<std::uint32_t>(slot_index),
                slot.generation.load(std::memory_order_acquire),
            };
            char source_id[48]{};
            std::snprintf(source_id, sizeof(source_id), "window:%u:%u", destination.handle.slot,
                          destination.handle.generation);
            copy_text(destination.source_id, source_id);
            copy_text(destination.application_id, slot.metadata.session.application_id);
            copy_text(destination.application_name, slot.metadata.session.application_name);
            copy_text(destination.window_title, wide_to_utf8(slot.metadata.window.title));
            copy_text(destination.session_id, slot.metadata.session.session_id);
            copy_text(destination.display_id, slot.metadata.display_id);
            destination.process_id = slot.metadata.session.process_id;
            destination.window_handle = reinterpret_cast<std::uint64_t>(slot.metadata.window.handle);
            destination.window_bounds_px = from_rect(slot.metadata.window.bounds);
            destination.left_position_m = slot.metadata.placement.left;
            destination.right_position_m = slot.metadata.placement.right;
            destination.gain_db = slot.metadata.gain_db;
            destination.sample_rate = slot.sample_rate.load(std::memory_order_relaxed);
            destination.channel_count = slot.channel_count.load(std::memory_order_relaxed);
            destination.capture_state = slot.capture_state.load(std::memory_order_acquire);
            destination.active = window_audio_slot_is_renderable(
                slot.active.load(std::memory_order_acquire),
                destination.capture_state,
                slot.pcm_ready.load(std::memory_order_acquire),
                slot.session_active.load(std::memory_order_acquire));
        }
        {
            std::lock_guard lock(snapshot_mutex_);
            snapshot_ = std::move(next);
        }
    }

    void publish_empty_snapshot() noexcept {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_ = {};
    }

    void discovery_loop() {
        ScopedComApartment apartment(COINIT_MULTITHREADED);
        if (!apartment.usable()) {
            set_error(hresult_text(
                "initialize window-audio discovery COM apartment",
                apartment.result()));
            running_.store(false, std::memory_order_release);
            return;
        }

        while (!stop_requested_.load(std::memory_order_acquire)) {
            WindowAudioConfig config{};
            std::uint64_t refresh_revision = 0;
            std::uint64_t coverage_policy_revision = 0;
            {
                std::lock_guard lock(config_mutex_);
                config = config_;
                refresh_revision =
                    refresh_revision_.load(std::memory_order_relaxed);
                coverage_policy_revision =
                    coverage_policy_revision_.load(
                        std::memory_order_relaxed);
            }
            const ProcessTree process_tree = read_process_tree();
            std::vector<DisplayInfo> displays = enumerate_displays(config);
            std::string discovery_error;
            SessionEnumeration enumeration =
                enumerate_sessions(config, discovery_error);
            std::vector<SessionCandidate> candidates =
                std::move(enumeration.candidates);
            discovery_passes_.fetch_add(1, std::memory_order_relaxed);
            sessions_seen_.fetch_add(
                candidates.size() +
                    enumeration.uncapturable_active_sessions,
                std::memory_order_relaxed);
            const bool discovery_succeeded =
                discovery_error.empty() && enumeration.complete;
            if (!discovery_error.empty()) set_error(std::move(discovery_error));
            std::size_t uncovered_active_sessions =
                enumeration.uncapturable_active_sessions;
            std::string coverage_detail =
                std::move(enumeration.coverage_detail);
            for (const SessionCandidate& candidate : candidates) {
                if (!candidate.session_active) continue;
                const WindowAudioSourceRule* rule =
                    rule_for(config, candidate.application_id);
                // The endpoint loopback client itself is exposed by Windows as
                // an active session owned by the engine. It is capture
                // bookkeeping, not programme audio. An ancestor/descendant
                // process tree remains a real feedback risk and is still
                // treated as uncovered below.
                const bool loop_risk =
                    window_audio_excluded_session_blocks_coverage(
                        candidate.process_id, config.excluded_process_id,
                        capture_tree_intersects_excluded_process(
                            process_tree, config.excluded_process_id,
                            candidate.process_id));
                const bool disabled =
                    rule != nullptr && !rule->enabled;
                if (loop_risk || disabled) {
                    ++uncovered_active_sessions;
                    if (coverage_detail.empty()) {
                        coverage_detail =
                            loop_risk ? "feedback-loop-risk:"
                                      : "source-rule-disabled:";
                        coverage_detail += candidate.application_name;
                        coverage_detail += ":pid=" +
                            std::to_string(candidate.process_id);
                    }
                }
            }
            uncovered_active_sessions_.store(
                uncovered_active_sessions, std::memory_order_relaxed);
            set_coverage_detail(std::move(coverage_detail));
            // Filter hazardous roots before parent/child deduplication. If a
            // shell owns one session while another legitimate child owns a
            // second, removing the shell only after deduplication would also
            // discard that safe sibling even though its capture tree does not
            // contain the engine.
            candidates.erase(
                std::remove_if(
                    candidates.begin(), candidates.end(),
                    [&](const SessionCandidate& candidate) {
                        return capture_tree_intersects_excluded_process(
                            process_tree, config.excluded_process_id,
                            candidate.process_id);
                    }),
                candidates.end());
            deduplicate_process_trees(candidates, process_tree);
            candidates_seen_.fetch_add(candidates.size(), std::memory_order_relaxed);

            std::vector<std::uint32_t> required_active_roots;
            required_active_roots.reserve(candidates.size());
            for (const SessionCandidate& candidate : candidates) {
                const WindowAudioSourceRule* rule =
                    rule_for(config, candidate.application_id);
                if (candidate.session_active && rule != nullptr &&
                    !rule->enabled) {
                    // Deduplication can promote an active allowed child into
                    // an inactive disabled parent. Re-check the effective root
                    // so that tree cannot disappear when another application
                    // switches the engine away from the endpoint mix.
                    ++uncovered_active_sessions;
                }
                if (candidate.session_active &&
                    (rule == nullptr || rule->enabled)) {
                    required_active_roots.push_back(candidate.process_id);
                }
            }
            std::sort(required_active_roots.begin(),
                      required_active_roots.end());
            const bool preliminary_coverage =
                discovery_succeeded &&
                uncovered_active_sessions == 0 &&
                required_active_roots.size() <=
                    config.max_applications;
            if (!preliminary_coverage ||
                required_active_roots != published_active_roots_) {
                // A same-sized A -> B replacement is still a coverage change.
                // Invalidate before recycling a slot so the callback cannot
                // briefly render A's process set while B is audible only in
                // the endpoint mix.
                publish_coverage_state(
                    false, required_active_roots.size());
            }
            reconcile_sessions(candidates, displays, process_tree, config);
            for (auto& slot : slots_) {
                if (slot.active.load(std::memory_order_acquire)) {
                    reconcile_inactive_pcm_fallback(slot);
                }
            }

            const bool every_required_root_assigned =
                std::all_of(
                    required_active_roots.begin(),
                    required_active_roots.end(),
                    [&](std::uint32_t process_id) {
                        return std::any_of(
                            slots_.begin(), slots_.end(),
                            [&](const CaptureSlot& slot) {
                                return slot.active.load(
                                           std::memory_order_acquire) &&
                                       slot.process_id.load(
                                           std::memory_order_acquire) ==
                                           process_id &&
                                       slot.session_active.load(
                                           std::memory_order_acquire) &&
                                       slot.capture_state.load(
                                           std::memory_order_acquire) ==
                                           WindowAudioCaptureState::capturing;
                            });
                    });
            const std::size_t audible_required_roots =
                static_cast<std::size_t>(std::count_if(
                    required_active_roots.begin(),
                    required_active_roots.end(),
                    [&](std::uint32_t process_id) {
                        return std::any_of(
                            slots_.begin(), slots_.end(),
                            [&](const CaptureSlot& slot) {
                                return slot.process_id.load(
                                           std::memory_order_acquire) ==
                                           process_id &&
                                       window_audio_slot_is_renderable(
                                           slot.active.load(
                                               std::memory_order_acquire),
                                           slot.capture_state.load(
                                               std::memory_order_acquire),
                                           slot.pcm_ready.load(
                                               std::memory_order_acquire),
                                           slot.session_active.load(
                                               std::memory_order_acquire));
                            });
                    }));
            publish_coverage_state(
                preliminary_coverage &&
                    every_required_root_assigned,
                audible_required_roots);
            published_active_roots_ = std::move(required_active_roots);
            if (!stop_requested_.load(std::memory_order_acquire)) {
                validated_coverage_policy_revision_.store(
                    coverage_policy_revision,
                    std::memory_order_release);
            }
            const bool capture_failed = std::any_of(
                slots_.begin(), slots_.end(), [](const CaptureSlot& slot) {
                    if (!slot.active.load(std::memory_order_acquire))
                        return false;
                    const auto state =
                        slot.capture_state.load(std::memory_order_acquire);
                    return state == WindowAudioCaptureState::failed ||
                           state ==
                               WindowAudioCaptureState::unsupported_format;
                });
            if (discovery_succeeded && !capture_failed) clear_error();
            publish_snapshot(displays);

            std::unique_lock lock(refresh_mutex_);
            refresh_cv_.wait_for(
                lock, std::chrono::milliseconds(config.refresh_interval_ms),
                [&] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           refresh_revision_.load(std::memory_order_acquire) !=
                               refresh_revision;
                });
        }
    }

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex config_mutex_;
    WindowAudioConfig config_{};
    std::atomic<std::uint64_t> refresh_revision_{};
    std::atomic<std::uint64_t> coverage_policy_revision_{};
    std::atomic<std::uint64_t> validated_coverage_policy_revision_{};
    std::array<CaptureSlot, kMaximumWindowAudioApplications> slots_{};
    std::thread discovery_thread_;
    std::atomic<bool> running_{};
    std::atomic<bool> stop_requested_{};
    std::mutex refresh_mutex_;
    std::condition_variable refresh_cv_;

    mutable std::mutex snapshot_mutex_;
    WindowAudioSnapshot snapshot_{};
    std::atomic<std::uint64_t> snapshot_sequence_{};
    std::atomic<std::uint64_t> realtime_sequence_{};
    std::atomic<bool> structural_coverage_complete_{};
    std::atomic<std::size_t> required_active_capture_count_{};
    std::vector<std::uint32_t> published_active_roots_{};
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::string coverage_detail_;

    std::atomic<std::uint64_t> discovery_passes_{};
    std::atomic<std::uint64_t> sessions_seen_{};
    std::atomic<std::uint64_t> candidates_seen_{};
    std::atomic<std::uint64_t> capture_start_failures_{};
    std::atomic<std::uint64_t> unsupported_formats_{};
    std::atomic<std::size_t> uncovered_active_sessions_{};
    std::atomic<std::uint64_t> fifo_overruns_{};
    std::atomic<std::uint64_t> fifo_underruns_{};
    std::atomic<std::uint64_t> captured_frames_{};
    std::atomic<std::uint64_t> duplicated_mono_frames_{};
    std::atomic<std::uint64_t> mmcss_registration_failures_{};
};

} // namespace

std::unique_ptr<IWindowAudioCapture> make_window_audio_capture() {
    return std::make_unique<WindowAudioCaptureWindows>();
}

} // namespace sound_spatializer

#else

namespace sound_spatializer {
namespace {

template <std::size_t Size>
void copy_stub_text(std::array<char, Size>& destination, std::string_view source) noexcept {
    static_assert(Size > 0);
    const std::size_t count = std::min(source.size(), Size - 1);
    if (count != 0) std::memcpy(destination.data(), source.data(), count);
    destination[count] = '\0';
}

class WindowAudioCaptureStub final : public IWindowAudioCapture {
public:
    bool start(const WindowAudioConfig&) override { return false; }
    bool reconfigure(const WindowAudioConfig&) override { return false; }
    void stop() noexcept override {}
    [[nodiscard]] bool running() const noexcept override { return false; }

    [[nodiscard]] WindowAudioPullResult pull(WindowAudioSlotHandle, StereoFrame* output,
                                              std::size_t frame_count) noexcept override {
        if (output != nullptr) std::fill_n(output, frame_count, StereoFrame{});
        return {};
    }

    [[nodiscard]] WindowAudioRealtimeSnapshot realtime_snapshot() const noexcept override { return {}; }

    [[nodiscard]] WindowAudioSnapshot snapshot() const override { return {}; }

    [[nodiscard]] WindowAudioDiagnostics diagnostics() const override {
        WindowAudioDiagnostics result{};
        copy_stub_text(result.last_error,
                       "Per-application process-loopback capture is available on Windows only");
        return result;
    }
};

} // namespace

std::unique_ptr<IWindowAudioCapture> make_window_audio_capture() {
    return std::make_unique<WindowAudioCaptureStub>();
}

} // namespace sound_spatializer

#endif
