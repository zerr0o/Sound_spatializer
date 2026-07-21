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
};

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

// Performs HRTF database queries; production code calls this only from HrtfPreparationWorker.
[[nodiscard]] bool build_binaural_filter_bank(const IHrtfDatabase& database,
                                               const std::array<Vec3f, 2>& head_relative_directions,
                                               const std::array<float, 2>& speaker_gains,
                                               HrirFilterBank& output) noexcept;

} // namespace sound_spatializer
