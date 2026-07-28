#pragma once

#include "sound_spatializer/acoustics.hpp"
#include "sound_spatializer/audio_backend.hpp"
#include "sound_spatializer/hrtf.hpp"
#include "sound_spatializer/hrtf_worker.hpp"
#include "sound_spatializer/latency_statistics.hpp"
#include "sound_spatializer/pose.hpp"
#include "sound_spatializer/window_audio.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sound_spatializer {

class SpatialAudioEngine final : public IAudioProcessor {
public:
    SpatialAudioEngine();
    explicit SpatialAudioEngine(std::unique_ptr<IAudioBackend> backend);
    SpatialAudioEngine(std::unique_ptr<IAudioBackend> backend,
                       std::unique_ptr<IWindowAudioCapture> window_audio_capture);
    SpatialAudioEngine(std::unique_ptr<IAudioBackend> backend,
                       std::unique_ptr<IWindowAudioCapture> window_audio_capture,
                       std::unique_ptr<IHrtfDatabase> builtin_hrtf);
    ~SpatialAudioEngine() override;

    SpatialAudioEngine(const SpatialAudioEngine&) = delete;
    SpatialAudioEngine& operator=(const SpatialAudioEngine&) = delete;

    [[nodiscard]] bool set_scene(const SceneConfigV2& scene, std::string& error);
    [[nodiscard]] SceneConfigV2 scene() const;
    void submit_head_pose(const HeadPoseSampleV1& pose) noexcept;
    void set_virtual_endpoint_id(std::string endpoint_id);

    [[nodiscard]] bool start_audio(std::string& error);
    void stop_audio() noexcept;
    [[nodiscard]] bool execute_command(const EngineCommandV1& command, std::string& error);
    [[nodiscard]] bool set_window_spatialization(const WindowAudioConfig& config,
                                                 std::string& error);
    [[nodiscard]] WindowAudioConfig window_spatialization() const;
    [[nodiscard]] EngineStatusV1 status() const;
    [[nodiscard]] std::size_t cached_personal_hrtf_count() const;

    void process_audio(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                       std::int64_t render_qpc) noexcept override;

private:
    struct PreparedScene {
        std::array<Vec3f, kDirectionalSourceCount> speaker_positions{};
        std::array<float, kDirectionalSourceCount> speaker_gains{};
        std::size_t source_count{2};
        Vec3f listener_position{};
        Quaternionf neutral_orientation{};
        std::array<BiquadParameters, kMaximumEqSections> eq_sections{};
        std::size_t eq_section_count{};
        std::array<std::array<ReflectionTap, EarlyReflectionProcessor::kMaximumReflectionTaps>,
                   kDirectionalSourceCount> early_reflections{};
        std::array<std::size_t, kDirectionalSourceCount> early_reflection_counts{};
        const IHrtfDatabase* hrtf{};
        MaterialBands late_rt60{};
        float master_gain_db{-6.0F};
        float eq_preamp_db{};
        float room_mix{};
        float prediction_limit_ms{20.0F};
        float lfe_gain_db{};
        bool eq_enabled{};
        bool phantom_centre_compensation{true};
        bool lfe_enabled{true};
        bool room_enabled{};
        bool late_reverb_enabled{};
        bool bypass{};
    };

    static_assert(std::is_trivially_copyable_v<PreparedScene>);

    [[nodiscard]] bool prepare_scene(const SceneConfigV2& scene, PreparedScene& prepared, std::string& warning_or_error,
                                     bool& is_error);
    void apply_prepared_scene(const PreparedScene& prepared, bool realtime_transition) noexcept;
    void request_hrtf_for_pose(const Quaternionf& orientation) noexcept;
    void consume_prepared_hrtf() noexcept;
    void reset_latency_diagnostics() noexcept;
    void process_chunk(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                       double render_time_seconds) noexcept;
    [[nodiscard]] static std::int64_t current_qpc() noexcept;
    [[nodiscard]] static double qpc_to_seconds(std::int64_t value) noexcept;

