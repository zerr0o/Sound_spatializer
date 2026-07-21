#pragma once

#include "sound_spatializer/math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sound_spatializer {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kPosePacketMagic = 0x31505353U; // SSP1
inline constexpr std::uint32_t kSampleRate = 48'000;
// HRIRs up to 512 taps stay on the zero-latency time-domain path. Longer
// SimpleFreeFieldHRIR responses use the preallocated partitioned tail in the
// direct-speaker convolver. 4,096 samples cover 85.3 ms at 48 kHz; longer
// responses are outside the bounded V1 real-time contract.
inline constexpr std::size_t kTimeDomainHrirTaps = 512;
inline constexpr std::size_t kMaximumHrirTaps = 4'096;
inline constexpr std::size_t kMaximumEqSections = 16;

enum class TrackingState : std::uint32_t {
    unavailable = 0,
    tracking = 1,
    held = 2,
    returning_to_neutral = 3,
};

enum class PoseWireTrackingState : std::uint16_t {
    lost = 0,
    acquiring = 1,
    tracking = 2,
};

struct HeadPoseSampleV1 {
    std::uint64_t sequence{};
    std::int64_t timestamp_qpc{};
    Quaternionf orientation{};
    Vec3f angular_velocity_rad_s{};
    float confidence{};
    TrackingState tracking_state{TrackingState::unavailable};
};

#pragma pack(push, 1)
struct HeadPosePacketV1 {
    std::uint32_t magic{kPosePacketMagic};
    std::uint16_t version{1};
    std::uint16_t tracking_state{static_cast<std::uint16_t>(PoseWireTrackingState::lost)};
    std::uint64_t sequence{};
    std::int64_t timestamp_qpc{};
    float quaternion_w{1.0F};
    float quaternion_x{};
    float quaternion_y{};
    float quaternion_z{};
    float angular_velocity_x{};
    float angular_velocity_y{};
    float angular_velocity_z{};
    float confidence{};
    std::uint32_t flags{};
    std::uint32_t crc32{};
};
#pragma pack(pop)

static_assert(sizeof(HeadPosePacketV1) == 64, "The pose wire format is ABI-stable");

enum class SpeakerChannel : std::uint32_t { left = 0, right = 1 };

struct SpeakerConfig {
    SpeakerChannel channel{SpeakerChannel::left};
    Vec3f position_m{};
    float gain_db{};
};

struct ListenerConfig {
    Vec3f position_m{};
    Quaternionf neutral_orientation{};
};

struct MaterialBands {
    float low{0.15F};
    float mid{0.20F};
    float high{0.30F};
};

struct SurfaceMaterial {
    std::string material_id{"generic"};
    MaterialBands absorption{};
    MaterialBands diffusion{0.10F, 0.10F, 0.10F};
};

struct RoomConfig {
    bool enabled{};
    Vec3f dimensions_m{5.0F, 2.7F, 4.0F};
    std::array<SurfaceMaterial, 6> surfaces{}; // -X,+X,-Z,+Z,-Y(floor),+Y(ceiling)
    std::uint32_t reflection_order{2};
    float early_window_ms{80.0F};
    bool late_reverb_enabled{true};
    MaterialBands late_rt60_s{0.45F, 0.38F, 0.28F};
    float wet_gain_db{-12.0F};
};

enum class AudioMode : std::uint32_t {
    shared_low_latency = 0,
    exclusive_pro = 1,
    compatibility_256 = 2,
};

enum class CaptureProvider : std::uint32_t {
    native_driver = 0,
    external_render = 1,
};

enum class BiquadType : std::uint32_t {
    peaking = 0,
    low_shelf = 1,
    high_shelf = 2,
    high_pass = 3,
    low_pass = 4,
};

