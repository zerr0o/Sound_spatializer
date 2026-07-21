#include "sound_spatializer/engine.hpp"

#include "sound_spatializer/config.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sound_spatializer {
namespace {

[[nodiscard]] double qpc_frequency() noexcept {
#if defined(_WIN32)
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return frequency;
#else
    return 1'000'000'000.0;
#endif
}

[[nodiscard]] std::string canonical_hrtf_cache_key(std::string_view utf8_path) {
    std::u8string native_utf8;
    native_utf8.reserve(utf8_path.size());
    for (const char character : utf8_path)
        native_utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(std::filesystem::path(native_utf8), error);
    if (error) {
        error.clear();
        canonical = std::filesystem::absolute(std::filesystem::path(native_utf8), error).lexically_normal();
        if (error)
            canonical = std::filesystem::path(native_utf8).lexically_normal();
    }
    const std::u8string generic = canonical.generic_u8string();
    std::string key;
    key.reserve(generic.size());
    for (const char8_t character : generic)
        key.push_back(static_cast<char>(character));
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
    return key;
}

} // namespace

SpatialAudioEngine::SpatialAudioEngine() : SpatialAudioEngine(create_system_audio_backend()) {}

SpatialAudioEngine::SpatialAudioEngine(std::unique_ptr<IAudioBackend> backend) : backend_(std::move(backend)) {
    if (!backend_)
        backend_ = std::make_unique<MockAudioBackend>();
    hrtf_lifetime_.push_back(std::make_unique<AnalyticHrtfDatabase>(kSampleRate));
    limiter_.prepare(static_cast<float>(kSampleRate));
    limiter_.set_ceiling_db(-0.1F);
    binaural_detector_.prepare(static_cast<float>(kSampleRate));
    for (auto& early : early_reflections_)
        (void)early.prepare(static_cast<float>(kSampleRate), 80.0F);
    (void)late_reverb_.prepare(static_cast<float>(kSampleRate), scene_.room.late_rt60_s);
    std::string ignored;
    (void)set_scene(scene_, ignored);
}

SpatialAudioEngine::~SpatialAudioEngine() { stop_audio(); }

bool SpatialAudioEngine::prepare_scene(const SceneConfigV1& scene, PreparedScene& prepared,
                                       std::string& warning_or_error, bool& is_error) {
    is_error = false;
    if (!validate_scene_config(scene, warning_or_error)) {
        is_error = true;
        return false;
    }

    const IHrtfDatabase* database = hrtf_lifetime_.front().get();
    warning_or_error.clear();
    if (scene.hrtf.sofa_path) {
        const std::string cache_key = canonical_hrtf_cache_key(*scene.hrtf.sofa_path);
        if (const auto cached = hrtf_cache_.find(cache_key); cached != hrtf_cache_.end()) {
            database = cached->second;
        } else {
            if (hrtf_cache_.size() >= kMaximumPersonalHrtfsPerProcess) {
                warning_or_error = "SOFA import refused: the per-process limit of 16 unique personal "
                                   "HRTFs was "
                                   "reached; restart the engine before importing another profile";
                is_error = true;
                return false;
            }
            HrtfLoadResult loaded = load_sofa_hrtf(*scene.hrtf.sofa_path, kSampleRate);
            if (!loaded.database) {
                warning_or_error = "SOFA import refused; current HRTF remains active: " + loaded.error;
                is_error = true;
                return false;
            }
            database = loaded.database.get();
            hrtf_lifetime_.push_back(std::move(loaded.database));
            hrtf_cache_.emplace(cache_key, database);
        }
    } else if (scene.hrtf.profile_id != "builtin-analytic-neutral") {
        warning_or_error =
            "HRTF profile '" + scene.hrtf.profile_id + "' was refused because no verified SOFA path was supplied";
        is_error = true;
        return false;
    }

    prepared = {};
    for (std::size_t index = 0; index < 2; ++index) {
        prepared.speaker_positions[index] = scene.speakers[index].position_m;
        prepared.speaker_gains[index] = db_to_linear(scene.speakers[index].gain_db);
    }
    prepared.listener_position = scene.listener.position_m;
    prepared.neutral_orientation = scene.listener.neutral_orientation.normalized_value();
    prepared.eq_section_count = scene.headphone_eq.sections.size();
    std::copy(scene.headphone_eq.sections.begin(), scene.headphone_eq.sections.end(), prepared.eq_sections.begin());
    prepared.hrtf = database;
    prepared.late_rt60 = estimate_room_rt60_eyring(scene.room);
    prepared.master_gain_db = scene.audio.master_gain_db;
    prepared.eq_preamp_db = scene.headphone_eq.preamp_db;
    prepared.room_mix = scene.audio.room_mix;
    prepared.prediction_limit_ms = scene.tracking.prediction_limit_ms;
    prepared.eq_enabled = scene.headphone_eq.enabled;
    prepared.room_enabled = scene.room.enabled;
    prepared.late_reverb_enabled = scene.room.late_reverb_enabled;
    prepared.bypass = scene.audio.bypass;

    if (scene.room.enabled) {
        ImageSourceModel image_sources;
        for (std::size_t source = 0; source < 2; ++source) {
            const std::vector<ReflectionTap> reflections =
                image_sources.calculate(scene.speakers[source].position_m, scene.listener.position_m, scene.room);
            prepared.early_reflection_counts[source] =
                std::min(reflections.size(), EarlyReflectionProcessor::kMaximumReflectionTaps);
            std::copy_n(reflections.begin(), prepared.early_reflection_counts[source],
                        prepared.early_reflections[source].begin());
        }
    }

    if (database->sample_rate() != kSampleRate || database->maximum_taps() == 0 ||
        database->maximum_taps() > kMaximumHrirTaps) {
        warning_or_error = "HRTF provider does not satisfy the 48 kHz / "
                           "bounded-tap engine contract";
        is_error = true;
        return false;
    }
    return true;
}

