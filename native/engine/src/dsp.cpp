#include "sound_spatializer/dsp.hpp"

#include "sound_spatializer/spectral.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP)
#include <immintrin.h>
#define SOUND_SPATIALIZER_USE_SSE2 1
#endif

namespace sound_spatializer {

void BypassCrossfade::reset(bool bypassed) noexcept {
    mix_ = bypassed ? 1.0F : 0.0F;
    target_mix_ = mix_;
    step_ = 0.0F;
    remaining_frames_ = 0;
}

void BypassCrossfade::set_bypassed(bool bypassed, std::uint32_t transition_frames) noexcept {
    const float target = bypassed ? 1.0F : 0.0F;
    if (target == target_mix_) return;
    target_mix_ = target;
    if (transition_frames == 0) {
        mix_ = target_mix_;
        step_ = 0.0F;
        remaining_frames_ = 0;
        return;
    }
    remaining_frames_ = transition_frames;
    step_ = (target_mix_ - mix_) / static_cast<float>(transition_frames);
}

void BypassCrossfade::process(const StereoFrame* dry, StereoFrame* processed,
                              std::size_t frame_count) noexcept {
    if (dry == nullptr || processed == nullptr) return;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (remaining_frames_ != 0) {
            mix_ += step_;
            --remaining_frames_;
            if (remaining_frames_ == 0) {
                mix_ = target_mix_;
                step_ = 0.0F;
            }
        }
        const float processed_mix = 1.0F - mix_;
        processed[frame].left = processed[frame].left * processed_mix + dry[frame].left * mix_;
        processed[frame].right = processed[frame].right * processed_mix + dry[frame].right * mix_;
    }
}

struct BinauralConvolver::PartitionedState {
    static constexpr std::size_t kFftSize = kPartitionFrames * 2;
    static constexpr std::size_t kMaximumTailPartitions =
        (kMaximumHrirTaps - kDirectTapLimit + kPartitionFrames - 1) / kPartitionFrames;
    static constexpr std::uint64_t kTailOffsetBlocks = kDirectTapLimit / kPartitionFrames;
    static constexpr std::size_t kOutputBlockSlots = static_cast<std::size_t>(kTailOffsetBlocks) + 2;
    static constexpr std::uint64_t kInvalidSequence = std::numeric_limits<std::uint64_t>::max();

    static_assert(kDirectTapLimit % kPartitionFrames == 0);
    static_assert(kMaximumTailPartitions > 0);

    using Spectrum = std::array<std::complex<float>, kFftSize>;

    struct FilterSpectra {
        std::array<std::array<Spectrum, kMaximumTailPartitions>, kMaximumBinauralSources * 2> paths{};
        std::size_t partition_count{};
        std::size_t source_count{2};
    };

    struct OutputBlock {
        std::uint64_t sequence{kInvalidSequence};
        std::array<std::array<float, kPartitionFrames>, 2> ears{};
    };

    using OutputCache = std::array<OutputBlock, kOutputBlockSlots>;

    PartitionedState() noexcept {
        build_twiddle_table(twiddles);
        input_sequences.fill(kInvalidSequence);
        invalidate(current_output);
        invalidate(target_output);
    }

    void transform(Spectrum& values, bool inverse) const noexcept {
        radix2_transform(values, twiddles, inverse);
    }

    void build_filter(const HrirFilterBank& bank, FilterSpectra& output) noexcept {
        output.source_count = bank.source_count;
        output.partition_count = 0;
        if (bank.tap_count <= kDirectTapLimit)
            return;
        const std::size_t tail_taps = bank.tap_count - kDirectTapLimit;
        output.partition_count = (tail_taps + kPartitionFrames - 1) / kPartitionFrames;
        for (std::size_t path = 0; path < bank.source_count * 2; ++path) {
            for (std::size_t partition = 0; partition < output.partition_count; ++partition) {
                Spectrum& spectrum = output.paths[path][partition];
                spectrum.fill({});
                const std::size_t first_tap = kDirectTapLimit + partition * kPartitionFrames;
                const std::size_t count = std::min(kPartitionFrames, bank.tap_count - first_tap);
                for (std::size_t tap = 0; tap < count; ++tap) {
                    spectrum[tap] = bank.coefficients[path][first_tap + tap];
                }
                transform(spectrum, false);
            }
        }
    }

    void blend_current_filter(float alpha) noexcept {
        const float current_gain = 1.0F - alpha;
        const std::size_t partitions = std::max(current_filter.partition_count, target_filter.partition_count);
        const std::size_t source_count = std::max(current_filter.source_count, target_filter.source_count);
        for (std::size_t path = 0; path < source_count * 2; ++path) {
            const std::size_t source = path / 2;
            const bool current_source_active = source < current_filter.source_count;
            const bool target_source_active = source < target_filter.source_count;
            for (std::size_t partition = 0; partition < partitions; ++partition) {
                for (std::size_t bin = 0; bin < kFftSize; ++bin) {
                    const std::complex<float> current =
                        current_source_active && partition < current_filter.partition_count
                            ? current_filter.paths[path][partition][bin]
                            : std::complex<float>{};
                    const std::complex<float> target =
                        target_source_active && partition < target_filter.partition_count
                            ? target_filter.paths[path][partition][bin]
                            : std::complex<float>{};
                    current_filter.paths[path][partition][bin] = current * current_gain + target * alpha;
                }
            }
        }
        current_filter.partition_count = partitions;
        current_filter.source_count = std::max(current_filter.source_count, target_filter.source_count);
    }

    void reset_stream() noexcept {
        for (auto& block : input_blocks)
            block.fill(0.0F);
        for (auto& block : previous_input_blocks)
            block.fill(0.0F);
        for (auto& source_spectra : input_spectra) {
            for (auto& spectrum : source_spectra)
                spectrum.fill({});
        }
        input_sequences.fill(kInvalidSequence);
        invalidate(current_output);
        invalidate(target_output);
        sample_position = 0;
        next_input_block = 0;
        target_filter = current_filter;
    }

    [[nodiscard]] StereoFrame output_sample(const OutputCache& cache, std::uint64_t block_sequence,
                                             std::size_t offset) const noexcept {
        const OutputBlock& block = cache[static_cast<std::size_t>(block_sequence % kOutputBlockSlots)];
        if (block.sequence != block_sequence) return {};
        return {block.ears[0][offset], block.ears[1][offset]};
    }