    std::unique_ptr<IAudioBackend> backend_{};
    std::unique_ptr<IWindowAudioCapture> window_audio_capture_{};
    mutable std::mutex control_mutex_{};
    SceneConfigV2 scene_{};
    WindowAudioConfig window_audio_config_{};
    std::string virtual_endpoint_id_{};
    std::string hrtf_warning_{};
    // Persistent, non-fatal diagnostic when a requested hardware audio mode
    // cannot be the effective mode. Guarded by control_mutex_.
    std::string audio_mode_warning_{};
    // The mode of the backend that was actually opened. This intentionally
    // remains independent from scene_.audio.mode: a delayed full-scene update
    // must not falsify hardware status while an existing stream is running.
    // Guarded by control_mutex_; empty while no backend opening is confirmed.
    std::optional<AudioMode> effective_audio_mode_{};
    std::vector<std::unique_ptr<IHrtfDatabase>> hrtf_lifetime_{};
    std::unordered_map<std::string, const IHrtfDatabase*> hrtf_cache_{};
    HrtfPreparationWorker hrtf_worker_{};
    static constexpr std::size_t kMaximumPersonalHrtfsPerProcess = 16;
    SpscRingBuffer<PreparedScene> scene_commands_{8};
    PreparedScene active_scene_{};
    AtomicPoseMailbox pose_mailbox_{};
    PosePredictor pose_predictor_{};
    std::uint64_t last_pose_sequence_{};
    std::atomic<float> tracking_hz_{};
    std::atomic<float> latency_p50_ms_{};
    std::atomic<float> latency_p95_ms_{};
    double previous_pose_time_seconds_{};
    float tracking_hz_smoothed_{};
    RealtimeLatencyPercentileWindow latency_percentiles_{};
    bool latency_tracking_continuous_{};
    Quaternionf last_requested_filter_orientation_{};
    bool filter_request_dirty_{true};
    bool filters_initialized_{};
    bool scene_filter_transition_active_{};
    bool room_filters_initialized_{};
    bool room_scene_filter_transition_active_{};
    std::uint64_t hrtf_request_generation_{};
    std::uint64_t active_scene_revision_{};
    std::uint64_t filter_scene_revision_{};
    std::uint64_t room_filter_scene_revision_{};
    std::atomic<std::uint32_t> tracking_state_{static_cast<std::uint32_t>(TrackingState::unavailable)};
    BinauralConvolver convolver_{};
    DiffuseFieldEqualizer diffuse_field_eq_{};
    BypassCrossfade bypass_crossfade_{};
    StereoParametricEq headphone_eq_{};
    TruePeakLimiter limiter_{};
    LfeRenderer lfe_renderer_{};
    PotentialBinauralDetector binaural_detector_{};
    std::atomic<bool> potentially_binaural_{};
    std::atomic<bool> window_audio_requested_{};
    std::atomic<bool> window_audio_rendering_status_{};
    bool active_window_audio_enabled_{};
    // The requested window mode and the source set that is safe to render are
    // deliberately separate. Process-loopback discovery/activation is
    // asynchronous; until at least one capture is actually ready, the endpoint
    // mix remains the audible fallback.
    bool active_window_audio_rendering_{};
    // PCM readiness is insufficient for a safe handoff: a sparse process slot
    // may not exist in the currently-active stereo HRTF bank. The endpoint
    // remains authoritative until the worker's matching scene revision has
    // been accepted by the convolver.
    bool window_filter_handoff_waiting_{};
    std::uint64_t window_filter_handoff_revision_{};
    static constexpr std::uint32_t kWindowTransportCrossfadeFrames = 240;
    std::uint32_t window_transport_crossfade_remaining_{};
    std::uint32_t window_output_declick_remaining_{};
    StereoFrame window_output_declick_hold_{};
    StereoFrame last_output_frame_{};
    bool last_output_frame_valid_{};
    std::size_t active_window_source_count_{};
    std::array<Vec3f, kMaximumBinauralSources> active_window_positions_{};
    std::array<float, kMaximumBinauralSources> active_window_gains_{};
    std::array<WindowAudioSlotHandle, kMaximumWindowAudioApplications>
        active_window_handles_{};
    // A generation joins the HRTF topology only after its first complete PCM
    // block. Once primed, a short ASRC underrun supplies silence without
    // tearing down and rebuilding the sparse filter bank.
    std::array<WindowAudioSlotHandle, kMaximumWindowAudioApplications>
        primed_window_handles_{};
    std::array<EarlyReflectionProcessor, kDirectionalSourceCount> early_reflections_{};
    AmbisonicBinauralDecoderOrder3 room_decoder_{};
    LateReverbFdn16 late_reverb_{};
    static constexpr std::size_t kMaximumProcessChunk = 2'048;
    std::array<float, kMaximumProcessChunk> room_mono_{};
    std::array<float, kMaximumProcessChunk> lfe_bed_{};
    std::array<std::array<float, LateReverbFdn16::kOutputChannels>, kMaximumProcessChunk> room_ambi_{};
    std::array<std::array<float, EarlyReflectionProcessor::kAmbisonicChannels>, kMaximumProcessChunk> room_early_{};
    std::array<std::array<float, EarlyReflectionProcessor::kAmbisonicChannels>, kMaximumProcessChunk>
        room_early_secondary_{};
    std::array<std::array<float, EarlyReflectionProcessor::kAmbisonicChannels>, kMaximumProcessChunk> room_combined_{};
    std::array<StereoFrame, kMaximumProcessChunk> room_binaural_{};
    std::array<StereoFrame, kMaximumProcessChunk> bypass_dry_{};
    std::array<StereoFrame, kMaximumProcessChunk> detector_input_{};
    std::array<DirectionalFrame, kMaximumProcessChunk> directional_input_{};
    std::array<std::array<StereoFrame, kMaximumProcessChunk>,
               kMaximumWindowAudioApplications>
        window_source_input_{};
};

} // namespace sound_spatializer
