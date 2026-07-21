#include "sound_spatializer/pose.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace sound_spatializer {

OneEuroFilter::OneEuroFilter(float min_cutoff_hz, float beta, float derivative_cutoff_hz) noexcept
    : min_cutoff_hz_(std::max(0.01F, min_cutoff_hz)),
      beta_(std::max(0.0F, beta)),
      derivative_cutoff_hz_(std::max(0.01F, derivative_cutoff_hz)) {}

void OneEuroFilter::reset(float value, double timestamp_seconds) noexcept {
    value_ = value;
    derivative_ = 0.0F;
    timestamp_seconds_ = timestamp_seconds;
    initialized_ = true;
}

float OneEuroFilter::smoothing_alpha(float cutoff_hz, float delta_seconds) noexcept {
    const float tau = 1.0F / (2.0F * kPi * cutoff_hz);
    return delta_seconds / (delta_seconds + tau);
}

float OneEuroFilter::filter(float value, double timestamp_seconds) noexcept {
    if (!initialized_) {
        reset(value, timestamp_seconds);
        return value;
    }
    const float delta = static_cast<float>(timestamp_seconds - timestamp_seconds_);
    if (!(delta > 0.0F) || delta > 1.0F) {
        reset(value, timestamp_seconds);
        return value;
    }
    const float raw_derivative = (value - value_) / delta;
    const float derivative_alpha = smoothing_alpha(derivative_cutoff_hz_, delta);
    derivative_ += derivative_alpha * (raw_derivative - derivative_);
    const float cutoff = min_cutoff_hz_ + beta_ * std::abs(derivative_);
    const float value_alpha = smoothing_alpha(cutoff, delta);
    value_ += value_alpha * (value - value_);
    timestamp_seconds_ = timestamp_seconds;
    return value_;
}

PosePredictor::PosePredictor() noexcept : PosePredictor(Settings{}) {}

PosePredictor::PosePredictor(Settings settings) noexcept
    : settings_(settings),
      filters_{OneEuroFilter(settings.min_cutoff_hz, settings.beta, settings.derivative_cutoff_hz),
               OneEuroFilter(settings.min_cutoff_hz, settings.beta, settings.derivative_cutoff_hz),
               OneEuroFilter(settings.min_cutoff_hz, settings.beta, settings.derivative_cutoff_hz)} {}

void PosePredictor::reset(const Quaternionf& neutral, double timestamp_seconds) noexcept {
    neutral_ = neutral.normalized_value();
    last_orientation_ = neutral_;
    last_rotation_vector_ = rotation_vector_from_quaternion(neutral_);
    angular_velocity_ = {};
    last_sample_time_seconds_ = timestamp_seconds;
    last_received_time_seconds_ = timestamp_seconds;
    lost_since_seconds_ = timestamp_seconds;
    sequence_ = 0;
    last_confidence_ = 0.0F;
    initialized_ = true;
    lost_ = false;
    has_tracked_sample_ = false;
    filters_[0].reset(last_rotation_vector_.x, timestamp_seconds);
    filters_[1].reset(last_rotation_vector_.y, timestamp_seconds);
    filters_[2].reset(last_rotation_vector_.z, timestamp_seconds);
}