    void ingest(const DirectionalFrame& input, bool morphing) noexcept {
        const std::size_t offset = static_cast<std::size_t>(sample_position % kPartitionFrames);
        for (std::size_t source = 0; source < kMaximumBinauralSources; ++source)
            input_blocks[source][offset] = input.sources[source];
        if (offset + 1 == kPartitionFrames) {
            const std::uint64_t sequence = next_input_block++;
            const std::size_t spectrum_slot = static_cast<std::size_t>(sequence % kMaximumTailPartitions);
            const std::size_t active_source_count =
                morphing ? std::max(current_filter.source_count, target_filter.source_count)
                         : current_filter.source_count;
            for (std::size_t source = 0; source < kMaximumBinauralSources; ++source) {
                Spectrum& input_spectrum = input_spectra[source][spectrum_slot];
                if (source < active_source_count) {
                    Spectrum spectrum{};
                    for (std::size_t sample = 0; sample < kPartitionFrames; ++sample) {
                        spectrum[sample] = previous_input_blocks[source][sample];
                        spectrum[kPartitionFrames + sample] = input_blocks[source][sample];
                    }
                    transform(spectrum, false);
                    input_spectrum = spectrum;
                } else {
                    // A later source-count expansion can rebuild pending tails
                    // safely without observing an older ring-slot spectrum.
                    input_spectrum.fill({});
                }
                previous_input_blocks[source] = input_blocks[source];
            }
            input_sequences[spectrum_slot] = sequence;
            render(sequence, current_filter, current_output);
            if (morphing) render(sequence, target_filter, target_output);
        }
        ++sample_position;
    }

    void rebuild_pending(const FilterSpectra& filter, OutputCache& output) noexcept {
        invalidate(output);
        if (next_input_block == 0) return;
        const std::uint64_t first_output = sample_position / kPartitionFrames;
        const std::uint64_t last_input = next_input_block - 1;
        const std::uint64_t last_output = last_input + kTailOffsetBlocks;
        for (std::uint64_t output_sequence = first_output; output_sequence <= last_output; ++output_sequence) {
            if (output_sequence < kTailOffsetBlocks) continue;
            render(output_sequence - kTailOffsetBlocks, filter, output);
        }
    }

    FilterSpectra current_filter{};
    FilterSpectra target_filter{};
    HrirFilterBank current_bank{};
    HrirFilterBank target_bank{};
    std::array<std::array<float, kTimeDomainHrirTaps * 2>, kMaximumBinauralSources> direct_history{};
    std::size_t direct_history_index{};
    std::uint32_t morph_total{};
    std::uint32_t morph_remaining{};
    std::array<std::array<Spectrum, kMaximumTailPartitions>, kMaximumBinauralSources> input_spectra{};
    std::array<std::uint64_t, kMaximumTailPartitions> input_sequences{};
    std::array<std::array<float, kPartitionFrames>, kMaximumBinauralSources> input_blocks{};
    std::array<std::array<float, kPartitionFrames>, kMaximumBinauralSources> previous_input_blocks{};
    OutputCache current_output{};
    OutputCache target_output{};
    Spectrum scratch{};
    std::array<std::complex<float>, kFftSize / 2> twiddles{};
    std::uint64_t sample_position{};
    std::uint64_t next_input_block{};

private:
    static void invalidate(OutputCache& cache) noexcept {
        for (auto& block : cache) {
            block.sequence = kInvalidSequence;
            block.ears = {};
        }
    }

    void render(std::uint64_t input_sequence, const FilterSpectra& filter, OutputCache& output) noexcept {
        const std::uint64_t output_sequence = input_sequence + kTailOffsetBlocks;
        OutputBlock& block = output[static_cast<std::size_t>(output_sequence % kOutputBlockSlots)];
        block.sequence = output_sequence;
        block.ears = {};
        for (std::size_t ear = 0; ear < 2; ++ear) {
            scratch.fill({});
            for (std::size_t source = 0; source < filter.source_count; ++source) {
                const std::size_t path = source * 2 + ear;
                for (std::size_t partition = 0; partition < filter.partition_count; ++partition) {
                    if (partition > input_sequence) break;
                    const std::uint64_t wanted_sequence = input_sequence - partition;
                    const std::size_t slot = static_cast<std::size_t>(wanted_sequence % kMaximumTailPartitions);
                    if (input_sequences[slot] != wanted_sequence) continue;
                    for (std::size_t bin = 0; bin < kFftSize; ++bin) {
                        scratch[bin] += input_spectra[source][slot][bin] * filter.paths[path][partition][bin];
                    }
                }
            }
            transform(scratch, true);
            for (std::size_t sample = 0; sample < kPartitionFrames; ++sample) {
                block.ears[ear][sample] = scratch[kPartitionFrames + sample].real();
            }
        }
    }
};

BinauralConvolver::BinauralConvolver() : partitioned_(std::make_unique<PartitionedState>()) {
    partitioned_->current_bank.coefficients[0][0] = 1.0F;
    partitioned_->current_bank.coefficients[3][0] = 1.0F;
    partitioned_->target_bank = partitioned_->current_bank;
    partitioned_->build_filter(partitioned_->current_bank, partitioned_->current_filter);
    partitioned_->target_filter = partitioned_->current_filter;
}

BinauralConvolver::~BinauralConvolver() = default;

void BinauralConvolver::reset() noexcept {
    for (auto& channel : partitioned_->direct_history) {
        channel.fill(0.0F);
    }
    partitioned_->direct_history_index = 0;
    partitioned_->morph_remaining = 0;
    partitioned_->morph_total = 0;
    partitioned_->reset_stream();
}

void BinauralConvolver::freeze_current_morph() noexcept {
    if (partitioned_->morph_remaining == 0 || partitioned_->morph_total == 0) {
        return;
    }
    const float alpha = 1.0F - static_cast<float>(partitioned_->morph_remaining) /
                                   static_cast<float>(partitioned_->morph_total);
    const std::size_t count = std::max(partitioned_->current_bank.tap_count, partitioned_->target_bank.tap_count);
    const std::size_t source_count =
        std::max(partitioned_->current_bank.source_count, partitioned_->target_bank.source_count);
    for (std::size_t path_index = 0; path_index < source_count * 2; ++path_index) {
        const std::size_t source = path_index / 2;
        const bool current_source_active = source < partitioned_->current_bank.source_count;
        const bool target_source_active = source < partitioned_->target_bank.source_count;
        for (std::size_t tap = 0; tap < count; ++tap) {
            const float current = current_source_active && tap < partitioned_->current_bank.tap_count
                                      ? partitioned_->current_bank.coefficients[path_index][tap]
                                      : 0.0F;
            const float target = target_source_active && tap < partitioned_->target_bank.tap_count
                                     ? partitioned_->target_bank.coefficients[path_index][tap]
                                     : 0.0F;
            partitioned_->current_bank.coefficients[path_index][tap] =
                current * (1.0F - alpha) + target * alpha;
        }
    }
    partitioned_->current_bank.tap_count = count;
    partitioned_->current_bank.source_count =
        std::max(partitioned_->current_bank.source_count, partitioned_->target_bank.source_count);
    partitioned_->blend_current_filter(alpha);
    partitioned_->rebuild_pending(partitioned_->current_filter, partitioned_->current_output);
    partitioned_->target_output = partitioned_->current_output;
    partitioned_->morph_remaining = 0;
    partitioned_->morph_total = 0;
}