bool SpatialAudioEngine::set_scene(const SceneConfigV1& scene, std::string& error) {
    std::scoped_lock lock(control_mutex_);
    PreparedScene prepared{};
    bool is_error = false;
    std::string diagnostic;
    if (!prepare_scene(scene, prepared, diagnostic, is_error)) {
        hrtf_warning_ = diagnostic;
        error = std::move(diagnostic);
        return false;
    }
    if (backend_->running()) {
        if (!scene_commands_.try_push(prepared)) {
            error = "real-time scene command queue is full";
            return false;
        }
    } else {
        apply_prepared_scene(prepared, false);
    }
    scene_ = scene;
    hrtf_warning_ = std::move(diagnostic);
    error.clear();
    return true;
}

SceneConfigV1 SpatialAudioEngine::scene() const {
    std::scoped_lock lock(control_mutex_);
    return scene_;
}

void SpatialAudioEngine::submit_head_pose(const HeadPoseSampleV1& pose) noexcept { pose_mailbox_.store(pose); }

void SpatialAudioEngine::set_virtual_endpoint_id(std::string endpoint_id) {
    std::scoped_lock lock(control_mutex_);
    virtual_endpoint_id_ = std::move(endpoint_id);
}

bool SpatialAudioEngine::start_audio(std::string& error) {
    AudioBackendConfig config{};
    const bool was_already_running = backend_->running();
    {
        // Device activation can cross the COM/RPC boundary and must never hold
        // the scene/status mutex. In particular, a driver that stalls during
        // WASAPI initialization must not suppress diagnostics from the main
        // status publisher.
        std::scoped_lock lock(control_mutex_);
        if (!backend_->running()) {
            binaural_detector_.reset();
            potentially_binaural_.store(false, std::memory_order_relaxed);
            effective_audio_mode_.reset();
        }
        config.capture_provider = scene_.audio.capture_provider;
        config.capture_endpoint_id = scene_.audio.capture_endpoint_id.value_or("");
        if (config.capture_provider == CaptureProvider::native_driver)
            config.native_test_override_endpoint_id = virtual_endpoint_id_;
        config.physical_output_endpoint_id = scene_.audio.output_device_id.value_or("");
        config.mode = scene_.audio.mode;
        config.requested_buffer_frames = scene_.audio.buffer_frames;
    }
    if (backend_->start(config, *this, error)) {
        // A repeated `start` is intentionally idempotent. In particular, the
        // desktop bootstrap can issue it immediately after a route command
        // already fell back from Pro to shared. Do not erase that capability
        // warning unless this call really opened a new stream.
        if (!was_already_running) {
            std::scoped_lock lock(control_mutex_);
            effective_audio_mode_ = config.mode;
            audio_mode_warning_.clear();
        }
        return true;
    }
    if (config.mode != AudioMode::exclusive_pro)
        return false;

    // Exclusive support is endpoint-specific. A format/period rejected by the
    // physical device is a mode capability miss, not a reason to leave the
    // entire engine offline. Make one bounded recovery attempt in the lowest-
    // period shared mode and publish the requested/effective mismatch.
    const std::string exclusive_error = error;
    backend_->stop();
    {
        std::scoped_lock lock(control_mutex_);
        effective_audio_mode_.reset();
    }
    SceneConfigV1 fallback = scene();
    fallback.audio.mode = AudioMode::shared_low_latency;
    fallback.audio.buffer_frames = 128;
    std::string fallback_error;
    config.mode = AudioMode::shared_low_latency;
    config.requested_buffer_frames = 128;
    if (!backend_->start(config, *this, fallback_error)) {
        error = "exclusive Pro mode failed: " + exclusive_error +
                "; shared low-latency fallback failed: " + fallback_error;
        return false;
    }
    {
        std::scoped_lock lock(control_mutex_);
        effective_audio_mode_ = AudioMode::shared_low_latency;
    }
    // Commit the effective scene only after the shared backend is actually
    // running. If both modes fail, callers still observe the requested scene
    // and a mode command can restore the exact previous configuration.
    if (!set_scene(fallback, fallback_error)) {
        backend_->stop();
        {
            std::scoped_lock lock(control_mutex_);
            effective_audio_mode_.reset();
        }
        error = "exclusive Pro mode failed: " + exclusive_error +
                "; shared low-latency fallback scene commit failed: " + fallback_error;
        return false;
    }
    {
        std::scoped_lock lock(control_mutex_);
        audio_mode_warning_ = "AUDIO_MODE_FALLBACK requested=exclusive-pro effective=shared-low-latency reason=" +
                              exclusive_error;
    }
    error.clear();
    return true;
}

