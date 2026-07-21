#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sound_spatializer {

// A deterministic, allocation-free rolling percentile estimator for the
// capture-to-render latency reported from the real-time audio callback.
//
// The first three tracked samples are intentionally ignored: reconnecting the
// pose transport can make those samples older than steady-state traffic and
// must not poison diagnostics long after tracking has settled. Thereafter the
// 64-sample window represents roughly 1.1 s at 60 Hz or 2.1 s at 30 Hz.
class RealtimeLatencyPercentileWindow final {
public:
    static constexpr std::size_t kCapacity = 64;
    static constexpr std::size_t kWarmupSamples = 3;
    static constexpr float kMaximumLatencyMs = 1'000.0F;
    static constexpr float kBinWidthMs = 0.5F;
    static constexpr std::size_t kHistogramBinCount = 2'001;

    RealtimeLatencyPercentileWindow() noexcept { reset(); }

    void reset() noexcept {
        histogram_.fill(0);
        samples_.fill(0);
        write_index_ = 0;
        sample_count_ = 0;
        warmup_count_ = 0;
    }

    // Returns false during warmup or for a non-finite measurement. Once true,
    // p50_ms and p95_ms contain nearest-rank percentiles of the current window.
    [[nodiscard]] bool push(float latency_ms, float& p50_ms, float& p95_ms) noexcept {
        if (!std::isfinite(latency_ms))
            return false;
        if (warmup_count_ < kWarmupSamples) {
            ++warmup_count_;
            return false;
        }

        const float bounded_latency = std::clamp(latency_ms, 0.0F, kMaximumLatencyMs);
        const auto rounded_bin = static_cast<std::size_t>(bounded_latency / kBinWidthMs + 0.5F);
        const auto bin = static_cast<std::uint16_t>(std::min(rounded_bin, kHistogramBinCount - 1));

        if (sample_count_ == kCapacity) {
            const std::uint16_t evicted = samples_[write_index_];
            --histogram_[evicted];
        } else {
            ++sample_count_;
        }
        samples_[write_index_] = bin;
        write_index_ = (write_index_ + 1) % kCapacity;
        ++histogram_[bin];

        const std::size_t p50_rank = (sample_count_ * 50 + 99) / 100;
        const std::size_t p95_rank = (sample_count_ * 95 + 99) / 100;
        std::size_t cumulative = 0;
        bool p50_found = false;
        for (std::size_t index = 0; index < histogram_.size(); ++index) {
            cumulative += histogram_[index];
            if (!p50_found && cumulative >= p50_rank) {
                p50_ms = static_cast<float>(index) * kBinWidthMs;
                p50_found = true;
            }
            if (cumulative >= p95_rank) {
                p95_ms = static_cast<float>(index) * kBinWidthMs;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t sample_count() const noexcept { return sample_count_; }

private:
    static_assert(kCapacity <= 255);
    static_assert((kHistogramBinCount - 1) * kBinWidthMs == kMaximumLatencyMs);

    std::array<std::uint8_t, kHistogramBinCount> histogram_{};
    std::array<std::uint16_t, kCapacity> samples_{};
    std::size_t write_index_{};
    std::size_t sample_count_{};
    std::size_t warmup_count_{};
};

} // namespace sound_spatializer
