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

SpatialAudioEngine::SpatialAudioEngine()
    : SpatialAudioEngine(create_system_audio_backend(), make_window_audio_capture()) {}

SpatialAudioEngine::SpatialAudioEngine(std::unique_ptr<IAudioBackend> backend)
    : SpatialAudioEngine(std::move(backend), make_window_audio_capture()) {}

SpatialAudioEngine::SpatialAudioEngine(
    std::unique_ptr<IAudioBackend> backend,
    std::unique_ptr<IWindowAudioCapture> window_audio_capture)
    : SpatialAudioEngine(std::move(backend), std::move(window_audio_capture),
                         std::make_unique<AnalyticHrtfDatabase>(kSampleRate)) {}

SpatialAudioEngine::SpatialAudioEngine(
    std::unique_ptr<IAudioBackend> backend,
    std::unique_ptr<IWindowAudioCapture> window_audio_capture,
    std::unique_ptr<IHrtfDatabase> builtin_hrtf)
    : backend_(std::move(backend)),
      window_audio_capture_(std::move(window_audio_capture)) {
    if (!backend_)
        backend_ = std::make_unique<MockAudioBackend>();
    if (!window_audio_capture_)
        window_audio_capture_ = make_window_audio_capture();
    if (!builtin_hrtf)
        builtin_hrtf =
            std::make_unique<AnalyticHrtfDatabase>(kSampleRate);
    hrtf_lifetime_.push_back(std::move(builtin_hrtf));
    limiter_.prepare(static_cast<float>(kSampleRate));
    limiter_.set_ceiling_db(-0.1F);
    lfe_renderer_.prepare(static_cast<float>(kSampleRate));
    binaural_detector_.prepare(static_cast<float>(kSampleRate));
    for (auto& early : early_reflections_)
        (void)early.prepare(static_cast<float>(kSampleRate), 80.0F);
    (void)late_reverb_.prepare(static_cast<float>(kSampleRate), scene_.room.late_rt60_s);
    std::string ignored;
    (void)set_scene(scene_, ignored);
}

SpatialAudioEngine::~SpatialAudioEngine() { stop_audio(); }