void SpatialAudioEngine::stop_audio() noexcept {
    backend_->stop();
    std::scoped_lock lock(control_mutex_);
    effective_audio_mode_.reset();
}

bool SpatialAudioEngine::execute_command(const EngineCommandV1& command, std::string& error) {
    switch (command.type) {
    case EngineCommandType::start:
        return start_audio(error);
    case EngineCommandType::stop:
        stop_audio();
        error.clear();
        return true;
    case EngineCommandType::set_scene:
        return set_scene(command.scene, error);
    case EngineCommandType::set_bypass: {
        SceneConfigV1 updated = scene();
        updated.audio.bypass = command.bool_value;
        return set_scene(updated, error);
    }
    case EngineCommandType::set_output_device: {
        const bool was_running = backend_->running();
        if (was_running)
            stop_audio();
        SceneConfigV1 updated = scene();
        updated.audio.output_device_id =
            command.string_value.empty() ? std::nullopt : std::optional<std::string>(command.string_value);
        if (!set_scene(updated, error))
            return false;
        return !was_running || start_audio(error);
    }
    case EngineCommandType::set_audio_mode: {
        const SceneConfigV1 previous = scene();
        const bool was_running = backend_->running();
        if (was_running)
            stop_audio();
        SceneConfigV1 updated = previous;
        updated.audio.mode = command.audio_mode;
        updated.audio.buffer_frames = command.audio_mode == AudioMode::compatibility_256 ? 256 : 128;
        if (!set_scene(updated, error)) {
            if (was_running) {
                std::string ignored;
                (void)set_scene(previous, ignored);
                (void)start_audio(ignored);
            }
            return false;
        }
        if (!was_running || start_audio(error))
            return true;

        // Switching modes is transactional. start_audio() already makes the
        // single bounded Pro -> shared recovery attempt; if that also fails,
        // restore the exact previously-running scene rather than leaving the
        // backend stopped.
        const std::string requested_mode_error = error;
        stop_audio();
        std::string recovery_error;
        const bool scene_recovered = set_scene(previous, recovery_error);
        const bool audio_recovered = scene_recovered && start_audio(recovery_error);
        error = "audio mode rejected: " + requested_mode_error + "; previous mode remains active";
        if (!scene_recovered || !audio_recovered)
            error += "; audio recovery failed: " + recovery_error;
        return false;
    }
    case EngineCommandType::calibrate_neutral: {
        SceneConfigV1 updated = scene();
        updated.listener.neutral_orientation = command.quaternion_value.normalized_value();
        return set_scene(updated, error);
    }
    case EngineCommandType::set_hrtf: {
        SceneConfigV1 updated = scene();
        updated.hrtf.profile_id = command.string_value;
        updated.hrtf.sofa_path = command.optional_string_value;
        return set_scene(updated, error);
    }
    case EngineCommandType::set_headphone_eq: {
        SceneConfigV1 updated = scene();
        updated.headphone_eq = command.headphone_eq;
        return set_scene(updated, error);
    }
    case EngineCommandType::set_audio_route: {
        const SceneConfigV1 previous = scene();
        SceneConfigV1 updated = previous;
        updated.audio.capture_provider = command.capture_provider;
        updated.audio.capture_endpoint_id = command.capture_endpoint_id;
        updated.audio.output_device_id = command.output_device_id;

        if (!validate_scene_config(updated, error))
            return false;
        const bool was_running = backend_->running();
        if (was_running)
            stop_audio();
        if (!set_scene(updated, error)) {
            if (was_running) {
                std::string ignored;
                (void)start_audio(ignored);
            }
            return false;
        }
        if (!was_running || start_audio(error))
            return true;

        const std::string route_error = error;
        stop_audio();
        std::string restore_error;
        const bool scene_restored = set_scene(previous, restore_error);
        bool audio_restored = !was_running;
        if (scene_restored && was_running)
            audio_restored = start_audio(restore_error);
        error = "audio route rejected: " + route_error;
        if (!scene_restored || !audio_restored)
            error += "; previous route restoration failed: " + restore_error;
        return false;
    }
    }
    error = "unsupported engine command";
    return false;
}

