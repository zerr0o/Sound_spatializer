#include "sound_spatializer/hrtf.hpp"

#include "sound_spatializer/spectral.hpp"

#include <mysofa.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <memory>
#include <vector>

namespace sound_spatializer {
namespace {

constexpr std::size_t kDiffuseFieldTaps = DiffuseFieldEqualizer::kMaximumTaps;

// libmysofa lays DataIR out as [measurement][receiver][sample], so the whole
// block is already the flat "every response back to back" form the designer
// wants. Runs once per file on the loading thread.
[[nodiscard]] bool build_diffuse_field_filter(const MYSOFA_HRTF& hrtf, std::uint32_t sample_rate,
                                              std::array<float, kDiffuseFieldTaps>& taps) {
    const std::size_t responses = static_cast<std::size_t>(hrtf.M) * hrtf.R;
    const std::size_t length = hrtf.N;
    if (responses == 0 || length == 0 || hrtf.DataIR.values == nullptr ||
        hrtf.DataIR.elements < responses * length) {
        return false;
    }
    return design_diffuse_field_inverse({hrtf.DataIR.values, responses * length}, length, sample_rate,
                                        taps);
}

class MySofaHrtfDatabase final : public IHrtfDatabase {
public:
    MySofaHrtfDatabase(MYSOFA_EASY* handle, std::uint32_t sample_rate, int filter_length,
                       const std::array<float, kDiffuseFieldTaps>& diffuse_field,
                       bool diffuse_field_valid) noexcept
        : handle_(handle), sample_rate_(sample_rate), filter_length_(filter_length),
          diffuse_field_(diffuse_field), diffuse_field_valid_(diffuse_field_valid) {}

    ~MySofaHrtfDatabase() override {
        if (handle_ != nullptr) {
            mysofa_close(handle_);
        }
    }

    [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return sample_rate_; }
    [[nodiscard]] std::size_t maximum_taps() const noexcept override {
        return std::min<std::size_t>(static_cast<std::size_t>(filter_length_) + 64, kMaximumHrirTaps);
    }

    [[nodiscard]] std::span<const float> diffuse_field_filter() const noexcept override {
        if (!diffuse_field_valid_) return {};
        return diffuse_field_;
    }

    bool query(const Vec3f& input_direction, std::span<float> left, std::span<float> right,
               std::size_t& tap_count) const noexcept override {
        if (handle_ == nullptr || filter_length_ <= 0 || static_cast<std::size_t>(filter_length_) > kMaximumHrirTaps ||
            left.size() < maximum_taps() || right.size() < maximum_taps()) {
            return false;
        }
        std::array<float, kMaximumHrirTaps> raw_left{};
        std::array<float, kMaximumHrirTaps> raw_right{};
        float left_delay = 0.0F;
        float right_delay = 0.0F;
        const Vec3f direction = normalized(input_direction);
        // SOFA Cartesian coordinates are X-front, Y-left, Z-up; the application uses
        // X-right, Y-up, Z-forward.
        mysofa_getfilter_float(handle_, direction.z, -direction.x, direction.y, raw_left.data(), raw_right.data(),
                               &left_delay, &right_delay);
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        const auto apply_fractional_delay = [this](const std::array<float, kMaximumHrirTaps>& source,
                                                    float delay_seconds, std::span<float> destination) noexcept {
            const float delay_samples = std::clamp(delay_seconds * static_cast<float>(sample_rate_), 0.0F, 63.0F);
            const std::size_t whole_delay = static_cast<std::size_t>(delay_samples);
            const float fraction = delay_samples - static_cast<float>(whole_delay);
            std::size_t end = 0;
            for (std::size_t tap = 0; tap < static_cast<std::size_t>(filter_length_); ++tap) {
                const std::size_t index = tap + whole_delay;
                if (index >= destination.size()) break;
                destination[index] += source[tap] * (1.0F - fraction);
                end = index + 1;
                if (fraction > 0.0F && index + 1 < destination.size()) {
                    destination[index + 1] += source[tap] * fraction;
                    end = index + 2;
                }
            }
            return end;
        };
        const std::size_t left_end = apply_fractional_delay(raw_left, left_delay, left);
        const std::size_t right_end = apply_fractional_delay(raw_right, right_delay, right);
        tap_count = std::max(left_end, right_end);
        return tap_count != 0;
    }

private:
    MYSOFA_EASY* handle_{};
    std::uint32_t sample_rate_{};
    int filter_length_{};
    std::array<float, kDiffuseFieldTaps> diffuse_field_{};
    bool diffuse_field_valid_{};
};

} // namespace

HrtfLoadResult load_sofa_hrtf_impl(std::string_view path, std::uint32_t sample_rate) {
    std::u8string utf8_path;
    utf8_path.reserve(path.size());
    for (const char character : path) utf8_path.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    const std::filesystem::path filesystem_path(utf8_path);
    int filter_length = 0;
    int error = MYSOFA_OK;
    MYSOFA_EASY* handle = mysofa_open(filesystem_path.string().c_str(), static_cast<float>(sample_rate),
                                      &filter_length, &error);
    if (handle == nullptr || error != MYSOFA_OK) {
        return {nullptr, "SOFA file could not be opened or is not a compatible SimpleFreeFieldHRIR dataset"};
    }
    // Reserve the same 64-sample bounded fractional-delay budget advertised by
    // maximum_taps(), so no accepted SOFA response is silently truncated.
    if (filter_length <= 0 || filter_length > static_cast<int>(kMaximumHrirTaps - 64)) {
        mysofa_close(handle);
        return {nullptr, "SOFA HRIR plus its 64-sample delay budget exceeds the 4,096-tap "
                         "(85.3 ms at 48 kHz) real-time limit"};
    }
    // A set whose common transfer function cannot be measured is still usable;
    // it simply renders without the correction rather than being refused.
    std::array<float, kDiffuseFieldTaps> diffuse_field{};
    const bool diffuse_field_valid =
        handle->hrtf != nullptr && build_diffuse_field_filter(*handle->hrtf, sample_rate, diffuse_field);
    return {std::make_unique<MySofaHrtfDatabase>(handle, sample_rate, filter_length, diffuse_field,
                                                 diffuse_field_valid),
            {}};
}

} // namespace sound_spatializer