bool BinauralConvolver::set_filters(const HrirFilterBank& filters, std::uint32_t morph_frames) noexcept {
    if (filters.tap_count == 0 || filters.tap_count > kMaximumHrirTaps ||
        filters.source_count == 0 || filters.source_count > kMaximumBinauralSources ||
        filters.coefficients.size() != kMaximumBinauralSources * 2) {
        return false;
    }
    freeze_current_morph();
    // Known RT limitation: set_filters() is consumed by the audio callback and
    // build_filter() still performs the partition-spectrum FFTs here. A future
    // bounded refactor must publish precomputed spectra from the HRTF worker;
    // this pass deliberately changes neither mailbox ownership nor slot reuse.
    partitioned_->build_filter(filters, partitioned_->target_filter);
    if (morph_frames == 0) {
        partitioned_->current_bank = filters;
        partitioned_->target_bank = filters;
        partitioned_->current_filter = partitioned_->target_filter;
        partitioned_->rebuild_pending(partitioned_->current_filter, partitioned_->current_output);
        partitioned_->target_output = partitioned_->current_output;
        partitioned_->morph_total = 0;
        partitioned_->morph_remaining = 0;
    } else {
        partitioned_->target_bank = filters;
        partitioned_->rebuild_pending(partitioned_->target_filter, partitioned_->target_output);
        partitioned_->morph_total = morph_frames;
        partitioned_->morph_remaining = morph_frames;
    }
    return true;
}

bool BinauralConvolver::morphing() const noexcept {
    return partitioned_->morph_remaining != 0;
}

std::uint32_t BinauralConvolver::morph_remaining_frames() const noexcept {
    return partitioned_->morph_remaining;
}

float BinauralConvolver::convolve(const float* history, const float* coefficients, std::size_t tap_count) noexcept {
    float sum = 0.0F;
    std::size_t index = 0;
#if defined(SOUND_SPATIALIZER_USE_SSE2)
    __m128 accumulator = _mm_setzero_ps();
    for (; index + 4 <= tap_count; index += 4) {
        const __m128 samples = _mm_loadu_ps(history + index);
        const __m128 taps = _mm_loadu_ps(coefficients + index);
        accumulator = _mm_add_ps(accumulator, _mm_mul_ps(samples, taps));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, accumulator);
    sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
#endif
    for (; index < tap_count; ++index) {
        sum += history[index] * coefficients[index];
    }
    return sum;
}

void BinauralConvolver::process(const DirectionalFrame* input, StereoFrame* output, std::size_t frame_count) noexcept {
    if (input == nullptr || output == nullptr) {
        return;
    }
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const DirectionalFrame input_frame = input[frame];
        partitioned_->direct_history_index =
            (partitioned_->direct_history_index + kTimeDomainHrirTaps - 1) % kTimeDomainHrirTaps;
        for (std::size_t source = 0; source < kMaximumBinauralSources; ++source) {
            auto& history = partitioned_->direct_history[source];
            history[partitioned_->direct_history_index] = input_frame.sources[source];
            history[partitioned_->direct_history_index + kTimeDomainHrirTaps] = input_frame.sources[source];
        }
        const std::size_t current_direct_taps =
            std::min(partitioned_->current_bank.tap_count, kTimeDomainHrirTaps);
        float current_left = 0.0F;
        float current_right = 0.0F;
        for (std::size_t source = 0; source < partitioned_->current_bank.source_count; ++source) {
            const float* history = partitioned_->direct_history[source].data() + partitioned_->direct_history_index;
            current_left += convolve(history, partitioned_->current_bank.path(source, 0).data(), current_direct_taps);
            current_right += convolve(history, partitioned_->current_bank.path(source, 1).data(), current_direct_taps);
        }
        const std::uint64_t output_block = partitioned_->sample_position / kPartitionFrames;
        const std::size_t output_offset = static_cast<std::size_t>(partitioned_->sample_position % kPartitionFrames);
        const StereoFrame current_tail =
            partitioned_->output_sample(partitioned_->current_output, output_block, output_offset);

        if (partitioned_->morph_remaining != 0) {
            const std::size_t target_direct_taps =
                std::min(partitioned_->target_bank.tap_count, kTimeDomainHrirTaps);
            float target_left = 0.0F;
            float target_right = 0.0F;
            for (std::size_t source = 0; source < partitioned_->target_bank.source_count; ++source) {
                const float* history = partitioned_->direct_history[source].data() + partitioned_->direct_history_index;
                target_left += convolve(history, partitioned_->target_bank.path(source, 0).data(), target_direct_taps);
                target_right += convolve(history, partitioned_->target_bank.path(source, 1).data(), target_direct_taps);
            }
            const StereoFrame target_tail =
                partitioned_->output_sample(partitioned_->target_output, output_block, output_offset);
            const float alpha = 1.0F - static_cast<float>(partitioned_->morph_remaining) /
                                           static_cast<float>(partitioned_->morph_total);
            output[frame] = {
                (current_left + current_tail.left) * (1.0F - alpha) +
                    (target_left + target_tail.left) * alpha,
                (current_right + current_tail.right) * (1.0F - alpha) +
                    (target_right + target_tail.right) * alpha,
            };
            --partitioned_->morph_remaining;
            if (partitioned_->morph_remaining == 0) {
                partitioned_->current_bank = partitioned_->target_bank;
                partitioned_->current_filter = partitioned_->target_filter;
                partitioned_->current_output = partitioned_->target_output;
            }
        } else {
            output[frame] = {current_left + current_tail.left, current_right + current_tail.right};
        }
        partitioned_->ingest(input_frame, partitioned_->morph_remaining != 0);
    }
}

void BinauralConvolver::process(const StereoFrame* input, StereoFrame* output, std::size_t frame_count) noexcept {
    if (input == nullptr || output == nullptr) return;
    constexpr std::size_t adapter_frames = 256;
    std::array<DirectionalFrame, adapter_frames> directional{};
    std::size_t offset = 0;
    while (offset < frame_count) {
        const std::size_t count = std::min(adapter_frames, frame_count - offset);
        for (std::size_t frame = 0; frame < count; ++frame) {
            directional[frame].sources[0] = input[offset + frame].left;
            directional[frame].sources[1] = input[offset + frame].right;
        }
        process(directional.data(), output + offset, count);
        offset += count;
    }
}

