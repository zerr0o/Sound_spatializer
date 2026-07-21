#pragma once

#include "sound_spatializer/spsc_ring_buffer.hpp"
#include "sound_spatializer/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sound_spatializer {

struct StereoFrame {
    float left{};
    float right{};
};

class BypassCrossfade {
public:
    void reset(bool bypassed) noexcept;
    void set_bypassed(bool bypassed, std::uint32_t transition_frames) noexcept;
    void process(const StereoFrame* dry, StereoFrame* processed, std::size_t frame_count) noexcept;

    [[nodiscard]] float mix() const noexcept { return mix_; }
    [[nodiscard]] std::uint32_t remaining_frames() const noexcept { return remaining_frames_; }

private:
    float mix_{};
    float target_mix_{};
    float step_{};
    std::uint32_t remaining_frames_{};
};

struct HrirFilterBank {
    std::size_t tap_count{1};
    // Index = source * 2 + ear: LL, LR, RL, RR.
    alignas(32) std::array<std::array<float, kMaximumHrirTaps>, 4> coefficients{};

    [[nodiscard]] const std::array<float, kMaximumHrirTaps>& path(std::size_t source,
                                                                  std::size_t ear) const noexcept {
        return coefficients[source * 2 + ear];
    }

    [[nodiscard]] std::array<float, kMaximumHrirTaps>& path(std::size_t source,
                                                            std::size_t ear) noexcept {
        return coefficients[source * 2 + ear];
    }
};

class BinauralConvolver {
public:
    static constexpr std::size_t kDirectTapLimit = kTimeDomainHrirTaps;
    static constexpr std::size_t kPartitionFrames = 128;

    BinauralConvolver();
    ~BinauralConvolver();

    BinauralConvolver(const BinauralConvolver&) = delete;
    BinauralConvolver& operator=(const BinauralConvolver&) = delete;

    void reset() noexcept;
    [[nodiscard]] bool set_filters(const HrirFilterBank& filters, std::uint32_t morph_frames) noexcept;
    void process(const StereoFrame* input, StereoFrame* output, std::size_t frame_count) noexcept;
    [[nodiscard]] bool morphing() const noexcept;
    [[nodiscard]] std::uint32_t morph_remaining_frames() const noexcept;

private:
    struct PartitionedState;

    [[nodiscard]] static float convolve(const float* history, const float* coefficients,
                                        std::size_t tap_count) noexcept;
    void freeze_current_morph() noexcept;

    std::unique_ptr<PartitionedState> partitioned_{};
};

struct BiquadCoefficients {
    float b0{1.0F};
    float b1{};
    float b2{};
    float a1{};
    float a2{};
};

[[nodiscard]] BiquadCoefficients design_biquad(const BiquadParameters& parameters,
                                                float sample_rate) noexcept;

class StereoParametricEq {
public:
    [[nodiscard]] bool configure(std::span<const BiquadParameters> sections,
                                 float sample_rate) noexcept;
    [[nodiscard]] bool configuration_matches(std::span<const BiquadParameters> sections,
                                             float sample_rate) const noexcept;
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    void reset() noexcept;
    void process(StereoFrame* frames, std::size_t frame_count) noexcept;
    [[nodiscard]] std::size_t section_count() const noexcept { return section_count_; }

private:
    struct State {
        float z1{};
        float z2{};
    };

    std::array<BiquadCoefficients, kMaximumEqSections> coefficients_{};
    std::array<BiquadParameters, kMaximumEqSections> parameters_{};
    std::array<std::array<State, kMaximumEqSections>, 2> states_{};
    std::size_t section_count_{};
    float sample_rate_{};
    bool enabled_{};
};

class TruePeakLimiter {
public:
    void prepare(float sample_rate) noexcept;
    void reset() noexcept;
    void set_master_gain_db(float gain_db) noexcept;
    void set_ceiling_db(float ceiling_db) noexcept;
    void process(StereoFrame* frames, std::size_t frame_count) noexcept;

    [[nodiscard]] float measured_true_peak() const noexcept { return measured_true_peak_.load(std::memory_order_relaxed); }
    [[nodiscard]] float gain_reduction_db() const noexcept;

private:
    [[nodiscard]] float estimate_true_peak(std::size_t channel, float sample) noexcept;

