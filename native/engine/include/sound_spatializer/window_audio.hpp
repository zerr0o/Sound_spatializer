#pragma once

#include "sound_spatializer/dsp.hpp"
#include "sound_spatializer/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sound_spatializer {

inline constexpr std::size_t kMaximumWindowAudioApplications = 8;
inline constexpr std::size_t kMaximumWindowAudioDisplays = 16;
inline constexpr std::size_t kMaximumWindowAudioSourceRules = 64;
inline constexpr std::size_t kWindowAudioApplicationIdBytes = 1'024;
inline constexpr std::size_t kWindowAudioDisplayIdBytes = 512;
inline constexpr std::uint64_t
    kWindowAudioInactivePcmFallbackQuietPeriodMs = 150;

struct WindowAudioSlotHandle {
    std::uint32_t slot{};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot < kMaximumWindowAudioApplications && generation != 0;
    }

    friend constexpr bool operator==(const WindowAudioSlotHandle&, const WindowAudioSlotHandle&) = default;
};

struct WindowAudioPixelBounds {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
};

// A calibrated display is a rectangular plane in engine world coordinates.
// orientation rotates the local +X (screen right), +Y (screen up) and +Z
// vectors. Display IDs are the Windows monitor device paths returned by
// QueryDisplayConfig whenever the OS exposes one.
struct WindowAudioDisplayCalibration {
    std::string display_id;
    Vec3f center_m{};
    float width_m{0.60F};
    float height_m{0.34F};
    Quaternionf orientation{};
};

struct WindowAudioSourceRule {
    std::string application_id;
    bool enabled{true};
    float gain_db{};
    float stereo_spread{0.72F};
    std::string fallback_display_id;
};

struct WindowAudioConfig {
    bool enabled{};
    // Empty selects the current default multimedia render endpoint for session
    // discovery. Capture itself uses Windows process-loopback and is not tied
    // to the endpoint after a process has been selected.
    std::string discovery_endpoint_id;
    Vec3f listener_position_m{0.0F, 1.20F, 0.0F};
    std::uint32_t excluded_process_id{};
    std::uint32_t refresh_interval_ms{10};
    std::size_t max_applications{kMaximumWindowAudioApplications};
    float stereo_spread{0.72F};
    bool follow_window_position{true};
    std::array<WindowAudioDisplayCalibration, kMaximumWindowAudioDisplays> display_calibrations{};
    std::size_t display_calibration_count{};
    std::array<WindowAudioSourceRule, kMaximumWindowAudioSourceRules> source_rules{};
    std::size_t source_rule_count{};
};

enum class WindowAudioCaptureState : std::uint32_t {
    inactive = 0,
    activating = 1,
    capturing = 2,
    unsupported_format = 3,
    failed = 4,
};

[[nodiscard]] constexpr bool window_audio_slot_is_renderable(
    bool active, WindowAudioCaptureState state, bool pcm_ready,
    bool session_active) noexcept {
    return active && state == WindowAudioCaptureState::capturing &&
           pcm_ready && session_active;
}

struct WindowAudioRealtimePlacement {
    Vec3f left_position_m{};
    Vec3f right_position_m{};
    float gain_linear{1.0F};
};

struct WindowAudioPullResult {
    std::size_t received_frames{};
    WindowAudioRealtimePlacement placement{};
    bool active{};
};

struct WindowAudioRealtimeSnapshot {
    std::uint64_t sequence{};
    std::array<WindowAudioSlotHandle, kMaximumWindowAudioApplications> handles{};
    std::size_t count{};
    std::size_t required_active_captures{};
    std::size_t ready_active_captures{};
    // A non-silent packet reached a pre-armed capture whose source-endpoint
    // session is still inactive. This can only revoke process authority: the
    // packet itself remains inaudible until discovery confirms the session.
    bool endpoint_fallback_requested{};
    // True only when every active session currently discovered on the source
    // endpoint is represented by a ready process-loopback slot. False is the
    // fail-safe instruction to keep the complete endpoint mix authoritative:
    // mixing a partial process set would either mute uncovered applications or
    // duplicate the covered ones.
    bool coverage_complete{};
};

[[nodiscard]] constexpr bool window_audio_coverage_is_complete(
    bool discovery_complete, std::size_t required_active_captures,
    std::size_t ready_active_captures) noexcept {
    return discovery_complete &&
           required_active_captures == ready_active_captures;
}