BiquadCoefficients design_biquad(const BiquadParameters& parameters, float sample_rate) noexcept {
    const float nyquist = std::max(100.0F, sample_rate * 0.5F);
    const float frequency = std::clamp(parameters.frequency_hz, 10.0F, nyquist * 0.98F);
    const float q = std::clamp(parameters.q, 0.05F, 30.0F);
    const float omega = 2.0F * kPi * frequency / sample_rate;
    const float cosine = std::cos(omega);
    const float sine = std::sin(omega);
    const float alpha = sine / (2.0F * q);
    const float amplitude = std::pow(10.0F, parameters.gain_db / 40.0F);

    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a0 = 1.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;

    switch (parameters.type) {
    case BiquadType::peaking:
        b0 = 1.0F + alpha * amplitude;
        b1 = -2.0F * cosine;
        b2 = 1.0F - alpha * amplitude;
        a0 = 1.0F + alpha / amplitude;
        a1 = -2.0F * cosine;
        a2 = 1.0F - alpha / amplitude;
        break;
    case BiquadType::low_shelf: {
        const float root_a = std::sqrt(amplitude);
        const float two_root_alpha = 2.0F * root_a * alpha;
        b0 = amplitude * ((amplitude + 1.0F) - (amplitude - 1.0F) * cosine + two_root_alpha);
        b1 = 2.0F * amplitude * ((amplitude - 1.0F) - (amplitude + 1.0F) * cosine);
        b2 = amplitude * ((amplitude + 1.0F) - (amplitude - 1.0F) * cosine - two_root_alpha);
        a0 = (amplitude + 1.0F) + (amplitude - 1.0F) * cosine + two_root_alpha;
        a1 = -2.0F * ((amplitude - 1.0F) + (amplitude + 1.0F) * cosine);
        a2 = (amplitude + 1.0F) + (amplitude - 1.0F) * cosine - two_root_alpha;
        break;
    }
    case BiquadType::high_shelf: {
        const float root_a = std::sqrt(amplitude);
        const float two_root_alpha = 2.0F * root_a * alpha;
        b0 = amplitude * ((amplitude + 1.0F) + (amplitude - 1.0F) * cosine + two_root_alpha);
        b1 = -2.0F * amplitude * ((amplitude - 1.0F) + (amplitude + 1.0F) * cosine);
        b2 = amplitude * ((amplitude + 1.0F) + (amplitude - 1.0F) * cosine - two_root_alpha);
        a0 = (amplitude + 1.0F) - (amplitude - 1.0F) * cosine + two_root_alpha;
        a1 = 2.0F * ((amplitude - 1.0F) - (amplitude + 1.0F) * cosine);
        a2 = (amplitude + 1.0F) - (amplitude - 1.0F) * cosine - two_root_alpha;
        break;
    }
    case BiquadType::high_pass:
        b0 = (1.0F + cosine) * 0.5F;
        b1 = -(1.0F + cosine);
        b2 = b0;
        a0 = 1.0F + alpha;
        a1 = -2.0F * cosine;
        a2 = 1.0F - alpha;
        break;
    case BiquadType::low_pass:
        b0 = (1.0F - cosine) * 0.5F;
        b1 = 1.0F - cosine;
        b2 = b0;
        a0 = 1.0F + alpha;
        a1 = -2.0F * cosine;
        a2 = 1.0F - alpha;
        break;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void LfeRenderer::prepare(float sample_rate) noexcept {
    coefficients_ = design_biquad({BiquadType::low_pass, 120.0F, 0.70710678F, 0.0F}, sample_rate);
    reset();
}

void LfeRenderer::reset() noexcept { states_ = {}; }

void LfeRenderer::configure(bool enabled, float gain_db, std::uint32_t transition_frames) noexcept {
    configured_gain_db_ = gain_db;
    target_enabled_ = enabled;
    target_gain_ = enabled ? db_to_linear(gain_db) : 0.0F;
    if (transition_frames == 0) {
        gain_ = target_gain_;
        gain_step_ = 0.0F;
        gain_frames_remaining_ = 0;
        enabled_ = enabled;
        if (!enabled_) reset();
        return;
    }
    enabled_ = true;
    gain_step_ = (target_gain_ - gain_) / static_cast<float>(transition_frames);
    gain_frames_remaining_ = transition_frames;
}

void LfeRenderer::set_enabled(bool enabled) noexcept { configure(enabled, configured_gain_db_, 0); }

void LfeRenderer::set_gain_db(float gain_db) noexcept { configure(target_enabled_, gain_db, 0); }

float LfeRenderer::process_sample(float input) noexcept {
    if (!enabled_) return 0.0F;
    if (gain_frames_remaining_ != 0) {
        gain_ += gain_step_;
        --gain_frames_remaining_;
        if (gain_frames_remaining_ == 0) {
            gain_ = target_gain_;
            gain_step_ = 0.0F;
            enabled_ = target_enabled_;
            if (!enabled_) {
                reset();
                return 0.0F;
            }
        }
    }
    float value = std::isfinite(input) ? input * gain_ : 0.0F;
    for (auto& state : states_) {
        const float output = coefficients_.b0 * value + state.z1;
        state.z1 = coefficients_.b1 * value - coefficients_.a1 * output + state.z2;
        state.z2 = coefficients_.b2 * value - coefficients_.a2 * output;
        value = output;
    }
    return value;
}

StereoFrame downmix_programme_to_stereo(const ProgrammeFrame& input, float rendered_lfe) noexcept {
    // Conventional -3 dB contribution for a phantom centre, each associated
    // surround, and the already-filtered mono LFE bed. The true-peak limiter
    // remains responsible for coherent multichannel summation headroom.
    constexpr float minus_three_db = 0.70710678F;
    return {
        input.front_left + minus_three_db * (input.front_center + input.surround_left + rendered_lfe),
        input.front_right + minus_three_db * (input.front_center + input.surround_right + rendered_lfe),
    };
}

DiffuseFieldEqualizer::DiffuseFieldEqualizer() noexcept {
    current_[0] = 1.0F;
    target_[0] = 1.0F;
}

void DiffuseFieldEqualizer::reset() noexcept {
    for (auto& channel : history_) channel.fill(0.0F);
    history_index_ = 0;
    mix_ = 0.0F;
    step_ = 0.0F;
    remaining_frames_ = 0;
}

void DiffuseFieldEqualizer::set_filter(std::span<const float> taps,
                                       std::uint32_t transition_frames) noexcept {
    std::array<float, kMaximumTaps> requested{};
    std::size_t requested_taps = std::min(taps.size(), kMaximumTaps);
    if (requested_taps == 0) {
        requested[0] = 1.0F;
        requested_taps = 1;
    } else {
        std::copy_n(taps.begin(), requested_taps, requested.begin());
    }

    // A pending transition is frozen at its current position before a new one
    // starts, so a burst of profile changes cannot leave a stale target behind.
    if (remaining_frames_ != 0) {
        for (std::size_t tap = 0; tap < kMaximumTaps; ++tap)
            current_[tap] = current_[tap] * (1.0F - mix_) + target_[tap] * mix_;
        current_taps_ = std::max(current_taps_, target_taps_);
        remaining_frames_ = 0;
        mix_ = 0.0F;
        step_ = 0.0F;
    }
    if (requested == current_ && requested_taps == current_taps_) return;

    target_ = requested;
    target_taps_ = requested_taps;
    if (transition_frames == 0) {
        current_ = target_;
        current_taps_ = target_taps_;
        mix_ = 0.0F;
        step_ = 0.0F;
        return;
    }
    mix_ = 0.0F;
    step_ = 1.0F / static_cast<float>(transition_frames);
    remaining_frames_ = transition_frames;
}

float DiffuseFieldEqualizer::convolve(const float* history, const float* coefficients,
                                      std::size_t tap_count) noexcept {
    float sum = 0.0F;
    for (std::size_t tap = 0; tap < tap_count; ++tap) sum += history[tap] * coefficients[tap];
    return sum;
}

void DiffuseFieldEqualizer::process(StereoFrame* frames, std::size_t frame_count) noexcept {
    if (frames == nullptr) return;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        history_index_ = (history_index_ + kMaximumTaps - 1) % kMaximumTaps;
        const float channels[2]{frames[frame].left, frames[frame].right};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            history_[channel][history_index_] = channels[channel];
            history_[channel][history_index_ + kMaximumTaps] = channels[channel];
        }
        const bool blending = remaining_frames_ != 0;
        float filtered[2]{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float* history = history_[channel].data() + history_index_;
            filtered[channel] = convolve(history, current_.data(), current_taps_);
            if (blending) {
                const float replacement = convolve(history, target_.data(), target_taps_);
                filtered[channel] = filtered[channel] * (1.0F - mix_) + replacement * mix_;
            }
        }
        frames[frame] = {filtered[0], filtered[1]};
        if (blending) {
            mix_ += step_;
            --remaining_frames_;
            if (remaining_frames_ == 0) {
                current_ = target_;
                current_taps_ = target_taps_;
                mix_ = 0.0F;
                step_ = 0.0F;
            }
        }
    }
}

