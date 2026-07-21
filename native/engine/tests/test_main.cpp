#include "sound_spatializer/acoustics.hpp"
#include "sound_spatializer/audio_backend.hpp"
#include "sound_spatializer/config.hpp"
#include "sound_spatializer/dsp.hpp"
#include "sound_spatializer/engine.hpp"
#include "sound_spatializer/hrtf.hpp"
#include "sound_spatializer/hrtf_worker.hpp"
#include "sound_spatializer/ipc.hpp"
#include "sound_spatializer/latency_statistics.hpp"
#include "sound_spatializer/pose.hpp"
#include "sound_spatializer/spsc_ring_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

using namespace sound_spatializer;

namespace {

int failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n';                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                                        \
    do {                                                                                                               \
        const double actual_value = static_cast<double>(actual);                                                       \
        const double expected_value = static_cast<double>(expected);                                                   \
        if (std::abs(actual_value - expected_value) > static_cast<double>(tolerance)) {                                \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK_NEAR failed: " << actual_value << " vs "              \
                      << expected_value << " (tol " << tolerance << ")\n";                                             \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

void test_spsc_fifo() {
    SpscRingBuffer<std::uint32_t> fifo(1'024);
    constexpr std::uint32_t count = 100'000;
    std::atomic<bool> producer_done{};
    std::thread producer([&] {
        for (std::uint32_t value = 0; value < count;) {
            if (fifo.try_push(value))
                ++value;
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::uint32_t expected = 0;
    while (expected < count) {
        std::uint32_t value = 0;
        if (fifo.try_pop(value)) {
            CHECK(value == expected);
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(producer_done.load(std::memory_order_acquire));
    CHECK(fifo.size() == 0);
}

void test_endpoint_contract_selection() {
    const std::array<AudioEndpointDescriptor, 3> endpoints{{
        {"speakers", true, 0, 0},
        {"virtual", true, 1, 1},
        {"old-virtual", false, 1, 1},
    }};
    const auto selected = select_capture_render_endpoint(endpoints, CaptureProvider::native_driver);
    CHECK(selected);
    CHECK(selected.id == "virtual");
    CHECK(select_capture_render_endpoint(endpoints, CaptureProvider::native_driver, {}, "speakers").id == "speakers");
    CHECK(select_capture_render_endpoint(endpoints, CaptureProvider::external_render, "speakers").id == "speakers");
    CHECK(!select_capture_render_endpoint(endpoints, CaptureProvider::external_render, "virtual"));
    CHECK(!select_capture_render_endpoint(endpoints, CaptureProvider::external_render, "old-virtual"));
    CHECK(!select_capture_render_endpoint(endpoints, CaptureProvider::external_render));
    CHECK(!select_capture_render_endpoint(endpoints, CaptureProvider::native_driver, "speakers"));
    CHECK(!select_capture_render_endpoint(std::span<const AudioEndpointDescriptor>(endpoints.data(), 1),
                                          CaptureProvider::native_driver));
    const std::array<AudioEndpointDescriptor, 2> duplicate{{{"a", true, 1, 1}, {"b", true, 1, 1}}};
    CHECK(!select_capture_render_endpoint(duplicate, CaptureProvider::native_driver));

    std::string route_error;
    AudioBackendConfig external{};
    external.capture_provider = CaptureProvider::external_render;
    CHECK(!validate_audio_backend_config(external, route_error));
    external.capture_endpoint_id = "external-source";
    CHECK(!validate_audio_backend_config(external, route_error));
    external.physical_output_endpoint_id = "headphones";
    CHECK(validate_audio_backend_config(external, route_error));
    external.physical_output_endpoint_id = "EXTERNAL-SOURCE";
    CHECK(!validate_audio_backend_config(external, route_error));
    external.physical_output_endpoint_id = "headphones";
    external.native_test_override_endpoint_id = "test-only";
    CHECK(!validate_audio_backend_config(external, route_error));

    AudioBackendConfig native{};
    native.capture_endpoint_id = "persisted-native-id";
    CHECK(!validate_audio_backend_config(native, route_error));
    native.capture_endpoint_id.clear();
    native.native_test_override_endpoint_id = "Native-Test";
    native.physical_output_endpoint_id = "native-test";
    CHECK(!validate_audio_backend_config(native, route_error));
    native = {};
    native.capture_provider = static_cast<CaptureProvider>(99);
    CHECK(!validate_audio_backend_config(native, route_error));
    CHECK(audio_reconnect_backoff_ms(0) == 250);
    CHECK(audio_reconnect_backoff_ms(1) == 500);
    CHECK(audio_reconnect_backoff_ms(2) == 1'000);
    CHECK(audio_reconnect_backoff_ms(3) == 2'000);
    CHECK(audio_reconnect_backoff_ms(100) == 2'000);
}

void test_wasapi_period_and_capture_budget_policy() {
    constexpr std::uint32_t default_mxcsr = 0x1F80U;
    constexpr std::uint32_t realtime_mxcsr = realtime_mxcsr_with_ftz_daz(default_mxcsr);
    CHECK((realtime_mxcsr & kMxcsrDenormalsAreZeroMask) != 0U);
    CHECK((realtime_mxcsr & kMxcsrFlushToZeroMask) != 0U);
    CHECK((realtime_mxcsr & default_mxcsr) == default_mxcsr);

    // Values from Microsoft's IAudioClient3 documentation example.
    const SharedAudioPeriodRange documented_range{448, 4, 48, 448};
    CHECK(select_shared_audio_period_frames(AudioMode::shared_low_latency, documented_range) == 48);
    CHECK(select_shared_audio_period_frames(AudioMode::exclusive_pro, documented_range) == 48);
    CHECK(select_shared_audio_period_frames(AudioMode::compatibility_256, documented_range) == 256);

    // Compatibility mode rounds upward to an advertised fundamental multiple.
    const SharedAudioPeriodRange coarse_range{480, 48, 96, 480};
    CHECK(select_shared_audio_period_frames(AudioMode::shared_low_latency, coarse_range) == 96);
    CHECK(select_shared_audio_period_frames(AudioMode::compatibility_256, coarse_range) == 288);
    CHECK(select_shared_audio_period_frames(AudioMode::compatibility_256, {192, 64, 64, 192}) == 192);
    CHECK(select_shared_audio_period_frames(AudioMode::shared_low_latency, {0, 0, 48, 448}) == 0);
    CHECK(select_shared_audio_period_frames(AudioMode::shared_low_latency, {0, 4, 449, 448}) == 0);

    auto packet_limited = make_capture_drain_budget(1'024);
    CHECK(packet_limited.try_account(128));
    CHECK(packet_limited.try_account(128));
    CHECK(packet_limited.try_account(128));
    CHECK(packet_limited.try_account(128));
    CHECK(!packet_limited.try_account(1));
    CHECK(packet_limited.processed_packets == 4);
    CHECK(packet_limited.processed_frames == 512);

    auto frame_limited = make_capture_drain_budget(256);
    CHECK(frame_limited.try_account(192));
    CHECK(!frame_limited.try_account(65));
    CHECK(frame_limited.try_account(64));
    CHECK(!frame_limited.try_account(1));
    CHECK(!make_capture_drain_budget(0).can_process(2));

    // A large legacy endpoint allocation must not itself become FIFO latency.
    CHECK(select_asrc_target_fill_frames(0, 48, 128) == 128);
    CHECK(select_asrc_target_fill_frames(0, 128, 128) == 256);
    CHECK(select_asrc_target_fill_frames(480, 48, 128) == 480);
    CHECK(select_asrc_target_fill_frames(3'840, 48, 128) == 3'840);
    CHECK(select_asrc_target_fill_frames(48, 128, 256) == 256);
}

void test_exclusive_render_format_policy_and_pcm_s32_conversion() {
    CHECK(select_exclusive_render_sample_format(true, true) == AudioSampleFormat::float32);
    CHECK(select_exclusive_render_sample_format(true, false) == AudioSampleFormat::float32);
    CHECK(select_exclusive_render_sample_format(false, true) == AudioSampleFormat::pcm_s32);
    CHECK(select_exclusive_render_sample_format(false, false) == AudioSampleFormat::unknown);
    CHECK(should_service_wasapi_render_on_wake(AudioMode::exclusive_pro, true));
    CHECK(!should_service_wasapi_render_on_wake(AudioMode::exclusive_pro, false));
    CHECK(should_service_wasapi_render_on_wake(AudioMode::shared_low_latency, true));
    CHECK(should_service_wasapi_render_on_wake(AudioMode::shared_low_latency, false));
    CHECK(should_service_wasapi_render_on_wake(AudioMode::compatibility_256, false));
    CHECK(audio_frames_to_reference_time(128) == 26'667U);
    CHECK(audio_frames_to_reference_time(144) == 30'000U);
    CHECK(audio_frames_to_reference_time(128, 0) == 0U);

    CHECK(float_to_pcm_s32(0.0F) == 0);
    CHECK(float_to_pcm_s32(0.5F) == 1'073'741'824);
    CHECK(float_to_pcm_s32(-0.5F) == -1'073'741'824);
    CHECK(float_to_pcm_s32(1.0F) == std::numeric_limits<std::int32_t>::max());
    CHECK(float_to_pcm_s32(2.0F) == std::numeric_limits<std::int32_t>::max());
    CHECK(float_to_pcm_s32(-1.0F) == std::numeric_limits<std::int32_t>::min());
    CHECK(float_to_pcm_s32(-2.0F) == std::numeric_limits<std::int32_t>::min());
    CHECK(float_to_pcm_s32(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(float_to_pcm_s32(std::numeric_limits<float>::infinity()) == 0);
    CHECK(float_to_pcm_s32(-std::numeric_limits<float>::infinity()) == 0);

    const std::array<StereoFrame, 3> input{{
        {-1.0F, 1.0F},
        {-0.5F, 0.5F},
        {std::numeric_limits<float>::quiet_NaN(), 2.0F},
    }};
    std::array<std::int32_t, input.size() * 2U> converted{};
    CHECK(!convert_stereo_float_to_pcm_s32(input.data(), converted.data(), input.size()));
    CHECK(converted[0] == std::numeric_limits<std::int32_t>::min());
    CHECK(converted[1] == std::numeric_limits<std::int32_t>::max());
    CHECK(converted[2] == -1'073'741'824);
    CHECK(converted[3] == 1'073'741'824);
    CHECK(converted[4] == 0);
    CHECK(converted[5] == std::numeric_limits<std::int32_t>::max());

    const std::array<StereoFrame, 2> invalid_silence{{
        {0.0F, -0.0F},
        {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()},
    }};
    std::array<std::int32_t, invalid_silence.size() * 2U> silent_output{{1, 1, 1, 1}};
    CHECK(convert_stereo_float_to_pcm_s32(
        invalid_silence.data(), silent_output.data(), invalid_silence.size()));
    CHECK(std::all_of(silent_output.begin(), silent_output.end(), [](std::int32_t sample) { return sample == 0; }));
}

void test_pose_wire_contract() {
    HeadPoseSampleV1 pose{42,
                          123456,
                          quaternion_from_rotation_vector({0.1F, -0.2F, 0.05F}),
                          {1.0F, -2.0F, 0.5F},
                          0.9F,
                          TrackingState::tracking};
    HeadPosePacketV1 packet = to_packet(pose, true);
    CHECK(sizeof(packet) == 64);
    CHECK(packet.crc32 != 0);
    HeadPoseSampleV1 decoded{};
    CHECK(from_packet(packet, decoded));
    CHECK(decoded.sequence == pose.sequence);
    CHECK(decoded.tracking_state == TrackingState::tracking);
    CHECK_NEAR(decoded.confidence, 0.9F, 1.0e-6F);

    packet.quaternion_x = std::numeric_limits<float>::quiet_NaN();
    packet.crc32 = pose_packet_crc32(packet);
    CHECK(!from_packet(packet, decoded));
    packet = to_packet(pose, true);
    packet.confidence = 1.5F;
    packet.crc32 = pose_packet_crc32(packet);
    CHECK(!from_packet(packet, decoded));
    packet = to_packet(pose, true);
    packet.angular_velocity_y = 100.0F;
    packet.crc32 = pose_packet_crc32(packet);
    CHECK(!from_packet(packet, decoded));
    packet = to_packet(pose, true);
    reinterpret_cast<std::byte*>(&packet)[24] ^= std::byte{0x01};
    CHECK(!from_packet(packet, decoded));

    pose.tracking_state = TrackingState::returning_to_neutral;
    packet = to_packet(pose);
    CHECK(packet.tracking_state == static_cast<std::uint16_t>(PoseWireTrackingState::acquiring));
}

void test_pose_prediction_and_world_lock() {
    PosePredictor predictor;
    predictor.reset({}, 0.0);
    CHECK(predictor.predict(0.0005).tracking_state == TrackingState::unavailable);
    HeadPoseSampleV1 identity{1, 0, {}, {}, 1.0F, TrackingState::tracking};
    (void)predictor.update(identity, 0.001, 0.001);
    const Quaternionf yaw_right = quaternion_from_rotation_vector({0.0F, kPi * 0.5F, 0.0F});
    HeadPoseSampleV1 turned{2, 0, yaw_right, {0.0F, kPi, 0.0F}, 1.0F, TrackingState::tracking};
    const HeadPoseSampleV1 filtered = predictor.update(turned, 0.101, 0.111);
    const Vec3f front_in_head = rotate(filtered.orientation.conjugate(), {0.0F, 0.0F, 1.0F});
    CHECK(front_in_head.x < -0.05F); // turn right: a world-front speaker moves to head-left
    const HeadPoseSampleV1 predicted = predictor.predict(0.116);
    CHECK(yaw_pitch_roll(predicted.orientation)[0] >= yaw_pitch_roll(filtered.orientation)[0]);
    const HeadPoseSampleV1 returning = predictor.predict(0.400);
    CHECK(returning.tracking_state == TrackingState::returning_to_neutral);
}

void test_pose_zero_confidence_and_low_latency_filter() {
    OneEuroFilter stationary_filter;
    stationary_filter.reset(0.0F, 0.0);
    float maximum_stationary_output = 0.0F;
    for (std::uint32_t frame = 1; frame <= 120; ++frame) {
        const float jitter = (frame & 1U) != 0U ? 0.5F * kPi / 180.0F : -0.5F * kPi / 180.0F;
        const float filtered_jitter = stationary_filter.filter(jitter, static_cast<double>(frame) / 30.0);
        if (frame > 30) {
            maximum_stationary_output = std::max(maximum_stationary_output, std::abs(filtered_jitter));
        }
    }
    CHECK(maximum_stationary_output < 0.1F * kPi / 180.0F);

    PosePredictor predictor;
    predictor.reset({}, 0.0);

    HeadPoseSampleV1 zero_confidence{
        1, 0, quaternion_from_rotation_vector({0.0F, 0.25F, 0.0F}), {}, 0.0F, TrackingState::tracking,
    };
    const HeadPoseSampleV1 accepted = predictor.update(zero_confidence, 1.0 / 30.0, 1.0 / 30.0);
    CHECK(accepted.tracking_state == TrackingState::tracking);
    CHECK(yaw_pitch_roll(accepted.orientation)[0] > 0.01F);

    // With a 60 degree/s yaw ramp at the degraded-but-supported 30 fps rate,
    // the adaptive filter should trail the most recent observation by less
    // than three degrees while retaining the low stationary cutoff.
    predictor.reset({}, 0.0);
    constexpr float angular_speed = kPi / 3.0F;
    HeadPoseSampleV1 filtered{};
    for (std::uint64_t frame = 1; frame <= 18; ++frame) {
        const double sample_time = static_cast<double>(frame) / 30.0;
        HeadPoseSampleV1 sample{
            frame,
            0,
            quaternion_from_rotation_vector({0.0F, angular_speed * static_cast<float>(sample_time), 0.0F}),
            {0.0F, angular_speed, 0.0F},
            0.0F,
            TrackingState::tracking,
        };
        filtered = predictor.update(sample, sample_time, sample_time);
    }
    const float expected_yaw = angular_speed * 0.6F;
    const float filtered_yaw = yaw_pitch_roll(filtered.orientation)[0];
    CHECK(expected_yaw - filtered_yaw < 3.0F * kPi / 180.0F);
    CHECK(filtered_yaw <= expected_yaw + 0.5F * kPi / 180.0F);
}

void test_pose_timeout_uses_reception_time_not_capture_time() {
    PosePredictor predictor;
    predictor.reset({}, 0.0);
    const Quaternionf yaw_right = quaternion_from_rotation_vector({0.0F, kPi * 0.25F, 0.0F});

    // The capture clock is consistently 600 ms behind the render clock. Each
    // freshly received sample must remain usable even though prediction is
    // deliberately bounded to the configured 20 ms horizon.
    HeadPoseSampleV1 stale_capture{1, 0, yaw_right, {}, 1.0F, TrackingState::tracking};
    CHECK(predictor.update(stale_capture, 0.400, 1.000).tracking_state == TrackingState::tracking);
    stale_capture.sequence = 2;
    CHECK(predictor.update(stale_capture, 0.430, 1.030).tracking_state == TrackingState::tracking);

    CHECK(predictor.predict(1.130).tracking_state == TrackingState::tracking);             // 100 ms since reception
    CHECK(predictor.predict(1.181).tracking_state == TrackingState::returning_to_neutral); // 151 ms
}

void test_pose_reacquisition_resets_filters_and_velocity() {
    PosePredictor predictor;
    predictor.reset({}, 0.0);

    const Quaternionf before_loss = quaternion_from_rotation_vector({0.0F, 0.35F, 0.0F});
    (void)predictor.update({1, 0, before_loss, {0.0F, 1.0F, 0.0F}, 1.0F, TrackingState::tracking},
                           0.033,
                           0.033);
    (void)predictor.update({2, 0, before_loss, {}, 0.0F, TrackingState::unavailable}, 0.066, 0.200);

    const Quaternionf after_loss = quaternion_from_rotation_vector({0.0F, -0.45F, 0.0F});
    const HeadPoseSampleV1 reacquired = predictor.update(
        // Simulate the very large neutral -> face velocity that an upstream
        // client could report on its first recovered frame.
        {3, 0, after_loss, {0.0F, 40.0F, 0.0F}, 1.0F, TrackingState::tracking},
        0.250,
        0.270);

    CHECK(reacquired.tracking_state == TrackingState::tracking);
    CHECK_NEAR(yaw_pitch_roll(reacquired.orientation)[0], -0.45F, 0.001F);
    CHECK(length(reacquired.angular_velocity_rad_s) < 0.001F);

    const HeadPoseSampleV1 predicted = predictor.predict(0.290);
    CHECK_NEAR(yaw_pitch_roll(predicted.orientation)[0], -0.45F, 0.001F);
}

void test_pose_brief_dropout_preserves_filter_without_velocity_spike() {
    PosePredictor predictor;
    predictor.reset({}, 0.0);
    const Quaternionf neutral{};
    (void)predictor.update({1, 0, neutral, {}, 1.0F, TrackingState::tracking}, 0.001, 0.001);
    (void)predictor.update({2, 0, neutral, {}, 1.0F, TrackingState::tracking}, 0.033, 0.033);
    (void)predictor.update({3, 0, neutral, {}, 0.0F, TrackingState::held}, 0.066, 0.066);

    const Quaternionf observed = quaternion_from_rotation_vector({0.0F, 0.20F, 0.0F});
    const HeadPoseSampleV1 resumed = predictor.update(
        {4, 0, observed, {0.0F, 40.0F, 0.0F}, 1.0F, TrackingState::tracking}, 0.100, 0.100);
    const float resumed_yaw = yaw_pitch_roll(resumed.orientation)[0];
    CHECK(resumed_yaw > 0.01F);
    CHECK(resumed_yaw < 0.19F); // One Euro history was preserved rather than reset to the raw 0.20 rad.
    CHECK(length(resumed.angular_velocity_rad_s) < 0.001F);
    CHECK_NEAR(yaw_pitch_roll(predictor.predict(0.120).orientation)[0], resumed_yaw, 0.001F);
}

void test_realtime_latency_percentiles_are_bounded_and_reactive() {
    RealtimeLatencyPercentileWindow window;
    float p50_ms = -1.0F;
    float p95_ms = -1.0F;

    // Startup transport samples are intentionally excluded from diagnostics.
    for (std::size_t index = 0; index < RealtimeLatencyPercentileWindow::kWarmupSamples; ++index)
        CHECK(!window.push(1'000.0F, p50_ms, p95_ms));
    CHECK(window.sample_count() == 0);
    CHECK(window.push(20.2F, p50_ms, p95_ms));
    CHECK_NEAR(p50_ms, 20.0F, 0.001F);
    CHECK_NEAR(p95_ms, 20.0F, 0.001F);

    // A lone post-warmup outlier no longer controls p95 once twenty recent
    // observations are available (nearest-rank p95).
    window.reset();
    for (std::size_t index = 0; index < RealtimeLatencyPercentileWindow::kWarmupSamples; ++index)
        CHECK(!window.push(10.0F, p50_ms, p95_ms));
    CHECK(window.push(800.0F, p50_ms, p95_ms));
    for (std::size_t index = 0; index < 19; ++index)
        CHECK(window.push(10.0F, p50_ms, p95_ms));
    CHECK_NEAR(p50_ms, 10.0F, 0.001F);
    CHECK_NEAR(p95_ms, 10.0F, 0.001F);

    // A real sustained regression enters p95 quickly, then is guaranteed to
    // disappear after at most one fixed window of healthy observations.
    window.reset();
    for (std::size_t index = 0; index < RealtimeLatencyPercentileWindow::kWarmupSamples; ++index)
        CHECK(!window.push(10.0F, p50_ms, p95_ms));
    for (std::size_t index = 0; index < RealtimeLatencyPercentileWindow::kCapacity - 4; ++index)
        CHECK(window.push(10.0F, p50_ms, p95_ms));
    for (std::size_t index = 0; index < 4; ++index)
        CHECK(window.push(100.0F, p50_ms, p95_ms));
    CHECK_NEAR(p50_ms, 10.0F, 0.001F);
    CHECK_NEAR(p95_ms, 100.0F, 0.001F);
    for (std::size_t index = 0; index < RealtimeLatencyPercentileWindow::kCapacity; ++index)
        CHECK(window.push(10.0F, p50_ms, p95_ms));
    CHECK(window.sample_count() == RealtimeLatencyPercentileWindow::kCapacity);
    CHECK_NEAR(p50_ms, 10.0F, 0.001F);
    CHECK_NEAR(p95_ms, 10.0F, 0.001F);

    CHECK(!window.push(std::numeric_limits<float>::quiet_NaN(), p50_ms, p95_ms));
    CHECK(window.sample_count() == RealtimeLatencyPercentileWindow::kCapacity);
}

void test_hrtf_lateral_sign() {
    AnalyticHrtfDatabase hrtf;
    std::array<float, kMaximumHrirTaps> left{};
    std::array<float, kMaximumHrirTaps> right{};
    std::size_t taps = 0;
    CHECK(hrtf.query({1.0F, 0.0F, 0.0F}, left, right, taps));
    float left_energy = 0.0F, right_energy = 0.0F;
    for (std::size_t index = 0; index < taps; ++index) {
        left_energy += left[index] * left[index];
        right_energy += right[index] * right[index];
    }
    CHECK(right_energy > left_energy); // +X world is the listener's right
}

void test_hrtf_worker_latest_wins_and_prepared_room_filters() {
    class ThreadCheckingHrtf final : public IHrtfDatabase {
    public:
        ThreadCheckingHrtf() : submitting_thread(std::this_thread::get_id()) {}
        [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return 48'000; }
        [[nodiscard]] std::size_t maximum_taps() const noexcept override { return 8; }
        bool query(const Vec3f& direction, std::span<float> left, std::span<float> right,
                   std::size_t& tap_count) const noexcept override {
            if (std::this_thread::get_id() == submitting_thread)
                queried_on_submitting_thread.store(true, std::memory_order_relaxed);
            query_count.fetch_add(1, std::memory_order_relaxed);
            std::fill(left.begin(), left.end(), 0.0F);
            std::fill(right.begin(), right.end(), 0.0F);
            left[0] = 0.75F + direction.x * 0.01F;
            right[0] = 0.75F - direction.x * 0.01F;
            tap_count = 1;
            return true;
        }

        const std::thread::id submitting_thread;
        mutable std::atomic<bool> queried_on_submitting_thread{};
        mutable std::atomic<std::uint32_t> query_count{};
    } hrtf;

    HrtfPreparationWorker worker;
    for (std::uint64_t generation = 1; generation <= 40; ++generation) {
        HrtfPreparationRequest request{};
        request.generation = generation;
        request.scene_revision = 7;
        request.database = &hrtf;
        request.head_relative_directions = {{{-0.5F, 0.0F, 1.0F}, {0.5F, 0.0F, 1.0F}}};
        request.speaker_gains = {1.0F, 1.0F};
        request.room_enabled = true;
        worker.submit_latest(request);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::uint64_t newest_direct_generation = 0;
    std::uint64_t newest_room_generation = 0;
    bool applied_prepared_room_bank = false;
    while (std::chrono::steady_clock::now() < deadline &&
           (newest_direct_generation != 40 || newest_room_generation != 40)) {
        bool consumed_update = false;
        const PreparedDirectHrtfUpdate* direct = nullptr;
        std::uint8_t direct_token = 0;
        if (worker.try_acquire_latest_direct(direct, direct_token)) {
            consumed_update = true;
            newest_direct_generation = std::max(newest_direct_generation, direct->generation);
            CHECK(direct->valid);
            CHECK(direct->scene_revision == 7);
            CHECK(direct->filters.tap_count == 1);
            worker.release_direct(direct_token);
        }

        const PreparedRoomHrtfUpdate* room = nullptr;
        std::uint8_t room_token = 0;
        if (worker.try_acquire_latest_room(room, room_token)) {
            consumed_update = true;
            newest_room_generation = std::max(newest_room_generation, room->generation);
            CHECK(room->valid);
            CHECK(room->scene_revision == 7);
            if (room->generation == 40) {
                const std::uint32_t query_count_before_apply = hrtf.query_count.load(std::memory_order_relaxed);
                AmbisonicBinauralDecoderOrder3 decoder;
                CHECK(decoder.apply_filter_bank(room->filters, 0));
                CHECK(hrtf.query_count.load(std::memory_order_relaxed) == query_count_before_apply);
                applied_prepared_room_bank = true;
            }
            worker.release_room(room_token);
        }

        if (!consumed_update)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(newest_direct_generation == 40);
    CHECK(newest_room_generation == 40);
    CHECK(applied_prepared_room_bank);
    CHECK(!hrtf.queried_on_submitting_thread.load(std::memory_order_relaxed));
    CHECK(hrtf.query_count.load(std::memory_order_relaxed) >= 34); // 2 direct + 32 room directions
}

void test_hrtf_worker_publishes_direct_before_failed_room() {
    class BlockingFailedRoomHrtf final : public IHrtfDatabase {
    public:
        [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return 48'000; }
        [[nodiscard]] std::size_t maximum_taps() const noexcept override { return 8; }
        bool query(const Vec3f& direction, std::span<float> left, std::span<float> right,
                   std::size_t& tap_count) const noexcept override {
            const float magnitude_squared =
                direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
            if (magnitude_squared < 1.5F) {
                room_query_started.store(true, std::memory_order_release);
                const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!allow_room_result.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < timeout) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
            }

            std::fill(left.begin(), left.end(), 0.0F);
            std::fill(right.begin(), right.end(), 0.0F);
            left[0] = 0.75F;
            right[0] = 0.75F;
            tap_count = 1;
            return true;
        }

        mutable std::atomic<bool> room_query_started{};
        mutable std::atomic<bool> allow_room_result{};
    } hrtf;

    HrtfPreparationWorker worker;
    HrtfPreparationRequest request{};
    request.generation = 1;
    request.scene_revision = 11;
    request.database = &hrtf;
    request.head_relative_directions = {{{-0.5F, 0.0F, 2.0F}, {0.5F, 0.0F, 2.0F}}};
    request.speaker_gains = {1.0F, 1.0F};
    request.room_enabled = true;
    worker.submit_latest(request);

    const auto direct_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool received_direct = false;
    while (std::chrono::steady_clock::now() < direct_deadline && !received_direct) {
        const PreparedDirectHrtfUpdate* direct = nullptr;
        std::uint8_t token = 0;
        if (!worker.try_acquire_latest_direct(direct, token)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        received_direct = direct->valid && direct->generation == 1 && direct->scene_revision == 11;
        CHECK(direct->filters.tap_count == 1);
        worker.release_direct(token);
    }
    CHECK(received_direct);
    CHECK(worker.latest_completed_generation() == 1);
    CHECK(worker.latest_room_completed_generation() == 0);

    hrtf.allow_room_result.store(true, std::memory_order_release);
    const auto room_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool received_failed_room = false;
    while (std::chrono::steady_clock::now() < room_deadline && !received_failed_room) {
        const PreparedRoomHrtfUpdate* room = nullptr;
        std::uint8_t token = 0;
        if (!worker.try_acquire_latest_room(room, token)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        received_failed_room = !room->valid && room->generation == 1 && room->scene_revision == 11;
        worker.release_room(token);
    }
    CHECK(hrtf.room_query_started.load(std::memory_order_acquire));
    CHECK(received_failed_room);
    CHECK(worker.latest_room_completed_generation() == 1);
}

void test_measured_sofa_itd_when_available() {
#if defined(SOUND_SPATIALIZER_HAS_MYSOFA)
    const std::filesystem::path sofa_path = std::filesystem::path(SOUND_SPATIALIZER_TEST_REPOSITORY_ROOT) /
                                            "resources" / "hrtf" / "data" / "sadie-d2-kemar.sofa";
    if (!std::filesystem::exists(sofa_path)) {
        std::cout << "SADIE D2 fixture not fetched; measured SOFA ITD test skipped\n";
        return;
    }
    const HrtfLoadResult loaded = load_sofa_hrtf(sofa_path.string(), kSampleRate);
    CHECK(loaded.database != nullptr);
    if (!loaded.database)
        return;
    std::array<float, kMaximumHrirTaps> left{};
    std::array<float, kMaximumHrirTaps> right{};
    std::size_t taps = 0;
    CHECK(loaded.database->query({1.0F, 0.0F, 0.0F}, left, right, taps));
    const auto energy_centroid = [taps](const auto& response) {
        double weighted = 0.0;
        double energy = 0.0;
        for (std::size_t index = 0; index < taps; ++index) {
            const double squared = static_cast<double>(response[index]) * response[index];
            weighted += static_cast<double>(index) * squared;
            energy += squared;
        }
        return std::pair{weighted / std::max(energy, 1.0e-20), energy};
    };
    const auto [left_centroid, left_energy] = energy_centroid(left);
    const auto [right_centroid, right_energy] = energy_centroid(right);
    CHECK(right_centroid + 0.25 < left_centroid); // +X world is right: right ear arrives first
    CHECK(right_energy > left_energy);
#endif
}

void test_convolver_and_morph() {
    HrirFilterBank bank{};
    bank.tap_count = 1;
    bank.path(0, 0)[0] = 1.0F;
    bank.path(0, 1)[0] = 0.5F;
    bank.path(1, 0)[0] = 0.25F;
    bank.path(1, 1)[0] = 1.0F;
    BinauralConvolver convolver;
    CHECK(convolver.set_filters(bank, 0));
    const StereoFrame input[2]{{1.0F, 0.0F}, {0.0F, 1.0F}};
    StereoFrame output[2]{};
    convolver.process(input, output, 2);
    CHECK_NEAR(output[0].left, 1.0F, 1.0e-6F);
    CHECK_NEAR(output[0].right, 0.5F, 1.0e-6F);
    CHECK_NEAR(output[1].left, 0.25F, 1.0e-6F);
    CHECK_NEAR(output[1].right, 1.0F, 1.0e-6F);

    HrirFilterBank muted{};
    muted.tap_count = 1;
    CHECK(convolver.set_filters(muted, 16));
    std::array<StereoFrame, 17> constant{};
    std::fill(constant.begin(), constant.end(), StereoFrame{1.0F, 0.0F});
    std::array<StereoFrame, 17> faded{};
    convolver.process(constant.data(), faded.data(), faded.size());
    for (std::size_t index = 1; index < faded.size(); ++index)
        CHECK(std::abs(faded[index].left - faded[index - 1].left) <= 0.07F);
}

void test_partitioned_convolver_matches_time_domain_reference() {
    constexpr std::size_t filter_taps = 1'201;
    constexpr std::size_t programme_frames = 1'537;
    const std::size_t rendered_frames = programme_frames + filter_taps;

    HrirFilterBank bank{};
    bank.tap_count = filter_taps;
    std::uint32_t coefficient_state = 0x31415926U;
    const auto next_coefficient = [&coefficient_state]() noexcept {
        coefficient_state = coefficient_state * 1664525U + 1013904223U;
        const float normalized = static_cast<float>(static_cast<std::int32_t>(coefficient_state)) / 2147483648.0F;
        return normalized * 0.003F;
    };
    for (auto& path : bank.coefficients) {
        for (std::size_t tap = 0; tap < filter_taps; ++tap)
            path[tap] = next_coefficient();
    }
    bank.path(0, 0)[0] += 0.75F;
    bank.path(1, 1)[0] += 0.65F;
    bank.path(0, 1)[512] += 0.20F;
    bank.path(1, 0)[1'024] -= 0.15F;

    std::vector<StereoFrame> input(rendered_frames);
    std::uint32_t input_state = 0x27182818U;
    for (std::size_t frame = 0; frame < programme_frames; ++frame) {
        input_state = input_state * 1103515245U + 12345U;
        input[frame].left = static_cast<float>(static_cast<std::int32_t>(input_state)) / 2147483648.0F;
        input_state = input_state * 1103515245U + 12345U;
        input[frame].right = static_cast<float>(static_cast<std::int32_t>(input_state)) / 2147483648.0F;
    }

    std::vector<StereoFrame> reference(rendered_frames);
    for (std::size_t frame = 0; frame < rendered_frames; ++frame) {
        const std::size_t taps = std::min(filter_taps, frame + 1);
        for (std::size_t tap = 0; tap < taps; ++tap) {
            const StereoFrame source = input[frame - tap];
            reference[frame].left += source.left * bank.path(0, 0)[tap] + source.right * bank.path(1, 0)[tap];
            reference[frame].right += source.left * bank.path(0, 1)[tap] + source.right * bank.path(1, 1)[tap];
        }
    }

    BinauralConvolver convolver;
    CHECK(convolver.set_filters(bank, 0));
    std::vector<StereoFrame> rendered(rendered_frames);
    constexpr std::array<std::size_t, 7> chunk_sizes{1, 17, 63, 128, 255, 7, 193};
    std::size_t position = 0;
    std::size_t chunk_index = 0;
    while (position < rendered_frames) {
        const std::size_t count = std::min(chunk_sizes[chunk_index % chunk_sizes.size()], rendered_frames - position);
        convolver.process(input.data() + position, rendered.data() + position, count);
        position += count;
        ++chunk_index;
    }
    CHECK_NEAR(rendered.front().left, reference.front().left,
               1.0e-6F); // no head-path block latency
    for (std::size_t frame = 0; frame < rendered_frames; ++frame) {
        CHECK_NEAR(rendered[frame].left, reference[frame].left, 2.0e-4F);
        CHECK_NEAR(rendered[frame].right, reference[frame].right, 2.0e-4F);
    }

    HrirFilterBank maximum_length{};
    maximum_length.tap_count = kMaximumHrirTaps;
    maximum_length.path(0, 0)[kMaximumHrirTaps - 1] = 1.0F;
    BinauralConvolver maximum_length_convolver;
    CHECK(maximum_length_convolver.set_filters(maximum_length, 0));
    std::vector<StereoFrame> maximum_input(kMaximumHrirTaps + 1);
    std::vector<StereoFrame> maximum_output(kMaximumHrirTaps + 1);
    maximum_input[0].left = 1.0F;
    maximum_length_convolver.process(maximum_input.data(), maximum_output.data(), maximum_output.size());
    CHECK_NEAR(maximum_output[kMaximumHrirTaps - 1].left, 1.0F, 2.0e-4F);
    CHECK_NEAR(maximum_output[kMaximumHrirTaps - 2].left, 0.0F, 2.0e-4F);
    CHECK_NEAR(maximum_output[kMaximumHrirTaps].left, 0.0F, 2.0e-4F);

    HrirFilterBank invalid{};
    invalid.tap_count = kMaximumHrirTaps + 1;
    CHECK(!convolver.set_filters(invalid, 0));
}

void test_partitioned_convolver_morph_and_interruption() {
    HrirFilterBank first{};
    first.tap_count = 513;
    first.path(0, 0)[0] = 0.1F;
    first.path(0, 0)[512] = 0.4F;
    HrirFilterBank second = first;
    second.tap_count = 1; // stale storage past tap_count must not leak into an interrupted morph
    HrirFilterBank third = first;
    third.path(0, 0)[512] = 0.2F;

    BinauralConvolver convolver;
    CHECK(convolver.set_filters(first, 0));
    std::array<StereoFrame, 1'024> warmup{};
    std::fill(warmup.begin(), warmup.end(), StereoFrame{1.0F, 0.0F});
    std::array<StereoFrame, 1'024> ignored{};
    convolver.process(warmup.data(), ignored.data(), warmup.size());
    CHECK_NEAR(ignored.back().left, 0.5F, 1.0e-5F);

    CHECK(convolver.set_filters(second, 256));
    std::array<StereoFrame, 80> first_part{};
    std::fill(first_part.begin(), first_part.end(), StereoFrame{1.0F, 0.0F});
    std::array<StereoFrame, 80> first_fade{};
    convolver.process(first_part.data(), first_fade.data(), first_part.size());
    CHECK(convolver.morphing());
    for (std::size_t frame = 1; frame < first_fade.size(); ++frame) {
        CHECK(first_fade[frame].left <= first_fade[frame - 1].left + 1.0e-5F);
        CHECK(std::abs(first_fade[frame].left - first_fade[frame - 1].left) < 0.003F);
    }

    const float before_interruption = first_fade.back().left;
    CHECK(convolver.set_filters(third, 128));
    std::array<StereoFrame, 160> second_part{};
    std::fill(second_part.begin(), second_part.end(), StereoFrame{1.0F, 0.0F});
    std::array<StereoFrame, 160> second_fade{};
    convolver.process(second_part.data(), second_fade.data(), second_fade.size());
    CHECK(std::abs(second_fade.front().left - before_interruption) < 0.003F);
    CHECK(!convolver.morphing());
    CHECK_NEAR(second_fade.back().left, 0.3F, 1.0e-4F);

    CHECK(convolver.set_filters(first, 4'800));
    std::array<StereoFrame, 128> scene_transition_input{};
    std::array<StereoFrame, 128> scene_transition_output{};
    convolver.process(scene_transition_input.data(), scene_transition_output.data(), scene_transition_input.size());
    const std::uint32_t scene_frames_remaining = convolver.morph_remaining_frames();
    CHECK(scene_frames_remaining == 4'672);
    CHECK(convolver.set_filters(second, scene_frames_remaining));
    CHECK(convolver.morph_remaining_frames() == scene_frames_remaining);
}

void test_eq_and_limiter() {
    StereoParametricEq eq;
    const BiquadParameters high_pass{BiquadType::high_pass, 100.0F, 0.707F, 0.0F};
    CHECK(eq.configure(std::span<const BiquadParameters>(&high_pass, 1), 48'000.0F));
    eq.set_enabled(true);
    std::array<StereoFrame, 4'096> dc{};
    std::fill(dc.begin(), dc.end(), StereoFrame{1.0F, 1.0F});
    eq.process(dc.data(), dc.size());
    CHECK(std::abs(dc.back().left) < 0.01F);

    TruePeakLimiter limiter;
    limiter.prepare(48'000.0F);
    limiter.set_master_gain_db(-6.0F);
    limiter.set_ceiling_db(-0.1F);
    std::array<StereoFrame, 32> loud{};
    std::fill(loud.begin(), loud.end(), StereoFrame{4.0F, -4.0F});
    limiter.process(loud.data(), loud.size());
    for (const auto& frame : loud) {
        CHECK(std::abs(frame.left) <= db_to_linear(-0.1F) + 1.0e-6F);
        CHECK(std::abs(frame.right) <= db_to_linear(-0.1F) + 1.0e-6F);
    }
    CHECK(limiter.gain_reduction_db() > 0.0F);
}

void test_bypass_crossfade_and_idempotent_eq() {
    BypassCrossfade bypass;
    bypass.reset(false);
    bypass.set_bypassed(true, 4);
    std::array<StereoFrame, 4> dry{};
    std::array<StereoFrame, 4> processed{};
    std::fill(dry.begin(), dry.end(), StereoFrame{1.0F, -1.0F});
    bypass.process(dry.data(), processed.data(), processed.size());
    for (std::size_t frame = 0; frame < processed.size(); ++frame) {
        const float expected = static_cast<float>(frame + 1) * 0.25F;
        CHECK_NEAR(processed[frame].left, expected, 1.0e-6F);
        CHECK_NEAR(processed[frame].right, -expected, 1.0e-6F);
    }
    CHECK_NEAR(bypass.mix(), 1.0F, 1.0e-6F);
    CHECK(bypass.remaining_frames() == 0);

    // Repeating the current target must not restart a completed transition.
    bypass.set_bypassed(true, 64);
    std::array<StereoFrame, 1> repeated_target{};
    bypass.process(dry.data(), repeated_target.data(), repeated_target.size());
    CHECK_NEAR(repeated_target[0].left, 1.0F, 1.0e-6F);
    CHECK(bypass.remaining_frames() == 0);

    // Reversing a transition starts from its current mix instead of jumping.
    bypass.reset(false);
    bypass.set_bypassed(true, 4);
    std::array<StereoFrame, 2> first_half{};
    bypass.process(dry.data(), first_half.data(), first_half.size());
    CHECK_NEAR(first_half.back().left, 0.5F, 1.0e-6F);
    bypass.set_bypassed(false, 2);
    std::array<StereoFrame, 2> reversed{};
    bypass.process(dry.data(), reversed.data(), reversed.size());
    CHECK_NEAR(reversed[0].left, 0.25F, 1.0e-6F);
    CHECK_NEAR(reversed[1].left, 0.0F, 1.0e-6F);

    const BiquadParameters high_pass{BiquadType::high_pass, 100.0F, 0.707F, 0.0F};
    StereoParametricEq reference;
    StereoParametricEq idempotent;
    CHECK(reference.configure(std::span<const BiquadParameters>(&high_pass, 1), 48'000.0F));
    CHECK(idempotent.configure(std::span<const BiquadParameters>(&high_pass, 1), 48'000.0F));
    reference.set_enabled(true);
    idempotent.set_enabled(true);
    std::array<StereoFrame, 256> reference_warmup{};
    std::array<StereoFrame, 256> idempotent_warmup{};
    std::fill(reference_warmup.begin(), reference_warmup.end(), StereoFrame{1.0F, 1.0F});
    idempotent_warmup = reference_warmup;
    reference.process(reference_warmup.data(), reference_warmup.size());
    idempotent.process(idempotent_warmup.data(), idempotent_warmup.size());
    CHECK(idempotent.configuration_matches(std::span<const BiquadParameters>(&high_pass, 1), 48'000.0F));
    CHECK(idempotent.configure(std::span<const BiquadParameters>(&high_pass, 1), 48'000.0F));
    std::array<StereoFrame, 1> reference_tail{{{1.0F, 1.0F}}};
    std::array<StereoFrame, 1> idempotent_tail = reference_tail;
    reference.process(reference_tail.data(), reference_tail.size());
    idempotent.process(idempotent_tail.data(), idempotent_tail.size());
    CHECK_NEAR(idempotent_tail[0].left, reference_tail[0].left, 1.0e-6F);
    CHECK_NEAR(idempotent_tail[0].right, reference_tail[0].right, 1.0e-6F);
    CHECK(!idempotent.configuration_matches(std::span<const BiquadParameters>(&high_pass, 1), 44'100.0F));
}

void test_potential_binaural_warning_detector() {
    PotentialBinauralDetector mono;
    PotentialBinauralDetector ordinary_stereo;
    PotentialBinauralDetector itd_fixture;
    mono.prepare(48'000.0F);
    ordinary_stereo.prepare(48'000.0F);
    itd_fixture.prepare(48'000.0F);

    std::uint32_t first_state = 0x12345678U;
    std::uint32_t second_state = 0x9ABCDEF0U;
    const auto next_noise = [](std::uint32_t& state) noexcept {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0F;
    };
    constexpr std::size_t delay_samples = 24; // 0.5 ms, inside the human ITD range
    std::array<float, 64> delay{};
    std::size_t delay_index = 0;
    std::array<StereoFrame, 256> mono_block{};
    std::array<StereoFrame, 256> stereo_block{};
    std::array<StereoFrame, 256> itd_block{};
    constexpr std::size_t total_frames = 48'000 * 3;
    for (std::size_t offset = 0; offset < total_frames; offset += mono_block.size()) {
        const std::size_t count = std::min(mono_block.size(), total_frames - offset);
        for (std::size_t frame = 0; frame < count; ++frame) {
            const float first = next_noise(first_state) * 0.25F;
            const float second = next_noise(second_state) * 0.25F;
            mono_block[frame] = {first, first};
            stereo_block[frame] = {first, second};
            const std::size_t read_index = (delay_index + delay.size() - delay_samples) % delay.size();
            itd_block[frame] = {first, delay[read_index] * 0.9F};
            delay[delay_index] = first;
            delay_index = (delay_index + 1) % delay.size();
        }
        mono.process(mono_block.data(), count);
        ordinary_stereo.process(stereo_block.data(), count);
        itd_fixture.process(itd_block.data(), count);
    }
    CHECK(!mono.potentially_binaural());
    CHECK(!ordinary_stereo.potentially_binaural());
    CHECK(itd_fixture.potentially_binaural());
}

void test_polyphase_resampler_quality_and_drift() {
    constexpr std::size_t source_count = 12'000;
    constexpr float frequency = 7'000.0F;
    std::vector<StereoFrame> source(source_count);
    for (std::size_t index = 0; index < source.size(); ++index) {
        const float sample = std::sin(2.0F * kPi * frequency * static_cast<float>(index) / 48'000.0F);
        source[index] = {sample, sample};
    }

    AsyncStereoResampler unity(16'384);
    CHECK(unity.push(source.data(), source.size()) == source.size());
    unity.set_target_fill(source.size() - 9);
    std::vector<StereoFrame> unity_output(4'000);
    CHECK(unity.render(unity_output.data(), unity_output.size(), 48'000.0F) == unity_output.size());
    double unity_error = 0.0;
    for (std::size_t index = 16; index < unity_output.size() - 16; ++index) {
        const double difference = unity_output[index].left - source[index].left;
        unity_error += difference * difference;
    }
    unity_error = std::sqrt(unity_error / static_cast<double>(unity_output.size() - 32));
    CHECK(unity_error < 1.0e-4);

    AsyncStereoResampler converted(16'384);
    CHECK(converted.push(source.data(), source.size()) == source.size());
    converted.set_target_fill(source.size() - 9);
    const float ratio = 48'000.0F / 44'100.0F;
    converted.set_nominal_ratio(ratio);
    std::vector<StereoFrame> converted_output(4'000);
    CHECK(converted.render(converted_output.data(), converted_output.size(), 44'100.0F) == converted_output.size());
    double conversion_error = 0.0;
    for (std::size_t index = 32; index < converted_output.size() - 32; ++index) {
        const float expected = std::sin(2.0F * kPi * frequency * static_cast<float>(index) / 44'100.0F);
        const double difference = converted_output[index].left - expected;
        conversion_error += difference * difference;
    }
    conversion_error = std::sqrt(conversion_error / static_cast<double>(converted_output.size() - 64));
    CHECK(conversion_error < 0.015);

    DriftController drift;
    CHECK(drift.update(700, 512, 0.01F) > 1.0F);
    drift.reset();
    CHECK(drift.update(300, 512, 0.01F) < 1.0F);

    ClockDriftEstimator clocks;
    float measured = 1.0F;
    for (std::uint64_t second = 0; second <= 200; ++second) {
        measured =
            clocks.update(second * 48'048, 48'000, second * 10'000'000, second * 48'000, 48'000, second * 10'000'000);
    }
    CHECK(measured > 1.0008F && measured < 1.0012F);
}

void test_resampler_xrun_event_telemetry() {
    AsyncStereoResampler resampler(32);
    std::array<StereoFrame, 64> output{};

    // An empty renderer is expected while the loopback stream is acquiring its
    // first packet. Repeated startup callbacks must not poison xrun telemetry.
    CHECK(resampler.render(output.data(), output.size(), 48'000.0F) == 0);
    CHECK(resampler.render(output.data(), output.size(), 48'000.0F) == 0);
    CHECK(resampler.underruns() == 0);

    std::array<StereoFrame, 16> short_packet{};
    CHECK(resampler.push(short_packet.data(), short_packet.size()) == short_packet.size());
    CHECK(resampler.render(output.data(), output.size(), 48'000.0F) == output.size());
    CHECK(resampler.underruns() == 1); // one event, not one count per missing frame
    CHECK(resampler.render(output.data(), output.size(), 48'000.0F) == output.size());
    CHECK(resampler.underruns() == 2);

    resampler.reset();
    CHECK(resampler.underruns() == 0);
    CHECK(resampler.overruns() == 0);

    std::array<StereoFrame, 64> oversized_packet{};
    CHECK(resampler.push(oversized_packet.data(), oversized_packet.size()) == 32);
    CHECK(resampler.overruns() == 1); // one truncated push is one overrun event
    CHECK(resampler.push(oversized_packet.data(), 1) == 0);
    CHECK(resampler.overruns() == 2);
}

void test_room_acoustics_and_coordinates() {
    RoomConfig room{};
    room.enabled = true;
    room.reflection_order = 2;
    ImageSourceModel model;
    const auto reflections = model.calculate({-1.0F, 1.2F, 1.5F}, {0.0F, 1.2F, 0.0F}, room);
    CHECK(!reflections.empty());
    CHECK(std::all_of(reflections.begin(), reflections.end(), [](const ReflectionTap& tap) {
        return tap.order >= 1 && tap.order <= 2 && tap.delay_seconds <= 0.08001F;
    }));
    const MaterialBands estimated_rt60 = estimate_room_rt60_eyring(room);
    CHECK(estimated_rt60.low >= estimated_rt60.mid);
    CHECK(estimated_rt60.mid >= estimated_rt60.high);

    RoomConfig frequency_dependent = room;
    frequency_dependent.surfaces[0].absorption = {0.0F, 0.0F, 0.0F};
    frequency_dependent.surfaces[0].diffusion = {0.0F, 0.5F, 0.99F};
    const auto band_reflections = model.calculate({-1.0F, 1.2F, 1.5F}, {0.0F, 1.2F, 0.0F}, frequency_dependent);
    CHECK(std::any_of(band_reflections.begin(), band_reflections.end(), [](const ReflectionTap& tap) {
        return tap.gain.low > tap.gain.mid && tap.gain.mid > tap.gain.high;
    }));

    const auto front = AmbisonicEncoderOrder3::encode_direction({0.0F, 0.0F, 1.0F});
    const auto right = AmbisonicEncoderOrder3::encode_direction({1.0F, 0.0F, 0.0F});
    const auto up = AmbisonicEncoderOrder3::encode_direction({0.0F, 1.0F, 0.0F});
    CHECK_NEAR(front[0], 1.0F, 1.0e-5F);
    CHECK(front[3] > 0.9F);
    CHECK(right[1] < -0.9F);
    CHECK(up[2] > 0.9F);

    EarlyReflectionProcessor early;
    CHECK(early.prepare(48'000.0F));
    CHECK(early.set_reflections(reflections, 0));
    std::vector<float> impulse(4'096);
    impulse[0] = 1.0F;
    std::vector<std::array<float, 16>> early_output(impulse.size());
    early.process(impulse.data(), early_output.data(), early_output.size());
    double early_energy = 0.0;
    for (const auto& frame : early_output)
        for (float sample : frame)
            early_energy += sample * sample;
    CHECK(early_energy > 0.0);

    LateReverbFdn16 fdn;
    CHECK(fdn.prepare(48'000.0F, {0.5F, 0.4F, 0.3F}));
    std::vector<std::array<float, 4>> late_output(6'000);
    fdn.process_mono(impulse.data(), late_output.data(), impulse.size());
    double late_energy = 0.0;
    for (const auto& frame : late_output)
        for (float sample : frame)
            late_energy += sample * sample;
    CHECK(late_energy > 0.0);
}

void test_order3_binaural_decoder_and_rotation() {
    AnalyticHrtfDatabase hrtf;
    AmbisonicBinauralDecoderOrder3 decoder;
    CHECK(decoder.update({}, hrtf, 0));

    const auto render_impulse = [](AmbisonicBinauralDecoderOrder3& target, const std::array<float, 16>& field) {
        std::array<std::array<float, 16>, 128> input{};
        std::array<StereoFrame, 128> output{};
        input[0] = field;
        target.process(input.data(), output.data(), output.size());
        return output;
    };

    double order_two_energy = 0.0;
    for (std::size_t channel = 4; channel <= 8; ++channel) {
        std::array<float, 16> field{};
        field[channel] = 1.0F;
        const auto output = render_impulse(decoder, field);
        for (const StereoFrame& frame : output)
            order_two_energy += frame.left * frame.left + frame.right * frame.right;
    }
    double order_three_energy = 0.0;
    for (std::size_t channel = 9; channel <= 15; ++channel) {
        std::array<float, 16> field{};
        field[channel] = 1.0F;
        const auto output = render_impulse(decoder, field);
        for (const StereoFrame& frame : output)
            order_three_energy += frame.left * frame.left + frame.right * frame.right;
    }
    CHECK(order_two_energy > 1.0e-6);
    CHECK(order_three_energy > 1.0e-6);

    const auto world_front = AmbisonicEncoderOrder3::encode_direction({0.0F, 0.0F, 1.0F});
    const auto facing_front = render_impulse(decoder, world_front);
    const Quaternionf world_to_turned_head = quaternion_from_rotation_vector({0.0F, -kPi * 0.5F, 0.0F});
    CHECK(decoder.update(world_to_turned_head, hrtf, 0));
    const auto facing_right = render_impulse(decoder, world_front);
    const auto ear_energy = [](const auto& output, bool left) {
        double energy = 0.0;
        for (const StereoFrame& frame : output) {
            const double sample = left ? frame.left : frame.right;
            energy += sample * sample;
        }
        return energy;
    };
    const double front_left_energy = ear_energy(facing_front, true);
    const double front_right_energy = ear_energy(facing_front, false);
    CHECK(std::abs(front_left_energy - front_right_energy) <
          0.10 * std::max(1.0e-12, front_left_energy + front_right_energy));
    CHECK(ear_energy(facing_right, true) > ear_energy(facing_right, false) * 1.02);

    class FixedItdHrtf final : public IHrtfDatabase {
    public:
        [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return 48'000; }
        [[nodiscard]] std::size_t maximum_taps() const noexcept override { return 32; }
        bool query(const Vec3f&, std::span<float> left, std::span<float> right,
                   std::size_t& tap_count) const noexcept override {
            std::fill(left.begin(), left.end(), 0.0F);
            std::fill(right.begin(), right.end(), 0.0F);
            left[12] = 1.0F;
            right[3] = 1.0F;
            tap_count = 16;
            return true;
        }
    } fixed_itd;
    AmbisonicBinauralDecoderOrder3 itd_decoder;
    CHECK(itd_decoder.update({}, fixed_itd, 0));
    std::array<float, 16> omnidirectional{};
    omnidirectional[0] = 1.0F;
    const auto itd_output = render_impulse(itd_decoder, omnidirectional);
    const auto onset = [](const auto& output, bool left) {
        for (std::size_t index = 0; index < output.size(); ++index) {
            const float sample = left ? output[index].left : output[index].right;
            if (std::abs(sample) > 0.1F)
                return index;
        }
        return output.size();
    };
    CHECK(onset(itd_output, false) == 3);
    CHECK(onset(itd_output, true) == 12);
}

void test_json_and_ui_commands() {
    SceneConfigV1 scene{};
    scene.headphone_eq.preamp_db = -6.0F;
    scene.room.late_reverb_enabled = false;
    const std::string json = scene_config_to_json(scene);
    CHECK(json.find("\"captureProvider\":\"native-driver\"") != std::string::npos);
    CHECK(json.find("\"captureEndpointId\":null") != std::string::npos);
    const auto parsed = scene_config_from_json(json);
    CHECK(parsed);
    CHECK(parsed.value->speakers[0].position_m.z > 0.0F);
    CHECK(parsed.value->room.dimensions_m.y == 2.7F);
    CHECK(!parsed.value->room.late_reverb_enabled);
    CHECK_NEAR(parsed.value->room.surfaces[0].diffusion.mid, 0.10F, 1.0e-6F);
    CHECK_NEAR(parsed.value->headphone_eq.preamp_db, -6.0F, 1.0e-6F);

    std::string legacy_json = json;
    const std::string canonical_route_prefix = "\"captureProvider\":\"native-driver\",\"captureEndpointId\":null,";
    const std::size_t route_prefix = legacy_json.find(canonical_route_prefix);
    CHECK(route_prefix != std::string::npos);
    if (route_prefix != std::string::npos)
        legacy_json.erase(route_prefix, canonical_route_prefix.size());
    const auto legacy_parsed = scene_config_from_json(legacy_json);
    CHECK(legacy_parsed);
    if (legacy_parsed) {
        CHECK(legacy_parsed.value->audio.capture_provider == CaptureProvider::native_driver);
        CHECK(!legacy_parsed.value->audio.capture_endpoint_id);
    }

    SceneConfigV1 external_scene = scene;
    external_scene.audio.capture_provider = CaptureProvider::external_render;
    external_scene.audio.capture_endpoint_id = "external-source";
    external_scene.audio.output_device_id = "usb-headphones";
    const auto external_roundtrip = scene_config_from_json(scene_config_to_json(external_scene));
    CHECK(external_roundtrip);
    if (external_roundtrip) {
        CHECK(external_roundtrip.value->audio.capture_provider == CaptureProvider::external_render);
        CHECK(external_roundtrip.value->audio.capture_endpoint_id == std::optional<std::string>("external-source"));
        CHECK(external_roundtrip.value->audio.output_device_id == std::optional<std::string>("usb-headphones"));
    }

    const std::array<std::string, 7> commands{
        R"({"schemaVersion":1,"type":"set-bypass","enabled":true})",
        R"({"schemaVersion":1,"type":"set-output-device","deviceId":"usb-headset"})",
        R"({"schemaVersion":1,"type":"set-audio-mode","mode":"exclusive-pro"})",
        R"({"schemaVersion":1,"type":"calibrate-neutral-pose","quaternion":[1,0,0,0]})",
        R"({"schemaVersion":1,"type":"set-hrtf","profileId":"sadie-d2-kemar","sofaPath":null})",
        R"({"schemaVersion":1,"type":"set-headphone-eq","eq":{"enabled":true,"preampDb":-3,"profileName":null,"bands":[{"id":"one","enabled":true,"type":"peak","frequencyHz":1000,"gainDb":-2,"q":1}]}})",
        R"({"schemaVersion":1,"type":"set-audio-route","captureProvider":"external-render","captureEndpointId":"external-source","outputDeviceId":"usb-headphones"})",
    };
    for (const std::string& command_json : commands) {
        const auto command = engine_command_from_json(command_json);
        CHECK(command);
        if (command) {
            if (command.value->type == EngineCommandType::set_audio_route) {
                CHECK(command.value->capture_provider == CaptureProvider::external_render);
                CHECK(command.value->capture_endpoint_id == std::optional<std::string>("external-source"));
                CHECK(command.value->output_device_id == std::optional<std::string>("usb-headphones"));
            }
            const auto roundtrip = engine_command_from_json(engine_command_to_json(*command.value));
            CHECK(roundtrip);
        }
    }
    const auto invalid_external_route = engine_command_from_json(
        R"({"schemaVersion":1,"type":"set-audio-route","captureProvider":"external-render","captureEndpointId":null,"outputDeviceId":"usb-headphones"})");
    CHECK(invalid_external_route);
    EngineStatusV1 status{};
    status.capture_state = StreamState::running;
    status.render_state = StreamState::running;
    status.audio_mode = AudioMode::exclusive_pro;
    status.render_sample_format = AudioSampleFormat::pcm_s32;
    const std::string status_json = engine_status_to_json(status);
    CHECK(status_json.find("\"captureState\":\"running\"") != std::string::npos);
    CHECK(status_json.find("\"audioMode\":\"exclusive-pro\"") != std::string::npos);
    CHECK(status_json.find("\"renderSampleFormat\":\"pcm-s32\"") != std::string::npos);
    CHECK(status_json.find("\"latencyP95Ms\"") != std::string::npos);
    CHECK(status_json.find("\"potentiallyBinaural\":false") != std::string::npos);

    SceneConfigV1 boundary = scene;
    boundary.room.enabled = true;
    boundary.listener.position_m.x = boundary.room.dimensions_m.x * 0.5F;
    std::string validation_error;
    CHECK(validate_scene_config(boundary, validation_error));
    boundary.listener.position_m.x += 0.001F;
    CHECK(!validate_scene_config(boundary, validation_error));
    CHECK(validation_error.find("listener must be inside") != std::string::npos);
    boundary = scene;
    boundary.room.enabled = true;
    boundary.speakers[0].position_m.z = boundary.room.dimensions_m.z * 0.5F + 0.001F;
    CHECK(!validate_scene_config(boundary, validation_error));
    CHECK(validation_error.find("speakers must be inside") != std::string::npos);
}

void test_json_frame_decoder() {
    CHECK(pose_sequence_is_newer(false, 0, 0));
    CHECK(pose_sequence_is_newer(true, 41, 42));
    CHECK(!pose_sequence_is_newer(true, 42, 42));
    CHECK(!pose_sequence_is_newer(true, 42, 41));

    CHECK(!pipe_has_complete_read(0, 4));
    CHECK(!pipe_has_complete_read(3, 4));
    CHECK(pipe_has_complete_read(4, 4));
    CHECK(!pipe_has_complete_read(59, sizeof(HeadPosePacketV1) - sizeof(std::uint32_t)));
    CHECK(pipe_has_complete_read(60, sizeof(HeadPosePacketV1) - sizeof(std::uint32_t)));
    CHECK(!pipe_has_complete_read(kMaximumJsonFrameBytes - 1, kMaximumJsonFrameBytes));
    CHECK(pipe_has_complete_read(kMaximumJsonFrameBytes, kMaximumJsonFrameBytes));

    const std::string first = R"({"schemaVersion":1,"type":"start"})";
    const std::string second = R"({"schemaVersion":1,"type":"stop"})";
    std::vector<std::byte> bytes = encode_json_frame(first);
    const auto second_frame = encode_json_frame(second);
    bytes.insert(bytes.end(), second_frame.begin(), second_frame.end());
    JsonFrameDecoder decoder;
    std::vector<std::string> frames;
    std::string error;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::size_t count = std::min<std::size_t>(3, bytes.size() - offset);
        CHECK(decoder.feed(std::span<const std::byte>(bytes.data() + offset, count), frames, error));
    }
    CHECK(frames.size() == 2);
    CHECK(frames[0] == first);
    CHECK(frames[1] == second);
}

void test_single_instance_guard() {
#if defined(_WIN32)
    const std::string endpoint = NamedPipeServer::endpoint_name();
    const std::string prefix = R"(\\.\pipe\SoundSpatializer.Engine.v1.)";
    CHECK(endpoint.starts_with(prefix));
    CHECK(std::all_of(endpoint.begin() + static_cast<std::ptrdiff_t>(prefix.size()), endpoint.end(), [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
               value == '-' || value == '.';
    }));
    CHECK(std::count(endpoint.begin() + static_cast<std::ptrdiff_t>(prefix.size()), endpoint.end(), '.') >= 1);

    const std::string pose_endpoint = NamedPipeServer::pose_endpoint_name();
    const std::string pose_prefix = R"(\\.\pipe\SoundSpatializer.Pose.v1.)";
    CHECK(pose_endpoint.starts_with(pose_prefix));
    CHECK(pose_endpoint.substr(pose_prefix.size()) == endpoint.substr(prefix.size()));
    CHECK(std::all_of(pose_endpoint.begin() + static_cast<std::ptrdiff_t>(pose_prefix.size()),
                      pose_endpoint.end(), [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '-' || value == '.';
    }));

    SingleInstanceGuard first;
    SingleInstanceGuard second;
    const std::string suffix = "Engine.test." +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
                               std::to_string(reinterpret_cast<std::uintptr_t>(&first));
    std::string error;
    CHECK(first.acquire(error, suffix));
    CHECK(first.acquired());
    CHECK(!first.already_running());
    CHECK(!second.acquire(error, suffix));
    CHECK(!second.acquired());
    CHECK(second.already_running());
#endif
}

class RouteRecordingBackend final : public IAudioBackend {
public:
    bool start(const AudioBackendConfig& config, IAudioProcessor&, std::string& error) override {
        ++start_count;
        attempted_modes.push_back(config.mode);
        last_config = config;
        if (!validate_audio_backend_config(config, error)) {
            running_ = false;
            return false;
        }
        if (fail_exclusive && config.mode == AudioMode::exclusive_pro) {
            running_ = false;
            error = "injected exclusive format failure";
            return false;
        }
        if (fail_next_start) {
            fail_next_start = false;
            running_ = false;
            error = "injected route start failure";
            return false;
        }
        running_ = true;
        diagnostics_.capture_state = StreamState::running;
        diagnostics_.render_state = StreamState::running;
        error.clear();
        return true;
    }

    void stop() noexcept override {
        ++stop_count;
        running_ = false;
        diagnostics_.capture_state = StreamState::stopped;
        diagnostics_.render_state = StreamState::stopped;
    }

    [[nodiscard]] bool running() const noexcept override { return running_; }
    [[nodiscard]] AudioBackendDiagnostics diagnostics() const override { return diagnostics_; }

    AudioBackendConfig last_config{};
    std::size_t start_count{};
    std::size_t stop_count{};
    bool fail_next_start{};
    bool fail_exclusive{};
    std::vector<AudioMode> attempted_modes{};

private:
    AudioBackendDiagnostics diagnostics_{};
    bool running_{};
};

class BlockingStartBackend final : public IAudioBackend {
public:
    bool start(const AudioBackendConfig&, IAudioProcessor&, std::string& error) override {
        {
            std::scoped_lock lock(mutex_);
            entered_ = true;
            diagnostics_.capture_state = StreamState::starting;
            diagnostics_.render_state = StreamState::starting;
        }
        condition_.notify_all();
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return released_; });
        running_ = true;
        diagnostics_.capture_state = StreamState::running;
        diagnostics_.render_state = StreamState::running;
        error.clear();
        return true;
    }

    void stop() noexcept override {
        release_start();
        std::scoped_lock lock(mutex_);
        running_ = false;
        diagnostics_.capture_state = StreamState::stopped;
        diagnostics_.render_state = StreamState::stopped;
    }

    [[nodiscard]] bool running() const noexcept override {
        std::scoped_lock lock(mutex_);
        return running_;
    }

    [[nodiscard]] AudioBackendDiagnostics diagnostics() const override {
        std::scoped_lock lock(mutex_);
        return diagnostics_;
    }

    [[nodiscard]] bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release_start() noexcept {
        {
            std::scoped_lock lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
    AudioBackendDiagnostics diagnostics_{};
    bool entered_{};
    bool released_{};
    bool running_{};
};

void test_status_remains_available_while_audio_start_blocks() {
    auto backend = std::make_unique<BlockingStartBackend>();
    BlockingStartBackend* blocker = backend.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(backend));

    std::string start_error;
    auto start = std::async(std::launch::async, [&] { return engine->start_audio(start_error); });
    CHECK(blocker->wait_until_entered(std::chrono::seconds(1)));

    auto status = std::async(std::launch::async, [&] { return engine->status(); });
    const bool status_ready = status.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
    CHECK(status_ready);
    blocker->release_start();
    CHECK(start.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    CHECK(start.get());
    if (status_ready)
        CHECK(status.get().capture_state == StreamState::starting);
    else
        (void)status.get();
    engine->stop_audio();
}

void test_atomic_audio_route_command() {
    auto backend = std::make_unique<RouteRecordingBackend>();
    RouteRecordingBackend* recorder = backend.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(backend));
    SceneConfigV1 initial = engine->scene();
    initial.audio.output_device_id = "initial-headphones";
    std::string error;
    CHECK(engine->set_scene(initial, error));
    CHECK(engine->start_audio(error));
    CHECK(recorder->start_count == 1);

    EngineCommandV1 route{};
    route.type = EngineCommandType::set_audio_route;
    route.capture_provider = CaptureProvider::external_render;
    route.capture_endpoint_id = "external-source";
    route.output_device_id = "usb-headphones";
    CHECK(engine->execute_command(route, error));
    CHECK(engine->scene().audio.capture_provider == CaptureProvider::external_render);
    CHECK(engine->scene().audio.capture_endpoint_id == route.capture_endpoint_id);
    CHECK(recorder->last_config.capture_provider == CaptureProvider::external_render);
    CHECK(recorder->last_config.capture_endpoint_id == "external-source");
    CHECK(recorder->last_config.physical_output_endpoint_id == "usb-headphones");
    CHECK(recorder->start_count == 2);
    CHECK(recorder->running());

    const SceneConfigV1 working_route = engine->scene();
    recorder->fail_next_start = true;
    route.capture_endpoint_id = "unavailable-source";
    route.output_device_id = "other-headphones";
    CHECK(!engine->execute_command(route, error));
    CHECK(error.find("audio route rejected") != std::string::npos);
    CHECK(engine->scene().audio.capture_provider == working_route.audio.capture_provider);
    CHECK(engine->scene().audio.capture_endpoint_id == working_route.audio.capture_endpoint_id);
    CHECK(engine->scene().audio.output_device_id == working_route.audio.output_device_id);
    CHECK(recorder->running());
    CHECK(recorder->last_config.capture_endpoint_id == "external-source");
    CHECK(recorder->last_config.physical_output_endpoint_id == "usb-headphones");
    CHECK(recorder->start_count == 4); // rejected route, then previous route restoration

    const std::size_t stops_before_invalid = recorder->stop_count;
    route.capture_endpoint_id = "USB-HEADPHONES";
    route.output_device_id = "usb-headphones";
    CHECK(!engine->execute_command(route, error));
    CHECK(recorder->stop_count == stops_before_invalid);
    CHECK(recorder->running());
    engine->stop_audio();
}

void test_audio_mode_command_recovers_from_unsupported_exclusive() {
    auto backend = std::make_unique<RouteRecordingBackend>();
    RouteRecordingBackend* recorder = backend.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(backend));
    std::string error;
    CHECK(engine->start_audio(error));
    CHECK(recorder->running());

    recorder->fail_exclusive = true;
    EngineCommandV1 mode{};
    mode.type = EngineCommandType::set_audio_mode;
    mode.audio_mode = AudioMode::exclusive_pro;
    CHECK(engine->execute_command(mode, error));
    CHECK(error.empty());
    CHECK(recorder->running());
    CHECK(engine->scene().audio.mode == AudioMode::shared_low_latency);
    CHECK(engine->scene().audio.buffer_frames == 128);
    CHECK(recorder->last_config.mode == AudioMode::shared_low_latency);
    const EngineStatusV1 fallback_status = engine->status();
    CHECK(fallback_status.audio_mode == AudioMode::shared_low_latency);
    CHECK(fallback_status.last_error.starts_with("AUDIO_MODE_FALLBACK "));
    CHECK(fallback_status.last_error.find("requested=exclusive-pro") != std::string::npos);
    CHECK(fallback_status.last_error.find("effective=shared-low-latency") != std::string::npos);
    CHECK(fallback_status.capture_state == StreamState::degraded);
    CHECK(fallback_status.render_state == StreamState::degraded);
    CHECK(recorder->attempted_modes.size() == 3);
    if (recorder->attempted_modes.size() == 3) {
        CHECK(recorder->attempted_modes[0] == AudioMode::shared_low_latency);
        CHECK(recorder->attempted_modes[1] == AudioMode::exclusive_pro);
        CHECK(recorder->attempted_modes[2] == AudioMode::shared_low_latency);
    }

    CHECK(engine->start_audio(error));
    CHECK(engine->status().last_error.starts_with("AUDIO_MODE_FALLBACK "));
    CHECK(engine->status().audio_mode == AudioMode::shared_low_latency);

    // A second request performs one bounded exclusive attempt followed by one
    // shared recovery; it never spins on the unsupported endpoint.
    const std::size_t starts_before_retry = recorder->start_count;
    CHECK(engine->execute_command(mode, error));
    CHECK(recorder->start_count == starts_before_retry + 2);
    CHECK(recorder->running());
    CHECK(engine->scene().audio.mode == AudioMode::shared_low_latency);

    mode.audio_mode = AudioMode::shared_low_latency;
    CHECK(engine->execute_command(mode, error));
    CHECK(recorder->running());
    CHECK(engine->status().last_error.empty());
    engine->stop_audio();
}

void test_effective_audio_mode_survives_a_stale_scene_update() {
    auto backend = std::make_unique<RouteRecordingBackend>();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(backend));
    std::string error;
    CHECK(engine->start_audio(error));
    CHECK(engine->status().audio_mode == AudioMode::shared_low_latency);

    // Full scene persistence is asynchronous in the desktop. A delayed scene
    // from an older UI may still contain the requested Pro value even though
    // the currently open backend is shared. Status must describe hardware.
    SceneConfigV1 stale = engine->scene();
    stale.audio.mode = AudioMode::exclusive_pro;
    stale.audio.buffer_frames = 128;
    CHECK(engine->set_scene(stale, error));
    CHECK(engine->scene().audio.mode == AudioMode::exclusive_pro);
    CHECK(engine->status().audio_mode == AudioMode::shared_low_latency);

    // An idempotent start cannot turn that stale request into an effective
    // mode claim without reopening the backend.
    CHECK(engine->start_audio(error));
    CHECK(engine->status().audio_mode == AudioMode::shared_low_latency);
    engine->stop_audio();
}

void test_failed_exclusive_and_shared_start_does_not_falsify_effective_scene() {
    auto backend = std::make_unique<RouteRecordingBackend>();
    RouteRecordingBackend* recorder = backend.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(backend));
    SceneConfigV1 requested = engine->scene();
    requested.audio.mode = AudioMode::exclusive_pro;
    requested.audio.buffer_frames = 128;
    std::string error;
    CHECK(engine->set_scene(requested, error));

    recorder->fail_exclusive = true;
    recorder->fail_next_start = true; // consumed by the one shared fallback
    CHECK(!engine->start_audio(error));
    CHECK(error.find("exclusive Pro mode failed") != std::string::npos);
    CHECK(error.find("shared low-latency fallback failed") != std::string::npos);
    CHECK(!recorder->running());
    CHECK(engine->scene().audio.mode == AudioMode::exclusive_pro);
    CHECK(engine->status().audio_mode == AudioMode::exclusive_pro);
    CHECK(recorder->attempted_modes.size() == 2);
    if (recorder->attempted_modes.size() == 2) {
        CHECK(recorder->attempted_modes[0] == AudioMode::exclusive_pro);
        CHECK(recorder->attempted_modes[1] == AudioMode::shared_low_latency);
    }
}

void test_engine_mock_pipeline() {
    auto mock = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock_pointer = mock.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(mock));
    SceneConfigV1 scene{};
    SceneConfigV1 unavailable = scene;
    unavailable.hrtf.profile_id = "sadie-d2-kemar";
    std::string error;
    CHECK(!engine->set_scene(unavailable, error));
    CHECK(engine->status().last_error.find("refused") != std::string::npos);
    scene.hrtf.profile_id = "builtin-analytic-neutral";
    CHECK(engine->set_scene(scene, error));
    CHECK(engine->start_audio(error));
    engine->submit_head_pose({1, 0, {}, {}, 1.0F, TrackingState::tracking});
    std::array<StereoFrame, 256> input{};
    input[0] = {1.0F, 0.0F};
    std::array<StereoFrame, 256> output{};
    mock_pointer->process_block(input.data(), output.data(), output.size());
    double energy = 0.0;
    for (const auto& frame : output)
        energy += frame.left * frame.left + frame.right * frame.right;
    CHECK(energy > 0.0);
    CHECK(engine->status().render_state == StreamState::running);
    engine->stop_audio();
    CHECK(engine->start_audio(error));

    scene.room.enabled = true;
    scene.audio.room_mix = 1.0F;
    CHECK(engine->set_scene(scene, error));
    std::array<StereoFrame, 64> warmup_input{};
    std::array<StereoFrame, 64> warmup_output{};
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        mock_pointer->process_block(warmup_input.data(), warmup_output.data(), warmup_output.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::array<StereoFrame, 4'096> room_input{};
    room_input[0] = {1.0F, 0.0F};
    std::array<StereoFrame, 4'096> room_output{};
    mock_pointer->process_block(room_input.data(), room_output.data(), room_output.size());
    double delayed_room_energy = 0.0;
    for (std::size_t index = 128; index < room_output.size(); ++index)
        delayed_room_energy +=
            room_output[index].left * room_output[index].left + room_output[index].right * room_output[index].right;
    CHECK(delayed_room_energy > 0.0);
    engine->stop_audio();
}

void test_engine_head_pose_changes_binaural_output() {
    auto mock = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock_pointer = mock.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(mock));

    SceneConfigV1 scene{};
    scene.hrtf.profile_id = "builtin-analytic-neutral";
    scene.speakers[0].position_m = {0.0F, 1.2F, 2.0F}; // left input source, world-front
    scene.speakers[1].position_m = {1.0F, 1.2F, 2.0F};
    scene.audio.master_gain_db = -12.0F; // keep the limiter outside the comparison
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;

    std::string error;
    CHECK(engine->set_scene(scene, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 128> silence{};
    std::array<StereoFrame, 128> scratch{};
    std::uint64_t sequence = 0;
    const auto settle_pose = [&](const Quaternionf& orientation) {
        // Repeated tracked samples let the One Euro filter converge while the
        // latest-wins HRTF worker prepares and the convolver completes its morph.
        for (std::size_t attempt = 0; attempt < 80; ++attempt) {
            engine->submit_head_pose({++sequence, 0, orientation, {}, 1.0F, TrackingState::tracking});
            mock_pointer->process_block(silence.data(), scratch.data(), scratch.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (std::size_t attempt = 0; attempt < 16; ++attempt) {
            mock_pointer->process_block(silence.data(), scratch.data(), scratch.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const auto render_left_impulse = [&] {
        std::array<StereoFrame, 256> input{};
        std::array<StereoFrame, 256> output{};
        input[0].left = 0.1F;
        mock_pointer->process_block(input.data(), output.data(), output.size());
        std::array<double, 2> energy{};
        for (const StereoFrame& frame : output) {
            energy[0] += static_cast<double>(frame.left) * frame.left;
            energy[1] += static_cast<double>(frame.right) * frame.right;
        }
        return energy;
    };

    settle_pose({});
    const auto neutral = render_left_impulse();
    CHECK(neutral[0] > 1.0e-8);
    CHECK(neutral[1] > 1.0e-8);
    CHECK(std::abs(neutral[0] - neutral[1]) < 0.10 * (neutral[0] + neutral[1]));

    const Quaternionf yaw_right = quaternion_from_rotation_vector({0.0F, kPi * 0.5F, 0.0F});
    settle_pose(yaw_right);
    const auto turned = render_left_impulse();
    CHECK(turned[0] > turned[1] * 2.0);
    CHECK(std::abs(turned[0] - neutral[0]) + std::abs(turned[1] - neutral[1]) > 0.20 * (neutral[0] + neutral[1]));
    CHECK(engine->status().tracking_hz > 0.0F);

    engine->stop_audio();
}

void test_hrtf_cache_when_fixture_is_available() {
#if defined(SOUND_SPATIALIZER_HAS_MYSOFA)
    const std::filesystem::path sofa_path = std::filesystem::path(SOUND_SPATIALIZER_TEST_REPOSITORY_ROOT) /
                                            "resources" / "hrtf" / "data" / "sadie-d2-kemar.sofa";
    if (!std::filesystem::exists(sofa_path))
        return;
    auto engine = std::make_unique<SpatialAudioEngine>(std::make_unique<MockAudioBackend>());
    SceneConfigV1 scene{};
    scene.hrtf.profile_id = "personal-test";
    scene.hrtf.sofa_path = sofa_path.string();
    std::string error;
    CHECK(engine->set_scene(scene, error));
    CHECK(engine->cached_personal_hrtf_count() == 1);
    CHECK(engine->set_scene(scene, error));
    CHECK(engine->cached_personal_hrtf_count() == 1);
#endif
}

} // namespace

int main() {
    test_spsc_fifo();
    test_endpoint_contract_selection();
    test_wasapi_period_and_capture_budget_policy();
    test_exclusive_render_format_policy_and_pcm_s32_conversion();
    test_pose_wire_contract();
    test_pose_prediction_and_world_lock();
    test_pose_zero_confidence_and_low_latency_filter();
    test_pose_timeout_uses_reception_time_not_capture_time();
    test_pose_reacquisition_resets_filters_and_velocity();
    test_pose_brief_dropout_preserves_filter_without_velocity_spike();
    test_realtime_latency_percentiles_are_bounded_and_reactive();
    test_hrtf_lateral_sign();
    test_hrtf_worker_latest_wins_and_prepared_room_filters();
    test_hrtf_worker_publishes_direct_before_failed_room();
    test_measured_sofa_itd_when_available();
    test_convolver_and_morph();
    test_partitioned_convolver_matches_time_domain_reference();
    test_partitioned_convolver_morph_and_interruption();
    test_eq_and_limiter();
    test_bypass_crossfade_and_idempotent_eq();
    test_potential_binaural_warning_detector();
    test_polyphase_resampler_quality_and_drift();
    test_resampler_xrun_event_telemetry();
    test_room_acoustics_and_coordinates();
    test_order3_binaural_decoder_and_rotation();
    test_json_and_ui_commands();
    test_json_frame_decoder();
    test_single_instance_guard();
    test_status_remains_available_while_audio_start_blocks();
    test_atomic_audio_route_command();
    test_audio_mode_command_recovers_from_unsupported_exclusive();
    test_effective_audio_mode_survives_a_stale_scene_update();
    test_failed_exclusive_and_shared_start_does_not_falsify_effective_scene();
    test_engine_mock_pipeline();
    test_engine_head_pose_changes_binaural_output();
    test_hrtf_cache_when_fixture_is_available();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All Sound Spatializer engine tests passed\n";
    return 0;
}