void SpatialAudioEngine::apply_prepared_scene(const PreparedScene& prepared, bool realtime_transition) noexcept {
    active_scene_ = prepared;
    const std::span<const BiquadParameters> eq_sections(active_scene_.eq_sections.data(),
                                                        active_scene_.eq_section_count);
    if (!headphone_eq_.configuration_matches(eq_sections, static_cast<float>(kSampleRate))) {
        (void)headphone_eq_.configure(eq_sections, static_cast<float>(kSampleRate));
    }
    headphone_eq_.set_enabled(active_scene_.eq_enabled);
    if (realtime_transition) {
        bypass_crossfade_.set_bypassed(active_scene_.bypass, 480U);
    } else {
        bypass_crossfade_.reset(active_scene_.bypass);
    }
    limiter_.set_master_gain_db(active_scene_.master_gain_db +
                                (active_scene_.eq_enabled ? active_scene_.eq_preamp_db : 0.0F));
    late_reverb_.configure_rt60(active_scene_.late_rt60);
    if (!active_scene_.room_enabled || !active_scene_.late_reverb_enabled)
        late_reverb_.reset();
    for (std::size_t source = 0; source < early_reflections_.size(); ++source) {
        const std::span<const ReflectionTap> reflections(active_scene_.early_reflections[source].data(),
                                                         active_scene_.early_reflection_counts[source]);
        (void)early_reflections_[source].set_reflections(reflections, active_scene_.room_enabled ? 4'800U : 0U);
        if (!active_scene_.room_enabled)
            early_reflections_[source].reset();
    }
    PosePredictor::Settings pose_settings{};
    pose_settings.maximum_prediction_ms = active_scene_.prediction_limit_ms;
    pose_predictor_ = PosePredictor(pose_settings);
    pose_predictor_.reset(active_scene_.neutral_orientation, qpc_to_seconds(current_qpc()));
    last_pose_sequence_ = 0;
    ++active_scene_revision_;
    filter_request_dirty_ = true;
    request_hrtf_for_pose(active_scene_.neutral_orientation);
}

void SpatialAudioEngine::request_hrtf_for_pose(const Quaternionf& orientation) noexcept {
    if (active_scene_.hrtf == nullptr)
        return;
    const Quaternionf normalized_orientation = orientation.normalized_value();
    const float cosine = std::abs(normalized_orientation.w * last_requested_filter_orientation_.w +
                                  normalized_orientation.x * last_requested_filter_orientation_.x +
                                  normalized_orientation.y * last_requested_filter_orientation_.y +
                                  normalized_orientation.z * last_requested_filter_orientation_.z);
    if (!filter_request_dirty_ && cosine > 0.99999F)
        return;

    // Wire quaternions map head-local to calibrated world. The inverse relative
    // rotation keeps world-fixed speakers stationary when the head rotates.
    const Quaternionf world_to_head = normalized_orientation.conjugate() * active_scene_.neutral_orientation;
    HrtfPreparationRequest request{};
    request.generation = ++hrtf_request_generation_;
    request.scene_revision = active_scene_revision_;
    request.database = active_scene_.hrtf;
    request.speaker_gains = active_scene_.speaker_gains;
    request.world_to_head = world_to_head;
    request.room_enabled = active_scene_.room_enabled;
    for (std::size_t index = 0; index < 2; ++index) {
        const Vec3f world_direction = active_scene_.speaker_positions[index] - active_scene_.listener_position;
        request.head_relative_directions[index] = rotate(world_to_head, world_direction);
    }
    hrtf_worker_.submit_latest(request);
    last_requested_filter_orientation_ = normalized_orientation;
    filter_request_dirty_ = false;
}

void SpatialAudioEngine::consume_prepared_hrtf() noexcept {
    const PreparedDirectHrtfUpdate* direct = nullptr;
    std::uint8_t direct_slot_token = 0;
    if (hrtf_worker_.try_acquire_latest_direct(direct, direct_slot_token)) {
        if (direct->valid && direct->scene_revision == active_scene_revision_) {
            const bool first_filters_for_scene = direct->scene_revision != filter_scene_revision_;
            std::uint32_t morph_frames = 0;
            if (filters_initialized_) {
                if (first_filters_for_scene) {
                    morph_frames = 4'800U;
                } else if (scene_filter_transition_active_) {
                    morph_frames = convolver_.morph_remaining_frames();
                    if (morph_frames == 0) {
                        scene_filter_transition_active_ = false;
                        morph_frames = 64U;
                    }
                } else {
                    morph_frames = 64U;
                }
            }

            if (convolver_.set_filters(direct->filters, morph_frames)) {
                filters_initialized_ = true;
                filter_scene_revision_ = direct->scene_revision;
                if (first_filters_for_scene)
                    scene_filter_transition_active_ = morph_frames != 0;
            }
        }
        hrtf_worker_.release_direct(direct_slot_token);
    }

    const PreparedRoomHrtfUpdate* room = nullptr;
    std::uint8_t room_slot_token = 0;
    if (!hrtf_worker_.try_acquire_latest_room(room, room_slot_token))
        return;

    if (room->valid && active_scene_.room_enabled && room->scene_revision == active_scene_revision_) {
        const bool first_filters_for_scene = room->scene_revision != room_filter_scene_revision_;
        std::uint32_t morph_frames = 0;
        if (room_filters_initialized_) {
            if (first_filters_for_scene) {
                morph_frames = 4'800U;
            } else if (room_scene_filter_transition_active_) {
                morph_frames = room_decoder_.morph_remaining_frames();
                if (morph_frames == 0) {
                    room_scene_filter_transition_active_ = false;
                    morph_frames = 64U;
                }
            } else {
                morph_frames = 64U;
            }
        }

        if (room_decoder_.apply_filter_bank(room->filters, morph_frames)) {
            room_filters_initialized_ = true;
            room_filter_scene_revision_ = room->scene_revision;
            if (first_filters_for_scene)
                room_scene_filter_transition_active_ = morph_frames != 0;
        }
    }
    hrtf_worker_.release_room(room_slot_token);
}

void SpatialAudioEngine::reset_latency_diagnostics() noexcept {
    latency_percentiles_.reset();
    latency_p50_ms_.store(0.0F, std::memory_order_relaxed);
    latency_p95_ms_.store(0.0F, std::memory_order_relaxed);
    previous_pose_time_seconds_ = 0.0;
    latency_tracking_continuous_ = false;
}

void SpatialAudioEngine::process_chunk(const StereoFrame* input, StereoFrame* output, std::size_t frame_count,
                                       double render_time_seconds) noexcept {
    std::copy_n(input, frame_count, bypass_dry_.begin());
    const StereoFrame* dry_input = bypass_dry_.data();
    const HeadPoseSampleV1 raw_pose = pose_mailbox_.load();
    HeadPoseSampleV1 rendered_pose{};
    if (raw_pose.sequence != last_pose_sequence_) {
        const double sample_time =
            raw_pose.timestamp_qpc != 0 ? qpc_to_seconds(raw_pose.timestamp_qpc) : render_time_seconds;
        rendered_pose = pose_predictor_.update(raw_pose, sample_time, render_time_seconds);
        if (raw_pose.tracking_state == TrackingState::tracking) {
            const double pose_interval_seconds = sample_time - previous_pose_time_seconds_;
            if (!latency_tracking_continuous_ || pose_interval_seconds <= 0.0 || pose_interval_seconds > 0.5)
                reset_latency_diagnostics();
            latency_tracking_continuous_ = true;
            if (previous_pose_time_seconds_ > 0.0 && sample_time > previous_pose_time_seconds_) {
                const float measured_hz =
                    std::clamp(static_cast<float>(1.0 / (sample_time - previous_pose_time_seconds_)), 0.0F, 240.0F);
                tracking_hz_smoothed_ += 0.1F * (measured_hz - tracking_hz_smoothed_);
                tracking_hz_.store(tracking_hz_smoothed_, std::memory_order_relaxed);
            }
            previous_pose_time_seconds_ = sample_time;
            const float latency_ms =
                std::clamp(static_cast<float>((render_time_seconds - sample_time) * 1'000.0), 0.0F, 1'000.0F);
            float p50_ms = 0.0F;
            float p95_ms = 0.0F;
            if (latency_percentiles_.push(latency_ms, p50_ms, p95_ms)) {
                latency_p50_ms_.store(p50_ms, std::memory_order_relaxed);
                latency_p95_ms_.store(p95_ms, std::memory_order_relaxed);
            }
        } else if (latency_tracking_continuous_) {
            reset_latency_diagnostics();
        }
        last_pose_sequence_ = raw_pose.sequence;
    } else {
        rendered_pose = pose_predictor_.predict(render_time_seconds);
    }
    if (rendered_pose.tracking_state != TrackingState::tracking && latency_tracking_continuous_)
        reset_latency_diagnostics();
    tracking_state_.store(static_cast<std::uint32_t>(rendered_pose.tracking_state), std::memory_order_relaxed);
    request_hrtf_for_pose(rendered_pose.orientation);
    consume_prepared_hrtf();
    binaural_detector_.process(dry_input, frame_count);
    potentially_binaural_.store(binaural_detector_.potentially_binaural(), std::memory_order_relaxed);

    convolver_.process(dry_input, output, frame_count);
    if (active_scene_.room_enabled && active_scene_.room_mix > 0.0F) {
        for (std::size_t index = 0; index < frame_count; ++index) {
            room_mono_[index] = dry_input[index].left * active_scene_.speaker_gains[0];
        }
        early_reflections_[0].process(room_mono_.data(), room_early_.data(), frame_count);
        for (std::size_t index = 0; index < frame_count; ++index)
            room_mono_[index] = dry_input[index].right * active_scene_.speaker_gains[1];
        early_reflections_[1].process(room_mono_.data(), room_early_secondary_.data(), frame_count);
        for (std::size_t index = 0; index < frame_count; ++index) {
            for (std::size_t channel = 0; channel < EarlyReflectionProcessor::kAmbisonicChannels; ++channel)
                room_early_[index][channel] += room_early_secondary_[index][channel];
            room_mono_[index] = (dry_input[index].left * active_scene_.speaker_gains[0] +
                                 dry_input[index].right * active_scene_.speaker_gains[1]) *
                                0.5F;
        }
        if (active_scene_.late_reverb_enabled) {
            late_reverb_.process_mono(room_mono_.data(), room_ambi_.data(), frame_count);
        }
        for (std::size_t index = 0; index < frame_count; ++index) {
            room_combined_[index] = room_early_[index];
            if (active_scene_.late_reverb_enabled) {
                for (std::size_t channel = 0; channel < LateReverbFdn16::kOutputChannels; ++channel)
                    room_combined_[index][channel] += room_ambi_[index][channel];
            }
        }
        room_decoder_.process(room_combined_.data(), room_binaural_.data(), frame_count);
        const float dry_gain = std::cos(active_scene_.room_mix * kPi * 0.5F);
        const float wet_gain = std::sin(active_scene_.room_mix * kPi * 0.5F);
        for (std::size_t index = 0; index < frame_count; ++index) {
            output[index].left = output[index].left * dry_gain + room_binaural_[index].left * wet_gain;
            output[index].right = output[index].right * dry_gain + room_binaural_[index].right * wet_gain;
        }
    }
    headphone_eq_.process(output, frame_count);
    limiter_.process(output, frame_count);
    bypass_crossfade_.process(dry_input, output, frame_count);
}

void SpatialAudioEngine::process_audio(const StereoFrame* input, StereoFrame* output, std::size_t frame_count,
                                       std::int64_t render_qpc) noexcept {
    if (output == nullptr)
        return;
    PreparedScene prepared{};
    if (scene_commands_.try_pop(prepared))
        apply_prepared_scene(prepared, true);
    const double render_time = qpc_to_seconds(render_qpc != 0 ? render_qpc : current_qpc());
    std::size_t offset = 0;
    while (offset < frame_count) {
        const std::size_t chunk = std::min(kMaximumProcessChunk, frame_count - offset);
        if (input == nullptr) {
            std::array<StereoFrame, kMaximumProcessChunk> silence{};
            process_chunk(silence.data(), output + offset, chunk,
                          render_time + static_cast<double>(offset) / kSampleRate);
        } else {
            process_chunk(input + offset, output + offset, chunk,
                          render_time + static_cast<double>(offset) / kSampleRate);
        }
        offset += chunk;
    }
}

EngineStatusV1 SpatialAudioEngine::status() const {
    const AudioBackendDiagnostics audio = backend_->diagnostics();
    EngineStatusV1 result{};
    result.capture_state = audio.capture_state;
    result.render_state = audio.render_state;
    result.tracking_state = static_cast<TrackingState>(tracking_state_.load(std::memory_order_relaxed));
    result.capture_sample_rate = audio.capture_sample_rate;
    result.render_sample_rate = audio.render_sample_rate;
    result.render_sample_format = audio.render_sample_format;
    result.capture_period_frames = audio.capture_period_frames;
    result.render_period_frames = audio.render_period_frames;
    result.fifo_fill_frames = audio.fifo_fill_frames;
    result.xruns = audio.capture_overruns + audio.render_underruns;
    result.callback_cpu_percent = audio.callback_cpu_percent;
    result.current_resample_ratio = audio.resample_ratio;
    result.tracking_hz = tracking_hz_.load(std::memory_order_relaxed);
    result.latency_p50_ms = latency_p50_ms_.load(std::memory_order_relaxed);
    result.latency_p95_ms = latency_p95_ms_.load(std::memory_order_relaxed);
    result.potentially_binaural = potentially_binaural_.load(std::memory_order_relaxed);
    {
        std::scoped_lock lock(control_mutex_);
        result.audio_mode = effective_audio_mode_.value_or(scene_.audio.mode);
        result.last_error = audio.last_error;
        const auto append_warning = [&result](const std::string& warning) {
            if (warning.empty()) return;
            if (!result.last_error.empty()) result.last_error += "; ";
            result.last_error += warning;
        };
        append_warning(audio_mode_warning_);
        append_warning(hrtf_warning_);
        if (!audio_mode_warning_.empty() || !hrtf_warning_.empty()) {
            if (result.capture_state == StreamState::running)
                result.capture_state = StreamState::degraded;
            if (result.render_state == StreamState::running)
                result.render_state = StreamState::degraded;
        }
    }
    return result;
}

std::size_t SpatialAudioEngine::cached_personal_hrtf_count() const {
    std::scoped_lock lock(control_mutex_);
    return hrtf_cache_.size();
}

std::int64_t SpatialAudioEngine::current_qpc() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

double SpatialAudioEngine::qpc_to_seconds(std::int64_t value) noexcept {
    return static_cast<double>(value) / qpc_frequency();
}

} // namespace sound_spatializer
