#include "sound_spatializer/spectral.hpp"

#include "sound_spatializer/math.hpp"

#include <algorithm>
#include <cmath>

namespace sound_spatializer {
namespace {

// -120 dBFS. Any magnitude below this is treated as the floor so the cepstral
// logarithm stays finite on the many exactly-zero bins a padded HRIR produces.
constexpr float kMagnitudeFloor = 1.0e-6F;

} // namespace

void build_twiddle_table(std::span<std::complex<float>> twiddles) noexcept {
    const std::size_t fft_size = twiddles.size() * 2;
    if (fft_size == 0) return;
    for (std::size_t index = 0; index < twiddles.size(); ++index) {
        const float phase = -2.0F * kPi * static_cast<float>(index) / static_cast<float>(fft_size);
        twiddles[index] = {std::cos(phase), std::sin(phase)};
    }
}

void radix2_transform(std::span<std::complex<float>> values,
                      std::span<const std::complex<float>> twiddles, bool inverse) noexcept {
    const std::size_t fft_size = values.size();
    if (fft_size < 2 || twiddles.size() * 2 != fft_size) return;

    for (std::size_t source = 1, destination = 0; source < fft_size; ++source) {
        std::size_t bit = fft_size >> 1U;
        while ((destination & bit) != 0U) {
            destination ^= bit;
            bit >>= 1U;
        }
        destination ^= bit;
        if (source < destination) std::swap(values[source], values[destination]);
    }

    for (std::size_t length = 2; length <= fft_size; length <<= 1U) {
        const std::size_t half = length >> 1U;
        const std::size_t twiddle_step = fft_size / length;
        for (std::size_t start = 0; start < fft_size; start += length) {
            for (std::size_t offset = 0; offset < half; ++offset) {
                std::complex<float> twiddle = twiddles[offset * twiddle_step];
                if (inverse) twiddle = std::conj(twiddle);
                const std::complex<float> even = values[start + offset];
                const std::complex<float> odd = values[start + offset + half] * twiddle;
                values[start + offset] = even + odd;
                values[start + offset + half] = even - odd;
            }
        }
    }
    if (inverse) {
        const float scale = 1.0F / static_cast<float>(fft_size);
        for (auto& value : values) value *= scale;
    }
}

void smooth_magnitude_fractional_octave(std::span<const float> magnitudes, std::span<float> smoothed,
                                        float fraction_denominator) noexcept {
    const std::size_t count = magnitudes.size();
    if (count == 0 || smoothed.size() != count) return;
    if (!(fraction_denominator > 0.0F)) {
        std::copy(magnitudes.begin(), magnitudes.end(), smoothed.begin());
        return;
    }
    // Band edges of a 1/N-octave window centred on each bin. Bin index is
    // proportional to frequency, so the edges are a plain ratio on the index.
    const float edge_ratio = std::pow(2.0F, 0.5F / fraction_denominator);

    // Both edges grow monotonically with the bin index, so a two-pointer window
    // keeps the pass linear and allocation-free even though the band widens
    // with frequency.
    std::size_t window_low = 0;
    std::size_t window_high = 0;
    double window_power = static_cast<double>(magnitudes[0]) * magnitudes[0];
    for (std::size_t index = 0; index < count; ++index) {
        const float centre = static_cast<float>(index);
        std::size_t low = static_cast<std::size_t>(std::ceil(centre / edge_ratio));
        std::size_t high = std::min(static_cast<std::size_t>(std::floor(centre * edge_ratio)), count - 1);
        low = std::min(low, high);
        while (window_high < high) {
            ++window_high;
            window_power += static_cast<double>(magnitudes[window_high]) * magnitudes[window_high];
        }
        while (window_low < low) {
            window_power -= static_cast<double>(magnitudes[window_low]) * magnitudes[window_low];
            ++window_low;
        }
        const double mean_power = window_power / static_cast<double>(high - low + 1);
        smoothed[index] = static_cast<float>(std::sqrt(std::max(mean_power, 0.0)));
    }
}

std::size_t minimum_phase_fir(std::span<const float> magnitudes, std::span<float> taps,
                              std::span<std::complex<float>> scratch,
                              std::span<const std::complex<float>> twiddles) noexcept {
    const std::size_t fft_size = scratch.size();
    if (fft_size < 4 || twiddles.size() * 2 != fft_size || magnitudes.size() != fft_size / 2 + 1 ||
        taps.empty()) {
        return 0;
    }

    // Real cepstrum of the log magnitude. The spectrum is Hermitian, so the
    // upper half mirrors bins 1..N/2-1.
    for (std::size_t bin = 0; bin <= fft_size / 2; ++bin) {
        const float magnitude = std::max(magnitudes[bin], kMagnitudeFloor);
        scratch[bin] = {std::log(magnitude), 0.0F};
    }
    for (std::size_t bin = fft_size / 2 + 1; bin < fft_size; ++bin) {
        scratch[bin] = scratch[fft_size - bin];
    }
    radix2_transform(scratch, twiddles, true);

    // Fold the anticausal half onto the causal one. This is what turns an
    // arbitrary magnitude into the unique minimum-phase spectrum sharing it.
    for (std::size_t index = 1; index < fft_size / 2; ++index) {
        scratch[index] *= 2.0F;
        scratch[fft_size - index] = {};
    }
    scratch[fft_size / 2] = {scratch[fft_size / 2].real(), 0.0F};

    radix2_transform(scratch, twiddles, false);
    for (auto& bin : scratch) bin = std::exp(bin);
    radix2_transform(scratch, twiddles, true);

    const std::size_t written = std::min(taps.size(), fft_size);
    for (std::size_t index = 0; index < written; ++index) taps[index] = scratch[index].real();
    return written;
}

float truncated_energy_fraction(std::span<const float> response, std::size_t taps) noexcept {
    double total = 0.0;
    double discarded = 0.0;
    for (std::size_t index = 0; index < response.size(); ++index) {
        const double energy = static_cast<double>(response[index]) * response[index];
        total += energy;
        if (index >= taps) discarded += energy;
    }
    if (total <= 0.0) return 0.0F;
    return static_cast<float>(discarded / total);
}

} // namespace sound_spatializer
