#include "sound_spatializer/acoustics.hpp"

#include "sound_spatializer/dsp.hpp"
#include "sound_spatializer/hrtf.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP)
#include <immintrin.h>
#define SOUND_SPATIALIZER_ACOUSTICS_USE_SSE2 1
#endif

namespace sound_spatializer {
namespace {

[[nodiscard]] bool point_inside_room(const Vec3f& point, const Vec3f& dimensions) noexcept {
    return point.x >= -dimensions.x * 0.5F && point.x <= dimensions.x * 0.5F &&
           point.y >= 0.0F && point.y <= dimensions.y &&
           point.z >= -dimensions.z * 0.5F && point.z <= dimensions.z * 0.5F;
}

[[nodiscard]] Vec3f mirror_across_surface(const Vec3f& point, const Vec3f& dimensions,
                                          std::size_t surface) noexcept {
    Vec3f result = point;
    switch (surface) {
    case 0: result.x = -dimensions.x - point.x; break;
    case 1: result.x = dimensions.x - point.x; break;
    case 2: result.z = -dimensions.z - point.z; break;
    case 3: result.z = dimensions.z - point.z; break;
    case 4: result.y = -point.y; break;
    case 5: result.y = 2.0F * dimensions.y - point.y; break;
    default: break;
    }
    return result;
}

[[nodiscard]] float reflection_gain(float absorption, float diffusion) noexcept {
    const float remaining_energy = (1.0F - std::clamp(absorption, 0.0F, 0.99F)) *
                                   (1.0F - std::clamp(diffusion, 0.0F, 0.99F));
    return std::sqrt(std::max(0.0F, remaining_energy));
}

[[nodiscard]] MaterialBands multiply(const MaterialBands& a, const MaterialBands& b) noexcept {
    return {a.low * b.low, a.mid * b.mid, a.high * b.high};
}

[[nodiscard]] MaterialBands surface_reflection(const SurfaceMaterial& material) noexcept {
    return {
        reflection_gain(material.absorption.low, material.diffusion.low),
        reflection_gain(material.absorption.mid, material.diffusion.mid),
        reflection_gain(material.absorption.high, material.diffusion.high),
    };
}

struct ImageState {
    Vec3f position{};
    MaterialBands gain{1.0F, 1.0F, 1.0F};
    std::uint32_t order{};
    std::size_t last_surface{6};
};

[[nodiscard]] float associated_legendre(unsigned degree, unsigned order, float value) noexcept {
    float pmm = 1.0F;
    if (order > 0) {
        const float root = std::sqrt(std::max(0.0F, 1.0F - value * value));
        float factor = 1.0F;
        for (unsigned index = 1; index <= order; ++index) {
            pmm *= factor * root; // no Condon-Shortley phase in the audio convention
            factor += 2.0F;
        }
    }
    if (degree == order) {
        return pmm;
    }
    float pmmp1 = value * static_cast<float>(2 * order + 1) * pmm;
    if (degree == order + 1) {
        return pmmp1;
    }
    float previous = pmm;
    float current = pmmp1;
    for (unsigned current_degree = order + 2; current_degree <= degree; ++current_degree) {
        const float next = (static_cast<float>(2 * current_degree - 1) * value * current -
                            static_cast<float>(current_degree + order - 1) * previous) /
                           static_cast<float>(current_degree - order);
        previous = current;
        current = next;
    }
    return current;
}

[[nodiscard]] float factorial_ratio(unsigned numerator, unsigned denominator) noexcept {
    float result = 1.0F;
    for (unsigned value = numerator + 1; value <= denominator; ++value) {
        result /= static_cast<float>(value);
    }
    return result;
}

} // namespace

std::vector<ReflectionTap> ImageSourceModel::calculate(const Vec3f& source_position,
                                                        const Vec3f& listener_position,
                                                        const RoomConfig& room) const {
    std::vector<ReflectionTap> result;
    if (!room.enabled || room.dimensions_m.x <= 0.1F || room.dimensions_m.y <= 0.1F ||
        room.dimensions_m.z <= 0.1F || !point_inside_room(source_position, room.dimensions_m) ||
        !point_inside_room(listener_position, room.dimensions_m)) {
        return result;
    }

    const std::uint32_t maximum_order = std::min<std::uint32_t>(room.reflection_order, 2);
    const float maximum_delay = std::clamp(room.early_window_ms, 1.0F, 80.0F) * 0.001F;
    std::vector<ImageState> frontier{{source_position, {1.0F, 1.0F, 1.0F}, 0, 6}};
    std::map<std::tuple<int, int, int, std::uint32_t>, ReflectionTap> unique_taps;

    for (std::uint32_t order = 1; order <= maximum_order; ++order) {
        std::vector<ImageState> next_frontier;
        next_frontier.reserve(frontier.size() * 5);
        for (const ImageState& state : frontier) {
            for (std::size_t surface = 0; surface < room.surfaces.size(); ++surface) {
                if (surface == state.last_surface) {
                    continue;
                }
                const Vec3f image = mirror_across_surface(state.position, room.dimensions_m, surface);
                const MaterialBands material_gain = surface_reflection(room.surfaces[surface]);
                const MaterialBands path_gain = multiply(state.gain, material_gain);
                next_frontier.push_back({image, path_gain, order, surface});

                const Vec3f offset = image - listener_position;
                const float distance = length(offset);
                const float delay = distance / kSpeedOfSoundMps;
                if (delay > maximum_delay || distance <= 0.01F) {
                    continue;
                }
                const float distance_attenuation = 1.0F / std::max(1.0F, distance);
                ReflectionTap tap{
                    delay,
                    normalized(offset),
                    {path_gain.low * distance_attenuation, path_gain.mid * distance_attenuation,
                     path_gain.high * distance_attenuation},
                    order,
                };
                const auto key = std::tuple{
                    static_cast<int>(std::lround(image.x * 1'000.0F)),
                    static_cast<int>(std::lround(image.y * 1'000.0F)),
                    static_cast<int>(std::lround(image.z * 1'000.0F)), order};
                auto [iterator, inserted] = unique_taps.try_emplace(key, tap);
                if (!inserted && tap.gain.mid > iterator->second.gain.mid) {
                    iterator->second = tap;
                }
            }
        }
        frontier = std::move(next_frontier);
    }

    result.reserve(unique_taps.size());
    for (const auto& [key, tap] : unique_taps) {
        (void)key;
        result.push_back(tap);
    }
    std::sort(result.begin(), result.end(), [](const ReflectionTap& left, const ReflectionTap& right) {
        return left.delay_seconds < right.delay_seconds;
    });
    return result;
}

MaterialBands estimate_room_rt60_eyring(const RoomConfig& room) noexcept {
    const float width = std::max(0.1F, room.dimensions_m.x);
    const float height = std::max(0.1F, room.dimensions_m.y);
    const float depth = std::max(0.1F, room.dimensions_m.z);
    const std::array<float, 6> areas{
        height * depth, height * depth, width * height, width * height, width * depth, width * depth};
    const float total_area = 2.0F * (height * depth + width * height + width * depth);
    const float volume = width * height * depth;
    const auto estimate_band = [&](auto absorption_member) noexcept {
        float area_weighted_absorption = 0.0F;
        for (std::size_t surface = 0; surface < room.surfaces.size(); ++surface) {
            area_weighted_absorption += areas[surface] *
                                        std::clamp(absorption_member(room.surfaces[surface].absorption), 0.0F, 0.99F);
        }
        const float mean_absorption = std::clamp(area_weighted_absorption / total_area, 0.001F, 0.99F);
        const float equivalent_absorption = -total_area * std::log(1.0F - mean_absorption);
        return std::clamp(0.161F * volume / std::max(0.001F, equivalent_absorption), 0.08F, 8.0F);
    };
    return {
        estimate_band([](const MaterialBands& value) { return value.low; }),
        estimate_band([](const MaterialBands& value) { return value.mid; }),
        estimate_band([](const MaterialBands& value) { return value.high; }),
    };
}

std::array<float, AmbisonicEncoderOrder3::kChannelCount>
AmbisonicEncoderOrder3::encode_direction(const Vec3f& input_direction) noexcept {
    std::array<float, kChannelCount> result{};
    const Vec3f direction = normalized(input_direction);
    // ACN/SN3D is evaluated in the conventional Ambisonic frame X-front, Y-left, Z-up.
    // Application world coordinates are X-right, Y-up, Z-forward.
    const float ambisonic_x = direction.z;
    const float ambisonic_y = -direction.x;
    const float ambisonic_z = direction.y;
    const float azimuth = std::atan2(ambisonic_y, ambisonic_x);
    const float vertical = std::clamp(ambisonic_z, -1.0F, 1.0F);
    for (unsigned degree = 0; degree <= 3; ++degree) {
        for (int order = -static_cast<int>(degree); order <= static_cast<int>(degree); ++order) {
            const unsigned absolute_order = static_cast<unsigned>(std::abs(order));
            const float normalization = std::sqrt((absolute_order == 0 ? 1.0F : 2.0F) *
                                                  factorial_ratio(degree - absolute_order, degree + absolute_order));
            const float polynomial = associated_legendre(degree, absolute_order, vertical);
            const float angular = order < 0 ? std::sin(static_cast<float>(absolute_order) * azimuth)
                                            : std::cos(static_cast<float>(absolute_order) * azimuth);
            const std::size_t acn = static_cast<std::size_t>(degree * (degree + 1) + order);
            result[acn] = normalization * polynomial * angular;
        }
    }
    return result;
}

bool EarlyReflectionProcessor::prepare(float sample_rate, float maximum_delay_ms) {
    if (sample_rate < 1'000.0F || maximum_delay_ms <= 0.0F || maximum_delay_ms > 1'000.0F) {
        return false;
    }
    sample_rate_ = sample_rate;
    const std::size_t samples = static_cast<std::size_t>(std::ceil(sample_rate * maximum_delay_ms * 0.001F)) + 2;
    for (auto& delay_line : delay_lines_) delay_line.assign(samples, 0.0F);
    active_taps_.reserve(kMaximumReflectionTaps);
    target_taps_.reserve(kMaximumReflectionTaps);
    low_coefficient_ = 1.0F - std::exp(-2.0F * kPi * 250.0F / sample_rate_);
    high_coefficient_ = 1.0F - std::exp(-2.0F * kPi * 4'000.0F / sample_rate_);
    low_state_ = 0.0F;
    mid_lowpass_state_ = 0.0F;
    write_index_ = 0;
    return true;
}

bool EarlyReflectionProcessor::set_reflections(std::span<const ReflectionTap> reflections,
                                                std::uint32_t crossfade_frames) {
    if (delay_lines_[0].empty() || reflections.size() > kMaximumReflectionTaps) {
        return false;
    }
    target_taps_.clear();
    for (const ReflectionTap& reflection : reflections) {
        const std::size_t delay = static_cast<std::size_t>(std::lround(reflection.delay_seconds * sample_rate_));
        if (delay >= delay_lines_[0].size()) {
            continue;
        }
        EncodedTap encoded{};
        encoded.delay_frames = static_cast<std::uint32_t>(delay);
        const auto harmonics = AmbisonicEncoderOrder3::encode_direction(reflection.arrival_direction);
        const std::array<float, 3> material_gains{reflection.gain.low, reflection.gain.mid, reflection.gain.high};
        for (std::size_t band = 0; band < encoded.band_gains.size(); ++band) {
            for (std::size_t channel = 0; channel < kAmbisonicChannels; ++channel) {
                encoded.band_gains[band][channel] = harmonics[channel] * material_gains[band];
            }
        }
        target_taps_.push_back(encoded);
    }
    if (active_taps_.empty() || crossfade_frames == 0) {
        active_taps_.swap(target_taps_);
        target_taps_.clear();
        crossfade_total_ = 0;
        crossfade_remaining_ = 0;
    } else {
        crossfade_total_ = crossfade_frames;
        crossfade_remaining_ = crossfade_frames;
    }
    return true;
}

void EarlyReflectionProcessor::reset() noexcept {
    for (auto& delay_line : delay_lines_) std::fill(delay_line.begin(), delay_line.end(), 0.0F);
    write_index_ = 0;
    crossfade_remaining_ = 0;
    low_state_ = 0.0F;
    mid_lowpass_state_ = 0.0F;
}

std::array<float, EarlyReflectionProcessor::kAmbisonicChannels> EarlyReflectionProcessor::render_taps(
    const std::vector<EncodedTap>& taps, std::size_t write_index) const noexcept {
    std::array<float, kAmbisonicChannels> output{};
    for (const EncodedTap& tap : taps) {
        const std::size_t read_index = (write_index + delay_lines_[0].size() - tap.delay_frames) %
                                       delay_lines_[0].size();
        for (std::size_t band = 0; band < delay_lines_.size(); ++band) {
            const float sample = delay_lines_[band][read_index];
            for (std::size_t channel = 0; channel < output.size(); ++channel) {
                output[channel] += sample * tap.band_gains[band][channel];
            }
        }
    }
    return output;
}

void EarlyReflectionProcessor::process(const float* mono_input,
                                       std::array<float, kAmbisonicChannels>* ambisonic_output,
                                       std::size_t frame_count) noexcept {
    if (mono_input == nullptr || ambisonic_output == nullptr || delay_lines_[0].empty()) {
        return;
    }
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        low_state_ += low_coefficient_ * (mono_input[frame] - low_state_);
        mid_lowpass_state_ += high_coefficient_ * (mono_input[frame] - mid_lowpass_state_);
        delay_lines_[0][write_index_] = low_state_;
        delay_lines_[1][write_index_] = mid_lowpass_state_ - low_state_;
        delay_lines_[2][write_index_] = mono_input[frame] - mid_lowpass_state_;
        const auto active = render_taps(active_taps_, write_index_);
        if (crossfade_remaining_ != 0) {
            const auto target = render_taps(target_taps_, write_index_);
            const float alpha = 1.0F - static_cast<float>(crossfade_remaining_) /
                                           static_cast<float>(crossfade_total_);
            for (std::size_t channel = 0; channel < kAmbisonicChannels; ++channel) {
                ambisonic_output[frame][channel] = active[channel] * (1.0F - alpha) + target[channel] * alpha;
            }
            --crossfade_remaining_;
            if (crossfade_remaining_ == 0) {
                active_taps_.swap(target_taps_); // constant-time and allocation-free on the audio thread
            }
        } else {
            ambisonic_output[frame] = active;
        }
        write_index_ = (write_index_ + 1) % delay_lines_[0].size();
    }
}

AmbisonicBinauralDecoderOrder3::AmbisonicBinauralDecoderOrder3() noexcept {
    // Four equal-area latitude bands and eight equidistant azimuths form an exactly
    // left/right and front/back symmetric fixed grid. Eight azimuths resolve |m| <= 3.
    for (std::size_t latitude = 0; latitude < 4; ++latitude) {
        const float y = -0.75F + 0.5F * static_cast<float>(latitude);
        const float radius = std::sqrt(std::max(0.0F, 1.0F - y * y));
        for (std::size_t azimuth = 0; azimuth < 8; ++azimuth) {
            const std::size_t point = latitude * 8 + azimuth;
            const float angle = 2.0F * kPi * static_cast<float>(azimuth) / 8.0F;
            quadrature_directions_[point] = {radius * std::sin(angle), y, radius * std::cos(angle)};
            quadrature_harmonics_[point] = AmbisonicEncoderOrder3::encode_direction(quadrature_directions_[point]);
        }
    }
}

bool AmbisonicBinauralDecoderOrder3::prepare_filter_bank(
    const Quaternionf& world_to_head, const IHrtfDatabase& hrtf,
    AmbisonicBinauralFilterBankOrder3& output) const noexcept {
    AmbisonicBinauralFilterBankOrder3 candidate{};
    // Room reflections are already temporally spread by their propagation and
    // FDN paths. Keep their 16x2 decoder bounded to the zero-latency 512-tap
    // head of a measured HRIR; the direct L/R speakers retain the complete
    // response through BinauralConvolver's partitioned tail.
    std::array<float, kMaximumHrirTaps> left{};
    std::array<float, kMaximumHrirTaps> right{};
    std::size_t pending_tap_count = 0;
    for (std::size_t point = 0; point < kQuadraturePoints; ++point) {
        std::size_t tap_count = 0;
        if (!hrtf.query(rotate(world_to_head, quadrature_directions_[point]), left, right, tap_count) ||
            tap_count == 0 || tap_count > kMaximumHrirTaps) {
            return false;
        }
        const std::size_t projected_taps = std::min(tap_count, kTimeDomainHrirTaps);
        pending_tap_count = std::max(pending_tap_count, projected_taps);
        for (std::size_t channel = 0; channel < kAmbisonicChannels; ++channel) {
            const unsigned degree = static_cast<unsigned>(std::sqrt(static_cast<float>(channel)));
            const float quadrature_weight = static_cast<float>(2U * degree + 1U) /
                                            static_cast<float>(kQuadraturePoints) *
                                            quadrature_harmonics_[point][channel];
            for (std::size_t tap = 0; tap < projected_taps; ++tap) {
                candidate.coefficients[0][channel][tap] += left[tap] * quadrature_weight;
                candidate.coefficients[1][channel][tap] += right[tap] * quadrature_weight;
            }
        }
    }

    candidate.tap_count = pending_tap_count;
    output = candidate;
    return true;
}

bool AmbisonicBinauralDecoderOrder3::apply_filter_bank(
    const AmbisonicBinauralFilterBankOrder3& filters, std::uint32_t crossfade_frames) noexcept {
    if (filters.tap_count == 0 || filters.tap_count > kTimeDomainHrirTaps) return false;

    freeze_current_morph();
    target_filters_ = filters.coefficients;
    target_tap_count_ = filters.tap_count;
    if (!initialized_ || crossfade_frames == 0) {
        active_filters_ = target_filters_;
        active_tap_count_ = target_tap_count_;
        crossfade_total_ = 0;
        crossfade_remaining_ = 0;
        initialized_ = true;
    } else {
        crossfade_total_ = crossfade_frames;
        crossfade_remaining_ = crossfade_frames;
    }
    return true;
}

bool AmbisonicBinauralDecoderOrder3::update(const Quaternionf& world_to_head, const IHrtfDatabase& hrtf,
                                             std::uint32_t crossfade_frames) noexcept {
    AmbisonicBinauralFilterBankOrder3 prepared{};
    return prepare_filter_bank(world_to_head, hrtf, prepared) && apply_filter_bank(prepared, crossfade_frames);
}

float AmbisonicBinauralDecoderOrder3::convolve(const float* history, const float* coefficients,
                                                std::size_t tap_count) noexcept {
    float sum = 0.0F;
    std::size_t tap = 0;
#if defined(SOUND_SPATIALIZER_ACOUSTICS_USE_SSE2)
    __m128 accumulator = _mm_setzero_ps();
    for (; tap + 4 <= tap_count; tap += 4) {
        accumulator = _mm_add_ps(accumulator,
                                 _mm_mul_ps(_mm_loadu_ps(history + tap), _mm_loadu_ps(coefficients + tap)));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, accumulator);
    sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
#endif
    for (; tap < tap_count; ++tap) sum += history[tap] * coefficients[tap];
    return sum;
}

void AmbisonicBinauralDecoderOrder3::freeze_current_morph() noexcept {
    if (crossfade_remaining_ == 0 || crossfade_total_ == 0) return;
    const float alpha = 1.0F - static_cast<float>(crossfade_remaining_) /
                                   static_cast<float>(crossfade_total_);
    const std::size_t taps = std::max(active_tap_count_, target_tap_count_);
    for (std::size_t ear = 0; ear < 2; ++ear) {
        for (std::size_t channel = 0; channel < kAmbisonicChannels; ++channel) {
            for (std::size_t tap = 0; tap < taps; ++tap) {
                active_filters_[ear][channel][tap] +=
                    (target_filters_[ear][channel][tap] - active_filters_[ear][channel][tap]) * alpha;
            }
        }
    }
    active_tap_count_ = taps;
    crossfade_remaining_ = 0;
}

void AmbisonicBinauralDecoderOrder3::reset() noexcept {
    active_filters_ = {};
    target_filters_ = {};
    for (auto& channel : history_) channel.fill(0.0F);
    active_tap_count_ = 1;
    target_tap_count_ = 1;
    history_index_ = 0;
    crossfade_total_ = 0;
    crossfade_remaining_ = 0;
    initialized_ = false;
}

void AmbisonicBinauralDecoderOrder3::process(
    const std::array<float, kAmbisonicChannels>* input, StereoFrame* output, std::size_t frame_count) noexcept {
    if (input == nullptr || output == nullptr) return;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        history_index_ = (history_index_ + kTimeDomainHrirTaps - 1) % kTimeDomainHrirTaps;
        for (std::size_t channel = 0; channel < kAmbisonicChannels; ++channel) {
            history_[channel][history_index_] = input[frame][channel];
            history_[channel][history_index_ + kTimeDomainHrirTaps] = input[frame][channel];
        }
        std::array<float, 2> active{};
        std::array<float, 2> target{};
        for (std::size_t channel = 0; channel < kAmbisonicChannels; ++channel) {
            const float* channel_history = history_[channel].data() + history_index_;
            active[0] += convolve(channel_history, active_filters_[0][channel].data(), active_tap_count_);
            active[1] += convolve(channel_history, active_filters_[1][channel].data(), active_tap_count_);
            if (crossfade_remaining_ != 0) {
                target[0] += convolve(channel_history, target_filters_[0][channel].data(), target_tap_count_);
                target[1] += convolve(channel_history, target_filters_[1][channel].data(), target_tap_count_);
            }
        }
        if (crossfade_remaining_ != 0) {
            const float alpha = 1.0F - static_cast<float>(crossfade_remaining_ - 1U) /
                                           static_cast<float>(crossfade_total_);
            output[frame] = {active[0] + (target[0] - active[0]) * alpha,
                             active[1] + (target[1] - active[1]) * alpha};
            --crossfade_remaining_;
            if (crossfade_remaining_ == 0) {
                active_filters_ = target_filters_;
                active_tap_count_ = target_tap_count_;
            }
        } else {
            output[frame] = {active[0], active[1]};
        }
    }
}

bool LateReverbFdn16::prepare(float sample_rate, const MaterialBands& rt60_seconds) {
    if (sample_rate < 1'000.0F) {
        return false;
    }
    sample_rate_ = sample_rate;
    constexpr std::array<float, kLineCount> delay_milliseconds{
        29.7F, 32.9F, 37.1F, 41.1F, 43.7F, 47.9F, 53.3F, 59.3F,
        61.7F, 67.1F, 71.3F, 73.9F, 79.7F, 83.9F, 89.3F, 97.1F,
    };
    for (std::size_t line = 0; line < kLineCount; ++line) {
        delay_lengths_[line] = std::max<std::size_t>(2, static_cast<std::size_t>(std::lround(
            delay_milliseconds[line] * 0.001F * sample_rate_)));
        for (auto& band : bands_) {
            band.lines[line].assign(delay_lengths_[line], 0.0F);
        }
    }
    low_coefficient_ = 1.0F - std::exp(-2.0F * kPi * 250.0F / sample_rate_);
    high_coefficient_ = 1.0F - std::exp(-2.0F * kPi * 4'000.0F / sample_rate_);
    prepared_ = true;
    configure_rt60(rt60_seconds, 0);
    reset();
    return true;
}

void LateReverbFdn16::configure_rt60(const MaterialBands& rt60_seconds, std::uint32_t morph_frames) noexcept {
    rt60_seconds_ = {
        std::clamp(rt60_seconds.low, 0.08F, 8.0F),
        std::clamp(rt60_seconds.mid, 0.08F, 8.0F),
        std::clamp(rt60_seconds.high, 0.08F, 8.0F),
    };
    const std::array<float, 3> rt60{rt60_seconds_.low, rt60_seconds_.mid, rt60_seconds_.high};
    for (std::size_t band = 0; band < bands_.size(); ++band) {
        for (std::size_t line = 0; line < kLineCount; ++line) {
            const float delay_seconds = static_cast<float>(delay_lengths_[line]) / sample_rate_;
            bands_[band].target_feedback_gains[line] = std::pow(10.0F, -3.0F * delay_seconds / rt60[band]);
        }
    }
    if (!gains_initialized_ || morph_frames == 0) {
        for (auto& band : bands_) band.feedback_gains = band.target_feedback_gains;
        gain_morph_remaining_ = 0;
        gains_initialized_ = true;
    } else {
        gain_morph_remaining_ = morph_frames;
    }
}

void LateReverbFdn16::reset() noexcept {
    for (auto& band : bands_) {
        for (auto& line : band.lines) {
            std::fill(line.begin(), line.end(), 0.0F);
        }
    }
    indices_.fill(0);
    low_state_ = 0.0F;
    mid_lowpass_state_ = 0.0F;
}

void LateReverbFdn16::process_mono(const float* input,
                                   std::array<float, kOutputChannels>* output,
                                   std::size_t frame_count) noexcept {
    if (!prepared_ || input == nullptr || output == nullptr) {
        return;
    }
    constexpr std::array<float, kLineCount> input_signs{
        1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1,
    };
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (gain_morph_remaining_ != 0) {
            const float reciprocal_remaining = 1.0F / static_cast<float>(gain_morph_remaining_);
            for (auto& band : bands_)
                for (std::size_t line = 0; line < kLineCount; ++line)
                    band.feedback_gains[line] +=
                        (band.target_feedback_gains[line] - band.feedback_gains[line]) * reciprocal_remaining;
            --gain_morph_remaining_;
        }
        low_state_ += low_coefficient_ * (input[frame] - low_state_);
        mid_lowpass_state_ += high_coefficient_ * (input[frame] - mid_lowpass_state_);
        const std::array<float, 3> input_bands{
            low_state_, mid_lowpass_state_ - low_state_, input[frame] - mid_lowpass_state_};
        std::array<float, kOutputChannels> accumulated{};

        for (std::size_t band_index = 0; band_index < bands_.size(); ++band_index) {
            auto& band = bands_[band_index];
            std::array<float, kLineCount> delayed{};
            float sum = 0.0F;
            for (std::size_t line = 0; line < kLineCount; ++line) {
                delayed[line] = band.lines[line][indices_[line]];
                sum += delayed[line];
            }
            const float householder_common = 2.0F * sum / static_cast<float>(kLineCount);
            for (std::size_t line = 0; line < kLineCount; ++line) {
                const float mixed_feedback = delayed[line] - householder_common;
                band.lines[line][indices_[line]] = input_bands[band_index] * input_signs[line] * 0.20F +
                                                  mixed_feedback * band.feedback_gains[line];
                accumulated[0] += delayed[line] * 0.0625F;
                accumulated[1] += delayed[line] * input_signs[line] * 0.0625F;
                accumulated[2] += delayed[line] * input_signs[(line + 5) % kLineCount] * 0.0625F;
                accumulated[3] += delayed[line] * input_signs[(line + 9) % kLineCount] * 0.0625F;
            }
        }
        output[frame] = accumulated;
        for (std::size_t line = 0; line < kLineCount; ++line) {
            indices_[line] = (indices_[line] + 1) % delay_lengths_[line];
        }
    }
}

} // namespace sound_spatializer