bool StereoParametricEq::configure(std::span<const BiquadParameters> sections, float sample_rate) noexcept {
    if (sections.size() > kMaximumEqSections || sample_rate < 1'000.0F) {
        return false;
    }
    if (configuration_matches(sections, sample_rate)) return true;
    section_count_ = sections.size();
    for (std::size_t index = 0; index < section_count_; ++index) {
        parameters_[index] = sections[index];
        coefficients_[index] = design_biquad(sections[index], sample_rate);
    }
    sample_rate_ = sample_rate;
    reset();
    return true;
}

bool StereoParametricEq::configuration_matches(std::span<const BiquadParameters> sections,
                                               float sample_rate) const noexcept {
    if (sample_rate_ != sample_rate || section_count_ != sections.size()) return false;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const BiquadParameters& current = parameters_[index];
        const BiquadParameters& requested = sections[index];
        if (current.type != requested.type || current.frequency_hz != requested.frequency_hz ||
            current.q != requested.q || current.gain_db != requested.gain_db) return false;
    }
    return true;
}

void StereoParametricEq::reset() noexcept {
    for (auto& channel : states_) {
        for (auto& section : channel) {
            section = {};
        }
    }
}

void StereoParametricEq::process(StereoFrame* frames, std::size_t frame_count) noexcept {
    if (!enabled_ || frames == nullptr) {
        return;
    }
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float channels[2]{frames[frame].left, frames[frame].right};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            for (std::size_t section = 0; section < section_count_; ++section) {
                const auto& coefficient = coefficients_[section];
                auto& state = states_[channel][section];
                const float output = coefficient.b0 * channels[channel] + state.z1;
                state.z1 = coefficient.b1 * channels[channel] - coefficient.a1 * output + state.z2;
                state.z2 = coefficient.b2 * channels[channel] - coefficient.a2 * output;
                channels[channel] = output;
            }
        }
        frames[frame] = {channels[0], channels[1]};
    }
}

void TruePeakLimiter::prepare(float sample_rate) noexcept {
    sample_rate_ = std::max(1'000.0F, sample_rate);
    release_coefficient_ = std::exp(-1.0F / (0.080F * sample_rate_));
    reset();
}

void TruePeakLimiter::reset() noexcept {
    applied_gain_ = 1.0F;
    for (auto& channel : history_) {
        channel.fill(0.0F);
    }
    measured_true_peak_.store(0.0F, std::memory_order_relaxed);
}

void TruePeakLimiter::set_master_gain_db(float gain_db) noexcept {
    master_gain_ = db_to_linear(std::clamp(gain_db, -60.0F, 12.0F));
}

void TruePeakLimiter::set_ceiling_db(float ceiling_db) noexcept {
    ceiling_ = db_to_linear(std::clamp(ceiling_db, -12.0F, -0.01F));
}

float TruePeakLimiter::estimate_true_peak(std::size_t channel, float sample) noexcept {
    auto& values = history_[channel];
    values[0] = values[1];
    values[1] = values[2];
    values[2] = values[3];
    values[3] = sample;
    float peak = std::abs(sample);
    // Causal 4x cubic reconstruction of the interval between the two latest samples.
    for (int phase = 1; phase < 4; ++phase) {
        const float t = static_cast<float>(phase) * 0.25F;
        const float a0 = -0.5F * values[0] + 1.5F * values[1] - 1.5F * values[2] + 0.5F * values[3];
        const float a1 = values[0] - 2.5F * values[1] + 2.0F * values[2] - 0.5F * values[3];
        const float a2 = -0.5F * values[0] + 0.5F * values[2];
        const float interpolated = ((a0 * t + a1) * t + a2) * t + values[1];
        peak = std::max(peak, std::abs(interpolated));
    }
    return peak;
}

void TruePeakLimiter::process(StereoFrame* frames, std::size_t frame_count) noexcept {
    if (frames == nullptr) {
        return;
    }
    float block_peak = 0.0F;
    for (std::size_t index = 0; index < frame_count; ++index) {
        float left = frames[index].left * master_gain_;
        float right = frames[index].right * master_gain_;
        const float estimated_peak = std::max(estimate_true_peak(0, left), estimate_true_peak(1, right));
        block_peak = std::max(block_peak, estimated_peak);
        const float requested_gain = estimated_peak > ceiling_ ? ceiling_ / estimated_peak : 1.0F;
        if (requested_gain < applied_gain_) {
            applied_gain_ = requested_gain; // zero-lookahead, immediate attack
        } else {
            applied_gain_ = requested_gain + release_coefficient_ * (applied_gain_ - requested_gain);
        }
        left *= applied_gain_;
        right *= applied_gain_;
        frames[index] = {std::clamp(left, -ceiling_, ceiling_), std::clamp(right, -ceiling_, ceiling_)};
    }
    measured_true_peak_.store(block_peak, std::memory_order_relaxed);
}

float TruePeakLimiter::gain_reduction_db() const noexcept { return -linear_to_db(applied_gain_); }

void PotentialBinauralDetector::prepare(float sample_rate) noexcept {
    const float safe_rate = std::clamp(sample_rate, 8'000.0F, 192'000.0F);
    downsample_factor_ = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(safe_rate / 12'000.0F)), 1, 16);
    const float analysis_rate = safe_rate / static_cast<float>(downsample_factor_);
    maximum_lag_ = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(analysis_rate * 0.0008F)), 1, 16);
    const float windows_per_second = analysis_rate / static_cast<float>(kAnalysisSamples);
    positive_windows_required_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(2.0F * windows_per_second)));
    negative_windows_required_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(4.0F * windows_per_second)));
    reset();
}