[[nodiscard]] constexpr bool
window_audio_inactive_pcm_requires_endpoint_fallback(
    std::uint64_t observed_epoch,
    std::uint64_t validated_epoch) noexcept {
    return observed_epoch != validated_epoch;
}

[[nodiscard]] constexpr bool window_audio_inactive_pcm_epoch_is_stable(
    std::uint64_t observed_epoch,
    std::uint64_t discovery_epoch,
    std::uint64_t quiet_since_ms,
    std::uint64_t now_ms) noexcept {
    return observed_epoch == discovery_epoch &&
           now_ms >= quiet_since_ms &&
           now_ms - quiet_since_ms >=
               kWindowAudioInactivePcmFallbackQuietPeriodMs;
}

struct WindowAudioDisplaySnapshot {
    std::array<char, kWindowAudioDisplayIdBytes> id{};
    std::array<char, 128> name{};
    bool primary{};
    WindowAudioPixelBounds bounds_px{};
    Vec3f center_m{};
    float width_m{};
    float height_m{};
    Quaternionf orientation{};
};

struct WindowAudioSourceSnapshot {
    WindowAudioSlotHandle handle{};
    std::array<char, 48> source_id{};
    std::array<char, kWindowAudioApplicationIdBytes> application_id{};
    std::array<char, 192> application_name{};
    std::array<char, 512> window_title{};
    std::array<char, 256> session_id{};
    std::array<char, kWindowAudioDisplayIdBytes> display_id{};
    std::uint32_t process_id{};
    std::uint64_t window_handle{};
    WindowAudioPixelBounds window_bounds_px{};
    Vec3f left_position_m{};
    Vec3f right_position_m{};
    float gain_db{};
    std::uint32_t sample_rate{};
    std::uint32_t channel_count{};
    WindowAudioCaptureState capture_state{WindowAudioCaptureState::inactive};
    bool active{};
};

struct WindowAudioSnapshot {
    std::uint64_t sequence{};
    std::array<WindowAudioDisplaySnapshot, kMaximumWindowAudioDisplays> displays{};
    std::size_t display_count{};
    std::array<WindowAudioSourceSnapshot, kMaximumWindowAudioApplications> window_sources{};
    std::size_t window_source_count{};
};

struct WindowAudioDiagnostics {
    bool supported{};
    bool running{};
    std::uint64_t discovery_passes{};
    std::uint64_t sessions_seen{};
    std::uint64_t candidates_seen{};
    std::uint64_t capture_start_failures{};
    std::uint64_t unsupported_formats{};
    std::uint64_t fifo_overruns{};
    std::uint64_t fifo_underruns{};
    std::uint64_t captured_frames{};
    std::uint64_t duplicated_mono_frames{};
    std::uint64_t mmcss_registration_failures{};
    std::size_t active_slots{};
    std::array<char, 256> last_error{};
};

class IWindowAudioCapture {
public:
    virtual ~IWindowAudioCapture() = default;

    virtual bool start(const WindowAudioConfig& config) = 0;
    // Control thread only. Applies placement, gain, rules and calibration
    // changes without recycling active process-loopback streams. Returns false
    // when the capture is not running or when a structural setting
    // (discovery endpoint, application limit or excluded process) requires a
    // stop/start cycle.
    virtual bool reconfigure(const WindowAudioConfig& config) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;

    // Real-time safe: no allocation, COM call, lock, wait or logging. Missing
    // frames are zero-filled. received_frames reports how many frames came from
    // the per-application FIFO, while active distinguishes silence from a stale
    // slot generation.
    [[nodiscard]] virtual WindowAudioPullResult pull(WindowAudioSlotHandle handle,
                                                     StereoFrame* output,
                                                     std::size_t frame_count) noexcept = 0;
    [[nodiscard]] virtual WindowAudioRealtimeSnapshot realtime_snapshot() const noexcept = 0;

    // Control/UI thread only. These copies may take a short metadata mutex.
    [[nodiscard]] virtual WindowAudioSnapshot snapshot() const = 0;
    [[nodiscard]] virtual WindowAudioDiagnostics diagnostics() const = 0;
};

// Windows 10 build 20348 / Windows 11 and newer use process-loopback. Other
// platforms return a silent implementation whose diagnostics.supported is false.
[[nodiscard]] std::unique_ptr<IWindowAudioCapture> make_window_audio_capture();

} // namespace sound_spatializer
