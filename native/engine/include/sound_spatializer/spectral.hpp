#pragma once

#include <complex>
#include <cstddef>
#include <span>

namespace sound_spatializer {

// Shared spectral primitives. The radix-2 transform is the one the partitioned
// convolver already ran on the audio callback, so it keeps its caller-owned
// twiddle table and its allocation-free contract. Everything else in this
// header is preparation-thread work: filter design that must never run inside
// process_audio().

// Fills `twiddles` with exp(-2i*pi*k/(2*twiddles.size())). The table therefore
// describes a transform of 2*twiddles.size() points.
void build_twiddle_table(std::span<std::complex<float>> twiddles) noexcept;

// In-place radix-2 decimation-in-time transform. `values.size()` must be a
// power of two equal to 2*twiddles.size(). The inverse scales by 1/N.
void radix2_transform(std::span<std::complex<float>> values,
                      std::span<const std::complex<float>> twiddles, bool inverse) noexcept;

// Power-average smoothing across a fractional-octave band. `magnitudes` and
// `smoothed` hold linear magnitudes for bins 0..N/2 of an N-point transform and
// must have the same size. `fraction_denominator` is 3 for third-octave
// smoothing. Bin 0 and any band narrower than one bin are passed through.
void smooth_magnitude_fractional_octave(std::span<const float> magnitudes, std::span<float> smoothed,
                                        float fraction_denominator) noexcept;

// Designs a minimum-phase FIR whose magnitude approximates `magnitudes` (bins
// 0..N/2 of an N-point transform, so magnitudes.size() == fft_size/2 + 1).
// `scratch` and `twiddles` must be sized for fft_size. Writes
// min(taps.size(), fft_size) coefficients and returns that count; the caller is
// responsible for checking how much energy the truncation discarded.
std::size_t minimum_phase_fir(std::span<const float> magnitudes, std::span<float> taps,
                              std::span<std::complex<float>> scratch,
                              std::span<const std::complex<float>> twiddles) noexcept;

// Fraction of the total energy of `response` that lies at or beyond `taps`.
// Used to prove that truncating a designed filter to the direct-path tap budget
// is inaudible.
[[nodiscard]] float truncated_energy_fraction(std::span<const float> response, std::size_t taps) noexcept;

} // namespace sound_spatializer