bool SpatialAudioEngine::prepare_scene(const SceneConfigV2& scene, PreparedScene& prepared,
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
    prepared.source_count = scene.audio.input_layout == InputLayout::surround_5_1 ? kDirectionalSourceCount : 2U;
    for (std::size_t index = 0; index < kDirectionalSourceCount; ++index) {
        prepared.speaker_positions[index] = scene.speakers[index].position_m;
        prepared.speaker_gains[index] = index < prepared.source_count && scene.speakers[index].enabled
                                            ? db_to_linear(scene.speakers[index].gain_db)
                                            : 0.0F;
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
    prepared.lfe_gain_db = scene.lfe.gain_db;
    prepared.eq_enabled = scene.headphone_eq.enabled;
    prepared.phantom_centre_compensation = scene.hrtf.phantom_centre_compensation;
    prepared.lfe_enabled = scene.audio.input_layout == InputLayout::surround_5_1 && scene.lfe.enabled;
    prepared.room_enabled = scene.room.enabled;
    prepared.late_reverb_enabled = scene.room.late_reverb_enabled;
    prepared.bypass = scene.audio.bypass;

    if (scene.room.enabled) {
        ImageSourceModel image_sources;
        for (std::size_t source = 0; source < prepared.source_count; ++source) {
            if (!scene.speakers[source].enabled) continue;
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

bool SpatialAudioEngine::set_scene(const SceneConfigV2& scene, std::string& error) {
    std::scoped_lock lock(control_mutex_);
    if (window_audio_config_.enabled &&
        scene.audio.input_layout != InputLayout::stereo) {
        error =
            "the 5.1 input layout cannot be enabled while window spatialization is active";
        return false;
    }
    if (backend_->running() && scene.audio.input_layout != scene_.audio.input_layout) {
        error = "input layout changes require an audio backend restart";
        return false;
    }
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

SceneConfigV2 SpatialAudioEngine::scene() const {
    std::scoped_lock lock(control_mutex_);
    return scene_;
}

bool SpatialAudioEngine::set_window_spatialization(const WindowAudioConfig& config,
                                                   std::string& error) {
    if (!validate_window_audio_config(config, error))
        return false;

    WindowAudioConfig previous{};
    WindowAudioConfig previous_runtime{};
    WindowAudioConfig runtime = config;
    const AudioBackendDiagnostics backend_diagnostics = backend_->diagnostics();
    bool audio_running = false;
    {
        std::scoped_lock lock(control_mutex_);
        if (config.enabled && scene_.audio.input_layout != InputLayout::stereo) {
            error = "window spatialization requires the stereo input layout";
            return false;
        }
        previous = window_audio_config_;
        previous_runtime = previous;
        runtime.discovery_endpoint_id =
            !backend_diagnostics.capture_endpoint_id.empty()
                ? backend_diagnostics.capture_endpoint_id
                : scene_.audio.capture_endpoint_id.value_or("");
        runtime.listener_position_m = scene_.listener.position_m;
        previous_runtime.discovery_endpoint_id =
            runtime.discovery_endpoint_id;
        previous_runtime.listener_position_m = runtime.listener_position_m;
#if defined(_WIN32)
        runtime.excluded_process_id = GetCurrentProcessId();
        previous_runtime.excluded_process_id = runtime.excluded_process_id;
#else
        runtime.excluded_process_id = 0;
        previous_runtime.excluded_process_id = 0;
#endif
        audio_running = backend_->running();
    }

    if (audio_running) {
        if (!config.enabled) {
            window_audio_requested_.store(false, std::memory_order_release);
            window_audio_rendering_status_.store(false,
                                                 std::memory_order_release);
            window_audio_capture_->stop();
        } else {
            const bool applied_live =
                previous.enabled && window_audio_capture_->running() &&
                window_audio_capture_->reconfigure(runtime);
            if (!applied_live) {
                window_audio_requested_.store(false, std::memory_order_release);
                window_audio_rendering_status_.store(
                    false, std::memory_order_release);
                window_audio_capture_->stop();
                if (!window_audio_capture_->start(runtime)) {
                    const WindowAudioDiagnostics diagnostics =
                        window_audio_capture_->diagnostics();
                    const auto diagnostic_text = [&diagnostics] {
                        const auto end = std::find(
                            diagnostics.last_error.begin(),
                            diagnostics.last_error.end(), '\0');
                        return std::string(diagnostics.last_error.begin(), end);
                    }();

                    if (previous.enabled &&
                        window_audio_capture_->start(previous_runtime)) {
                        window_audio_requested_.store(
                            true, std::memory_order_release);
                    }
                    error =
                        diagnostic_text.empty()
                            ? "window process-loopback capture could not start"
                            : "window process-loopback capture could not start: " +
                                  diagnostic_text;
                    return false;
                }
            }
        }
    }

    {
        std::scoped_lock lock(control_mutex_);
        window_audio_config_ = config;
    }
    window_audio_requested_.store(config.enabled && audio_running,
                                  std::memory_order_release);
    error.clear();
    return true;
}

WindowAudioConfig SpatialAudioEngine::window_spatialization() const {
    std::scoped_lock lock(control_mutex_);
    return window_audio_config_;
}

void SpatialAudioEngine::submit_head_pose(const HeadPoseSampleV1& pose) noexcept { pose_mailbox_.store(pose); }

void SpatialAudioEngine::set_virtual_endpoint_id(std::string endpoint_id) {
    std::scoped_lock lock(control_mutex_);
    virtual_endpoint_id_ = std::move(endpoint_id);
}

bool SpatialAudioEngine::start_audio(std::string& error) {
    AudioBackendConfig config{};
    WindowAudioConfig window_config{};
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
        config.input_layout = scene_.audio.input_layout;
        config.mode = scene_.audio.mode;
        config.requested_buffer_frames = scene_.audio.buffer_frames;
        window_config = window_audio_config_;
        window_config.discovery_endpoint_id = config.capture_endpoint_id;
        window_config.listener_position_m = scene_.listener.position_m;
#if defined(_WIN32)
        window_config.excluded_process_id = GetCurrentProcessId();
#endif
    }
    const auto start_window_capture = [this, &window_config, &error]() {
        if (!window_config.enabled) {
            window_audio_capture_->stop();
            window_audio_requested_.store(false, std::memory_order_release);
            window_audio_rendering_status_.store(false,
                                                 std::memory_order_release);
            return true;
        }
        if (window_audio_capture_->running()) {
            window_audio_requested_.store(true, std::memory_order_release);
            return true;
        }
        window_config.discovery_endpoint_id =
            backend_->diagnostics().capture_endpoint_id;
        if (window_config.discovery_endpoint_id.empty()) {
            error =
                "window process-loopback capture cannot resolve the active "
                "capture render endpoint";
            window_audio_requested_.store(false, std::memory_order_release);
            window_audio_rendering_status_.store(false,
                                                 std::memory_order_release);
            return false;
        }
        if (window_audio_capture_->start(window_config)) {
            window_audio_requested_.store(true, std::memory_order_release);
            return true;
        }
        const WindowAudioDiagnostics diagnostics = window_audio_capture_->diagnostics();
        const auto end =
            std::find(diagnostics.last_error.begin(), diagnostics.last_error.end(), '\0');
        const std::string detail(diagnostics.last_error.begin(), end);
        error = detail.empty() ? "window process-loopback capture could not start"
                               : "window process-loopback capture could not start: " + detail;
        window_audio_requested_.store(false, std::memory_order_release);
        window_audio_rendering_status_.store(false,
                                             std::memory_order_release);
        return false;
    };
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
        if (!start_window_capture()) {
            backend_->stop();
            std::scoped_lock lock(control_mutex_);
            effective_audio_mode_.reset();
            return false;
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
    SceneConfigV2 fallback = scene();
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
    if (!start_window_capture()) {
        backend_->stop();
        {
            std::scoped_lock lock(control_mutex_);
            effective_audio_mode_.reset();
        }
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
    window_audio_requested_.store(false, std::memory_order_release);
    window_audio_rendering_status_.store(false, std::memory_order_release);
    window_audio_capture_->stop();
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
    case EngineCommandType::set_scene: {
        const SceneConfigV2 previous = scene();
        if (command.scene.audio.input_layout == previous.audio.input_layout)
            return set_scene(command.scene, error);
        if (!validate_scene_config(command.scene, error)) return false;
        const bool was_running = backend_->running();
        if (was_running) stop_audio();
        if (set_scene(command.scene, error) && (!was_running || start_audio(error))) return true;

        const std::string layout_error = error;
        stop_audio();
        std::string restore_error;
        const bool scene_restored = set_scene(previous, restore_error);
        const bool audio_restored = scene_restored && (!was_running || start_audio(restore_error));
        error = "input layout change rejected: " + layout_error;
        if (!scene_restored || !audio_restored)
            error += "; previous scene restoration failed: " + restore_error;
        return false;
    }
    case EngineCommandType::set_bypass: {
        SceneConfigV2 updated = scene();
        updated.audio.bypass = command.bool_value;
        return set_scene(updated, error);
    }
    case EngineCommandType::set_output_device: {
        const bool was_running = backend_->running();
        if (was_running)
            stop_audio();
        SceneConfigV2 updated = scene();
        updated.audio.output_device_id =
            command.string_value.empty() ? std::nullopt : std::optional<std::string>(command.string_value);
        if (!set_scene(updated, error))
            return false;
        return !was_running || start_audio(error);
    }
    case EngineCommandType::set_audio_mode: {
        const SceneConfigV2 previous = scene();
        const bool was_running = backend_->running();
        if (was_running)
            stop_audio();
        SceneConfigV2 updated = previous;
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
        SceneConfigV2 updated = scene();
        updated.listener.neutral_orientation = command.quaternion_value.normalized_value();
        return set_scene(updated, error);
    }
    case EngineCommandType::set_hrtf: {
        SceneConfigV2 updated = scene();
        updated.hrtf.profile_id = command.string_value;
        updated.hrtf.sofa_path = command.optional_string_value;
        return set_scene(updated, error);
    }
    case EngineCommandType::set_headphone_eq: {
        SceneConfigV2 updated = scene();
        updated.headphone_eq = command.headphone_eq;
        return set_scene(updated, error);
    }
    case EngineCommandType::set_audio_route: {
        const SceneConfigV2 previous = scene();
        SceneConfigV2 updated = previous;
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
    case EngineCommandType::set_window_spatialization: {
        const ParseResult<WindowAudioConfig> parsed =
            window_audio_config_from_json(command.string_value);
        if (!parsed) {
            error = parsed.error;
            return false;
        }
        return set_window_spatialization(*parsed.value, error);
    }
    }
    error = "unsupported engine command";
    return false;
}

void SpatialAudioEngine::apply_prepared_scene(const PreparedScene& prepared, bool realtime_transition) noexcept {
    const bool input_layout_changed = active_scene_.source_count != prepared.source_count;
    active_scene_ = prepared;
    if (input_layout_changed) {
        binaural_detector_.reset();
        potentially_binaural_.store(false, std::memory_order_relaxed);
    }
    const std::span<const BiquadParameters> eq_sections(active_scene_.eq_sections.data(),
                                                        active_scene_.eq_section_count);
    if (!headphone_eq_.configuration_matches(eq_sections, static_cast<float>(kSampleRate))) {
        (void)headphone_eq_.configure(eq_sections, static_cast<float>(kSampleRate));
    }
    headphone_eq_.set_enabled(active_scene_.eq_enabled);
    // The provider owns its own neutralization curve; switching profiles swaps
    // it. An empty span falls back to a unit impulse, so a provider that needs
    // no correction and a profile change take the same crossfade.
    const std::span<const float> diffuse_field = active_scene_.hrtf != nullptr
                                                     ? active_scene_.hrtf->diffuse_field_filter()
                                                     : std::span<const float>{};
    diffuse_field_eq_.set_filter(diffuse_field, realtime_transition ? 4'800U : 0U);
    if (!realtime_transition) diffuse_field_eq_.reset();
    if (realtime_transition) {
        bypass_crossfade_.set_bypassed(active_scene_.bypass, 480U);
    } else {
        bypass_crossfade_.reset(active_scene_.bypass);
    }
    // Master gain and protection are common to both branches. EQ preamp belongs
    // exclusively to the processed branch so a true bypass never inherits its
    // attenuation during or after the crossfade.
    limiter_.set_master_gain_db(active_scene_.master_gain_db);
    lfe_renderer_.configure(active_scene_.lfe_enabled, active_scene_.lfe_gain_db,
                            realtime_transition ? 4'800U : 0U);
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
    request.world_to_head = world_to_head;
    if (active_window_audio_rendering_) {
        // Keep sparse slot indices stable, but do not make the real-time
        // convolver visit the unused tail up to the eight-application ceiling.
        // The permanent endpoint fallback occupies 0/1; the highest active
        // process slot determines the smallest safe bank above that.
        request.source_count =
            std::max<std::size_t>(2U, active_window_source_count_);
        request.room_enabled = false;
        std::copy(active_window_gains_.begin(), active_window_gains_.end(),
                  request.speaker_gains.begin());
        for (std::size_t index = kFirstWindowBinauralSource;
             index < active_window_source_count_; ++index) {
            const Vec3f world_direction =
                active_window_positions_[index] - active_scene_.listener_position;
            request.head_relative_directions[index] =
                rotate(world_to_head, world_direction);
        }
        // Preserve the endpoint at permanent indices 0/1 inside every window
        // bank. Application holes at 2..17 remain true null HRIRs.
        request.speaker_gains[kEndpointFallbackLeftSource] =
            active_scene_.speaker_gains[0];
        request.speaker_gains[kEndpointFallbackRightSource] =
            active_scene_.speaker_gains[1];
        request.head_relative_directions[kEndpointFallbackLeftSource] =
            rotate(world_to_head,
                   active_scene_.speaker_positions[0] -
                       active_scene_.listener_position);
        request.head_relative_directions[kEndpointFallbackRightSource] =
            rotate(world_to_head,
                   active_scene_.speaker_positions[1] -
                       active_scene_.listener_position);
    } else {
        std::copy(active_scene_.speaker_gains.begin(),
                  active_scene_.speaker_gains.end(),
                  request.speaker_gains.begin());
        request.source_count = active_scene_.source_count;
        request.room_enabled =
            active_scene_.room_enabled && !active_window_audio_enabled_;
        for (std::size_t index = 0; index < active_scene_.source_count; ++index) {
            const Vec3f world_direction =
                active_scene_.speaker_positions[index] -
                active_scene_.listener_position;
            request.head_relative_directions[index] =
                rotate(world_to_head, world_direction);
        }
    }
    if (active_scene_.phantom_centre_compensation) {
        // Only genuine stereo emitter pairs comb. In endpoint mode that is the
        // front left/right pair; the 5.1 centre and surrounds are not a pair in
        // that sense. In window mode every application owns a stereo pair, and
        // the endpoint fallback keeps its own at indices 0/1.
        const std::size_t pairs =
            std::min(request.source_count / 2, PhantomCentreCompensator::kPairCount);
        request.compensated_pair_mask =
            active_window_audio_rendering_ ? (1U << pairs) - 1U : 0x1U;
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
            if (first_filters_for_scene &&
                window_filter_handoff_waiting_) {
                // Both transport directions are guarded by an explicitly
                // renderable endpoint path. Keep their filter morph on the same
                // bounded horizon as the PCM transport fade.
                morph_frames = kWindowTransportCrossfadeFrames;
            } else if (filters_initialized_) {
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

void SpatialAudioEngine::process_chunk(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                                       double render_time_seconds) noexcept {
    bool render_window_audio = false;
    std::size_t active_window_application_count = 0;
    bool window_topology_changed = false;
    bool window_filter_parameters_changed = false;
    const bool previous_window_audio_rendering =
        active_window_audio_rendering_;
    const bool previous_handoff_waiting =
        window_filter_handoff_waiting_;
    if (active_window_audio_enabled_) {
        const WindowAudioRealtimeSnapshot realtime =
            window_audio_capture_->realtime_snapshot();
        const std::size_t application_count =
            std::min(realtime.count, kMaximumWindowAudioApplications);
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            directional_input_[frame].sources.fill(0.0F);
            bypass_dry_[frame] = {};
            detector_input_[frame] = {};
            lfe_bed_[frame] = 0.0F;
        }

        const std::size_t previous_source_count = active_window_source_count_;
        const auto previous_positions = active_window_positions_;
        const auto previous_gains = active_window_gains_;
        const auto previous_handles = active_window_handles_;
        active_window_source_count_ = 0;
        active_window_positions_.fill(Vec3f{});
        active_window_gains_.fill(0.0F);
        active_window_handles_.fill(WindowAudioSlotHandle{});

        for (std::size_t application = 0; application < application_count; ++application) {
            const WindowAudioSlotHandle handle = realtime.handles[application];
            if (!handle.valid()) continue;
            const std::size_t slot = handle.slot;
            std::fill_n(window_source_input_[slot].data(), frame_count,
                        StereoFrame{});
            const WindowAudioPullResult pulled =
                window_audio_capture_->pull(handle, window_source_input_[slot].data(),
                                            frame_count);
            // `capturing` is published before the first process-loopback packet
            // arrives. Keep the endpoint fallback until the ASRC can supply a
            // complete first block. After that first block, a transient
            // underrun is an audio dropout, not a topology mutation: retain
            // the stable handle and let the missing tail remain silent.
            if (!pulled.active) {
                if (primed_window_handles_[slot] == handle)
                    primed_window_handles_[slot] =
                        WindowAudioSlotHandle{};
                continue;
            }
            if (primed_window_handles_[slot] != handle) {
                if (pulled.received_frames != frame_count) continue;
                primed_window_handles_[slot] = handle;
            }

            // A process capture slot owns a stable HRTF pair. Never compact
            // indices when a lower slot disappears: doing so would feed an
            // application through another application's still-active filters
            // while the asynchronous replacement bank is prepared.
            const std::size_t left_source =
                kFirstWindowBinauralSource + slot * 2U;
            const std::size_t right_source = left_source + 1U;
            if (right_source >= kMaximumBinauralSources) continue;
            active_window_handles_[slot] = handle;
            active_window_positions_[left_source] =
                pulled.placement.left_position_m;
            active_window_positions_[right_source] =
                pulled.placement.right_position_m;
            active_window_gains_[left_source] = pulled.placement.gain_linear;
            active_window_gains_[right_source] = pulled.placement.gain_linear;
            ++active_window_application_count;

            for (std::size_t frame = 0; frame < frame_count; ++frame) {
                const StereoFrame source = window_source_input_[slot][frame];
                directional_input_[frame].sources[left_source] = source.left;
                directional_input_[frame].sources[right_source] = source.right;
                bypass_dry_[frame].left +=
                    source.left * pulled.placement.gain_linear;
                bypass_dry_[frame].right +=
                    source.right * pulled.placement.gain_linear;
            }
            active_window_source_count_ =
                std::max(active_window_source_count_, right_source + 1U);
        }

        // Process-loopback is all-or-nothing. The endpoint mix contains every
        // application, so partially replacing it would either mute an
        // uncovered session or duplicate the covered sessions. Continue
        // pulling every slot above to drain/prime its FIFO, but only hand over
        // once discovery confirms complete active-session coverage.
        // Re-snapshot after draining every process FIFO. A capture thread can
        // revoke coverage between the initial snapshot and its pull; that veto
        // must restore the endpoint in this callback rather than one callback
        // later.
        const WindowAudioRealtimeSnapshot post_pull_realtime =
            window_audio_capture_->realtime_snapshot();
        render_window_audio =
            realtime.sequence == post_pull_realtime.sequence &&
            post_pull_realtime.coverage_complete &&
            !post_pull_realtime.endpoint_fallback_requested &&
            active_window_application_count ==
                post_pull_realtime.required_active_captures &&
            active_window_application_count != 0U &&
            active_window_source_count_ != 0U;
        if (render_window_audio) {
            window_topology_changed =
                previous_source_count != active_window_source_count_;
            for (std::size_t slot = 0;
                 !window_topology_changed &&
                 slot < active_window_handles_.size();
                 ++slot) {
                window_topology_changed =
                    active_window_handles_[slot] != previous_handles[slot];
            }
            for (std::size_t source = 0;
                 !window_filter_parameters_changed &&
                 source < active_window_source_count_;
                 ++source) {
                const Vec3f delta =
                    active_window_positions_[source] - previous_positions[source];
                window_filter_parameters_changed =
                    length(delta) > 0.001F ||
                    std::abs(active_window_gains_[source] -
                             previous_gains[source]) > 0.0001F;
            }
            filter_request_dirty_ = filter_request_dirty_ ||
                                    window_topology_changed ||
                                    window_filter_parameters_changed;
        }
    }

    const bool window_rendering_changed =
        render_window_audio != active_window_audio_rendering_;
    if (window_rendering_changed ||
        (render_window_audio && window_topology_changed)) {
        const bool returning_to_endpoint =
            previous_window_audio_rendering && !previous_handoff_waiting;
        if (returning_to_endpoint && last_output_frame_valid_) {
            // The old process stream may have disappeared already (capture
            // failure, route change, explicit disable). A zero-latency
            // de-click therefore bridges from the last sample actually sent
            // to the new endpoint path instead of pretending future process
            // PCM is still available.
            window_output_declick_hold_ = last_output_frame_;
            window_output_declick_remaining_ =
                kWindowTransportCrossfadeFrames;
        }
        active_window_audio_rendering_ = render_window_audio;
        window_transport_crossfade_remaining_ = 0U;
        ++active_scene_revision_;
        window_filter_handoff_waiting_ = true;
        window_filter_handoff_revision_ = active_scene_revision_;
        filter_request_dirty_ = true;
        binaural_detector_.reset();
        potentially_binaural_.store(false, std::memory_order_relaxed);
    }

    if (!render_window_audio) {
        for (std::size_t index = 0; index < frame_count; ++index) {
            const ProgrammeFrame& programme = input[index];
            DirectionalFrame& directional = directional_input_[index];
            directional.sources = {
                programme.front_left,
                programme.front_right,
                active_scene_.source_count > 2 ? programme.front_center : 0.0F,
                active_scene_.source_count > 2 ? programme.surround_left : 0.0F,
                active_scene_.source_count > 2 ? programme.surround_right : 0.0F,
            };
            const float lfe =
                active_scene_.source_count > 2 ? programme.lfe : 0.0F;
            lfe_bed_[index] = lfe_renderer_.process_sample(lfe);
            bypass_dry_[index] =
                downmix_programme_to_stereo(programme, lfe_bed_[index]);
            detector_input_[index] = {programme.front_left,
                                      programme.front_right};
        }
    }
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

    if (window_filter_handoff_waiting_ && filters_initialized_ &&
        filter_scene_revision_ == window_filter_handoff_revision_) {
        // This is the explicit worker/convolver acknowledgement. Starting the
        // transport fade from elapsed time alone is unsafe because a sparse
        // slot above zero is absent from the old two-source bank.
        window_filter_handoff_waiting_ = false;
        window_transport_crossfade_remaining_ =
            kWindowTransportCrossfadeFrames;
    }

    if (window_filter_handoff_waiting_) {
        // Keep the endpoint as the sole input while the worker is preparing the
        // matching bank. Both endpoint and window banks reserve 0/1 for this
        // path, while process applications start at index 2. Process FIFOs were
        // already pulled above.
        for (std::size_t index = 0; index < frame_count; ++index) {
            directional_input_[index].sources.fill(0.0F);
            directional_input_[index].sources[0] =
                input[index].front_left;
            directional_input_[index].sources[1] =
                input[index].front_right;
            bypass_dry_[index] = {input[index].front_left,
                                  input[index].front_right};
        }
    } else if (window_transport_crossfade_remaining_ != 0U) {
        // The endpoint loopback and process-loopback streams are not guaranteed
        // to be phase-aligned. The forward transition uses an equal-power PCM
        // fade; the reverse transition is covered by the bounded output
        // de-click because the process stream may already be unavailable.
        for (std::size_t index = 0; index < frame_count; ++index) {
            if (window_transport_crossfade_remaining_ == 0U)
                break;
            const float progress =
                1.0F -
                static_cast<float>(window_transport_crossfade_remaining_) /
                    static_cast<float>(kWindowTransportCrossfadeFrames);
            const float endpoint_gain =
                std::cos(progress * kPi * 0.5F);
            const float process_gain =
                std::sin(progress * kPi * 0.5F);
            DirectionalFrame& directional = directional_input_[index];
            if (render_window_audio) {
                for (std::size_t source = kFirstWindowBinauralSource;
                     source < kMaximumBinauralSources; ++source) {
                    directional.sources[source] *= process_gain;
                }
                directional.sources[kEndpointFallbackLeftSource] =
                    input[index].front_left * endpoint_gain;
                directional.sources[kEndpointFallbackRightSource] =
                    input[index].front_right * endpoint_gain;
                bypass_dry_[index].left =
                    bypass_dry_[index].left * process_gain +
                    input[index].front_left * endpoint_gain;
                bypass_dry_[index].right =
                    bypass_dry_[index].right * process_gain +
                    input[index].front_right * endpoint_gain;
            } else {
                // Process PCM is no longer guaranteed to exist. Both banks
                // render the endpoint from the same permanent indices.
                directional.sources[kEndpointFallbackLeftSource] =
                    input[index].front_left;
                directional.sources[kEndpointFallbackRightSource] =
                    input[index].front_right;
                bypass_dry_[index] = {input[index].front_left,
                                      input[index].front_right};
            }
            --window_transport_crossfade_remaining_;
        }
    }
    window_audio_rendering_status_.store(
        render_window_audio && !window_filter_handoff_waiting_,
        std::memory_order_release);
    const StereoFrame* dry_input = bypass_dry_.data();

    if (!active_window_audio_enabled_ && active_scene_.source_count == 2) {
        binaural_detector_.process(detector_input_.data(), frame_count);
        potentially_binaural_.store(binaural_detector_.potentially_binaural(), std::memory_order_relaxed);
    } else {
        potentially_binaural_.store(false, std::memory_order_relaxed);
    }

    convolver_.process(directional_input_.data(), output, frame_count);
    if (!active_window_audio_enabled_ && active_scene_.room_enabled &&
        active_scene_.room_mix > 0.0F) {
        for (std::size_t index = 0; index < frame_count; ++index) room_early_[index].fill(0.0F);
        for (std::size_t source = 0; source < active_scene_.source_count; ++source) {
            for (std::size_t index = 0; index < frame_count; ++index)
                room_mono_[index] = directional_input_[index].sources[source] * active_scene_.speaker_gains[source];
            early_reflections_[source].process(room_mono_.data(), room_early_secondary_.data(), frame_count);
            for (std::size_t index = 0; index < frame_count; ++index) {
                for (std::size_t channel = 0; channel < EarlyReflectionProcessor::kAmbisonicChannels; ++channel)
                    room_early_[index][channel] += room_early_secondary_[index][channel];
            }
        }
        for (std::size_t index = 0; index < frame_count; ++index) {
            float late_input = 0.0F;
            for (std::size_t source = 0; source < active_scene_.source_count; ++source)
                late_input += directional_input_[index].sources[source] * active_scene_.speaker_gains[source];
            room_mono_[index] = late_input / static_cast<float>(active_scene_.source_count);
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
    // Everything above this point went through an HRTF, direct or reflected, so
    // it all carries the set's common transfer function exactly once. The LFE
    // bed below is head-invariant and must not be equalized.
    diffuse_field_eq_.process(output, frame_count);
    constexpr float equal_power_mono = 0.70710678F;
    for (std::size_t index = 0; index < frame_count; ++index) {
        const float lfe = lfe_bed_[index] * equal_power_mono;
        output[index].left += lfe;
        output[index].right += lfe;
    }
    headphone_eq_.process(output, frame_count);
    if (active_scene_.eq_enabled) {
        const float eq_preamp = db_to_linear(active_scene_.eq_preamp_db);
        for (std::size_t index = 0; index < frame_count; ++index) {
            output[index].left *= eq_preamp;
            output[index].right *= eq_preamp;
        }
    }
    bypass_crossfade_.process(dry_input, output, frame_count);
    limiter_.process(output, frame_count);
    for (std::size_t index = 0; index < frame_count; ++index) {
        if (window_output_declick_remaining_ != 0U) {
            const float progress =
                1.0F -
                static_cast<float>(window_output_declick_remaining_) /
                    static_cast<float>(kWindowTransportCrossfadeFrames);
            const float previous_gain = 1.0F - progress;
            output[index].left =
                window_output_declick_hold_.left * previous_gain +
                output[index].left * progress;
            output[index].right =
                window_output_declick_hold_.right * previous_gain +
                output[index].right * progress;
            --window_output_declick_remaining_;
        }
        last_output_frame_ = output[index];
        last_output_frame_valid_ = true;
    }
}

void SpatialAudioEngine::process_audio(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                                       std::int64_t render_qpc) noexcept {
    if (output == nullptr)
        return;
    PreparedScene prepared{};
    if (scene_commands_.try_pop(prepared))
        apply_prepared_scene(prepared, true);
    const bool requested_window_audio =
        window_audio_requested_.load(std::memory_order_acquire);
    if (requested_window_audio != active_window_audio_enabled_) {
        active_window_audio_enabled_ = requested_window_audio;
        active_window_source_count_ = 0;
        active_window_positions_.fill(Vec3f{});
        active_window_gains_.fill(0.0F);
        active_window_handles_.fill(WindowAudioSlotHandle{});
        primed_window_handles_.fill(WindowAudioSlotHandle{});
        window_filter_handoff_waiting_ = false;
        window_filter_handoff_revision_ = 0U;
        window_transport_crossfade_remaining_ = 0U;
        ++active_scene_revision_;
        filter_request_dirty_ = true;
        for (auto& reflections : early_reflections_) reflections.reset();
        late_reverb_.reset();
        room_decoder_.reset();
        room_filters_initialized_ = false;
        room_filter_scene_revision_ = 0;
    }
    const double render_time = qpc_to_seconds(render_qpc != 0 ? render_qpc : current_qpc());
    std::size_t offset = 0;
    while (offset < frame_count) {
        const std::size_t chunk = std::min(kMaximumProcessChunk, frame_count - offset);
        if (input == nullptr) {
            std::array<ProgrammeFrame, kMaximumProcessChunk> silence{};
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
    const WindowAudioSnapshot window_snapshot = window_audio_capture_->snapshot();
    const WindowAudioDiagnostics window_diagnostics =
        window_audio_capture_->diagnostics();
    EngineStatusV1 result{};
    result.capture_state = audio.capture_state;
    result.render_state = audio.render_state;
    result.tracking_state = static_cast<TrackingState>(tracking_state_.load(std::memory_order_relaxed));
    result.capture_sample_rate = audio.capture_sample_rate;
    result.capture_channels = audio.capture_channels;
    result.capture_channel_mask = audio.capture_channel_mask;
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
    result.window_audio_source_count =
        static_cast<std::uint32_t>(window_diagnostics.active_slots);
    result.window_audio_json =
        window_audio_status_to_json(window_snapshot, window_diagnostics);
    {
        std::scoped_lock lock(control_mutex_);
        result.audio_mode = effective_audio_mode_.value_or(scene_.audio.mode);
        result.input_layout = scene_.audio.input_layout;
        result.window_audio_enabled = window_audio_config_.enabled;
        result.window_audio_rendering =
            window_audio_rendering_status_.load(std::memory_order_acquire);
        result.last_error = audio.last_error;
        const auto append_warning = [&result](const std::string& warning) {
            if (warning.empty()) return;
            if (!result.last_error.empty()) result.last_error += "; ";
            result.last_error += warning;
        };
        append_warning(audio_mode_warning_);
        append_warning(hrtf_warning_);
        bool window_audio_warning = false;
        if (window_audio_config_.enabled &&
            window_diagnostics.last_error[0] != '\0') {
            const auto end = std::find(window_diagnostics.last_error.begin(),
                                       window_diagnostics.last_error.end(), '\0');
            append_warning(std::string(window_diagnostics.last_error.begin(), end));
            window_audio_warning = true;
        }
        if (!audio_mode_warning_.empty() || !hrtf_warning_.empty() ||
            window_audio_warning) {
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
