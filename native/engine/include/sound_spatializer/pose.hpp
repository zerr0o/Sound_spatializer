#pragma once

#include "sound_spatializer/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace sound_spatializer {

class OneEuroFilter {
public:
    OneEuroFilter(float min_cutoff_hz = 1.2F, float beta = 3.0F, float derivative_cutoff_hz = 1.0F) noexcept;

    void reset(float value, double timestamp_seconds) noexcept;
    [[nodiscard]] float filter(float value, double timestamp_seconds) noexcept;

private:
    [[nodiscard]] static float smoothing_alpha(float cutoff_hz, float delta_seconds) noexcept;

    float min_cutoff_hz_;
    float beta_;
    float derivative_cutoff_hz_;
    float value_{};
    float derivative_{};
    double timestamp_seconds_{};
    bool initialized_{};
};

class PosePredictor {
public:
    struct Settings {
        float min_cutoff_hz{1.2F};
        // Rotation vectors are expressed in radians. A substantially larger
        // beta than landmark filters expressed in pixels/normalised units is
        // required to keep the dynamic lag below one 30 fps camera frame.
        float beta{3.0F};
        float derivative_cutoff_hz{1.0F};
        float maximum_prediction_ms{20.0F};
        float hold_ms{150.0F};
        float neutral_return_ms{1'000.0F};
    };

    PosePredictor() noexcept;
    explicit PosePredictor(Settings settings) noexcept;

    void reset(const Quaternionf& neutral, double timestamp_seconds) noexcept;
    [[nodiscard]] HeadPoseSampleV1 update(const HeadPoseSampleV1& sample,
                                          double sample_time_seconds,
                                          double render_time_seconds) noexcept;
    [[nodiscard]] HeadPoseSampleV1 predict(double render_time_seconds) noexcept;
    [[nodiscard]] HeadPoseSampleV1 on_missing(double render_time_seconds) noexcept;

private:
    Settings settings_;
    std::array<OneEuroFilter, 3> filters_;
    Quaternionf neutral_{};
    Quaternionf last_orientation_{};
    Vec3f last_rotation_vector_{};
    Vec3f angular_velocity_{};
    double last_sample_time_seconds_{};
    double last_received_time_seconds_{};
    double lost_since_seconds_{};
    std::uint64_t sequence_{};
    float last_confidence_{};
    bool initialized_{};
    bool lost_{};
    bool has_tracked_sample_{};
};

class AtomicPoseMailbox {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "Head-pose mailbox requires lock-free 64-bit atomics on the target");
    AtomicPoseMailbox() noexcept;
    void store(const HeadPoseSampleV1& value) noexcept;
    [[nodiscard]] HeadPoseSampleV1 load() const noexcept;

private:
    static std::uint32_t float_bits(float value) noexcept;
    static float bits_float(std::uint32_t value) noexcept;

    mutable std::atomic<std::uint64_t> guard_{};
    std::atomic<std::uint64_t> sequence_{};
    std::atomic<std::int64_t> timestamp_{};
    std::array<std::atomic<std::uint32_t>, 4> orientation_{};
    std::array<std::atomic<std::uint32_t>, 3> velocity_{};
    std::atomic<std::uint32_t> confidence_{};
    std::atomic<std::uint32_t> state_{};
};

} // namespace sound_spatializer