struct BiquadParameters {
    BiquadType type{BiquadType::peaking};
    float frequency_hz{1'000.0F};
    float q{0.70710678F};
    float gain_db{};
};

struct HeadphoneEqConfig {
    bool enabled{};
    float preamp_db{};
    std::vector<BiquadParameters> sections{};
};

struct AudioConfig {
    CaptureProvider capture_provider{CaptureProvider::native_driver};
    std::optional<std::string> capture_endpoint_id{};
    std::optional<std::string> output_device_id{};
    AudioMode mode{AudioMode::shared_low_latency};
    std::uint32_t sample_rate{kSampleRate};
    std::uint32_t buffer_frames{128};
    bool bypass{};
    float master_gain_db{-6.0F};
    float room_mix{0.18F};
};

struct TrackingConfig {
    bool enabled{true};
    std::optional<std::string> camera_device_id{};
    std::uint32_t minimum_fps{60};
    float prediction_limit_ms{20.0F};
};

struct HrtfConfig {
    std::string profile_id{"builtin-analytic-neutral"};
    std::optional<std::string> sofa_path{};
};

struct SceneConfigV1 {
    std::uint32_t schema_version{1};
    AudioConfig audio{};
    TrackingConfig tracking{};
    std::array<SpeakerConfig, 2> speakers{
        SpeakerConfig{SpeakerChannel::left, {-1.0F, 1.2F, 1.7320508F}, 0.0F},
        SpeakerConfig{SpeakerChannel::right, {1.0F, 1.2F, 1.7320508F}, 0.0F},
    };
    ListenerConfig listener{{0.0F, 1.2F, 0.0F}, {}};
    HrtfConfig hrtf{};
    HeadphoneEqConfig headphone_eq{};
    RoomConfig room{};
};

enum class EngineCommandType : std::uint32_t {
    start,
    stop,
    set_bypass,
    set_output_device,
    set_audio_mode,
    calibrate_neutral,
    set_scene,
    set_hrtf,
    set_headphone_eq,
    set_audio_route,
};

struct EngineCommandV1 {
    std::uint32_t schema_version{1};
    EngineCommandType type{EngineCommandType::start};
    bool bool_value{};
    std::string string_value{};
    std::optional<std::string> optional_string_value{};
    Quaternionf quaternion_value{};
    HeadphoneEqConfig headphone_eq{};
    AudioMode audio_mode{AudioMode::shared_low_latency};
    CaptureProvider capture_provider{CaptureProvider::native_driver};
    std::optional<std::string> capture_endpoint_id{};
    std::optional<std::string> output_device_id{};
    SceneConfigV1 scene{};
};

enum class StreamState : std::uint32_t { stopped, starting, running, degraded, failed };

// Effective sample representation exposed by the audio backend. DSP remains
// float32 regardless of the physical endpoint format; pcm_s32 therefore means
// that only the final WASAPI render boundary is converted to signed PCM.
enum class AudioSampleFormat : std::uint32_t { unknown, float32, pcm_s32 };

struct EngineStatusV1 {
    std::uint32_t schema_version{1};
    StreamState capture_state{StreamState::stopped};
    StreamState render_state{StreamState::stopped};
    TrackingState tracking_state{TrackingState::unavailable};
    AudioMode audio_mode{AudioMode::shared_low_latency};
    AudioSampleFormat render_sample_format{AudioSampleFormat::unknown};
    std::uint32_t capture_sample_rate{};
    std::uint32_t render_sample_rate{};
    std::uint32_t capture_period_frames{};
    std::uint32_t render_period_frames{};
    std::uint32_t fifo_fill_frames{};
    std::uint64_t xruns{};
    float callback_cpu_percent{};
    float tracking_hz{};
    float latency_p50_ms{};
    float latency_p95_ms{};
    float current_resample_ratio{1.0F};
    bool potentially_binaural{};
    std::string last_error{};
};

[[nodiscard]] inline std::uint32_t pose_packet_crc32(const HeadPosePacketV1& packet) noexcept {
    std::uint32_t crc = 0xFFFF'FFFFU;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&packet);
    for (std::size_t index = 0; index < 60; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB8'8320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] inline HeadPosePacketV1 to_packet(const HeadPoseSampleV1& value, bool include_crc = false) noexcept {
    const PoseWireTrackingState wire_state = value.tracking_state == TrackingState::tracking
                                                 ? PoseWireTrackingState::tracking
                                                 : (value.tracking_state == TrackingState::unavailable
                                                        ? PoseWireTrackingState::lost
                                                        : PoseWireTrackingState::acquiring);
    const Quaternionf orientation = value.orientation.normalized_value();
    HeadPosePacketV1 packet{
        kPosePacketMagic,
        1,
        static_cast<std::uint16_t>(wire_state),
        value.sequence,
        value.timestamp_qpc,
        orientation.w,
        orientation.x,
        orientation.y,
        orientation.z,
        value.angular_velocity_rad_s.x,
        value.angular_velocity_rad_s.y,
        value.angular_velocity_rad_s.z,
        value.confidence,
        0,
        0, // CRC zero is valid on the trusted local named pipe.
    };
    if (include_crc) packet.crc32 = pose_packet_crc32(packet);
    return packet;
}

[[nodiscard]] inline bool from_packet(const HeadPosePacketV1& packet, HeadPoseSampleV1& value) noexcept {
    if (packet.magic != kPosePacketMagic || packet.version != 1 ||
        packet.tracking_state > static_cast<std::uint16_t>(PoseWireTrackingState::tracking) ||
        (packet.crc32 != 0 && packet.crc32 != pose_packet_crc32(packet))) {
        return false;
    }
    const Quaternionf orientation{packet.quaternion_w, packet.quaternion_x, packet.quaternion_y, packet.quaternion_z};
    const float quaternion_norm = std::sqrt(orientation.w * orientation.w + orientation.x * orientation.x +
                                            orientation.y * orientation.y + orientation.z * orientation.z);
    if (!std::isfinite(quaternion_norm) || quaternion_norm < 0.8F || quaternion_norm > 1.2F ||
        !std::isfinite(packet.angular_velocity_x) || !std::isfinite(packet.angular_velocity_y) ||
        !std::isfinite(packet.angular_velocity_z) || std::abs(packet.angular_velocity_x) > 50.0F ||
        std::abs(packet.angular_velocity_y) > 50.0F || std::abs(packet.angular_velocity_z) > 50.0F ||
        !std::isfinite(packet.confidence) || packet.confidence < 0.0F || packet.confidence > 1.0F ||
        packet.flags != 0) {
        return false;
    }
    const auto wire_state = static_cast<PoseWireTrackingState>(packet.tracking_state);
    const TrackingState internal_state = wire_state == PoseWireTrackingState::tracking
                                             ? TrackingState::tracking
                                             : (wire_state == PoseWireTrackingState::acquiring
                                                    ? TrackingState::held
                                                    : TrackingState::unavailable);
    value = {
        packet.sequence,
        packet.timestamp_qpc,
        orientation.normalized_value(),
        {packet.angular_velocity_x, packet.angular_velocity_y, packet.angular_velocity_z},
        packet.confidence,
        internal_state,
    };
    return true;
}

} // namespace sound_spatializer