void PotentialBinauralDetector::reset() noexcept {
    left_.fill(0.0F);
    right_.fill(0.0F);
    analysis_index_ = 0;
    downsample_count_ = 0;
    downsample_left_ = 0.0F;
    downsample_right_ = 0.0F;
    positive_windows_ = 0;
    negative_windows_ = 0;
    potentially_binaural_ = false;
}

void PotentialBinauralDetector::process(const StereoFrame* frames, std::size_t frame_count) noexcept {
    if (frames == nullptr) return;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        downsample_left_ += frames[frame].left;
        downsample_right_ += frames[frame].right;
        if (++downsample_count_ != downsample_factor_) continue;
        const float scale = 1.0F / static_cast<float>(downsample_factor_);
        left_[analysis_index_] = downsample_left_ * scale;
        right_[analysis_index_] = downsample_right_ * scale;
        downsample_count_ = 0;
        downsample_left_ = 0.0F;
        downsample_right_ = 0.0F;
        if (++analysis_index_ == kAnalysisSamples) {
            analyze_window();
            analysis_index_ = 0;
        }
    }
}

void PotentialBinauralDetector::analyze_window() noexcept {
    double mean_left = 0.0;
    double mean_right = 0.0;
    for (std::size_t index = 0; index < kAnalysisSamples; ++index) {
        mean_left += left_[index];
        mean_right += right_[index];
    }
    mean_left /= static_cast<double>(kAnalysisSamples);
    mean_right /= static_cast<double>(kAnalysisSamples);

    double left_energy = 0.0;
    double right_energy = 0.0;
    double mid_energy = 0.0;
    double side_energy = 0.0;
    double zero_cross = 0.0;
    for (std::size_t index = 0; index < kAnalysisSamples; ++index) {
        const double left = static_cast<double>(left_[index]) - mean_left;
        const double right = static_cast<double>(right_[index]) - mean_right;
        left_energy += left * left;
        right_energy += right * right;
        zero_cross += left * right;
        const double mid = 0.5 * (left + right);
        const double side = 0.5 * (left - right);
        mid_energy += mid * mid;
        side_energy += side * side;
    }
    const double full_normalizer = std::sqrt(left_energy * right_energy) + 1.0e-20;
    const double zero_correlation = std::abs(zero_cross / full_normalizer);
    double best_nonzero_correlation = 0.0;
    for (std::uint32_t lag = 1; lag <= maximum_lag_; ++lag) {
        for (int direction = 0; direction < 2; ++direction) {
            double cross = 0.0;
            double first_energy = 0.0;
            double second_energy = 0.0;
            for (std::size_t index = 0; index + lag < kAnalysisSamples; ++index) {
                const std::size_t shifted = index + lag;
                const double first = direction == 0 ? static_cast<double>(left_[index]) - mean_left
                                                    : static_cast<double>(right_[index]) - mean_right;
                const double second = direction == 0 ? static_cast<double>(right_[shifted]) - mean_right
                                                     : static_cast<double>(left_[shifted]) - mean_left;
                cross += first * second;
                first_energy += first * first;
                second_energy += second * second;
            }
            const double normalized_cross = std::abs(cross) / (std::sqrt(first_energy * second_energy) + 1.0e-20);
            best_nonzero_correlation = std::max(best_nonzero_correlation, normalized_cross);
        }
    }
    const double side_to_mid = side_energy / (mid_energy + 1.0e-20);
    const double level_ratio = left_energy / (right_energy + 1.0e-20);
    const bool enough_signal = left_energy + right_energy > 1.0e-8;
    const bool evidence = enough_signal && best_nonzero_correlation > 0.72 &&
                          best_nonzero_correlation > zero_correlation + 0.12 &&
                          side_to_mid > 0.02 && side_to_mid < 8.0 && level_ratio > 0.1 && level_ratio < 10.0;
    if (evidence) {
        negative_windows_ = 0;
        positive_windows_ = std::min(positive_windows_ + 1U, positive_windows_required_);
        if (positive_windows_ >= positive_windows_required_) potentially_binaural_ = true;
    } else {
        positive_windows_ = potentially_binaural_ ? positive_windows_ : 0;
        negative_windows_ = std::min(negative_windows_ + 1U, negative_windows_required_);
        if (potentially_binaural_ && negative_windows_ >= negative_windows_required_) {
            potentially_binaural_ = false;
            positive_windows_ = 0;
        }
    }
}

DriftController::DriftController() noexcept : DriftController(Settings{}) {}
DriftController::DriftController(Settings settings) noexcept : settings_(settings) {}

void DriftController::reset() noexcept { integral_ = 0.0F; }

float DriftController::update(std::size_t fill_frames, std::size_t target_frames, float elapsed_seconds) noexcept {
    if (target_frames == 0) {
        return 1.0F;
    }
    const float error = (static_cast<float>(fill_frames) - static_cast<float>(target_frames)) /
                        static_cast<float>(target_frames);
    const float maximum = settings_.maximum_correction_ppm * 1.0e-6F;
    integral_ = std::clamp(integral_ + error * settings_.integral_gain * std::max(elapsed_seconds, 0.0F),
                           -maximum, maximum);
    const float correction = std::clamp(error * settings_.proportional_gain + integral_, -maximum, maximum);
    return 1.0F + correction;
}

void ClockDriftEstimator::reset() noexcept {
    previous_capture_position_ = 0;
    previous_capture_qpc_ = 0;
    previous_render_position_ = 0;
    previous_render_qpc_ = 0;
    ratio_ = 1.0F;
    initialized_ = false;
}

