#include "sound_spatializer/hrtf.hpp"

#include <algorithm>
#include <cmath>

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

bool build_binaural_filter_bank(const IHrtfDatabase& database,
                                const std::array<Vec3f, 2>& head_relative_directions,
                                const std::array<float, 2>& speaker_gains,
                                HrirFilterBank& output) noexcept {
    HrirFilterBank candidate{};
    std::size_t maximum_taps = 0;
    for (std::size_t source = 0; source < 2; ++source) {
        std::size_t taps = 0;
        if (!database.query(head_relative_directions[source], candidate.path(source, 0),
                            candidate.path(source, 1), taps) ||
            taps == 0 || taps > kMaximumHrirTaps) {
            return false;
        }
        maximum_taps = std::max(maximum_taps, taps);
        for (std::size_t tap = 0; tap < taps; ++tap) {
            candidate.path(source, 0)[tap] *= speaker_gains[source];
            candidate.path(source, 1)[tap] *= speaker_gains[source];
        }
    }
    candidate.tap_count = maximum_taps;
    output = candidate;
    return true;
}

} // namespace sound_spatializer
