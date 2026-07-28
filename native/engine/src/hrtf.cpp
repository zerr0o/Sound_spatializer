#include "sound_spatializer/hrtf.hpp"

#include "sound_spatializer/spectral.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace sound_spatializer {

#if defined(SOUND_SPATIALIZER_HAS_MYSOFA)
HrtfLoadResult load_sofa_hrtf_impl(std::string_view path, std::uint32_t sample_rate);
#endif

AnalyticHrtfDatabase::AnalyticHrtfDatabase(std::uint32_t sample_rate) noexcept
    : sample_rate_(std::max<std::uint32_t>(8'000, sample_rate)) {}

bool AnalyticHrtfDatabase::query(const Vec3f& direction_from_listener,
                                 std::span<float> left,
                                 std::span<float> right,
                                 std::size_t& tap_count) const noexcept {
    if (left.size() < maximum_taps() || right.size() < maximum_taps()) {
        return false;
    }
    std::fill(left.begin(), left.end(), 0.0F);
    std::fill(right.begin(), right.end(), 0.0F);

    const Vec3f direction = normalized(direction_from_listener);
    if (length(direction) < 0.5F) {
        return false;
    }
    const float azimuth = std::atan2(direction.x, direction.z);
    const float lateral = std::sin(azimuth); // +1 at the right ear
    constexpr float maximum_itd_seconds = 0.00065F;
    const float interaural_delay = lateral * maximum_itd_seconds * static_cast<float>(sample_rate_);
    const float base_delay = 4.0F + std::abs(interaural_delay);
    const float left_delay = base_delay + std::max(0.0F, interaural_delay);
    const float right_delay = base_delay + std::max(0.0F, -interaural_delay);
    const float left_gain = std::pow(10.0F, (-5.5F * std::max(0.0F, lateral)) / 20.0F);
    const float right_gain = std::pow(10.0F, (-5.5F * std::max(0.0F, -lateral)) / 20.0F);

    const auto place_fractional_impulse = [direction](std::span<float> destination, float delay, float gain) noexcept {
        const std::size_t index = std::min<std::size_t>(static_cast<std::size_t>(delay), destination.size() - 2);
        const float fraction = delay - static_cast<float>(index);
        destination[index] += gain * (1.0F - fraction);
        destination[index + 1] += gain * fraction;
        // A quiet pinna-like echo keeps this development profile externalized without claiming personalization.
        if (index + 7 < destination.size()) {
            destination[index + 7] += gain * (0.05F + 0.03F * direction.y);
        }
    };
    place_fractional_impulse(left, left_delay, left_gain);
    place_fractional_impulse(right, right_delay, right_gain);
    tap_count = maximum_taps();
    return true;
}

HrtfLoadResult load_sofa_hrtf(std::string_view path, std::uint32_t sample_rate) {
#if defined(SOUND_SPATIALIZER_HAS_MYSOFA)
    return load_sofa_hrtf_impl(path, sample_rate);
#else
    (void)path;
    (void)sample_rate;
    return {nullptr, "libmysofa is not available in this build"};
#endif
}

namespace {

// Level normalization is not tapered: it has to apply at every frequency. Only
// the spectral shaping fades out at the edges of the corrected band, where
// inverting a measurement would amplify noise.
constexpr float kDiffuseFieldMaximumCut = 0.125893F;   // -18 dB
constexpr float kDiffuseFieldMaximumBoost = 3.981072F; // +12 dB
constexpr float kDiffuseFieldMaximumLevelTrim = 15.848932F; // +-24 dB
constexpr float kDiffuseFieldLowEdgeHz = 150.0F;
constexpr float kDiffuseFieldLowFullHz = 300.0F;
constexpr float kDiffuseFieldHighFullHz = 15'000.0F;
constexpr float kDiffuseFieldHighEdgeHz = 18'000.0F;

[[nodiscard]] float band_edge_weight(float frequency_hz, float low_edge, float low_full,
                                     float high_full, float high_edge) noexcept {
    if (frequency_hz <= low_edge || frequency_hz >= high_edge) return 0.0F;
    if (frequency_hz >= low_full && frequency_hz <= high_full) return 1.0F;
    const float progress = frequency_hz < low_full
                               ? (frequency_hz - low_edge) / (low_full - low_edge)
                               : (high_edge - frequency_hz) / (high_edge - high_full);
    return 0.5F - 0.5F * std::cos(kPi * std::clamp(progress, 0.0F, 1.0F));
}

// The comb never reaches below this frequency: it would need an inter-emitter
// delay longer than any plausible head. Above the upper edge the correction
// would only chase measurement noise.
constexpr float kCompensationLowEdgeHz = 200.0F;
constexpr float kCompensationLowFullHz = 400.0F;
constexpr float kCompensationHighFullHz = 14'000.0F;
constexpr float kCompensationHighEdgeHz = 16'000.0F;
// Filling a notch is not free: a hard-panned source has a mid component too, so
// an unbounded boost would colour content that never combed in the first place.
// The ceiling trades a little residual notch for that side effect.
constexpr float kCompensationMinimumGain = 0.501187F;  // -6 dB
constexpr float kCompensationMaximumGain = 3.162278F;  // +10 dB
constexpr float kCompensationSmoothingOctaveDenominator = 3.0F;
// cos(2 degrees): below this the emitter has moved enough to redesign.
constexpr float kPairCacheCosine = 0.99939F;

[[nodiscard]] bool direction_matches(const Vec3f& current, const Vec3f& cached) noexcept {
    const Vec3f first = normalized(current);
    const Vec3f second = normalized(cached);
    if (length(first) < 0.5F || length(second) < 0.5F) return false;
    return first.x * second.x + first.y * second.y + first.z * second.z > kPairCacheCosine;
}

[[nodiscard]] float compensation_weight(float frequency_hz) noexcept {
    return band_edge_weight(frequency_hz, kCompensationLowEdgeHz, kCompensationLowFullHz,
                            kCompensationHighFullHz, kCompensationHighEdgeHz);
}

} // namespace

bool design_diffuse_field_inverse(std::span<const float> responses, std::size_t length,
                                  std::uint32_t sample_rate, std::span<float> taps) {
    if (length == 0 || responses.size() < length || taps.empty()) return false;
    const std::size_t count = responses.size() / length;
    if (count == 0) return false;
    std::size_t fft_size = 512;
    while (fft_size < length && fft_size < 4'096) fft_size <<= 1U;
    if (length > fft_size || taps.size() > fft_size) return false;
    const std::size_t bins = fft_size / 2 + 1;

    std::vector<std::complex<float>> twiddles(fft_size / 2);
    build_twiddle_table(twiddles);
    std::vector<std::complex<float>> scratch(fft_size);
    std::vector<double> power(bins, 0.0);

    for (std::size_t response = 0; response < count; ++response) {
        const float* samples = responses.data() + response * length;
        std::fill(scratch.begin(), scratch.end(), std::complex<float>{});
        for (std::size_t tap = 0; tap < length; ++tap) scratch[tap] = {samples[tap], 0.0F};
        radix2_transform(scratch, twiddles, false);
        for (std::size_t bin = 0; bin < bins; ++bin) power[bin] += std::norm(scratch[bin]);
    }

    const double normalizer = 1.0 / static_cast<double>(count);
    std::vector<float> magnitude(bins);
    for (std::size_t bin = 0; bin < bins; ++bin)
        magnitude[bin] = static_cast<float>(std::sqrt(power[bin] * normalizer));

    std::vector<float> smoothed(bins);
    smooth_magnitude_fractional_octave(magnitude, smoothed, 3.0F);

    // Separate the broadband level from the spectral shape. Only the shape may
    // be tapered at the band edges; a level trim that faded out would leave the
    // sub-bass and the top octave uncorrected and louder than everything else.
    const float bin_hz = static_cast<float>(sample_rate) / static_cast<float>(fft_size);
    double log_sum = 0.0;
    std::size_t counted = 0;
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const float frequency = static_cast<float>(bin) * bin_hz;
        if (frequency < kDiffuseFieldLowFullHz || frequency > kDiffuseFieldHighFullHz) continue;
        log_sum += std::log(std::max(smoothed[bin], 1.0e-9F));
        ++counted;
    }
    if (counted == 0) return false;
    const float measured_level = static_cast<float>(std::exp(log_sum / static_cast<double>(counted)));
    const float level_trim = std::clamp(1.0F / std::max(measured_level, 1.0e-9F),
                                        1.0F / kDiffuseFieldMaximumLevelTrim,
                                        kDiffuseFieldMaximumLevelTrim);

    std::vector<float> correction(bins);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const float shape = std::clamp(measured_level / std::max(smoothed[bin], 1.0e-9F),
                                       kDiffuseFieldMaximumCut, kDiffuseFieldMaximumBoost);
        const float weight = band_edge_weight(static_cast<float>(bin) * bin_hz, kDiffuseFieldLowEdgeHz,
                                              kDiffuseFieldLowFullHz, kDiffuseFieldHighFullHz,
                                              kDiffuseFieldHighEdgeHz);
        correction[bin] = level_trim * std::pow(shape, weight);
    }

    return minimum_phase_fir(correction, taps, scratch, twiddles) == taps.size();
}

struct PhantomCentreCompensator::State {
    static constexpr std::size_t kBins = kFftSize / 2 + 1;

    struct PairCache {
        Vec3f left{};
        Vec3f right{};
        bool valid{};
        std::array<std::complex<float>, kFftSize> correction{};
    };

    State() noexcept { build_twiddle_table(twiddles); }

    std::array<std::complex<float>, kFftSize / 2> twiddles{};
    // [ear][side] where side 0 is the left emitter and side 1 the right one.
    std::array<std::array<std::array<std::complex<float>, kFftSize>, 2>, 2> spectra{};
    std::array<std::complex<float>, kFftSize> scratch{};
    std::array<float, kBins> interference{};
    std::array<float, kBins> smoothed{};
    std::array<float, kBins> correction_magnitude{};
    std::array<float, kCorrectionTaps> correction_taps{};
    std::array<PairCache, kPairCount> cache{};
};

PhantomCentreCompensator::PhantomCentreCompensator() : state_(std::make_unique<State>()) {}
PhantomCentreCompensator::~PhantomCentreCompensator() = default;

void PhantomCentreCompensator::reset() noexcept {
    for (auto& entry : state_->cache) entry.valid = false;
}

bool PhantomCentreCompensator::compensate_pair(HrirFilterBank& bank, std::size_t pair_index,
                                               const Vec3f& left_direction,
                                               const Vec3f& right_direction) noexcept {
    const std::size_t left_source = pair_index * 2;
    const std::size_t right_source = left_source + 1;
    if (pair_index >= kPairCount || right_source >= bank.source_count) return false;
    if (bank.tap_count == 0 || bank.tap_count > kMaximumCompensatedTaps) return false;

    State& state = *state_;
    for (std::size_t ear = 0; ear < 2; ++ear) {
        for (std::size_t side = 0; side < 2; ++side) {
            const auto& path = bank.path(side == 0 ? left_source : right_source, ear);
            auto& spectrum = state.spectra[ear][side];
            spectrum.fill({});
            for (std::size_t tap = 0; tap < bank.tap_count; ++tap) spectrum[tap] = {path[tap], 0.0F};
            radix2_transform(spectrum, state.twiddles, false);
        }
    }

    State::PairCache& cache = state.cache[pair_index];
    const bool reusable = cache.valid && direction_matches(left_direction, cache.left) &&
                          direction_matches(right_direction, cache.right);
    if (!reusable) {
        // Interference factor: the coherent sum divided by the power sum. Both
        // terms carry the HRTFs' own magnitude, so the ratio isolates what the
        // second emitter adds and nothing else.
        bool has_energy = false;
        for (std::size_t bin = 0; bin < State::kBins; ++bin) {
            double accumulated = 0.0;
            for (std::size_t ear = 0; ear < 2; ++ear) {
                const std::complex<float> left = state.spectra[ear][0][bin];
                const std::complex<float> right = state.spectra[ear][1][bin];
                const double power_sum = static_cast<double>(std::norm(left)) + std::norm(right);
                if (power_sum <= 1.0e-20) {
                    accumulated += 1.0;
                    continue;
                }
                const double coherent = std::abs(left + right);
                accumulated += coherent * coherent / power_sum;
                has_energy = true;
            }
            state.interference[bin] = static_cast<float>(std::sqrt(0.5 * accumulated));
        }
        if (!has_energy) return false;

        smooth_magnitude_fractional_octave(state.interference, state.smoothed,
                                           kCompensationSmoothingOctaveDenominator);
        constexpr float bin_hz = static_cast<float>(kSampleRate) / static_cast<float>(kFftSize);
        for (std::size_t bin = 0; bin < State::kBins; ++bin) {
            const float measured = std::max(state.interference[bin], 1.0e-6F);
            const float raw = std::clamp(state.smoothed[bin] / measured, kCompensationMinimumGain,
                                         kCompensationMaximumGain);
            const float weight = compensation_weight(static_cast<float>(bin) * bin_hz);
            // Interpolate in dB so the tapered bands stay monotonic.
            state.correction_magnitude[bin] = std::pow(raw, weight);
        }

        if (minimum_phase_fir(state.correction_magnitude, state.correction_taps, state.scratch,
                              state.twiddles) != kCorrectionTaps) {
            return false;
        }
        cache.correction.fill({});
        for (std::size_t tap = 0; tap < kCorrectionTaps; ++tap)
            cache.correction[tap] = {state.correction_taps[tap], 0.0F};
        radix2_transform(cache.correction, state.twiddles, false);
        cache.left = left_direction;
        cache.right = right_direction;
        cache.valid = true;
    }

    for (std::size_t ear = 0; ear < 2; ++ear) {
        auto& left_spectrum = state.spectra[ear][0];
        auto& right_spectrum = state.spectra[ear][1];
        for (std::size_t bin = 0; bin < kFftSize; ++bin) {
            const std::complex<float> corrected_sum =
                cache.correction[bin] * (left_spectrum[bin] + right_spectrum[bin]);
            const std::complex<float> difference = left_spectrum[bin] - right_spectrum[bin];
            left_spectrum[bin] = 0.5F * (corrected_sum + difference);
            right_spectrum[bin] = 0.5F * (corrected_sum - difference);
        }
        radix2_transform(left_spectrum, state.twiddles, true);
        radix2_transform(right_spectrum, state.twiddles, true);
    }

    const std::size_t compensated_taps =
        std::min(bank.tap_count + kCorrectionTaps - 1, kMaximumHrirTaps);
    for (std::size_t ear = 0; ear < 2; ++ear) {
        for (std::size_t side = 0; side < 2; ++side) {
            auto& path = bank.path(side == 0 ? left_source : right_source, ear);
            const auto& spectrum = state.spectra[ear][side];
            for (std::size_t tap = 0; tap < compensated_taps; ++tap) path[tap] = spectrum[tap].real();
            for (std::size_t tap = compensated_taps; tap < bank.tap_count; ++tap) path[tap] = 0.0F;
        }
    }
    bank.tap_count = std::max(bank.tap_count, compensated_taps);
    return true;
}

bool build_binaural_filter_bank(const IHrtfDatabase& database,
                                const std::array<Vec3f, kMaximumBinauralSources>& head_relative_directions,
                                const std::array<float, kMaximumBinauralSources>& speaker_gains,
                                std::size_t source_count,
                                HrirFilterBank& output,
                                std::uint32_t compensated_pair_mask,
                                PhantomCentreCompensator* compensator) noexcept {
    if (source_count == 0 || source_count > kMaximumBinauralSources ||
        output.coefficients.size() != kMaximumBinauralSources * 2) {
        return false;
    }
    output.tap_count = 0;
    output.source_count = source_count;
    for (auto& path : output.coefficients) path.fill(0.0F);
    // A bank containing only muted paths is still a valid silent bank.
    std::size_t maximum_taps = 1;
    for (std::size_t source = 0; source < source_count; ++source) {
        // Window-aware rendering deliberately keeps stable, sparse slot
        // indices. A missing slot has zero gain and no meaningful direction;
        // querying it can make strict providers reject the entire bank.
        if (std::abs(speaker_gains[source]) <=
            std::numeric_limits<float>::epsilon()) {
            continue;
        }
        std::size_t taps = 0;
        if (!database.query(head_relative_directions[source], output.path(source, 0),
                            output.path(source, 1), taps) ||
            taps == 0 || taps > kMaximumHrirTaps) {
            return false;
        }
        maximum_taps = std::max(maximum_taps, taps);
        for (std::size_t tap = 0; tap < taps; ++tap) {
            output.path(source, 0)[tap] *= speaker_gains[source];
            output.path(source, 1)[tap] *= speaker_gains[source];
        }
    }
    output.tap_count = maximum_taps;

    if (compensator != nullptr && compensated_pair_mask != 0) {
        for (std::size_t pair = 0; pair < PhantomCentreCompensator::kPairCount; ++pair) {
            if ((compensated_pair_mask & (1U << pair)) == 0U) continue;
            const std::size_t left_source = pair * 2;
            const std::size_t right_source = left_source + 1;
            if (right_source >= source_count) continue;
            // A muted or absent emitter cannot interfere with anything, and its
            // direction is meaningless. Leave the pair alone.
            if (std::abs(speaker_gains[left_source]) <= std::numeric_limits<float>::epsilon() ||
                std::abs(speaker_gains[right_source]) <= std::numeric_limits<float>::epsilon()) {
                continue;
            }
            (void)compensator->compensate_pair(output, pair, head_relative_directions[left_source],
                                               head_relative_directions[right_source]);
        }
    }
    return true;
}

} // namespace sound_spatializer
