#pragma once

#include "sound_spatializer/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace sound_spatializer {

class IHrtfDatabase;
struct StereoFrame;

struct ReflectionTap {
    float delay_seconds{};
    Vec3f arrival_direction{};
    MaterialBands gain{};
    std::uint32_t order{};
};

class ImageSourceModel {
public:
    static constexpr float kSpeedOfSoundMps = 343.0F;

    [[nodiscard]] std::vector<ReflectionTap> calculate(const Vec3f& source_position,
                                                        const Vec3f& listener_position,
                                                        const RoomConfig& room) const;
};

[[nodiscard]] MaterialBands estimate_room_rt60_eyring(const RoomConfig& room) noexcept;

class AmbisonicEncoderOrder3 {
public:
    static constexpr std::size_t kChannelCount = 16;
    [[nodiscard]] static std::array<float, kChannelCount> encode_direction(const Vec3f& direction) noexcept;
};

class EarlyReflectionProcessor {
public:
    static constexpr std::size_t kAmbisonicChannels = AmbisonicEncoderOrder3::kChannelCount;
    static constexpr std::size_t kMaximumReflectionTaps = 256;

    [[nodiscard]] bool prepare(float sample_rate, float maximum_delay_ms = 80.0F);
    [[nodiscard]] bool set_reflections(std::span<const ReflectionTap> reflections,
                                       std::uint32_t crossfade_frames = 4'800);
    void reset() noexcept;
    void process(const float* mono_input,
                 std::array<float, kAmbisonicChannels>* ambisonic_output,
                 std::size_t frame_count) noexcept;

private:
    struct EncodedTap {
        std::uint32_t delay_frames{};
        std::array<std::array<float, kAmbisonicChannels>, 3> band_gains{};
    };

    [[nodiscard]] std::array<float, kAmbisonicChannels> render_taps(
        const std::vector<EncodedTap>& taps, std::size_t write_index) const noexcept;

    float sample_rate_{48'000.0F};
    std::array<std::vector<float>, 3> delay_lines_{};
    std::vector<EncodedTap> active_taps_{};
    std::vector<EncodedTap> target_taps_{};
    std::size_t write_index_{};
    std::uint32_t crossfade_total_{};
    std::uint32_t crossfade_remaining_{};
    float low_state_{};
    float mid_lowpass_state_{};
    float low_coefficient_{};
    float high_coefficient_{};
};

struct AmbisonicBinauralFilterBankOrder3 {
    static constexpr std::size_t kAmbisonicChannels = AmbisonicEncoderOrder3::kChannelCount;
    using FirMatrix = std::array<
        std::array<std::array<float, kTimeDomainHrirTaps>, kAmbisonicChannels>, 2>;

    FirMatrix coefficients{};
    std::size_t tap_count{};
};

static_assert(std::is_trivially_copyable_v<AmbisonicBinauralFilterBankOrder3>);

// Fixed 32-point quadrature projection into 16x2 time-domain FIRs. Projection is deliberately
// separated from application so every HRTF lookup can run on the preparation worker rather than
// on the MMCSS audio callback.
class AmbisonicBinauralDecoderOrder3 {
public:
    static constexpr std::size_t kAmbisonicChannels = AmbisonicEncoderOrder3::kChannelCount;
    static constexpr std::size_t kQuadraturePoints = 32;

    AmbisonicBinauralDecoderOrder3() noexcept;
    [[nodiscard]] bool prepare_filter_bank(const Quaternionf& world_to_head, const IHrtfDatabase& hrtf,
                                           AmbisonicBinauralFilterBankOrder3& output) const noexcept;
    [[nodiscard]] bool apply_filter_bank(const AmbisonicBinauralFilterBankOrder3& filters,
                                         std::uint32_t crossfade_frames = 64) noexcept;
    // Convenience for offline/tests only: production audio uses prepare_filter_bank on the worker
    // and apply_filter_bank on the callback.
    [[nodiscard]] bool update(const Quaternionf& world_to_head, const IHrtfDatabase& hrtf,
                              std::uint32_t crossfade_frames = 64) noexcept;
    void reset() noexcept;
    void process(const std::array<float, kAmbisonicChannels>* input, StereoFrame* output,
                  std::size_t frame_count) noexcept;
    [[nodiscard]] std::uint32_t morph_remaining_frames() const noexcept { return crossfade_remaining_; }

private:
    using FirMatrix = AmbisonicBinauralFilterBankOrder3::FirMatrix;

    [[nodiscard]] static float convolve(const float* history, const float* coefficients,
                                        std::size_t tap_count) noexcept;
    void freeze_current_morph() noexcept;

    std::array<Vec3f, kQuadraturePoints> quadrature_directions_{};
    std::array<std::array<float, kAmbisonicChannels>, kQuadraturePoints> quadrature_harmonics_{};
    FirMatrix active_filters_{};
    FirMatrix target_filters_{};
    std::array<std::array<float, kTimeDomainHrirTaps * 2>, kAmbisonicChannels> history_{};
    std::size_t active_tap_count_{1};
    std::size_t target_tap_count_{1};
    std::size_t history_index_{};
    std::uint32_t crossfade_total_{};
    std::uint32_t crossfade_remaining_{};
    bool initialized_{};
};

class LateReverbFdn16 {
public:
    static constexpr std::size_t kLineCount = 16;
    static constexpr std::size_t kOutputChannels = 4; // first-order Ambisonics, ACN/SN3D

    [[nodiscard]] bool prepare(float sample_rate, const MaterialBands& rt60_seconds);
    void configure_rt60(const MaterialBands& rt60_seconds, std::uint32_t morph_frames = 4'800) noexcept;
    void reset() noexcept;
    void process_mono(const float* input,
                      std::array<float, kOutputChannels>* output,
                      std::size_t frame_count) noexcept;

private:
    struct BandDelayNetwork {
        std::array<std::vector<float>, kLineCount> lines{};
        std::array<float, kLineCount> feedback_gains{};
        std::array<float, kLineCount> target_feedback_gains{};
    };

    float sample_rate_{48'000.0F};
    MaterialBands rt60_seconds_{};
    std::array<BandDelayNetwork, 3> bands_{};
    std::array<std::size_t, kLineCount> delay_lengths_{};
    std::array<std::size_t, kLineCount> indices_{};
    float low_state_{};
    float mid_lowpass_state_{};
    float low_coefficient_{};
    float high_coefficient_{};
    std::uint32_t gain_morph_remaining_{};
    bool prepared_{};
    bool gains_initialized_{};
};

} // namespace sound_spatializer
