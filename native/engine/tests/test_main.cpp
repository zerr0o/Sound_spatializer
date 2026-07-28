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
#include "sound_spatializer/spectral.hpp"
#include "sound_spatializer/spsc_ring_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

void test_surround_input_contracts() {
    CHECK(kStereoChannelMask == 0x0000'0003U);
    CHECK(kSurround51BackChannelMask == 0x0000'003FU);
    CHECK(kSurround51ChannelMask == 0x0000'060FU);
    CHECK(is_supported_surround_5_1_channel_mask(kSurround51BackChannelMask));
    CHECK(is_supported_surround_5_1_channel_mask(kSurround51ChannelMask));
    CHECK(!is_supported_surround_5_1_channel_mask(kStereoChannelMask));
    CHECK(!is_supported_surround_5_1_channel_mask(0U));
    CHECK(!is_supported_surround_5_1_channel_mask(0x0000'063FU));
    CHECK(expected_input_channel_count(InputLayout::stereo) == 2U);
    CHECK(expected_input_channel_count(InputLayout::surround_5_1) == 6U);

    AudioBackendConfig native_surround{};
    native_surround.capture_provider = CaptureProvider::native_driver;
    native_surround.input_layout = InputLayout::surround_5_1;
    native_surround.physical_output_endpoint_id = "headphones";
    std::string error;
    CHECK(!validate_audio_backend_config(native_surround, error));
    CHECK(error.find("external render endpoint") != std::string::npos);

    AudioBackendConfig external_surround{};
    external_surround.capture_provider = CaptureProvider::external_render;
    external_surround.capture_endpoint_id = "vb-cable-5.1";
    external_surround.physical_output_endpoint_id = "headphones";
    external_surround.input_layout = InputLayout::surround_5_1;
    CHECK(validate_audio_backend_config(external_surround, error));
    CHECK(error.empty());
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

void test_diffuse_field_equalizer_unit_impulse_and_crossfade() {
    DiffuseFieldEqualizer equalizer;
    // The default is a unit impulse: a provider with nothing to remove must be
    // bit-transparent, not merely close.
    std::array<StereoFrame, 8> frames{};
    for (std::size_t index = 0; index < frames.size(); ++index)
        frames[index] = {static_cast<float>(index) * 0.125F, -static_cast<float>(index) * 0.25F};
    const auto original = frames;
    equalizer.process(frames.data(), frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index) {
        CHECK(frames[index].left == original[index].left);
        CHECK(frames[index].right == original[index].right);
    }

    // A two-tap filter must apply to both channels independently and keep the
    // history across process() calls.
    const std::array<float, 2> taps{0.5F, 0.25F};
    equalizer.set_filter(taps, 0);
    equalizer.reset();
    CHECK(equalizer.active_taps() == 2);
    std::array<StereoFrame, 3> impulse{StereoFrame{1.0F, -1.0F}, StereoFrame{}, StereoFrame{}};
    equalizer.process(impulse.data(), 1);
    equalizer.process(impulse.data() + 1, 2);
    CHECK_NEAR(impulse[0].left, 0.5F, 1.0e-7F);
    CHECK_NEAR(impulse[1].left, 0.25F, 1.0e-7F);
    CHECK_NEAR(impulse[2].left, 0.0F, 1.0e-7F);
    CHECK_NEAR(impulse[0].right, -0.5F, 1.0e-7F);
    CHECK_NEAR(impulse[1].right, -0.25F, 1.0e-7F);

    // Switching back to "no equalization" crossfades instead of jumping.
    equalizer.reset();
    equalizer.set_filter({}, 4);
    CHECK(equalizer.transitioning());
    std::array<StereoFrame, 6> ramp{};
    ramp.fill(StereoFrame{1.0F, 1.0F});
    equalizer.process(ramp.data(), ramp.size());
    CHECK(!equalizer.transitioning());
    CHECK(equalizer.active_taps() == 1);
    // The tail is fully on the unit impulse again.
    CHECK_NEAR(ramp[5].left, 1.0F, 1.0e-6F);
    // ... and no intermediate sample overshot either endpoint of the fade.
    for (const StereoFrame& frame : ramp) CHECK(frame.left > 0.0F && frame.left <= 1.0F + 1.0e-6F);
}

void test_diffuse_field_inverse_neutralizes_level_and_tilt() {
    constexpr std::size_t kLength = 256;
    constexpr float kBinHz = static_cast<float>(kSampleRate) / 2'048.0F;
    std::vector<std::complex<float>> twiddles(1'024);
    build_twiddle_table(twiddles);

    const auto response_db = [&](std::span<const float> taps, float frequency_hz) {
        std::vector<std::complex<float>> spectrum(2'048);
        for (std::size_t tap = 0; tap < taps.size(); ++tap) spectrum[tap] = {taps[tap], 0.0F};
        radix2_transform(spectrum, twiddles, false);
        const std::size_t bin = static_cast<std::size_t>(std::lround(frequency_hz / kBinHz));
        return 20.0F * std::log10(std::max(std::abs(spectrum[bin]), 1.0e-9F));
    };

    // A set that is 12 dB hot and carries a broad 3 kHz resonance, built from a
    // resonant biquad so the designer sees something realistic rather than a
    // synthetic magnitude it could invert exactly.
    const BiquadCoefficients resonance =
        design_biquad({BiquadType::peaking, 3'000.0F, 1.2F, 9.0F}, static_cast<float>(kSampleRate));
    std::vector<float> responses(kLength * 8, 0.0F);
    for (std::size_t index = 0; index < 8; ++index) {
        float z1 = 0.0F;
        float z2 = 0.0F;
        // Vary the onset so the blocks are not all the same response.
        const std::size_t onset = index * 2;
        for (std::size_t tap = 0; tap < kLength; ++tap) {
            const float input = tap == onset ? 3.981072F : 0.0F; // +12 dB impulse
            const float output = resonance.b0 * input + z1;
            z1 = resonance.b1 * input - resonance.a1 * output + z2;
            z2 = resonance.b2 * input - resonance.a2 * output;
            responses[index * kLength + tap] = output;
        }
    }

    std::array<float, DiffuseFieldEqualizer::kMaximumTaps> taps{};
    CHECK(design_diffuse_field_inverse(responses, kLength, kSampleRate, taps));
    const float at_3k = response_db(taps, 3'000.0F);
    const float at_500 = response_db(taps, 500.0F);
    const float at_100 = response_db(taps, 100.0F);
    std::cout << "  synthetic +12 dB set with a 3 kHz resonance -> correction " << at_500 << " dB at 500 Hz, "
              << at_3k << " dB at 3 kHz, " << at_100 << " dB at 100 Hz\n";
    // The level trim cancels the 12 dB offset inside the shaped band ...
    CHECK_NEAR(at_500, -12.0F, 2.0F);
    // ... and still reaches the bass, which is only level-trimmed and never
    // spectrally shaped. A trim that faded out here would leave the sub-bass
    // 12 dB louder than everything else.
    CHECK(std::abs(at_100 - at_500) < 3.0F);
    // The resonance is cut on top of the trim.
    CHECK(at_3k < at_500 - 5.0F);

    // A set that is already neutral must come back as an almost flat unit gain.
    std::vector<float> neutral(kLength * 4, 0.0F);
    for (std::size_t index = 0; index < 4; ++index) neutral[index * kLength + index] = 1.0F;
    std::array<float, DiffuseFieldEqualizer::kMaximumTaps> neutral_taps{};
    CHECK(design_diffuse_field_inverse(neutral, kLength, kSampleRate, neutral_taps));
    for (const float frequency : {200.0F, 1'000.0F, 4'000.0F, 10'000.0F})
        CHECK(std::abs(response_db(neutral_taps, frequency)) < 1.0F);
}

void test_measured_sofa_diffuse_field_equalization_when_available() {
#if defined(SOUND_SPATIALIZER_HAS_MYSOFA)
    const std::filesystem::path sofa_path = std::filesystem::path(SOUND_SPATIALIZER_TEST_REPOSITORY_ROOT) /
                                            "resources" / "hrtf" / "data" / "sadie-d2-kemar.sofa";
    if (!std::filesystem::exists(sofa_path)) {
        std::cout << "SADIE D2 fixture not fetched; diffuse-field equalization test skipped\n";
        return;
    }
    const HrtfLoadResult loaded = load_sofa_hrtf(sofa_path.string(), kSampleRate);
    CHECK(loaded.database != nullptr);
    if (!loaded.database) return;
    const std::span<const float> filter = loaded.database->diffuse_field_filter();
    CHECK(filter.size() == DiffuseFieldEqualizer::kMaximumTaps);
    if (filter.empty()) return;

    constexpr std::size_t kFft = 2'048;
    constexpr float kBinHz = static_cast<float>(kSampleRate) / static_cast<float>(kFft);
    std::vector<std::complex<float>> twiddles(kFft / 2);
    build_twiddle_table(twiddles);
    std::vector<std::complex<float>> spectrum(kFft);
    for (std::size_t tap = 0; tap < filter.size(); ++tap) spectrum[tap] = {filter[tap], 0.0F};
    radix2_transform(spectrum, twiddles, false);

    const auto gain_db = [&](float frequency_hz) {
        const std::size_t bin = static_cast<std::size_t>(std::lround(frequency_hz / kBinHz));
        return 20.0F * std::log10(std::max(std::abs(spectrum[bin]), 1.0e-9F));
    };
    // Independent cross-check through the public query API, so a mistake in the
    // loader's own averaging cannot hide behind itself. This is what
    // established that the shipped SADIE sets are already diffuse-field
    // equalized but roughly 10 dB hot.
    std::vector<double> averaged(kFft / 2 + 1, 0.0);
    std::size_t sampled = 0;
    {
        std::array<float, kMaximumHrirTaps> left{};
        std::array<float, kMaximumHrirTaps> right{};
        std::vector<std::complex<float>> ear_spectrum(kFft);
        for (int azimuth = 0; azimuth < 360; azimuth += 15) {
            for (int elevation = -60; elevation <= 60; elevation += 30) {
                const float a = static_cast<float>(azimuth) * kPi / 180.0F;
                const float e = static_cast<float>(elevation) * kPi / 180.0F;
                const Vec3f direction{std::sin(a) * std::cos(e), std::sin(e), std::cos(a) * std::cos(e)};
                std::size_t taps = 0;
                if (!loaded.database->query(direction, left, right, taps)) continue;
                for (const auto* ear : {&left, &right}) {
                    std::fill(ear_spectrum.begin(), ear_spectrum.end(), std::complex<float>{});
                    for (std::size_t tap = 0; tap < taps && tap < kFft; ++tap)
                        ear_spectrum[tap] = {(*ear)[tap], 0.0F};
                    radix2_transform(ear_spectrum, twiddles, false);
                    for (std::size_t bin = 0; bin < averaged.size(); ++bin)
                        averaged[bin] += std::norm(ear_spectrum[bin]);
                    ++sampled;
                }
            }
        }
    }
    CHECK(sampled > 0);
    if (sampled == 0) return;
    const auto measured_db = [&](float frequency_hz) {
        const std::size_t bin = static_cast<std::size_t>(std::lround(frequency_hz / kBinHz));
        return static_cast<float>(
            10.0 * std::log10(std::max(averaged[bin] / static_cast<double>(sampled), 1e-20)));
    };
    std::cout << "  SADIE D2 direction-averaged magnitude:";
    for (const float frequency : {200.0F, 1'000.0F, 3'000.0F, 6'000.0F, 12'000.0F})
        std::cout << ' ' << frequency / 1'000.0F << "k:" << measured_db(frequency) << "dB";
    std::cout << " (" << sampled << " responses)\n";

    std::cout << "  SADIE D2 neutralization:";
    for (const float frequency : {200.0F, 500.0F, 1'000.0F, 2'000.0F, 3'000.0F, 4'000.0F, 6'000.0F,
                                  8'000.0F, 12'000.0F}) {
        std::cout << ' ' << frequency / 1'000.0F << "k:" << gain_db(frequency) << "dB";
    }
    std::cout << '\n';

    // Measured, not assumed. The shipped SADIE II sets are already diffuse-field
    // equalized, so their direction-averaged response is flat within a couple of
    // decibels and there is no midrange resonance left to remove.
    for (const float frequency : {200.0F, 1'000.0F, 2'000.0F, 3'000.0F, 4'000.0F, 6'000.0F, 8'000.0F,
                                  12'000.0F}) {
        CHECK(std::abs(measured_db(frequency) - measured_db(1'000.0F)) < 3.0F);
    }
    // What is not neutral is the absolute level: about 10 dB hot once libmysofa
    // has normalized on whichever measurement minimizes azimuth+elevation. Left
    // alone it pins the true-peak limiter. The correction must therefore be an
    // essentially flat attenuation that matches the measured excess ...
    const float reference = gain_db(1'000.0F);
    CHECK_NEAR(reference, -measured_db(1'000.0F), 2.0F);
    CHECK(reference < -6.0F);
    // ... and it must reach the bass, which the spectral shaping never touches.
    for (const float frequency : {60.0F, 200.0F, 500.0F, 2'000.0F, 3'000.0F, 4'000.0F, 6'000.0F,
                                  8'000.0F, 12'000.0F}) {
        CHECK(std::abs(gain_db(frequency) - reference) < 3.0F);
    }
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

void test_five_source_two_ear_convolver_channel_isolation() {
    HrirFilterBank bank{};
    bank.tap_count = 1;
    bank.source_count = kDirectionalSourceCount;
    for (std::size_t source = 0; source < kDirectionalSourceCount; ++source) {
        // Unique gains and opposite signs make both source-index and ear-index
        // swaps visible without relying on energy-only comparisons.
        bank.path(source, 0)[0] = 0.125F * static_cast<float>(source + 1U);
        bank.path(source, 1)[0] = -0.0625F * static_cast<float>(source + 1U);
    }

    BinauralConvolver convolver;
    CHECK(convolver.set_filters(bank, 0));
    for (std::size_t source = 0; source < kDirectionalSourceCount; ++source) {
        convolver.reset();
        DirectionalFrame impulse{};
        impulse.sources[source] = 1.0F;
        StereoFrame output{};
        convolver.process(&impulse, &output, 1);
        CHECK_NEAR(output.left, bank.path(source, 0)[0], 1.0e-6F);
        CHECK_NEAR(output.right, bank.path(source, 1)[0], 1.0e-6F);

        DirectionalFrame silence{};
        StereoFrame tail{1.0F, 1.0F};
        convolver.process(&silence, &tail, 1);
        CHECK_NEAR(tail.left, 0.0F, 1.0e-6F);
        CHECK_NEAR(tail.right, 0.0F, 1.0e-6F);
    }
}

void test_sixteen_source_two_ear_convolver_capacity_and_isolation() {
    static_assert(kDirectionalSourceCount == 5);
    static_assert(kMaximumWindowBinauralSources == 16);
    static_assert(kMaximumBinauralSources == 18);
    static_assert(std::tuple_size_v<decltype(DirectionalFrame::sources)> == kMaximumBinauralSources);

    HrirFilterBank bank{};
    bank.tap_count = 1;
    bank.source_count = kMaximumBinauralSources;
    for (std::size_t source = 0; source < kMaximumBinauralSources; ++source) {
        bank.path(source, 0)[0] = 0.03125F * static_cast<float>(source + 1U);
        bank.path(source, 1)[0] = -0.015625F * static_cast<float>(source + 1U);
    }

    BinauralConvolver convolver;
    CHECK(convolver.set_filters(bank, 0));
    for (std::size_t source = 0; source < kMaximumBinauralSources; ++source) {
        convolver.reset();
        DirectionalFrame impulse{};
        impulse.sources[source] = 1.0F;
        StereoFrame output{};
        convolver.process(&impulse, &output, 1);
        CHECK_NEAR(output.left, bank.path(source, 0)[0], 1.0e-6F);
        CHECK_NEAR(output.right, bank.path(source, 1)[0], 1.0e-6F);
    }

    // Exercise the partitioned tail for the final source, not only its direct
    // history slot.
    HrirFilterBank tail_bank{};
    tail_bank.tap_count = kTimeDomainHrirTaps + 1;
    tail_bank.source_count = kMaximumBinauralSources;
    tail_bank.path(kMaximumBinauralSources - 1, 0)[kTimeDomainHrirTaps] = 0.75F;
    tail_bank.path(kMaximumBinauralSources - 1, 1)[kTimeDomainHrirTaps] = -0.5F;
    CHECK(convolver.set_filters(tail_bank, 0));
    convolver.reset();
    std::array<DirectionalFrame, kTimeDomainHrirTaps + 2> input{};
    std::array<StereoFrame, kTimeDomainHrirTaps + 2> output{};
    input[0].sources[kMaximumBinauralSources - 1] = 1.0F;
    convolver.process(input.data(), output.data(), output.size());
    CHECK_NEAR(output[kTimeDomainHrirTaps].left, 0.75F, 2.0e-4F);
    CHECK_NEAR(output[kTimeDomainHrirTaps].right, -0.5F, 2.0e-4F);

    HrirFilterBank too_many{};
    too_many.source_count = kMaximumBinauralSources + 1;
    CHECK(!convolver.set_filters(too_many, 0));

    HrirFilterBank undersized{};
    undersized.coefficients.pop_back();
    CHECK(!convolver.set_filters(undersized, 0));
}

void test_sixteen_source_filter_builder_and_worker_capacity() {
    class DirectionEncodingHrtf final : public IHrtfDatabase {
    public:
        [[nodiscard]] std::uint32_t sample_rate() const noexcept override { return kSampleRate; }
        [[nodiscard]] std::size_t maximum_taps() const noexcept override { return 1; }
        bool query(const Vec3f& direction, std::span<float> left, std::span<float> right,
                   std::size_t& tap_count) const noexcept override {
            query_count.fetch_add(1, std::memory_order_relaxed);
            if (length(direction) < 0.5F)
                return false;
            std::fill(left.begin(), left.end(), 0.0F);
            std::fill(right.begin(), right.end(), 0.0F);
            left[0] = direction.x;
            right[0] = -direction.x;
            tap_count = 1;
            return true;
        }

        mutable std::atomic<std::uint32_t> query_count{};
    } hrtf;

    std::array<Vec3f, kMaximumBinauralSources> directions{};
    std::array<float, kMaximumBinauralSources> gains{};
    for (std::size_t source = 0; source < kMaximumBinauralSources; ++source) {
        directions[source] = {static_cast<float>(source + 1U), 0.0F, 1.0F};
        gains[source] = 1.0F / static_cast<float>(source + 1U);
    }

    HrirFilterBank built{};
    CHECK(build_binaural_filter_bank(hrtf, directions, gains, kMaximumBinauralSources, built));
    CHECK(built.source_count == kMaximumBinauralSources);
    CHECK(built.tap_count == 1);
    for (std::size_t source = 0; source < kMaximumBinauralSources; ++source) {
        CHECK_NEAR(built.path(source, 0)[0], 1.0F, 1.0e-6F);
        CHECK_NEAR(built.path(source, 1)[0], -1.0F, 1.0e-6F);
    }
    CHECK(!build_binaural_filter_bank(hrtf, directions, gains, 0, built));
    CHECK(!build_binaural_filter_bank(hrtf, directions, gains, kMaximumBinauralSources + 1, built));

    HrirFilterBank undersized{};
    undersized.coefficients.pop_back();
    CHECK(!build_binaural_filter_bank(hrtf, directions, gains, 2, undersized));

    // Stable application slots are intentionally sparse. Muted gaps must not
    // query a provider with their meaningless zero direction or invalidate the
    // active higher-index pair.
    std::array<Vec3f, kMaximumBinauralSources> sparse_directions{};
    std::array<float, kMaximumBinauralSources> sparse_gains{};
    sparse_directions[2] = {1.0F, 0.0F, 1.0F};
    sparse_directions[3] = {2.0F, 0.0F, 1.0F};
    sparse_gains[2] = 0.5F;
    sparse_gains[3] = 0.25F;
    hrtf.query_count.store(0, std::memory_order_relaxed);
    CHECK(build_binaural_filter_bank(hrtf, sparse_directions, sparse_gains, 4,
                                     built));
    CHECK(hrtf.query_count.load(std::memory_order_relaxed) == 2U);
    CHECK_NEAR(built.path(0, 0)[0], 0.0F, 1.0e-7F);
    CHECK_NEAR(built.path(1, 1)[0], 0.0F, 1.0e-7F);
    CHECK_NEAR(built.path(2, 0)[0], 0.5F, 1.0e-7F);
    CHECK_NEAR(built.path(3, 0)[0], 0.5F, 1.0e-7F);

    HrtfPreparationWorker worker;
    HrtfPreparationRequest request{};
    request.generation = 1;
    request.scene_revision = 17;
    request.database = &hrtf;
    request.head_relative_directions = directions;
    request.speaker_gains = gains;
    request.source_count = kMaximumBinauralSources;
    worker.submit_latest(request);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool received = false;
    while (!received && std::chrono::steady_clock::now() < deadline) {
        const PreparedDirectHrtfUpdate* direct = nullptr;
        std::uint8_t token = 0;
        if (!worker.try_acquire_latest_direct(direct, token)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        received = direct->valid && direct->generation == 1 && direct->scene_revision == 17 &&
                   direct->filters.source_count == kMaximumBinauralSources;
        CHECK_NEAR(direct->filters.path(kMaximumBinauralSources - 1, 0)[0], 1.0F, 1.0e-6F);
        CHECK_NEAR(direct->filters.path(kMaximumBinauralSources - 1, 1)[0], -1.0F, 1.0e-6F);
        worker.release_direct(token);
    }
    CHECK(received);
}

// Measurement harness for the timbre of a virtual emitter pair. Everything a
// listener calls "tin can" shows up here as a deviation of the correlated
// response from its own third-octave mean.
struct EmitterPairMeasurement {
    static constexpr std::size_t kFftSize = 2'048;
    static constexpr std::size_t kBins = kFftSize / 2 + 1;
    static constexpr float kBinHz = static_cast<float>(kSampleRate) / static_cast<float>(kFftSize);
    static constexpr float kLowHz = 500.0F;
    static constexpr float kHighHz = 10'000.0F;

    EmitterPairMeasurement() : twiddles(kFftSize / 2) { build_twiddle_table(twiddles); }

    [[nodiscard]] std::vector<float> magnitude(const HrirFilterBank& bank, std::size_t source,
                                               std::size_t ear) const {
        std::vector<std::complex<float>> spectrum(kFftSize);
        const auto& path = bank.path(source, ear);
        for (std::size_t tap = 0; tap < bank.tap_count && tap < kFftSize; ++tap)
            spectrum[tap] = {path[tap], 0.0F};
        radix2_transform(spectrum, twiddles, false);
        std::vector<float> result(kBins);
        for (std::size_t bin = 0; bin < kBins; ++bin) result[bin] = std::abs(spectrum[bin]);
        return result;
    }

    // Magnitude seen by one ear when both emitters carry the same signal.
    [[nodiscard]] std::vector<float> correlated_magnitude(const HrirFilterBank& bank,
                                                          std::size_t ear) const {
        std::vector<std::complex<float>> spectrum(kFftSize);
        const auto& left = bank.path(0, ear);
        const auto& right = bank.path(1, ear);
        for (std::size_t tap = 0; tap < bank.tap_count && tap < kFftSize; ++tap)
            spectrum[tap] = {left[tap] + right[tap], 0.0F};
        radix2_transform(spectrum, twiddles, false);
        std::vector<float> result(kBins);
        for (std::size_t bin = 0; bin < kBins; ++bin) result[bin] = std::abs(spectrum[bin]);
        return result;
    }

    // Worst departure from the local third-octave mean, in dB. This is the comb.
    [[nodiscard]] float ripple_db(const std::vector<float>& magnitudes) const {
        std::vector<float> smoothed(magnitudes.size());
        smooth_magnitude_fractional_octave(magnitudes, smoothed, 3.0F);
        float worst = 0.0F;
        for (std::size_t bin = first_bin(); bin <= last_bin(); ++bin) {
            const float reference = std::max(smoothed[bin], 1.0e-9F);
            const float measured = std::max(magnitudes[bin], 1.0e-9F);
            worst = std::max(worst, std::abs(20.0F * std::log10(measured / reference)));
        }
        return worst;
    }

    [[nodiscard]] static float worst_difference_db(const std::vector<float>& first,
                                                   const std::vector<float>& second) {
        float worst = 0.0F;
        for (std::size_t bin = first_bin(); bin <= last_bin(); ++bin) {
            const float a = std::max(first[bin], 1.0e-9F);
            const float b = std::max(second[bin], 1.0e-9F);
            worst = std::max(worst, std::abs(20.0F * std::log10(a / b)));
        }
        return worst;
    }

    [[nodiscard]] static std::size_t first_bin() {
        return static_cast<std::size_t>(std::ceil(kLowHz / kBinHz));
    }
    [[nodiscard]] static std::size_t last_bin() {
        return static_cast<std::size_t>(std::floor(kHighHz / kBinHz));
    }

    [[nodiscard]] static bool build_pair(const IHrtfDatabase& database, float half_angle_degrees,
                                         bool compensate, PhantomCentreCompensator& compensator,
                                         HrirFilterBank& bank) {
        const float radians = half_angle_degrees * kPi / 180.0F;
        std::array<Vec3f, kMaximumBinauralSources> directions{};
        std::array<float, kMaximumBinauralSources> gains{};
        directions[0] = {-std::sin(radians), 0.0F, std::cos(radians)};
        directions[1] = {std::sin(radians), 0.0F, std::cos(radians)};
        gains[0] = 1.0F;
        gains[1] = 1.0F;
        return build_binaural_filter_bank(database, directions, gains, 2, bank,
                                          compensate ? 0x1U : 0x0U, &compensator);
    }

    std::vector<std::complex<float>> twiddles;
};

void test_phantom_centre_compensation_flattens_the_inter_emitter_comb() {
    const EmitterPairMeasurement measurement{};
    const AnalyticHrtfDatabase hrtf{kSampleRate};

    // Endpoint loudspeakers sit at +-30 degrees; a window emitter pair sweeps
    // the whole range below as the window is moved and resized.
    for (const float half_angle : {30.0F, 22.0F, 15.0F, 8.0F}) {
        PhantomCentreCompensator plain_compensator;
        PhantomCentreCompensator compensator;
        HrirFilterBank plain{};
        HrirFilterBank corrected{};
        CHECK(EmitterPairMeasurement::build_pair(hrtf, half_angle, false, plain_compensator, plain));
        CHECK(EmitterPairMeasurement::build_pair(hrtf, half_angle, true, compensator, corrected));

        // The correction must stay on the zero-latency direct path.
        CHECK(corrected.tap_count <= kTimeDomainHrirTaps);

        float plain_ripple = 0.0F;
        float corrected_ripple = 0.0F;
        float panned_shift = 0.0F;
        for (std::size_t ear = 0; ear < 2; ++ear) {
            plain_ripple = std::max(plain_ripple,
                                    measurement.ripple_db(measurement.correlated_magnitude(plain, ear)));
            corrected_ripple =
                std::max(corrected_ripple,
                         measurement.ripple_db(measurement.correlated_magnitude(corrected, ear)));
            panned_shift = std::max(
                panned_shift, EmitterPairMeasurement::worst_difference_db(
                                  measurement.magnitude(corrected, 0, ear), measurement.magnitude(plain, 0, ear)));
        }
        std::cout << "  emitter pair +-" << half_angle << " deg: correlated ripple " << plain_ripple
                  << " dB -> " << corrected_ripple << " dB, hard-panned shift " << panned_shift
                  << " dB, taps " << corrected.tap_count << '\n';

        // The uncompensated pair must actually exhibit the defect, otherwise the
        // harness is measuring nothing.
        CHECK(plain_ripple > 7.0F);
        CHECK(corrected_ripple < 3.0F);
        CHECK(plain_ripple - corrected_ripple > 4.0F);
        // A hard-panned source has a mid component, so it cannot be perfectly
        // untouched. Bound the collateral instead of pretending it is zero.
        CHECK(panned_shift < 6.0F);
    }

    // A muted or absent partner has nothing to interfere with: the compensator
    // must leave that pair exactly as the database returned it.
    PhantomCentreCompensator compensator;
    std::array<Vec3f, kMaximumBinauralSources> directions{};
    std::array<float, kMaximumBinauralSources> gains{};
    directions[0] = {-0.5F, 0.0F, 1.0F};
    directions[1] = {0.5F, 0.0F, 1.0F};
    gains[0] = 1.0F;
    gains[1] = 0.0F;
    HrirFilterBank solo{};
    HrirFilterBank solo_reference{};
    CHECK(build_binaural_filter_bank(hrtf, directions, gains, 2, solo, 0x1U, &compensator));
    CHECK(build_binaural_filter_bank(hrtf, directions, gains, 2, solo_reference));
    CHECK(solo.tap_count == solo_reference.tap_count);
    for (std::size_t tap = 0; tap < solo.tap_count; ++tap)
        CHECK_NEAR(solo.path(0, 0)[tap], solo_reference.path(0, 0)[tap], 1.0e-6F);

    // Repeating the same geometry must reuse the cached correction and produce
    // the identical bank; the worker relies on that to survive head tracking.
    HrirFilterBank first{};
    HrirFilterBank second{};
    PhantomCentreCompensator cached;
    CHECK(EmitterPairMeasurement::build_pair(hrtf, 22.0F, true, cached, first));
    CHECK(EmitterPairMeasurement::build_pair(hrtf, 22.0F, true, cached, second));
    for (std::size_t tap = 0; tap < first.tap_count; ++tap)
        CHECK_NEAR(first.path(0, 0)[tap], second.path(0, 0)[tap], 1.0e-7F);
}

void test_measured_sofa_render_level_stays_off_the_limiter() {
#if defined(SOUND_SPATIALIZER_HAS_MYSOFA)
    const std::filesystem::path sofa_path = std::filesystem::path(SOUND_SPATIALIZER_TEST_REPOSITORY_ROOT) /
                                            "resources" / "hrtf" / "data" / "sadie-d2-kemar.sofa";
    if (!std::filesystem::exists(sofa_path)) {
        std::cout << "SADIE D2 fixture not fetched; render level test skipped\n";
        return;
    }
    const HrtfLoadResult loaded = load_sofa_hrtf(sofa_path.string(), kSampleRate);
    CHECK(loaded.database != nullptr);
    if (!loaded.database) return;

    constexpr std::size_t kFft = 2'048;
    constexpr float kBinHz = static_cast<float>(kSampleRate) / static_cast<float>(kFft);
    std::vector<std::complex<float>> twiddles(kFft / 2);
    build_twiddle_table(twiddles);
    const auto transform = [&](std::span<const float> taps, std::size_t count) {
        std::vector<std::complex<float>> spectrum(kFft);
        for (std::size_t tap = 0; tap < count && tap < kFft; ++tap) spectrum[tap] = {taps[tap], 0.0F};
        radix2_transform(spectrum, twiddles, false);
        return spectrum;
    };

    PhantomCentreCompensator compensator;
    HrirFilterBank bank{};
    CHECK(EmitterPairMeasurement::build_pair(*loaded.database, 30.0F, true, compensator, bank));

    const std::span<const float> neutralization = loaded.database->diffuse_field_filter();
    const std::vector<std::complex<float>> equalizer = transform(neutralization, neutralization.size());

    // Broadband gain applied to content that is correlated between the two
    // emitters, which is the loudest case the limiter ever sees.
    const auto correlated_gain_db = [&](bool with_equalizer) {
        double power = 0.0;
        std::size_t counted = 0;
        for (std::size_t ear = 0; ear < 2; ++ear) {
            const std::vector<std::complex<float>> left = transform(bank.path(0, ear), bank.tap_count);
            const std::vector<std::complex<float>> right = transform(bank.path(1, ear), bank.tap_count);
            for (std::size_t bin = 0; bin < kFft / 2 + 1; ++bin) {
                const float frequency = static_cast<float>(bin) * kBinHz;
                if (frequency < 200.0F || frequency > 10'000.0F) continue;
                std::complex<float> summed = left[bin] + right[bin];
                if (with_equalizer) summed *= equalizer[bin];
                power += std::norm(summed);
                ++counted;
            }
        }
        return 10.0F * std::log10(std::max(power / std::max<std::size_t>(counted, 1), 1e-20));
    };

    const float raw_gain = correlated_gain_db(false);
    const float neutral_gain = correlated_gain_db(true);
    std::cout << "  SADIE D2 correlated render gain: " << raw_gain << " dB raw -> " << neutral_gain
              << " dB neutralized\n";

    // The provider on its own is far hotter than unity. With a -6 dB master gain
    // and a -0.1 dBFS ceiling, that alone puts the zero-lookahead limiter into
    // permanent deep reduction on ordinary programme material.
    CHECK(raw_gain > 12.0F);
    // Neutralized, only the coherent summation of the two emitters is left,
    // which is the same few decibels a real loudspeaker pair produces.
    CHECK(neutral_gain > -3.0F);
    CHECK(neutral_gain < 8.0F);
#endif
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

void test_lfe_symmetric_low_pass_and_downmix() {
    LfeRenderer impulse_renderer;
    impulse_renderer.prepare(static_cast<float>(kSampleRate));
    impulse_renderer.configure(true, 0.0F);
    double impulse_energy = 0.0;
    for (std::size_t frame = 0; frame < 2'048; ++frame) {
        const float filtered = impulse_renderer.process_sample(frame == 0 ? 1.0F : 0.0F);
        const StereoFrame downmixed = downmix_programme_to_stereo({}, filtered);
        CHECK_NEAR(downmixed.left, downmixed.right, 1.0e-7F);
        CHECK_NEAR(downmixed.left, filtered * 0.70710678F, 1.0e-7F);
        impulse_energy += static_cast<double>(filtered) * filtered;
    }
    CHECK(impulse_energy > 0.0);

    const auto steady_state_rms = [](float frequency_hz) {
        LfeRenderer renderer;
        renderer.prepare(static_cast<float>(kSampleRate));
        renderer.configure(true, 0.0F);
        constexpr std::size_t total_frames = kSampleRate;
        constexpr std::size_t measured_frames = kSampleRate / 2U;
        double energy = 0.0;
        for (std::size_t frame = 0; frame < total_frames; ++frame) {
            const float phase = 2.0F * kPi * frequency_hz * static_cast<float>(frame) /
                                static_cast<float>(kSampleRate);
            const float filtered = renderer.process_sample(std::sin(phase));
            if (frame >= total_frames - measured_frames)
                energy += static_cast<double>(filtered) * filtered;
        }
        return std::sqrt(energy / static_cast<double>(measured_frames));
    };
    const double bass_rms = steady_state_rms(60.0F);
    const double out_of_band_rms = steady_state_rms(1'000.0F);
    CHECK(bass_rms > 0.5);
    CHECK(out_of_band_rms < bass_rms * 0.01);

    LfeRenderer disabled;
    disabled.prepare(static_cast<float>(kSampleRate));
    disabled.configure(false, 0.0F);
    CHECK_NEAR(disabled.process_sample(1.0F), 0.0F, 1.0e-7F);
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

void test_programme_resampler_channel_isolation() {
    constexpr std::size_t source_frames = 512;
    constexpr std::size_t rendered_frames = 256;
    for (std::size_t active_channel = 0; active_channel < kProgrammeChannelCount; ++active_channel) {
        std::array<ProgrammeFrame, source_frames> source{};
        for (std::size_t frame = 0; frame < source.size(); ++frame) {
            const float phase = 2.0F * kPi * 1'000.0F * static_cast<float>(frame) /
                                static_cast<float>(kSampleRate);
            source[frame][active_channel] = std::sin(phase);
        }

        AsyncProgrammeResampler resampler(1'024);
        CHECK(resampler.push(source.data(), source.size()) == source.size());
        // Priming consumes nine future samples. Matching that post-prime fill
        // keeps the unity-ratio path free from drift-controller correction.
        resampler.set_target_fill(source.size() - 9U);
        std::array<ProgrammeFrame, rendered_frames> output{};
        CHECK(resampler.render(output.data(), output.size(), static_cast<float>(kSampleRate)) ==
              output.size());

        double peak_signal_error = 0.0;
        double peak_crosstalk = 0.0;
        double signal_energy = 0.0;
        for (std::size_t frame = 16; frame < output.size() - 16U; ++frame) {
            peak_signal_error = std::max(
                peak_signal_error,
                std::abs(static_cast<double>(output[frame][active_channel] -
                                             source[frame][active_channel])));
            signal_energy += static_cast<double>(output[frame][active_channel]) *
                             output[frame][active_channel];
            for (std::size_t channel = 0; channel < kProgrammeChannelCount; ++channel) {
                if (channel == active_channel) continue;
                peak_crosstalk = std::max(
                    peak_crosstalk,
                    std::abs(static_cast<double>(output[frame][channel])));
            }
        }
        CHECK(signal_energy > 1.0);
        CHECK(peak_signal_error < 1.0e-4);
        CHECK(peak_crosstalk < 1.0e-7);
    }
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

void test_process_resampler_rebases_discontinuities_and_excess_backlog() {
    AsyncStereoResampler discontinuity(128);
    discontinuity.set_target_fill(24);
    discontinuity.set_rebase_threshold(64);
    std::array<StereoFrame, 32> old_audio{};
    old_audio.fill({1.0F, 1.0F});
    std::array<StereoFrame, 32> new_audio{};
    new_audio.fill({2.0F, 2.0F});
    CHECK(discontinuity.push(old_audio.data(), old_audio.size()) ==
          old_audio.size());
    discontinuity.request_rebase();
    CHECK(discontinuity.push(new_audio.data(), new_audio.size()) ==
          new_audio.size());
    std::array<StereoFrame, 8> output{};
    CHECK(discontinuity.render(output.data(), output.size(), 48'000.0F) ==
          output.size());
    CHECK(discontinuity.rebases() == 1U);
    CHECK_NEAR(output.front().left, 2.0F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 2.0F, 1.0e-5F);

    AsyncStereoResampler backlog(128);
    backlog.set_target_fill(24);
    backlog.set_rebase_threshold(32);
    std::array<StereoFrame, 64> queued{};
    std::fill_n(queued.begin(), 40, StereoFrame{1.0F, 1.0F});
    std::fill(queued.begin() + 40, queued.end(), StereoFrame{3.0F, 3.0F});
    CHECK(backlog.push(queued.data(), queued.size()) == queued.size());
    CHECK(backlog.render(output.data(), output.size(), 48'000.0F) ==
          output.size());
    CHECK(backlog.rebases() == 1U);
    CHECK(backlog.fill_frames() < 24U);
    CHECK_NEAR(output.front().left, 3.0F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 3.0F, 1.0e-5F);
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

void test_scene_v1_migration_and_v2_roundtrip() {
    constexpr std::string_view legacy_scene = R"json({
        "schemaVersion": 1,
        "audio": {
            "outputDeviceId": "legacy-headphones",
            "mode": "shared-low-latency",
            "sampleRate": 48000,
            "bufferFrames": 128,
            "bypass": false,
            "masterGainDb": -6,
            "roomMix": 0.18
        },
        "tracking": {
            "enabled": true,
            "cameraDeviceId": null,
            "minimumFps": 60,
            "predictionLimitMs": 20
        },
        "listener": {
            "positionM": [0, 1.2, 0],
            "neutralOrientation": [1, 0, 0, 0]
        },
        "speakers": [
            {"channel": "left", "positionM": [-1.25, 1.2, 1.5], "gainDb": -60},
            {"channel": "right", "positionM": [1.25, 1.2, 1.5], "gainDb": -3}
        ],
        "hrtf": {"profileId": "builtin-analytic-neutral", "sofaPath": null},
        "headphoneEq": {"enabled": false, "preampDb": -6, "filters": []},
        "room": {
            "enabled": false,
            "dimensionsM": [5, 2.7, 4],
            "surfaces": [
                {"materialId": "wall-left", "absorption": [0.1, 0.2, 0.3], "diffusion": [0.1, 0.1, 0.1]},
                {"materialId": "wall-right", "absorption": [0.1, 0.2, 0.3], "diffusion": [0.1, 0.1, 0.1]},
                {"materialId": "wall-front", "absorption": [0.1, 0.2, 0.3], "diffusion": [0.1, 0.1, 0.1]},
                {"materialId": "wall-back", "absorption": [0.1, 0.2, 0.3], "diffusion": [0.1, 0.1, 0.1]},
                {"materialId": "floor", "absorption": [0.1, 0.2, 0.3], "diffusion": [0.1, 0.1, 0.1]},
                {"materialId": "ceiling", "absorption": [0.1, 0.2, 0.3], "diffusion": [0.1, 0.1, 0.1]}
            ],
            "earlyReflectionOrder": 2,
            "earlyReflectionLimitMs": 80,
            "lateReverbEnabled": true
        }
    })json";

    const auto migrated = scene_config_from_json(legacy_scene);
    CHECK(migrated);
    if (migrated) {
        CHECK(migrated.value->schema_version == 2U);
        CHECK(migrated.value->audio.capture_provider == CaptureProvider::native_driver);
        CHECK(!migrated.value->audio.capture_endpoint_id);
        CHECK(migrated.value->audio.input_layout == InputLayout::stereo);
        CHECK(migrated.value->audio.output_device_id ==
              std::optional<std::string>("legacy-headphones"));
        CHECK(migrated.value->speakers[0].channel == SpeakerChannel::front_left);
        CHECK(migrated.value->speakers[1].channel == SpeakerChannel::front_right);
        CHECK(!migrated.value->speakers[0].enabled);
        CHECK(migrated.value->speakers[1].enabled);
        CHECK_NEAR(migrated.value->speakers[0].position_m.x, -1.25F, 1.0e-6F);
        CHECK_NEAR(migrated.value->speakers[1].gain_db, -3.0F, 1.0e-6F);
        CHECK(migrated.value->speakers[2].channel == SpeakerChannel::front_center);
        CHECK(migrated.value->speakers[3].channel == SpeakerChannel::surround_left);
        CHECK(migrated.value->speakers[4].channel == SpeakerChannel::surround_right);

        const std::string migrated_json = scene_config_to_json(*migrated.value);
        CHECK(migrated_json.find("\"schemaVersion\":2") != std::string::npos);
        CHECK(migrated_json.find("\"inputLayout\":\"stereo\"") != std::string::npos);
        CHECK(migrated_json.find("\"front-center\"") != std::string::npos);
        CHECK(migrated_json.find("\"lfe\":") != std::string::npos);
        // A scene written before the phantom-centre correction existed carries
        // no such field and must adopt the default rather than be rejected.
        CHECK(migrated.value->hrtf.phantom_centre_compensation);
        const auto reparsed = scene_config_from_json(migrated_json);
        CHECK(reparsed);
        if (reparsed) {
            CHECK(reparsed.value->schema_version == 2U);
            CHECK(reparsed.value->audio.input_layout == InputLayout::stereo);
            CHECK(!reparsed.value->speakers[0].enabled);
            CHECK(reparsed.value->speakers[4].channel == SpeakerChannel::surround_right);
            CHECK(reparsed.value->hrtf.phantom_centre_compensation);
        }
    }

    SceneConfigV2 surround{};
    surround.audio.capture_provider = CaptureProvider::external_render;
    surround.audio.capture_endpoint_id = "vb-cable-5.1";
    surround.audio.output_device_id = "usb-headphones";
    surround.audio.input_layout = InputLayout::surround_5_1;
    surround.audio.master_gain_db = -9.0F;
    surround.lfe.enabled = false;
    surround.lfe.gain_db = -4.5F;
    surround.speakers[2].enabled = false;
    surround.speakers[3].gain_db = -2.0F;
    surround.speakers[4].position_m.z = -1.25F;
    surround.hrtf.phantom_centre_compensation = false;
    std::string validation_error;
    CHECK(validate_scene_config(surround, validation_error));
    const std::string v2_json = scene_config_to_json(surround);
    const auto v2_roundtrip = scene_config_from_json(v2_json);
    CHECK(v2_roundtrip);
    if (v2_roundtrip) {
        CHECK(v2_roundtrip.value->schema_version == 2U);
        CHECK(v2_roundtrip.value->audio.capture_provider == CaptureProvider::external_render);
        CHECK(v2_roundtrip.value->audio.capture_endpoint_id ==
              std::optional<std::string>("vb-cable-5.1"));
        CHECK(v2_roundtrip.value->audio.output_device_id ==
              std::optional<std::string>("usb-headphones"));
        CHECK(v2_roundtrip.value->audio.input_layout == InputLayout::surround_5_1);
        CHECK_NEAR(v2_roundtrip.value->audio.master_gain_db, -9.0F, 1.0e-6F);
        CHECK(!v2_roundtrip.value->lfe.enabled);
        CHECK_NEAR(v2_roundtrip.value->lfe.gain_db, -4.5F, 1.0e-6F);
        CHECK(!v2_roundtrip.value->hrtf.phantom_centre_compensation);
        for (std::size_t source = 0; source < kDirectionalSourceCount; ++source) {
            CHECK(v2_roundtrip.value->speakers[source].channel == surround.speakers[source].channel);
            CHECK(v2_roundtrip.value->speakers[source].enabled == surround.speakers[source].enabled);
            CHECK_NEAR(v2_roundtrip.value->speakers[source].gain_db,
                       surround.speakers[source].gain_db, 1.0e-6F);
            CHECK_NEAR(v2_roundtrip.value->speakers[source].position_m.x,
                       surround.speakers[source].position_m.x, 1.0e-6F);
            CHECK_NEAR(v2_roundtrip.value->speakers[source].position_m.y,
                       surround.speakers[source].position_m.y, 1.0e-6F);
            CHECK_NEAR(v2_roundtrip.value->speakers[source].position_m.z,
                       surround.speakers[source].position_m.z, 1.0e-6F);
        }
    }
}

void test_json_and_ui_commands() {
    SceneConfigV2 scene{};
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

    SceneConfigV2 external_scene = scene;
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

    SceneConfigV2 boundary = scene;
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

void test_window_spatialization_contract_status_and_persistence() {
    CHECK(!window_audio_coverage_is_complete(false, 0, 0));
    CHECK(window_audio_coverage_is_complete(true, 0, 0));
    CHECK(!window_audio_coverage_is_complete(true, 1, 0));
    CHECK(window_audio_coverage_is_complete(true, 1, 1));
    CHECK(!window_audio_coverage_is_complete(true, 1, 2));
    CHECK(!WindowAudioRealtimeSnapshot{}.coverage_complete);
    CHECK(!window_audio_inactive_pcm_requires_endpoint_fallback(7, 7));
    CHECK(window_audio_inactive_pcm_requires_endpoint_fallback(8, 7));
    CHECK(!window_audio_excluded_session_blocks_coverage(
        42, 42, true));
    CHECK(window_audio_excluded_session_blocks_coverage(
        41, 42, true));
    CHECK(!window_audio_excluded_session_blocks_coverage(
        41, 42, false));
    CHECK(!window_audio_slot_is_renderable(
        true, WindowAudioCaptureState::capturing, false, true));
    CHECK(!window_audio_slot_is_renderable(
        true, WindowAudioCaptureState::activating, true, true));
    CHECK(!window_audio_slot_is_renderable(
        true, WindowAudioCaptureState::capturing, true, false));
    CHECK(window_audio_slot_is_renderable(
        true, WindowAudioCaptureState::capturing, true, true));

    WindowAudioConfig config{};
    WindowAudioSourceRule default_rule{};
    CHECK_NEAR(config.stereo_spread, 0.72F, 1.0e-7F);
    CHECK(config.placement_mode ==
          WindowAudioPlacementMode::proportional);
    CHECK(config.refresh_interval_ms == 10U);
    CHECK_NEAR(default_rule.stereo_spread, 0.72F, 1.0e-7F);
    config.enabled = true;
    config.max_applications = 3;
    config.stereo_spread = 0.75F;
    config.placement_mode = WindowAudioPlacementMode::window_edges;
    config.follow_window_position = false;
    config.display_calibration_count = 1;
    config.display_calibrations[0].display_id = R"(\\?\DISPLAY#RIGHT)";
    config.display_calibrations[0].center_m = {0.8F, 1.25F, 0.9F};
    config.display_calibrations[0].width_m = 0.62F;
    config.display_calibrations[0].height_m = 0.35F;
    config.source_rule_count = 1;
    config.source_rules[0].application_id = R"(C:\Apps\Player.exe)";
    config.source_rules[0].gain_db = -2.5F;
    config.source_rules[0].stereo_spread = 0.4F;
    config.source_rules[0].fallback_display_id =
        config.display_calibrations[0].display_id;

    std::string error;
    CHECK(validate_window_audio_config(config, error));
    WindowAudioConfig invalid_refresh = config;
    invalid_refresh.refresh_interval_ms = 9;
    CHECK(!validate_window_audio_config(invalid_refresh, error));
    WindowAudioConfig case_duplicate = config;
    case_duplicate.source_rule_count = 2;
    case_duplicate.source_rules[1] = case_duplicate.source_rules[0];
    case_duplicate.source_rules[1].application_id = R"(C:\APPS\PLAYER.EXE)";
    CHECK(!validate_window_audio_config(case_duplicate, error));
    const std::string json = window_audio_config_to_json(config);
    CHECK(json.find("\"schemaVersion\":1") != std::string::npos);
    CHECK(json.find("\"maxSources\":3") != std::string::npos);
    CHECK(json.find("\"emitterPlacementMode\":\"window-edges\"") !=
          std::string::npos);
    CHECK(json.find("\"followWindowPosition\":false") != std::string::npos);

    const auto parsed = window_audio_config_from_json(json);
    CHECK(parsed);
    if (parsed) {
        CHECK(parsed.value->enabled);
        CHECK(parsed.value->max_applications == 3U);
        CHECK_NEAR(parsed.value->stereo_spread, 0.75F, 1.0e-6F);
        CHECK(parsed.value->placement_mode ==
              WindowAudioPlacementMode::window_edges);
        CHECK(parsed.value->display_calibration_count == 1U);
        CHECK(parsed.value->display_calibrations[0].display_id ==
              config.display_calibrations[0].display_id);
        CHECK_NEAR(parsed.value->display_calibrations[0].center_m.x, 0.8F,
                   1.0e-6F);
        CHECK(parsed.value->source_rule_count == 1U);
        CHECK(parsed.value->source_rules[0].application_id ==
              config.source_rules[0].application_id);
        CHECK_NEAR(parsed.value->source_rules[0].gain_db, -2.5F, 1.0e-6F);
    }
    const auto legacy_placement = window_audio_config_from_json(
        R"({"schemaVersion":1,"enabled":true,"maxSources":8,"stereoSpread":0.72,"followWindowPosition":true,"displayCalibrations":[],"sourceRules":[]})");
    CHECK(legacy_placement);
    if (legacy_placement) {
        CHECK(legacy_placement.value->placement_mode ==
              WindowAudioPlacementMode::proportional);
    }
    CHECK(!window_audio_config_from_json(
        R"({"schemaVersion":1,"enabled":true,"maxSources":8,"stereoSpread":0.72,"emitterPlacementMode":"invalid","followWindowPosition":true,"displayCalibrations":[],"sourceRules":[]})"));

    const std::string command_json =
        R"({"schemaVersion":1,"type":"set-window-spatialization","config":)" +
        json + '}';
    auto command = engine_command_from_json(command_json);
    CHECK(command);
    if (command) {
        command.value->command_id = 77;
        CHECK(command.value->type ==
              EngineCommandType::set_window_spatialization);
        const auto roundtrip =
            engine_command_from_json(engine_command_to_json(*command.value));
        CHECK(roundtrip);
        if (roundtrip) CHECK(roundtrip.value->command_id == 77);
    }
    const std::string rejected_result =
        engine_command_result_to_json(77, false, false,
                                      "configuration refused");
    CHECK(rejected_result.find("\"kind\":\"command-result\"") !=
          std::string::npos);
    CHECK(rejected_result.find("\"commandId\":77") != std::string::npos);
    CHECK(rejected_result.find("\"accepted\":false") != std::string::npos);
    CHECK(rejected_result.find("\"persisted\":false") !=
          std::string::npos);
    const auto rejected_command_id = engine_command_id_from_json(
        R"({"schemaVersion":1,"commandId":77,"type":"set-window-spatialization","config":{"schemaVersion":1,"enabled":true,"maxSources":0}})");
    CHECK(rejected_command_id);
    if (rejected_command_id) CHECK(*rejected_command_id.value == 77U);
    const auto legacy_command_id = engine_command_id_from_json(
        R"({"schemaVersion":1,"type":"start"})");
    CHECK(legacy_command_id);
    if (legacy_command_id) CHECK(*legacy_command_id.value == 0U);
    CHECK(!window_audio_config_from_json(
        R"({"schemaVersion":1,"enabled":true,"maxSources":0,"stereoSpread":1,"followWindowPosition":true,"displayCalibrations":[],"sourceRules":[]})"));
    CHECK(!window_audio_config_from_json(
        R"({"schemaVersion":1,"enabled":false,"maxSources":1,"stereoSpread":1,"followWindowPosition":true,"displayCalibrations":[],"sourceRules":[],"unknown":1})"));

    WindowAudioSnapshot snapshot{};
    snapshot.sequence = 7;
    snapshot.display_count = 1;
    std::snprintf(snapshot.displays[0].id.data(),
                  snapshot.displays[0].id.size(), "%s", "display-right");
    std::snprintf(snapshot.displays[0].name.data(),
                  snapshot.displays[0].name.size(), "%s", "Right display");
    snapshot.displays[0].primary = true;
    snapshot.displays[0].bounds_px = {1'920, 0, 4'480, 1'440};
    snapshot.displays[0].center_m = {0.8F, 1.25F, 0.9F};
    snapshot.displays[0].width_m = 0.62F;
    snapshot.displays[0].height_m = 0.35F;
    snapshot.window_source_count = 1;
    auto& source = snapshot.window_sources[0];
    std::snprintf(source.source_id.data(), source.source_id.size(), "%s",
                  "window:0:1");
    std::snprintf(source.application_id.data(), source.application_id.size(),
                  "%s", R"(C:\Apps\Player.exe)");
    std::snprintf(source.application_name.data(),
                  source.application_name.size(), "%s", "Player");
    std::snprintf(source.window_title.data(), source.window_title.size(), "%s",
                  "Music");
    std::snprintf(source.display_id.data(), source.display_id.size(), "%s",
                  "display-right");
    source.process_id = 42;
    source.left_position_m = {0.6F, 1.25F, 0.9F};
    source.right_position_m = {1.0F, 1.25F, 0.9F};
    source.sample_rate = 48'000;
    source.channel_count = 2;
    source.capture_state = WindowAudioCaptureState::capturing;
    source.active = true;
    WindowAudioDiagnostics diagnostics{};
    diagnostics.supported = true;
    diagnostics.running = true;
    diagnostics.discovery_passes = 9;
    diagnostics.active_slots = 1;
    diagnostics.uncovered_active_sessions = 0;
    diagnostics.required_active_captures = 1;
    diagnostics.ready_active_captures = 1;
    diagnostics.coverage_complete = true;
    std::snprintf(diagnostics.coverage_detail.data(),
                  diagnostics.coverage_detail.size(), "%s", "");
    const std::string window_status =
        window_audio_status_to_json(snapshot, diagnostics);
    CHECK(window_status.find("\"sourceCount\":1") != std::string::npos);
    CHECK(window_status.find("\"display-right\"") != std::string::npos);
    CHECK(window_status.find("\"captureState\":\"capturing\"") !=
          std::string::npos);
    CHECK(window_status.find("\"requiredActiveCaptures\":1") !=
          std::string::npos);
    CHECK(window_status.find("\"uncoveredActiveSessions\":0") !=
          std::string::npos);
    CHECK(window_status.find("\"coverageComplete\":true") !=
          std::string::npos);
    CHECK(window_status.find("\"coverageDetail\":\"\"") !=
          std::string::npos);

    EngineStatusV1 status{};
    status.window_audio_enabled = true;
    status.window_audio_rendering = true;
    status.window_audio_source_count = 1;
    status.window_audio_json = window_status;
    const std::string engine_status = engine_status_to_json(status);
    CHECK(engine_status.find("\"spatialInputMode\":\"process-windows\"") !=
          std::string::npos);
    CHECK(engine_status.find(
              "\"requestedSpatialInputMode\":\"process-windows\"") !=
          std::string::npos);
    CHECK(engine_status.find("\"windowAudio\":{\"supported\":true") !=
          std::string::npos);

    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("sound-spatializer-window-config-test-" + std::to_string(unique));
    ConfigStore store(directory);
    CHECK(store.save_window_audio_config(config, error));
    const auto loaded = store.load_window_audio_config();
    CHECK(loaded);
    if (loaded) {
        CHECK(loaded.value->max_applications == config.max_applications);
        CHECK(loaded.value->display_calibration_count ==
              config.display_calibration_count);
        CHECK(loaded.value->source_rule_count == config.source_rule_count);
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    CHECK(!cleanup_error);
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
        diagnostics_.capture_endpoint_id =
            config.capture_provider == CaptureProvider::external_render
                ? config.capture_endpoint_id
                : (!config.native_test_override_endpoint_id.empty()
                       ? config.native_test_override_endpoint_id
                       : "fixture-native-render-endpoint");
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

class StereoWindowCaptureFixture final : public IWindowAudioCapture {
public:
    bool start(const WindowAudioConfig& config) override {
        ++start_count;
        last_config = config;
        running_ = true;
        return true;
    }

    bool reconfigure(const WindowAudioConfig& config) override {
        ++reconfigure_count;
        if (!running_ ||
            config.discovery_endpoint_id !=
                last_config.discovery_endpoint_id ||
            config.max_applications != last_config.max_applications ||
            config.excluded_process_id != last_config.excluded_process_id) {
            return false;
        }
        last_config = config;
        return true;
    }

    void stop() noexcept override {
        ++stop_count;
        running_ = false;
    }

    [[nodiscard]] bool running() const noexcept override { return running_; }

    [[nodiscard]] WindowAudioPullResult pull(
        WindowAudioSlotHandle handle, StereoFrame* output,
        std::size_t frame_count) noexcept override {
        ++pull_count;
        if (request_endpoint_fallback_on_pull)
            endpoint_fallback_requested = true;
        const bool primary =
            ready &&
            handle == WindowAudioSlotHandle{slot_index, generation};
        const bool secondary =
            secondary_ready &&
            handle == WindowAudioSlotHandle{secondary_slot_index,
                                             secondary_generation};
        if (!running_ || (!primary && !secondary) || output == nullptr) {
            if (output != nullptr)
                std::fill_n(output, frame_count, StereoFrame{});
            return {};
        }
        if (primary && !pcm_ready) {
            std::fill_n(output, frame_count, StereoFrame{});
            return {
                0,
                {{-0.35F, 1.2F, 1.0F}, {0.35F, 1.2F, 1.0F}, 1.0F},
                true,
            };
        }
        if (secondary && !secondary_pcm_ready) {
            std::fill_n(output, frame_count, StereoFrame{});
            return {
                0,
                {{0.45F, 1.2F, 1.0F}, {0.75F, 1.2F, 1.0F}, 1.0F},
                true,
            };
        }
        const StereoFrame samples =
            primary ? StereoFrame{0.2F, 0.4F}
                    : StereoFrame{0.1F, 0.05F};
        const std::size_t received_frames =
            primary && primary_received_frames < frame_count
                ? primary_received_frames
                : frame_count;
        std::fill_n(output, received_frames, samples);
        std::fill(output + static_cast<std::ptrdiff_t>(received_frames),
                  output + static_cast<std::ptrdiff_t>(frame_count),
                  StereoFrame{});
        return {
            received_frames,
            {primary
                 ? WindowAudioRealtimePlacement{
                       {-0.35F, 1.2F, 1.0F},
                       {0.35F, 1.2F, 1.0F}, 1.0F}
                 : WindowAudioRealtimePlacement{
                       {0.45F, 1.2F, 1.0F},
                       {0.75F, 1.2F, 1.0F}, 1.0F}},
            true,
        };
    }

    [[nodiscard]] WindowAudioRealtimeSnapshot realtime_snapshot()
        const noexcept override {
        if (!running_ || (!ready && !secondary_ready)) return {};
        WindowAudioRealtimeSnapshot result{};
        result.sequence = 2;
        if (ready)
            result.handles[result.count++] = {slot_index, generation};
        if (secondary_ready)
            result.handles[result.count++] = {
                secondary_slot_index, secondary_generation};
        result.required_active_captures = result.count;
        result.ready_active_captures = result.count;
        result.endpoint_fallback_requested =
            endpoint_fallback_requested;
        result.coverage_complete = coverage_complete;
        return result;
    }

    [[nodiscard]] WindowAudioSnapshot snapshot() const override {
        WindowAudioSnapshot result{};
        if (!running_) return result;
        result.sequence = 1;
        result.window_source_count = ready ? 1U : 0U;
        result.window_sources[0].handle = {slot_index, generation};
        result.window_sources[0].active = ready;
        result.window_sources[0].sample_rate = 48'000;
        result.window_sources[0].channel_count = 2;
        result.window_sources[0].capture_state =
            ready ? WindowAudioCaptureState::capturing
                  : WindowAudioCaptureState::activating;
        std::snprintf(result.window_sources[0].source_id.data(),
                      result.window_sources[0].source_id.size(), "%s",
                      "window:0:1");
        std::snprintf(result.window_sources[0].application_id.data(),
                      result.window_sources[0].application_id.size(), "%s",
                      "fixture-player.exe");
        std::snprintf(result.window_sources[0].application_name.data(),
                      result.window_sources[0].application_name.size(), "%s",
                      "Fixture player");
        if (secondary_ready) {
            auto& secondary = result.window_sources[result.window_source_count++];
            secondary.handle = {secondary_slot_index,
                                secondary_generation};
            secondary.active = true;
            secondary.sample_rate = 48'000;
            secondary.channel_count = 2;
            secondary.capture_state = WindowAudioCaptureState::capturing;
        }
        return result;
    }

    [[nodiscard]] WindowAudioDiagnostics diagnostics() const override {
        WindowAudioDiagnostics result{};
        result.supported = true;
        result.running = running_;
        result.active_slots =
            running_ ? static_cast<std::size_t>(ready) +
                           static_cast<std::size_t>(secondary_ready)
                     : 0U;
        return result;
    }

    WindowAudioConfig last_config{};
    std::size_t start_count{};
    std::size_t reconfigure_count{};
    std::size_t stop_count{};
    std::size_t pull_count{};
    bool ready{true};
    bool pcm_ready{true};
    bool coverage_complete{true};
    bool endpoint_fallback_requested{};
    bool request_endpoint_fallback_on_pull{};
    std::size_t primary_received_frames{
        std::numeric_limits<std::size_t>::max()};
    std::uint32_t slot_index{};
    std::uint32_t generation{1};
    bool secondary_ready{};
    bool secondary_pcm_ready{true};
    std::uint32_t secondary_slot_index{4};
    std::uint32_t secondary_generation{1};

private:
    bool running_{};
};

class BlockingDirectHrtf final : public IHrtfDatabase {
public:
    [[nodiscard]] std::uint32_t sample_rate() const noexcept override {
        return kSampleRate;
    }

    [[nodiscard]] std::size_t maximum_taps() const noexcept override {
        return 1;
    }

    bool query(const Vec3f&, std::span<float> left, std::span<float> right,
               std::size_t& tap_count) const noexcept override {
        query_entered.store(true, std::memory_order_release);
        while (!allow_queries.load(std::memory_order_acquire))
            std::this_thread::yield();
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        left[0] = 0.75F;
        right[0] = 0.75F;
        tap_count = 1;
        query_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    mutable std::atomic<bool> query_entered{};
    mutable std::atomic<bool> allow_queries{};
    mutable std::atomic<std::uint32_t> query_count{};
};

[[nodiscard]] bool finish_window_handoff(
    SpatialAudioEngine& engine, MockAudioBackend& backend,
    const std::array<StereoFrame, 64>& endpoint_mix,
    std::array<StereoFrame, 64>& output) {
    for (std::size_t attempt = 0; attempt < 500; ++attempt) {
        backend.process_block(endpoint_mix.data(), output.data(),
                              output.size());
        if (engine.status().window_audio_rendering) {
            // The acknowledgement starts a 240-frame handoff. Four further
            // 64-frame callbacks guarantee that both PCM and HRTF morphs have
            // completed without relying on worker timing.
            for (std::size_t block = 0; block < 4; ++block) {
                backend.process_block(endpoint_mix.data(), output.data(),
                                      output.size());
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

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
    SceneConfigV2 initial = engine->scene();
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

    const SceneConfigV2 working_route = engine->scene();
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
    SceneConfigV2 stale = engine->scene();
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
    SceneConfigV2 requested = engine->scene();
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
    SceneConfigV2 scene{};
    SceneConfigV2 unavailable = scene;
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

void test_engine_mock_surround_status_and_programme_input() {
    auto mock = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock_pointer = mock.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(mock));

    SceneConfigV2 scene{};
    scene.audio.capture_provider = CaptureProvider::external_render;
    scene.audio.capture_endpoint_id = "vb-cable-5.1";
    scene.audio.output_device_id = "usb-headphones";
    scene.audio.input_layout = InputLayout::surround_5_1;
    scene.audio.bypass = true;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.lfe.enabled = false;
    scene.room.enabled = false;

    std::string error;
    CHECK(engine->set_scene(scene, error));
    CHECK(engine->start_audio(error));
    const EngineStatusV1 status = engine->status();
    CHECK(status.capture_state == StreamState::running);
    CHECK(status.render_state == StreamState::running);
    CHECK(status.input_layout == InputLayout::surround_5_1);
    CHECK(status.capture_channels == 6U);
    CHECK(status.capture_channel_mask == kSurround51ChannelMask);

    std::array<ProgrammeFrame, 32> input{};
    input[0].front_left = 0.01F;
    input[0].front_right = 0.02F;
    input[0].front_center = 0.03F;
    input[0].surround_left = 0.04F;
    input[0].surround_right = 0.05F;
    std::array<StereoFrame, 32> output{};
    mock_pointer->process_block(input.data(), output.data(), output.size());
    const StereoFrame expected = downmix_programme_to_stereo(input[0], 0.0F);
    CHECK_NEAR(output[0].left, expected.left, 1.0e-5F);
    CHECK_NEAR(output[0].right, expected.right, 1.0e-5F);
    CHECK(output[0].left != output[0].right);
    CHECK(std::all_of(output.begin() + 1, output.end(), [](const StereoFrame& frame) {
        return std::abs(frame.left) < 1.0e-7F && std::abs(frame.right) < 1.0e-7F;
    }));
    engine->stop_audio();
}

void test_engine_window_mode_preserves_each_application_stereo_pair() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = true;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 1;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));
    CHECK(window->start_count == 1U);
    CHECK(window->last_config.enabled);
    CHECK(window->last_config.discovery_endpoint_id ==
          "mock-native-render-endpoint");

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.9F, 0.1F});
    std::array<StereoFrame, 64> output{};
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(window->pull_count > 0U);
    CHECK_NEAR(output.front().left, 0.9F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.1F, 1.0e-5F);
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.2F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.4F, 1.0e-5F);
    }
    EngineStatusV1 status = engine->status();
    CHECK(status.window_audio_enabled);
    CHECK(status.window_audio_rendering);
    CHECK(status.window_audio_source_count == 1U);
    CHECK(engine_status_to_json(status).find(
              "\"spatialInputMode\":\"process-windows\"") !=
          std::string::npos);

    config.enabled = false;
    CHECK(engine->set_window_spatialization(config, error));
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    // The first sample is continuous with the last process sample; the
    // unavailable old stream is then de-clicked toward the endpoint over
    // 240 frames without adding output latency.
    CHECK_NEAR(output.front().left, 0.2F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.4F, 1.0e-5F);
    for (const StereoFrame& frame : output)
        CHECK(std::isfinite(frame.left) && std::isfinite(frame.right));
    for (std::size_t block = 0; block < 4; ++block)
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.9F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.1F, 1.0e-5F);
    }
    status = engine->status();
    CHECK(!status.window_audio_enabled);
    CHECK(window->stop_count >= 1U);
    engine->stop_audio();
}

void test_engine_window_mode_falls_back_until_process_capture_is_ready() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    window->ready = false;
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = true;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 1;
    CHECK(engine->set_window_spatialization(config, error));

    SceneConfigV2 incompatible = scene;
    incompatible.audio.input_layout = InputLayout::surround_5_1;
    CHECK(!engine->set_scene(incompatible, error));
    CHECK(error.find("cannot be enabled") != std::string::npos);
    CHECK(engine->scene().audio.input_layout == InputLayout::stereo);

    CHECK(engine->start_audio(error));
    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.9F, 0.1F});
    std::array<StereoFrame, 64> output{};

    // Process-loopback activation is asynchronous. Until a capturing slot is
    // published, the endpoint mix must remain audible rather than being
    // replaced with silence.
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.9F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.1F, 1.0e-5F);
    }
    EngineStatusV1 fallback_status = engine->status();
    CHECK(fallback_status.window_audio_enabled);
    CHECK(!fallback_status.window_audio_rendering);
    const std::string fallback_status_json =
        engine_status_to_json(fallback_status);
    CHECK(fallback_status_json.find(
              "\"spatialInputMode\":\"endpoint-mix\"") !=
          std::string::npos);
    CHECK(fallback_status_json.find(
              "\"requestedSpatialInputMode\":\"process-windows\"") !=
          std::string::npos);

    // A capture thread publishes `capturing` before its ASRC can produce PCM.
    // That intermediate state must keep the endpoint fallback audible.
    window->ready = true;
    window->pcm_ready = false;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.9F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.1F, 1.0e-5F);
    }
    CHECK(!engine->status().window_audio_rendering);

    // Once a complete block is ready, use a short preallocated transport fade
    // before rendering only the preserved process stereo pair.
    window->pcm_ready = true;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK_NEAR(output.front().left, 0.9F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.1F, 1.0e-5F);
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.2F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.4F, 1.0e-5F);
    }
    CHECK(engine->status().window_audio_rendering);

    // A failed/stopped process capture returns to the endpoint through a
    // bounded zero-latency de-click.
    window->ready = false;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK_NEAR(output.front().left, 0.2F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.4F, 1.0e-5F);
    for (std::size_t block = 0; block < 4; ++block)
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.9F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.1F, 1.0e-5F);
    }
    engine->stop_audio();
}

void test_engine_window_handoff_waits_for_sparse_hrtf_bank() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    window->slot_index = 3;
    auto delayed_hrtf = std::make_unique<BlockingDirectHrtf>();
    BlockingDirectHrtf* hrtf = delayed_hrtf.get();
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture), std::move(delayed_hrtf));

    const auto query_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!hrtf->query_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < query_deadline) {
        std::this_thread::yield();
    }
    CHECK(hrtf->query_entered.load(std::memory_order_acquire));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = false;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 4;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.15F, 0.10F});
    std::array<StereoFrame, 64> output{};

    // Hold the worker for more than twice the old 240-frame PCM fade. A source
    // in slot 3 maps to directional indices 8/9, which the initial stereo bank
    // cannot render. The endpoint must nevertheless remain at unity.
    for (std::size_t block = 0; block < 8; ++block) {
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
        for (const StereoFrame& frame : output) {
            CHECK_NEAR(frame.left, 0.15F, 1.0e-5F);
            CHECK_NEAR(frame.right, 0.10F, 1.0e-5F);
        }
        CHECK(!engine->status().window_audio_rendering);
    }

    hrtf->allow_queries.store(true, std::memory_order_release);
    bool acknowledged = false;
    for (std::size_t attempt = 0; attempt < 500 && !acknowledged;
         ++attempt) {
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
        for (const StereoFrame& frame : output) {
            CHECK(std::abs(frame.left) + std::abs(frame.right) > 0.02F);
        }
        acknowledged = engine->status().window_audio_rendering;
        if (!acknowledged)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(acknowledged);

    for (std::size_t block = 0; block < 4; ++block)
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.45F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.45F, 1.0e-5F);
    }
    CHECK(hrtf->query_count.load(std::memory_order_relaxed) >= 2U);
    engine->stop_audio();
}

void test_engine_window_underrun_keeps_primed_topology() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    window->slot_index = 2;
    auto hrtf_database = std::make_unique<BlockingDirectHrtf>();
    BlockingDirectHrtf* hrtf = hrtf_database.get();
    hrtf->allow_queries.store(true, std::memory_order_release);
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture),
        std::move(hrtf_database));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = false;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 4;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.15F, 0.10F});
    std::array<StereoFrame, 64> output{};
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    CHECK(engine->status().window_audio_rendering);
    const std::uint32_t queries_before_underrun =
        hrtf->query_count.load(std::memory_order_acquire);

    // Once this generation produced a complete block, one empty ASRC render
    // is silence inside the same topology. It must not trigger endpoint
    // fallback or a second sparse-bank rebuild.
    window->primary_received_frames = 0;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(engine->status().window_audio_rendering);
    CHECK(hrtf->query_count.load(std::memory_order_acquire) ==
          queries_before_underrun);
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.0F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.0F, 1.0e-5F);
    }

    window->primary_received_frames =
        std::numeric_limits<std::size_t>::max();
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(engine->status().window_audio_rendering);
    CHECK(hrtf->query_count.load(std::memory_order_acquire) ==
          queries_before_underrun);
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.45F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.45F, 1.0e-5F);
    }
    engine->stop_audio();
}

void test_engine_window_incomplete_coverage_keeps_complete_endpoint_mix() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = true;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 4;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.15F, 0.10F});
    std::array<StereoFrame, 64> output{};
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.2F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.4F, 1.0e-5F);
    }

    // Model the capture-thread latch: a non-silent packet arrived on a
    // pre-armed slot while endpoint discovery still reports it inactive.
    // Structural coverage deliberately remains unchanged, proving the endpoint
    // fallback is restored by the atomic veto before another discovery pass.
    CHECK(window->coverage_complete);
    window->request_endpoint_fallback_on_pull = true;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(window->endpoint_fallback_requested);
    CHECK(!engine->status().window_audio_rendering);
    CHECK_NEAR(output.front().left, 0.2F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.4F, 1.0e-5F);
    for (std::size_t block = 0; block < 4; ++block)
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.15F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.10F, 1.0e-5F);
    }

    window->request_endpoint_fallback_on_pull = false;
    window->endpoint_fallback_requested = false;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(!engine->status().window_audio_rendering);
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    CHECK(engine->status().window_audio_rendering);

    const std::size_t pulls_before_fallback = window->pull_count;
    window->coverage_complete = false;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(!engine->status().window_audio_rendering);
    CHECK(window->pull_count > pulls_before_fallback);
    CHECK_NEAR(output.front().left, 0.2F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.4F, 1.0e-5F);
    for (std::size_t block = 0; block < 4; ++block)
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.15F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.10F, 1.0e-5F);
    }

    window->coverage_complete = true;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(!engine->status().window_audio_rendering);
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.15F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.10F, 1.0e-5F);
    }
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.2F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.4F, 1.0e-5F);
    }
    engine->stop_audio();
}