HeadPoseSampleV1 PosePredictor::update(const HeadPoseSampleV1& sample,
                                       double sample_time_seconds,
                                       double render_time_seconds) noexcept {
    if (!initialized_) {
        reset(neutral_, sample_time_seconds);
    }
    // MediaPipe's facial transform can remain valid even when its optional
    // landmark confidence field is reported as exactly zero. The explicit
    // tracking state and the validated finite quaternion are authoritative;
    // confidence remains diagnostic metadata.
    if (sample.tracking_state != TrackingState::tracking) {
        return on_missing(render_time_seconds);
    }

    const Vec3f raw = rotation_vector_from_quaternion(sample.orientation);
    const bool resumed = lost_ || !has_tracked_sample_;
    const float received_gap_ms =
        std::max(0.0F, static_cast<float>((render_time_seconds - last_received_time_seconds_) * 1'000.0));
    const bool reset_filters = !has_tracked_sample_ || (lost_ && received_gap_ms >= settings_.hold_ms);
    Vec3f filtered{};
    if (reset_filters) {
        // A stale One Euro derivative and the client velocity from the
        // neutral -> face discontinuity must not survive a tracking gap. Both
        // would be extrapolated as real head motion and manifest as a burst of
        // random HRTF directions immediately after reacquisition.
        filters_[0].reset(raw.x, sample_time_seconds);
        filters_[1].reset(raw.y, sample_time_seconds);
        filters_[2].reset(raw.z, sample_time_seconds);
        filtered = raw;
        angular_velocity_ = {};
    } else {
        filtered = {
            filters_[0].filter(raw.x, sample_time_seconds),
            filters_[1].filter(raw.y, sample_time_seconds),
            filters_[2].filter(raw.z, sample_time_seconds),
        };
    }
    const Quaternionf filtered_orientation = quaternion_from_rotation_vector(filtered);
    const float delta = static_cast<float>(sample_time_seconds - last_sample_time_seconds_);
    if (resumed) {
        angular_velocity_ = {};
    } else if (delta > 0.0001F && delta < 0.25F) {
        const Quaternionf world_delta = filtered_orientation * last_orientation_.conjugate();
        const Vec3f measured_velocity = rotation_vector_from_quaternion(world_delta) / delta;
        constexpr float velocity_smoothing = 0.35F;
        angular_velocity_ = angular_velocity_ * (1.0F - velocity_smoothing) + measured_velocity * velocity_smoothing;
    }
    if (!resumed && length(sample.angular_velocity_rad_s) > 0.0001F) {
        angular_velocity_ = angular_velocity_ * 0.5F + sample.angular_velocity_rad_s * 0.5F;
    }

    last_rotation_vector_ = filtered;
    last_orientation_ = filtered_orientation;
    last_sample_time_seconds_ = sample_time_seconds;
    last_received_time_seconds_ = render_time_seconds;
    sequence_ = sample.sequence;
    last_confidence_ = sample.confidence;
    lost_ = false;
    has_tracked_sample_ = true;

    const float prediction_seconds = std::clamp(static_cast<float>(render_time_seconds - sample_time_seconds),
                                                0.0F,
                                                settings_.maximum_prediction_ms * 0.001F);
    const Quaternionf predicted = quaternion_from_rotation_vector(angular_velocity_ * prediction_seconds) * last_orientation_;
    return {sample.sequence, sample.timestamp_qpc, predicted.normalized_value(), angular_velocity_, sample.confidence,
            TrackingState::tracking};
}

HeadPoseSampleV1 PosePredictor::predict(double render_time_seconds) noexcept {
    if (!initialized_) {
        reset(neutral_, render_time_seconds);
    }
    if (!has_tracked_sample_) {
        return {sequence_, 0, neutral_, {}, 0.0F, TrackingState::unavailable};
    }
    const float received_age_seconds =
        std::max(0.0F, static_cast<float>(render_time_seconds - last_received_time_seconds_));
    if (!lost_ && received_age_seconds <= settings_.hold_ms * 0.001F) {
        const float prediction_seconds =
            std::clamp(static_cast<float>(render_time_seconds - last_sample_time_seconds_),
                       0.0F,
                       settings_.maximum_prediction_ms * 0.001F);
        const Quaternionf predicted = quaternion_from_rotation_vector(angular_velocity_ * prediction_seconds) * last_orientation_;
        return {sequence_, 0, predicted.normalized_value(), angular_velocity_, last_confidence_, TrackingState::tracking};
    }
    if (!lost_) {
        lost_ = true;
        lost_since_seconds_ = last_received_time_seconds_;
    }
    return on_missing(render_time_seconds);
}

HeadPoseSampleV1 PosePredictor::on_missing(double render_time_seconds) noexcept {
    if (!initialized_) {
        reset(neutral_, render_time_seconds);
    }
    if (!has_tracked_sample_) {
        return {++sequence_, 0, neutral_, {}, 0.0F, TrackingState::unavailable};
    }
    if (!lost_) {
        lost_ = true;
        lost_since_seconds_ = render_time_seconds;
    }
    const float missing_ms = static_cast<float>((render_time_seconds - lost_since_seconds_) * 1'000.0);
    Quaternionf output = last_orientation_;
    TrackingState state = TrackingState::held;
    if (missing_ms > settings_.hold_ms) {
        const float progress = (missing_ms - settings_.hold_ms) / std::max(1.0F, settings_.neutral_return_ms);
        output = slerp(last_orientation_, neutral_, progress);
        state = TrackingState::returning_to_neutral;
        if (progress >= 1.0F) {
            output = neutral_;
            angular_velocity_ = {};
        }
    }
    return {++sequence_, 0, output, angular_velocity_, 0.0F, state};
}

AtomicPoseMailbox::AtomicPoseMailbox() noexcept {
    store(HeadPoseSampleV1{});
}

std::uint32_t AtomicPoseMailbox::float_bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
float AtomicPoseMailbox::bits_float(std::uint32_t value) noexcept { return std::bit_cast<float>(value); }

void AtomicPoseMailbox::store(const HeadPoseSampleV1& value) noexcept {
    guard_.fetch_add(1, std::memory_order_acq_rel);
    sequence_.store(value.sequence, std::memory_order_relaxed);
    timestamp_.store(value.timestamp_qpc, std::memory_order_relaxed);
    orientation_[0].store(float_bits(value.orientation.w), std::memory_order_relaxed);
    orientation_[1].store(float_bits(value.orientation.x), std::memory_order_relaxed);
    orientation_[2].store(float_bits(value.orientation.y), std::memory_order_relaxed);
    orientation_[3].store(float_bits(value.orientation.z), std::memory_order_relaxed);
    velocity_[0].store(float_bits(value.angular_velocity_rad_s.x), std::memory_order_relaxed);
    velocity_[1].store(float_bits(value.angular_velocity_rad_s.y), std::memory_order_relaxed);
    velocity_[2].store(float_bits(value.angular_velocity_rad_s.z), std::memory_order_relaxed);
    confidence_.store(float_bits(value.confidence), std::memory_order_relaxed);
    state_.store(static_cast<std::uint32_t>(value.tracking_state), std::memory_order_relaxed);
    guard_.fetch_add(1, std::memory_order_release);
}

HeadPoseSampleV1 AtomicPoseMailbox::load() const noexcept {
    HeadPoseSampleV1 result{};
    for (;;) {
        const std::uint64_t begin = guard_.load(std::memory_order_acquire);
        if ((begin & 1U) != 0U) {
            continue;
        }
        result.sequence = sequence_.load(std::memory_order_relaxed);
        result.timestamp_qpc = timestamp_.load(std::memory_order_relaxed);
        result.orientation = {
            bits_float(orientation_[0].load(std::memory_order_relaxed)),
            bits_float(orientation_[1].load(std::memory_order_relaxed)),
            bits_float(orientation_[2].load(std::memory_order_relaxed)),
            bits_float(orientation_[3].load(std::memory_order_relaxed)),
        };
        result.angular_velocity_rad_s = {
            bits_float(velocity_[0].load(std::memory_order_relaxed)),
            bits_float(velocity_[1].load(std::memory_order_relaxed)),
            bits_float(velocity_[2].load(std::memory_order_relaxed)),
        };
        result.confidence = bits_float(confidence_.load(std::memory_order_relaxed));
        result.tracking_state = static_cast<TrackingState>(state_.load(std::memory_order_relaxed));
        const std::uint64_t end = guard_.load(std::memory_order_acquire);
        if (begin == end) {
            return result;
        }
    }
}

} // namespace sound_spatializer
