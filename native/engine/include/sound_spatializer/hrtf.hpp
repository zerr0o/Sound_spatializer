#pragma once

#include "sound_spatializer/dsp.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace sound_spatializer {

class IHrtfDatabase {
public:
    virtual ~IHrtfDatabase() = default;
    [[nodiscard]] virtual std::uint32_t sample_rate() const noexcept = 0;
    [[nodiscard]] virtual std::size_t maximum_taps() const noexcept = 0;
    virtual bool query(const Vec3f& direction_from_listener,
                       std::span<float> left,
                       std::span<float> right,
                       std::size_t& tap_count) const noexcept = 0;

    // Minimum-phase filter that makes the set neutral on the binaural bus: its
    // direction-averaged power gain becomes unity and whatever
    // direction-independent spectral tilt it carries is flattened.
    //
    // Both parts matter, and the level part matters most. A SOFA set carries no
    // absolute level convention, and libmysofa's own normalization keys on a
    // measurement it picks by minimizing azimuth+elevation, which on a set with
    // a bottom pole is not the frontal response at all. The shipped SADIE
    // profiles come out of it about 10 dB hot, which drives the true-peak
    // limiter into permanent deep gain reduction and is audible as hardness
    // long before any spectral tilt is.
    //
    // The component is direction-independent by construction, so applying this
    // filter once on the binaural bus is equivalent to correcting every
    // response. An empty span means the provider is already neutral.
    [[nodiscard]] virtual std::span<const float> diffuse_field_filter() const noexcept { return {}; }
};

// Designs that filter from a set of measured responses. `responses` holds
// `responses.size() / length` blocks of `length` samples; ear and direction
// interleaving is irrelevant because every block is power-averaged. Allocates,
// and is meant to run once per file on the loading thread.
[[nodiscard]] bool design_diffuse_field_inverse(std::span<const float> responses, std::size_t length,
                                                std::uint32_t sample_rate, std::span<float> taps);

class AnalyticHrtfDatabase final : public IHrtfDatabase {
public:
    explicit AnalyticHrtfDatabase(std::uint32_t sample_rate = kSampleRate) noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return sample_rate_; }
    [[nodiscard]] std::size_t maximum_taps() const noexcept override { return 64; }
    bool query(const Vec3f& direction_from_listener,
               std::span<float> left,
               std::span<float> right,
               std::size_t& tap_count) const noexcept override;

private:
    std::uint32_t sample_rate_;
};

struct HrtfLoadResult {
    std::unique_ptr<IHrtfDatabase> database{};
    std::string error{};
};

[[nodiscard]] HrtfLoadResult load_sofa_hrtf(std::string_view path, std::uint32_t sample_rate);

// Two coherent virtual emitters reach each ear with the delay that separates
// them, so everything correlated between their inputs is comb filtered. The
// notch is deep (10 dB and more) and its frequency follows the emitter angle,
// which is what makes a moving window sound metallic.
//
// The fix keeps the two emitters and rewrites their filters so that only the
// mid component sees a correction:
//
//   H'_left  = [ C*(H_left + H_right) + (H_left - H_right) ] / 2
//   H'_right = [ C*(H_left + H_right) - (H_left - H_right) ] / 2
//
// which is exactly `mid * C * (H_left + H_right) + side * (H_left - H_right)`.
// Hard-panned content is therefore untouched, and the real-time convolver,
// its morphing and the sparse window slots need no change at all.
//
// C is derived from the interference factor |H_left + H_right| divided by the
// power sum sqrt(|H_left|^2 + |H_right|^2). That ratio cancels the HRTFs' own
// magnitude structure, so smoothing it flattens the inter-emitter comb without
// touching the pinna notches that carry elevation cues.
//
// Preparation-thread only: this designs filters and must never run inside the
// audio callback.
class PhantomCentreCompensator {
public:
    static constexpr std::size_t kFftSize = 1'024;
    static constexpr std::size_t kCorrectionTaps = 128;
    static constexpr std::size_t kPairCount = kMaximumBinauralSources / 2;
    // Beyond this the linear convolution would wrap inside kFftSize.
    static constexpr std::size_t kMaximumCompensatedTaps = kFftSize - kCorrectionTaps + 1;

    PhantomCentreCompensator();
    ~PhantomCentreCompensator();

    PhantomCentreCompensator(const PhantomCentreCompensator&) = delete;
    PhantomCentreCompensator& operator=(const PhantomCentreCompensator&) = delete;

    void reset() noexcept;

    // Rewrites both paths of `pair_index` (sources 2*pair_index and
    // 2*pair_index+1) in place and raises `bank.tap_count` to cover the longer
    // response. Returns false and leaves the bank untouched when the pair is
    // outside the bounded budget or has nothing to correct.
    [[nodiscard]] bool compensate_pair(HrirFilterBank& bank, std::size_t pair_index,
                                       const Vec3f& left_direction,
                                       const Vec3f& right_direction) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Performs HRTF database queries; production code calls this only from HrtfPreparationWorker.
// `compensated_pair_mask` bit i requests phantom-centre compensation for the
// emitter pair (2i, 2i+1); it is ignored unless `compensator` is supplied.
[[nodiscard]] bool build_binaural_filter_bank(const IHrtfDatabase& database,
                                               const std::array<Vec3f, kMaximumBinauralSources>& head_relative_directions,
                                               const std::array<float, kMaximumBinauralSources>& speaker_gains,
                                               std::size_t source_count,
                                               HrirFilterBank& output,
                                               std::uint32_t compensated_pair_mask = 0,
                                               PhantomCentreCompensator* compensator = nullptr) noexcept;

} // namespace sound_spatializer