void test_engine_window_replacement_waits_for_every_required_capture() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    window->secondary_slot_index = 1;
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = true;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 4;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.15F, 0.10F});
    std::array<StereoFrame, 64> output{};
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.2F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.4F, 1.0e-5F);
    }

    // A is already authoritative when a second required handle B appears.
    // Model B becoming unready after the snapshot was published: A must not
    // satisfy B's required-capture count and keep the partial process set live.
    window->secondary_ready = true;
    window->secondary_pcm_ready = false;
    const std::size_t pulls_before_replacement = window->pull_count;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(window->pull_count >= pulls_before_replacement + 2U);
    CHECK(!engine->status().window_audio_rendering);
    CHECK_NEAR(output.front().left, 0.2F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.4F, 1.0e-5F);
    for (std::size_t block = 0; block < 4; ++block)
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.15F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.10F, 1.0e-5F);
    }

    window->secondary_pcm_ready = true;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK(!engine->status().window_audio_rendering);
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.3F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.45F, 1.0e-5F);
    }
    engine->stop_audio();
}

void test_engine_window_set_mutations_require_new_hrtf_ack() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    window->slot_index = 1;
    auto delayed_hrtf = std::make_unique<BlockingDirectHrtf>();
    BlockingDirectHrtf* hrtf = delayed_hrtf.get();
    hrtf->allow_queries.store(true, std::memory_order_release);
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture), std::move(delayed_hrtf));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = false;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 6;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.15F, 0.10F});
    std::array<StereoFrame, 64> output{};
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));

    hrtf->query_entered.store(false, std::memory_order_release);
    hrtf->allow_queries.store(false, std::memory_order_release);
    window->secondary_ready = true;
    window->secondary_slot_index = 4;

    // Adding B while A is already rendered changes the sparse source set
    // without changing the boolean mode. It must still return to the endpoint
    // until a bank containing both stable pairs is acknowledged.
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK_NEAR(output.front().left, 0.45F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.45F, 1.0e-5F);
    for (const StereoFrame& frame : output)
        CHECK(std::abs(frame.left) + std::abs(frame.right) > 0.02F);
    CHECK(!engine->status().window_audio_rendering);

    const auto blocked_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!hrtf->query_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < blocked_deadline) {
        std::this_thread::yield();
    }
    CHECK(hrtf->query_entered.load(std::memory_order_acquire));

    // Reusing A's slot with a new generation while the previous mutation is
    // still preparing must supersede that in-flight revision, not render the
    // replacement through A's old filters.
    ++window->generation;
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    for (const StereoFrame& frame : output)
        CHECK(std::abs(frame.left) + std::abs(frame.right) > 0.02F);
    CHECK(!engine->status().window_audio_rendering);

    hrtf->allow_queries.store(true, std::memory_order_release);
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.5625F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.5625F, 1.0e-5F);
    }
    engine->stop_audio();
}