float ClockDriftEstimator::update(std::uint64_t capture_position, std::uint64_t capture_frequency,
                                  std::uint64_t capture_qpc_100ns, std::uint64_t render_position,
                                  std::uint64_t render_frequency, std::uint64_t render_qpc_100ns) noexcept {
    if (capture_frequency == 0 || render_frequency == 0) return ratio_;
    if (!initialized_) {
        previous_capture_position_ = capture_position;
        previous_capture_qpc_ = capture_qpc_100ns;
        previous_render_position_ = render_position;
        previous_render_qpc_ = render_qpc_100ns;
        initialized_ = true;
        return ratio_;
    }
    if (capture_position < previous_capture_position_ || render_position < previous_render_position_ ||
        capture_qpc_100ns < previous_capture_qpc_ || render_qpc_100ns < previous_render_qpc_) {
        reset();
        return ratio_;
    }
    if (capture_position == previous_capture_position_ || render_position == previous_render_position_ ||
        capture_qpc_100ns == previous_capture_qpc_ || render_qpc_100ns == previous_render_qpc_) return ratio_;
    const double capture_elapsed = static_cast<double>(capture_qpc_100ns - previous_capture_qpc_) * 1.0e-7;
    const double render_elapsed = static_cast<double>(render_qpc_100ns - previous_render_qpc_) * 1.0e-7;
    const double capture_relative_rate = static_cast<double>(capture_position - previous_capture_position_) /
                                         static_cast<double>(capture_frequency) / capture_elapsed;
    const double render_relative_rate = static_cast<double>(render_position - previous_render_position_) /
                                        static_cast<double>(render_frequency) / render_elapsed;
    previous_capture_position_ = capture_position;
    previous_capture_qpc_ = capture_qpc_100ns;
    previous_render_position_ = render_position;
    previous_render_qpc_ = render_qpc_100ns;
    if (!std::isfinite(capture_relative_rate) || !std::isfinite(render_relative_rate) ||
        capture_relative_rate < 0.9 || capture_relative_rate > 1.1 ||
        render_relative_rate < 0.9 || render_relative_rate > 1.1) return ratio_;
    const float observed_ratio = std::clamp(static_cast<float>(capture_relative_rate / render_relative_rate),
                                            0.998F, 1.002F);
    ratio_ += 0.02F * (observed_ratio - ratio_);
    return ratio_;
}

AsyncStereoResampler::AsyncStereoResampler(std::size_t fifo_capacity_frames) : fifo_(fifo_capacity_frames) {
    target_fill_frames_ = std::min<std::size_t>(fifo_capacity_frames / 4, 512);
    build_kernel_table();
}

void AsyncStereoResampler::build_kernel_table() noexcept {
    constexpr float radius = static_cast<float>(kKernelTaps) * 0.5F;
    constexpr int centre_index = static_cast<int>(kKernelTaps / 2) - 1;
    for (std::size_t phase_index = 0; phase_index < kKernelPhases; ++phase_index) {
        const float fraction = static_cast<float>(phase_index) / static_cast<float>(kKernelPhases);
        float sum = 0.0F;
        for (std::size_t tap = 0; tap < kKernelTaps; ++tap) {
            const float offset = static_cast<float>(static_cast<int>(tap) - centre_index);
            const float distance = fraction - offset;
            const float sinc = std::abs(distance) < 1.0e-7F
                                   ? 1.0F
                                   : std::sin(kPi * distance) / (kPi * distance);
            const float normalized_distance = std::clamp(distance / radius, -1.0F, 1.0F);
            const float window = 0.42F + 0.5F * std::cos(kPi * normalized_distance) +
                                 0.08F * std::cos(2.0F * kPi * normalized_distance);
            kernel_table_[phase_index][tap] = sinc * window;
            sum += kernel_table_[phase_index][tap];
        }
        if (std::abs(sum) > 1.0e-8F) {
            for (float& coefficient : kernel_table_[phase_index]) {
                coefficient /= sum;
            }
        }
    }
}

bool AsyncStereoResampler::prime() noexcept {
    constexpr std::size_t centre_index = kKernelTaps / 2 - 1;
    constexpr std::size_t required_future_samples = kKernelTaps - centre_index;
    if (fifo_.size() <
        std::max(required_future_samples, startup_fill_frames_)) {
        return false;
    }
    source_window_.fill({});
    for (std::size_t tap = centre_index; tap < kKernelTaps; ++tap) {
        if (!fifo_.try_pop(source_window_[tap])) {
            return false;
        }
    }
    primed_ = true;
    return true;
}

std::size_t AsyncStereoResampler::push(const StereoFrame* frames, std::size_t frame_count) noexcept {
    if (frames == nullptr) {
        return 0;
    }
    const std::size_t pushed = fifo_.push(frames, frame_count);
    if (pushed != frame_count) {
        // Diagnostics report discontinuity events, not the number of frames
        // discarded by a single full-FIFO write.
        overruns_.fetch_add(1, std::memory_order_relaxed);
        request_rebase();
    }
    return pushed;
}

std::size_t AsyncStereoResampler::render(StereoFrame* output, std::size_t frame_count,
                                         float output_sample_rate) noexcept {
    if (output == nullptr || frame_count == 0) {
        return 0;
    }
    const auto reset_interpolator = [this] {
        drift_controller_.reset();
        source_window_.fill({});
        phase_ = 0.0;
        primed_ = false;
        current_ratio_.store(nominal_ratio_, std::memory_order_relaxed);
    };

    bool rebased = false;
    const std::uint64_t requested_revision =
        requested_rebase_revision_.load(std::memory_order_acquire);
    if (requested_revision != applied_rebase_revision_) {
        const std::size_t boundary =
            requested_rebase_boundary_.load(std::memory_order_relaxed);
        (void)fifo_.discard_before(boundary);
        reset_interpolator();
        applied_rebase_revision_ = requested_revision;
        rebased = true;
    }

    const std::size_t fill_before_render = fifo_.size();
    if (rebase_threshold_frames_ != 0 &&
        fill_before_render > rebase_threshold_frames_) {
        const std::size_t retained =
            std::min(fill_before_render, target_fill_frames_);
        (void)fifo_.discard_oldest(fill_before_render - retained);
        reset_interpolator();
        rebased = true;
    }
    if (rebased)
        rebases_.fetch_add(1, std::memory_order_relaxed);

    if (!primed_) {
        if (!prime()) {
            std::fill_n(output, frame_count, StereoFrame{});
            // Startup silence before the first usable capture packet is normal
            // priming, not a stream discontinuity.
            return 0;
        }
    }
    const float elapsed = static_cast<float>(frame_count) / std::max(1.0F, output_sample_rate);
    const float ratio = nominal_ratio_ * drift_controller_.update(fifo_.size(), target_fill_frames_, elapsed);
    current_ratio_.store(ratio, std::memory_order_relaxed);

    bool block_underfed = false;
    std::size_t produced = 0;
    for (; produced < frame_count; ++produced) {
        const std::size_t phase_index = std::min<std::size_t>(
            static_cast<std::size_t>(phase_ * static_cast<double>(kKernelPhases)), kKernelPhases - 1);
        const auto& kernel = kernel_table_[phase_index];
        StereoFrame filtered{};
        for (std::size_t tap = 0; tap < kKernelTaps; ++tap) {
            filtered.left += source_window_[tap].left * kernel[tap];
            filtered.right += source_window_[tap].right * kernel[tap];
        }
        output[produced] = filtered;
        phase_ += ratio;
        while (phase_ >= 1.0) {
            std::move(source_window_.begin() + 1, source_window_.end(), source_window_.begin());
            if (!fifo_.try_pop(source_window_.back())) {
                source_window_.back() = {};
                block_underfed = true;
            }
            phase_ -= 1.0;
        }
    }
    if (block_underfed) underruns_.fetch_add(1, std::memory_order_relaxed);
    return frame_count;
}

