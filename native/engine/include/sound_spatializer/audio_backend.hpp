#pragma once

#include "sound_spatializer/dsp.hpp"
#include "sound_spatializer/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace sound_spatializer {

struct AudioBackendConfig {
    CaptureProvider capture_provider{CaptureProvider::native_driver};
    std::string capture_endpoint_id{};
    // Development-only override supplied by --virtual-endpoint or its legacy
    // environment variable. It is deliberately ignored outside native mode.
    std::string native_test_override_endpoint_id{};
    std::string physical_output_endpoint_id{};
    InputLayout input_layout{InputLayout::stereo};
    AudioMode mode{AudioMode::shared_low_latency};
    std::uint32_t requested_buffer_frames{128};
};

struct AudioBackendDiagnostics {
    StreamState capture_state{StreamState::stopped};
    StreamState render_state{StreamState::stopped};
    std::uint32_t capture_channels{};
    std::uint32_t capture_channel_mask{};
    std::uint32_t capture_sample_rate{};
    std::uint32_t render_sample_rate{};
    AudioSampleFormat render_sample_format{AudioSampleFormat::unknown};
    std::uint32_t capture_period_frames{};
    std::uint32_t render_period_frames{};
    std::uint32_t fifo_fill_frames{};
    std::uint64_t capture_overruns{};
    std::uint64_t render_underruns{};
    float callback_cpu_percent{};
    float resample_ratio{1.0F};
    // Canonical endpoint selected by the backend after marker/ID resolution.
    // Process-loopback discovery must use this exact render endpoint.
    std::string capture_endpoint_id{};
    std::string last_error{};
};

inline constexpr std::uint32_t kStereoChannelMask = 0x0000'0003U;
inline constexpr std::uint32_t kSurround51BackChannelMask = 0x0000'003FU;
inline constexpr std::uint32_t kSurround51ChannelMask = 0x0000'060FU;

[[nodiscard]] constexpr bool is_supported_surround_5_1_channel_mask(
    std::uint32_t channel_mask) noexcept {
    return channel_mask == kSurround51BackChannelMask ||
           channel_mask == kSurround51ChannelMask;
}

[[nodiscard]] constexpr std::uint32_t expected_input_channel_count(
    InputLayout layout) noexcept {
    switch (layout) {
    case InputLayout::stereo: return 2U;
    case InputLayout::surround_5_1: return 6U;
    }
    return 0U;
}

// Selection policy is platform-neutral so the priority is unit-tested even
// though the concrete WAVEFORMATEX probing lives in the Windows backend.
[[nodiscard]] constexpr AudioSampleFormat select_exclusive_render_sample_format(
    bool float32_supported, bool pcm_s32_supported) noexcept {
    if (float32_supported) return AudioSampleFormat::float32;
    if (pcm_s32_supported) return AudioSampleFormat::pcm_s32;
    return AudioSampleFormat::unknown;
}

// The conversion is intentionally allocation-free and noexcept: it runs after
// the float DSP directly at the WASAPI render boundary. Non-finite samples are
// treated as silence so a DSP fault cannot reach the DAC as full-scale noise.
[[nodiscard]] inline std::int32_t float_to_pcm_s32(float sample) noexcept {
    if (!std::isfinite(sample)) return 0;
    if (sample >= 1.0F) return std::numeric_limits<std::int32_t>::max();
    if (sample <= -1.0F) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(static_cast<double>(sample) * 2'147'483'648.0);
}

// Returns true when every converted sample is zero, allowing WASAPI's SILENT
// flag to be used without an additional scan.
[[nodiscard]] inline bool convert_stereo_float_to_pcm_s32(
    const StereoFrame* input, std::int32_t* interleaved_output,
    std::size_t frame_count) noexcept {
    bool all_silent = true;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::int32_t left = float_to_pcm_s32(input[frame].left);
        const std::int32_t right = float_to_pcm_s32(input[frame].right);
        interleaved_output[frame * 2U] = left;
        interleaved_output[frame * 2U + 1U] = right;
        all_silent = all_silent && left == 0 && right == 0;
    }
    return all_silent;
}