void exercise_engine_window_exit_waits_for_endpoint_hrtf_bank(
    std::uint32_t slot_index) {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    window->slot_index = slot_index;
    auto delayed_hrtf = std::make_unique<BlockingDirectHrtf>();
    BlockingDirectHrtf* hrtf = delayed_hrtf.get();
    hrtf->allow_queries.store(true, std::memory_order_release);
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture), std::move(delayed_hrtf));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = false;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 4;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.15F, 0.10F});
    std::array<StereoFrame, 64> output{};
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.45F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.45F, 1.0e-5F);
    }

    const std::uint32_t queries_before_exit =
        hrtf->query_count.load(std::memory_order_acquire);
    hrtf->query_entered.store(false, std::memory_order_release);
    hrtf->allow_queries.store(false, std::memory_order_release);
    window->ready = false;

    // The completed window bank keeps 0/1 permanently assigned to the dormant
    // endpoint while every application lives at 2..17. That endpoint must
    // remain audible until the worker publishes the new two-source bank.
    mock->process_block(endpoint_mix.data(), output.data(), output.size());
    CHECK_NEAR(output.front().left, 0.45F, 1.0e-5F);
    CHECK_NEAR(output.front().right, 0.45F, 1.0e-5F);
    for (const StereoFrame& frame : output)
        CHECK(std::abs(frame.left) + std::abs(frame.right) > 0.02F);
    CHECK(!engine->status().window_audio_rendering);

    const auto blocked_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!hrtf->query_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < blocked_deadline) {
        std::this_thread::yield();
    }
    CHECK(hrtf->query_entered.load(std::memory_order_acquire));
    for (std::size_t block = 0; block < 4; ++block) {
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
        for (const StereoFrame& frame : output)
            CHECK(std::abs(frame.left) + std::abs(frame.right) > 0.02F);
    }
    for (std::size_t block = 0; block < 4; ++block) {
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
        for (const StereoFrame& frame : output) {
            CHECK_NEAR(frame.left, 0.1875F, 1.0e-5F);
            CHECK_NEAR(frame.right, 0.1875F, 1.0e-5F);
        }
    }

    hrtf->allow_queries.store(true, std::memory_order_release);
    for (std::size_t attempt = 0; attempt < 500; ++attempt) {
        mock->process_block(endpoint_mix.data(), output.data(), output.size());
        for (const StereoFrame& frame : output) {
            CHECK(std::abs(frame.left) + std::abs(frame.right) > 0.02F);
        }
        if (hrtf->query_count.load(std::memory_order_acquire) >=
            queries_before_exit + 2U) {
            for (std::size_t block = 0; block < 5; ++block) {
                mock->process_block(endpoint_mix.data(), output.data(),
                                    output.size());
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(hrtf->query_count.load(std::memory_order_acquire) >=
          queries_before_exit + 2U);
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.1875F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.1875F, 1.0e-5F);
    }
    engine->stop_audio();
}