void AsyncStereoResampler::request_rebase() noexcept {
    requested_rebase_boundary_.store(fifo_.producer_sequence(),
                                     std::memory_order_relaxed);
    requested_rebase_revision_.fetch_add(1, std::memory_order_release);
}

void AsyncStereoResampler::reset() noexcept {
    fifo_.reset();
    drift_controller_.reset();
    source_window_.fill({});
    phase_ = 0.0;
    primed_ = false;
    underruns_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    rebases_.store(0, std::memory_order_relaxed);
    requested_rebase_boundary_.store(fifo_.producer_sequence(),
                                     std::memory_order_relaxed);
    applied_rebase_revision_ =
        requested_rebase_revision_.load(std::memory_order_acquire);
    current_ratio_.store(nominal_ratio_, std::memory_order_relaxed);
}

void AsyncStereoResampler::set_nominal_ratio(float input_rate_over_output_rate) noexcept {
    nominal_ratio_ = std::clamp(input_rate_over_output_rate, 0.5F, 2.0F);
}

AsyncProgrammeResampler::AsyncProgrammeResampler(std::size_t fifo_capacity_frames) : fifo_(fifo_capacity_frames) {
    target_fill_frames_ = std::min<std::size_t>(fifo_capacity_frames / 4, 512);
    build_kernel_table();
}

void AsyncProgrammeResampler::build_kernel_table() noexcept {
    constexpr float radius = static_cast<float>(kKernelTaps) * 0.5F;
    constexpr int centre_index = static_cast<int>(kKernelTaps / 2) - 1;
    for (std::size_t phase_index = 0; phase_index < kKernelPhases; ++phase_index) {
        const float fraction = static_cast<float>(phase_index) / static_cast<float>(kKernelPhases);
        float sum = 0.0F;
        for (std::size_t tap = 0; tap < kKernelTaps; ++tap) {
            const float offset = static_cast<float>(static_cast<int>(tap) - centre_index);
            const float distance = fraction - offset;
            const float sinc = std::abs(distance) < 1.0e-7F
                                   ? 1.0F
                                   : std::sin(kPi * distance) / (kPi * distance);
            const float normalized_distance = std::clamp(distance / radius, -1.0F, 1.0F);
            const float window = 0.42F + 0.5F * std::cos(kPi * normalized_distance) +
                                 0.08F * std::cos(2.0F * kPi * normalized_distance);
            kernel_table_[phase_index][tap] = sinc * window;
            sum += kernel_table_[phase_index][tap];
        }
        if (std::abs(sum) > 1.0e-8F) {
            for (float& coefficient : kernel_table_[phase_index]) coefficient /= sum;
        }
    }
}

bool AsyncProgrammeResampler::prime() noexcept {
    constexpr std::size_t centre_index = kKernelTaps / 2 - 1;
    constexpr std::size_t required_future_samples = kKernelTaps - centre_index;
    if (fifo_.size() < required_future_samples) return false;
    source_window_.fill({});
    for (std::size_t tap = centre_index; tap < kKernelTaps; ++tap) {
        if (!fifo_.try_pop(source_window_[tap])) return false;
    }
    primed_ = true;
    return true;
}

std::size_t AsyncProgrammeResampler::push(const ProgrammeFrame* frames, std::size_t frame_count) noexcept {
    if (frames == nullptr) return 0;
    const std::size_t pushed = fifo_.push(frames, frame_count);
    if (pushed != frame_count) overruns_.fetch_add(1, std::memory_order_relaxed);
    return pushed;
}

std::size_t AsyncProgrammeResampler::render(ProgrammeFrame* output, std::size_t frame_count,
                                             float output_sample_rate) noexcept {
    if (output == nullptr || frame_count == 0) return 0;
    if (!primed_ && !prime()) {
        std::fill_n(output, frame_count, ProgrammeFrame{});
        return 0;
    }
    const float elapsed = static_cast<float>(frame_count) / std::max(1.0F, output_sample_rate);
    const float ratio = nominal_ratio_ * drift_controller_.update(fifo_.size(), target_fill_frames_, elapsed);
    current_ratio_.store(ratio, std::memory_order_relaxed);

    bool block_underfed = false;
    for (std::size_t produced = 0; produced < frame_count; ++produced) {
        const std::size_t phase_index = std::min<std::size_t>(
            static_cast<std::size_t>(phase_ * static_cast<double>(kKernelPhases)), kKernelPhases - 1);
        const auto& kernel = kernel_table_[phase_index];
        ProgrammeFrame filtered{};
        for (std::size_t tap = 0; tap < kKernelTaps; ++tap) {
            for (std::size_t channel = 0; channel < kProgrammeChannelCount; ++channel)
                filtered[channel] += source_window_[tap][channel] * kernel[tap];
        }
        output[produced] = filtered;
        phase_ += ratio;
        while (phase_ >= 1.0) {
            std::move(source_window_.begin() + 1, source_window_.end(), source_window_.begin());
            if (!fifo_.try_pop(source_window_.back())) {
                source_window_.back() = {};
                block_underfed = true;
            }
            phase_ -= 1.0;
        }
    }
    if (block_underfed) underruns_.fetch_add(1, std::memory_order_relaxed);
    return frame_count;
}

void AsyncProgrammeResampler::reset() noexcept {
    fifo_.reset();
    drift_controller_.reset();
    source_window_.fill({});
    phase_ = 0.0;
    primed_ = false;
    underruns_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    current_ratio_.store(nominal_ratio_, std::memory_order_relaxed);
}

void AsyncProgrammeResampler::set_nominal_ratio(float input_rate_over_output_rate) noexcept {
    nominal_ratio_ = std::clamp(input_rate_over_output_rate, 0.5F, 2.0F);
}

} // namespace sound_spatializer