struct AudioEndpointDescriptor {
    std::string id{};
    bool active{};
    std::uint32_t endpoint_marker{};
    std::uint32_t contract_version{};
};

struct AudioEndpointSelection {
    std::string id{};
    std::string error{};
    [[nodiscard]] explicit operator bool() const noexcept { return !id.empty(); }
};

struct SharedAudioPeriodRange {
    std::uint32_t default_period_frames{};
    std::uint32_t fundamental_period_frames{};
    std::uint32_t minimum_period_frames{};
    std::uint32_t maximum_period_frames{};
};

inline constexpr std::uint32_t kMxcsrDenormalsAreZeroMask = 1U << 6U;
inline constexpr std::uint32_t kMxcsrFlushToZeroMask = 1U << 15U;

[[nodiscard]] constexpr std::uint32_t realtime_mxcsr_with_ftz_daz(std::uint32_t mxcsr) noexcept {
    return mxcsr | kMxcsrDenormalsAreZeroMask | kMxcsrFlushToZeroMask;
}

// IAudioClient3 requires the requested period to be both inside the advertised
// range and an integral multiple of the fundamental period. Keep this policy
// platform-neutral so it is covered by the dependency-free test executable.
[[nodiscard]] constexpr std::uint32_t select_shared_audio_period_frames(
    AudioMode mode, SharedAudioPeriodRange range) noexcept {
    if (range.fundamental_period_frames == 0U || range.minimum_period_frames == 0U ||
        range.maximum_period_frames < range.minimum_period_frames) {
        return 0U;
    }

    const std::uint64_t fundamental = range.fundamental_period_frames;
    const std::uint64_t first =
        ((static_cast<std::uint64_t>(range.minimum_period_frames) + fundamental - 1U) / fundamental) * fundamental;
    const std::uint64_t last =
        (static_cast<std::uint64_t>(range.maximum_period_frames) / fundamental) * fundamental;
    if (first > last) return 0U;

    if (mode != AudioMode::compatibility_256) return static_cast<std::uint32_t>(first);

    constexpr std::uint64_t compatibility_target_frames = 256U;
    const std::uint64_t bounded_target = compatibility_target_frames < first
                                             ? first
                                             : (compatibility_target_frames > last ? last
                                                                                   : compatibility_target_frames);
    const std::uint64_t aligned_target = ((bounded_target + fundamental - 1U) / fundamental) * fundamental;
    return static_cast<std::uint32_t>(aligned_target <= last ? aligned_target : last);
}

struct CaptureDrainBudget {
    std::uint32_t maximum_packets{};
    std::uint32_t maximum_frames{};
    std::uint32_t processed_packets{};
    std::uint32_t processed_frames{};

    [[nodiscard]] constexpr bool can_process(std::uint32_t packet_frames) const noexcept {
        return packet_frames != 0U && processed_packets < maximum_packets && processed_frames <= maximum_frames &&
               packet_frames <= maximum_frames - processed_frames;
    }

    [[nodiscard]] constexpr bool try_account(std::uint32_t packet_frames) noexcept {
        if (!can_process(packet_frames)) return false;
        ++processed_packets;
        processed_frames += packet_frames;
        return true;
    }
};

[[nodiscard]] constexpr CaptureDrainBudget make_capture_drain_budget(
    std::uint32_t capture_buffer_frames) noexcept {
    // A WASAPI packet cannot exceed the endpoint buffer. One full buffer keeps
    // work per wake finite; the packet limit also bounds pathological tiny-packet
    // queues without preventing a normal endpoint from catching up.
    return {4U, capture_buffer_frames == 0U ? 1U : capture_buffer_frames, 0U, 0U};
}