void test_engine_window_exit_waits_for_endpoint_hrtf_bank() {
    // Slot 0 previously collided with endpoint indices 0/1, while a higher
    // sparse slot exposed the missing-path silence independently. Exercise
    // both mappings against the same deterministically blocked worker.
    exercise_engine_window_exit_waits_for_endpoint_hrtf_bank(0);
    exercise_engine_window_exit_waits_for_endpoint_hrtf_bank(3);
}

void test_engine_window_mode_applies_control_updates_without_capture_cut() {
    auto backend = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock = backend.get();
    auto capture = std::make_unique<StereoWindowCaptureFixture>();
    StereoWindowCaptureFixture* window = capture.get();
    auto engine = std::make_unique<SpatialAudioEngine>(
        std::move(backend), std::move(capture));

    SceneConfigV2 scene{};
    scene.audio.input_layout = InputLayout::stereo;
    scene.audio.bypass = true;
    scene.audio.master_gain_db = 0.0F;
    scene.audio.room_mix = 0.0F;
    scene.room.enabled = false;
    std::string error;
    CHECK(engine->set_scene(scene, error));

    WindowAudioConfig config{};
    config.enabled = true;
    config.max_applications = 1;
    CHECK(engine->set_window_spatialization(config, error));
    CHECK(engine->start_audio(error));
    const std::size_t initial_start_count = window->start_count;
    const std::size_t initial_stop_count = window->stop_count;

    WindowAudioConfig live_update = config;
    live_update.refresh_interval_ms = 50;
    live_update.stereo_spread = 0.35F;
    live_update.follow_window_position = false;
    live_update.display_calibration_count = 1;
    live_update.display_calibrations[0].display_id =
        "fixture-display";
    live_update.display_calibrations[0].center_m =
        {0.8F, 1.2F, 1.1F};
    live_update.display_calibrations[0].width_m = 0.72F;
    live_update.display_calibrations[0].height_m = 0.41F;
    live_update.source_rule_count = 1;
    live_update.source_rules[0].application_id =
        "fixture-player.exe";
    live_update.source_rules[0].gain_db = -6.0F;
    live_update.source_rules[0].stereo_spread = 0.2F;
    live_update.source_rules[0].fallback_display_id =
        "fixture-display";

    CHECK(engine->set_window_spatialization(live_update, error));
    CHECK(window->reconfigure_count == 1U);
    CHECK(window->start_count == initial_start_count);
    CHECK(window->stop_count == initial_stop_count);
    CHECK(window->running());
    CHECK_NEAR(window->last_config.stereo_spread, 0.35F, 1.0e-7F);
    CHECK(window->last_config.display_calibration_count == 1U);
    CHECK(window->last_config.source_rule_count == 1U);
    CHECK_NEAR(window->last_config.source_rules[0].gain_db, -6.0F,
               1.0e-7F);

    std::array<StereoFrame, 64> endpoint_mix{};
    endpoint_mix.fill({0.9F, 0.1F});
    std::array<StereoFrame, 64> output{};
    CHECK(finish_window_handoff(*engine, *mock, endpoint_mix, output));
    for (const StereoFrame& frame : output) {
        CHECK_NEAR(frame.left, 0.2F, 1.0e-5F);
        CHECK_NEAR(frame.right, 0.4F, 1.0e-5F);
    }

    WindowAudioConfig structural_update = live_update;
    structural_update.max_applications = 2;
    CHECK(engine->set_window_spatialization(structural_update, error));
    CHECK(window->reconfigure_count == 2U);
    CHECK(window->start_count == initial_start_count + 1U);
    CHECK(window->stop_count == initial_stop_count + 1U);
    CHECK(window->running());

    engine->stop_audio();
}