    float sample_rate_{48'000.0F};
    float master_gain_{0.5011872F};
    float ceiling_{0.9885531F};
    float release_coefficient_{0.9995F};
    float applied_gain_{1.0F};
    std::array<std::array<float, 4>, 2> history_{};
    std::atomic<float> measured_true_peak_{};
};

// Conservative warning-only heuristic. It looks for a stable non-zero interaural-delay
// correlation peak and never changes, bypasses, or classifies the programme automatically.
class PotentialBinauralDetector {
public:
    void prepare(float sample_rate) noexcept;
    void reset() noexcept;
    void process(const StereoFrame* frames, std::size_t frame_count) noexcept;
    [[nodiscard]] bool potentially_binaural() const noexcept { return potentially_binaural_; }

private:
    static constexpr std::size_t kAnalysisSamples = 512;
    void analyze_window() noexcept;

    std::array<float, kAnalysisSamples> left_{};
    std::array<float, kAnalysisSamples> right_{};
    std::size_t analysis_index_{};
    std::uint32_t downsample_factor_{4};
    std::uint32_t downsample_count_{};
    float downsample_left_{};
    float downsample_right_{};
    std::uint32_t maximum_lag_{9};
    std::uint32_t positive_windows_required_{47};
    std::uint32_t negative_windows_required_{94};
    std::uint32_t positive_windows_{};
    std::uint32_t negative_windows_{};
    bool potentially_binaural_{};
};

class DriftController {
public:
    struct Settings {
        float proportional_gain{0.0005F};
        float integral_gain{0.0002F};
        float maximum_correction_ppm{1'500.0F};
    };

    DriftController() noexcept;
    explicit DriftController(Settings settings) noexcept;
    void reset() noexcept;
    [[nodiscard]] float update(std::size_t fill_frames, std::size_t target_frames,
                               float elapsed_seconds) noexcept;

private:
    Settings settings_;
    float integral_{};
};

class ClockDriftEstimator {
public:
    void reset() noexcept;
    [[nodiscard]] float update(std::uint64_t capture_position, std::uint64_t capture_frequency,
                               std::uint64_t capture_qpc_100ns, std::uint64_t render_position,
                               std::uint64_t render_frequency, std::uint64_t render_qpc_100ns) noexcept;
    [[nodiscard]] float ratio() const noexcept { return ratio_; }

private:
    std::uint64_t previous_capture_position_{};
    std::uint64_t previous_capture_qpc_{};
    std::uint64_t previous_render_position_{};
    std::uint64_t previous_render_qpc_{};
    float ratio_{1.0F};
    bool initialized_{};
};

class AsyncStereoResampler {
public:
    explicit AsyncStereoResampler(std::size_t fifo_capacity_frames = 8'192);

    [[nodiscard]] std::size_t push(const StereoFrame* frames, std::size_t frame_count) noexcept;
    [[nodiscard]] std::size_t render(StereoFrame* output, std::size_t frame_count,
                                     float output_sample_rate) noexcept;
    void reset() noexcept;
    void set_nominal_ratio(float input_rate_over_output_rate) noexcept;
    void set_target_fill(std::size_t frames) noexcept { target_fill_frames_ = frames; }

    [[nodiscard]] std::size_t fill_frames() const noexcept { return fifo_.size(); }
    [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t overruns() const noexcept { return overruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] float current_ratio() const noexcept { return current_ratio_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kKernelTaps = 16;
    static constexpr std::size_t kKernelPhases = 256;

    void build_kernel_table() noexcept;
    [[nodiscard]] bool prime() noexcept;

    SpscRingBuffer<StereoFrame> fifo_;
    DriftController drift_controller_{};
    std::array<std::array<float, kKernelTaps>, kKernelPhases> kernel_table_{};
    std::array<StereoFrame, kKernelTaps> source_window_{};
    double phase_{};
    float nominal_ratio_{1.0F};
    std::size_t target_fill_frames_{512};
    bool primed_{};
    std::atomic<std::uint64_t> underruns_{};
    std::atomic<std::uint64_t> overruns_{};
    std::atomic<float> current_ratio_{1.0F};
};

[[nodiscard]] inline float db_to_linear(float decibels) noexcept {
    return std::pow(10.0F, decibels / 20.0F);
}

[[nodiscard]] inline float linear_to_db(float linear) noexcept {
    return 20.0F * std::log10(std::max(linear, 1.0e-12F));
}

} // namespace sound_spatializer