// Keep only the amount of asynchronous-rate-conversion history needed to
// bridge one observed capture packet and two render periods. In particular,
// a legacy WASAPI loopback buffer can be much larger than its actual packet
// cadence; using that entire buffer as the target silently adds tens of
// milliseconds of steady-state latency.
[[nodiscard]] constexpr std::uint32_t select_asrc_target_fill_frames(
    std::uint32_t observed_capture_packet_frames, std::uint32_t render_period_frames,
    std::uint32_t requested_buffer_frames) noexcept {
    constexpr std::uint64_t minimum_guard_frames = 128U;
    std::uint64_t target = requested_buffer_frames > minimum_guard_frames
                               ? requested_buffer_frames
                               : minimum_guard_frames;
    const std::uint64_t render_guard = static_cast<std::uint64_t>(render_period_frames) * 2U;
    if (render_guard > target) target = render_guard;
    if (observed_capture_packet_frames > target) target = observed_capture_packet_frames;
    constexpr std::uint64_t maximum_uint32 = 0xFFFF'FFFFULL;
    return static_cast<std::uint32_t>(target > maximum_uint32 ? maximum_uint32 : target);
}

// WASAPI packet sizes vary from wake to wake. The ASRC target must follow the
// largest packet observed so far rather than the latest one: a target that
// moves on every packet turns the drift controller into a follower of its own
// setpoint, and its ratio corrections stop reflecting real clock drift.
[[nodiscard]] constexpr std::uint32_t capture_packet_high_water(std::uint32_t previous_high_water,
                                                                std::uint32_t observed_frames) noexcept {
    return observed_frames > previous_high_water ? observed_frames : previous_high_water;
}

[[nodiscard]] constexpr std::uint32_t audio_reconnect_backoff_ms(std::uint32_t attempt) noexcept {
    return 250U << (attempt < 3U ? attempt : 3U);
}

// Shared mode may opportunistically render after either endpoint wakes because
// padding reports the writable region. Event-driven exclusive mode must only
// consume its complete buffer after the render event itself is signalled.
[[nodiscard]] constexpr bool should_service_wasapi_render_on_wake(
    AudioMode mode, bool render_event_signalled) noexcept {
    return mode != AudioMode::exclusive_pro || render_event_signalled;
}

[[nodiscard]] constexpr std::uint64_t audio_frames_to_reference_time(
    std::uint32_t frames, std::uint32_t sample_rate = kSampleRate) noexcept {
    return sample_rate == 0U
               ? 0U
               : (10'000'000ULL * frames + static_cast<std::uint64_t>(sample_rate) / 2ULL) /
                     sample_rate;
}

[[nodiscard]] bool validate_audio_backend_config(const AudioBackendConfig& config, std::string& error) noexcept;

[[nodiscard]] AudioEndpointSelection select_capture_render_endpoint(
    std::span<const AudioEndpointDescriptor> endpoints, CaptureProvider provider,
    std::string_view explicit_endpoint_id = {}, std::string_view native_test_override_id = {});

class IAudioProcessor {
public:
    virtual ~IAudioProcessor() = default;
    virtual void process_audio(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                               std::int64_t render_qpc) noexcept = 0;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    [[nodiscard]] virtual bool start(const AudioBackendConfig& config, IAudioProcessor& processor,
                                     std::string& error) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual AudioBackendDiagnostics diagnostics() const = 0;
};

class MockAudioBackend final : public IAudioBackend {
public:
    [[nodiscard]] bool start(const AudioBackendConfig& config, IAudioProcessor& processor,
                             std::string& error) override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override { return running_; }
    [[nodiscard]] AudioBackendDiagnostics diagnostics() const override { return diagnostics_; }

    void process_block(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                       std::int64_t render_qpc = 0) noexcept;
    // Compatibility helper for existing stereo-only engine tests and embedders.
    // The samples are expanded into FL/FR while every other programme channel
    // remains silent before entering the common six-channel processor contract.
    void process_block(const StereoFrame* input, StereoFrame* output, std::size_t frame_count,
                       std::int64_t render_qpc = 0) noexcept;

private:
    IAudioProcessor* processor_{};
    AudioBackendDiagnostics diagnostics_{};
    bool running_{};
};

[[nodiscard]] std::unique_ptr<IAudioBackend> create_system_audio_backend();

} // namespace sound_spatializer