void test_engine_head_pose_changes_binaural_output() {
    auto mock = std::make_unique<MockAudioBackend>();
    MockAudioBackend* mock_pointer = mock.get();
    auto engine = std::make_unique<SpatialAudioEngine>(std::move(mock));

    SceneConfigV2 scene{};
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
    SceneConfigV2 scene{};
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
    test_surround_input_contracts();
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
    test_diffuse_field_equalizer_unit_impulse_and_crossfade();
    test_diffuse_field_inverse_neutralizes_level_and_tilt();
    test_measured_sofa_diffuse_field_equalization_when_available();
    test_convolver_and_morph();
    test_five_source_two_ear_convolver_channel_isolation();
    test_sixteen_source_two_ear_convolver_capacity_and_isolation();
    test_sixteen_source_filter_builder_and_worker_capacity();
    test_phantom_centre_compensation_flattens_the_inter_emitter_comb();
    test_measured_sofa_render_level_stays_off_the_limiter();
    test_partitioned_convolver_matches_time_domain_reference();
    test_partitioned_convolver_morph_and_interruption();
    test_lfe_symmetric_low_pass_and_downmix();
    test_eq_and_limiter();
    test_bypass_crossfade_and_idempotent_eq();
    test_potential_binaural_warning_detector();
    test_polyphase_resampler_quality_and_drift();
    test_programme_resampler_channel_isolation();
    test_resampler_xrun_event_telemetry();
    test_process_resampler_rebases_discontinuities_and_excess_backlog();
    test_room_acoustics_and_coordinates();
    test_order3_binaural_decoder_and_rotation();
    test_scene_v1_migration_and_v2_roundtrip();
    test_json_and_ui_commands();
    test_window_spatialization_contract_status_and_persistence();
    test_json_frame_decoder();
    test_single_instance_guard();
    test_status_remains_available_while_audio_start_blocks();
    test_atomic_audio_route_command();
    test_audio_mode_command_recovers_from_unsupported_exclusive();
    test_effective_audio_mode_survives_a_stale_scene_update();
    test_failed_exclusive_and_shared_start_does_not_falsify_effective_scene();
    test_engine_mock_pipeline();
    test_engine_mock_surround_status_and_programme_input();
    test_engine_window_mode_preserves_each_application_stereo_pair();
    test_engine_window_mode_falls_back_until_process_capture_is_ready();
    test_engine_window_handoff_waits_for_sparse_hrtf_bank();
    test_engine_window_underrun_keeps_primed_topology();
    test_engine_window_incomplete_coverage_keeps_complete_endpoint_mix();
    test_engine_window_replacement_waits_for_every_required_capture();
    test_engine_window_set_mutations_require_new_hrtf_ack();
    test_engine_window_exit_waits_for_endpoint_hrtf_bank();
    test_engine_window_mode_applies_control_updates_without_capture_cut();
    test_engine_head_pose_changes_binaural_output();
    test_hrtf_cache_when_fixture_is_available();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All Sound Spatializer engine tests passed\n";
    return 0;
}
