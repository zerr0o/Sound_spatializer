#pragma once

#include "sound_spatializer/types.hpp"
#include "sound_spatializer/window_audio.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace sound_spatializer {

template <typename T>
struct ParseResult {
    std::optional<T> value{};
    std::string error{};

    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }
};

[[nodiscard]] bool validate_scene_config(const SceneConfigV2& scene, std::string& error) noexcept;
[[nodiscard]] std::string scene_config_to_json(const SceneConfigV2& scene);
[[nodiscard]] ParseResult<SceneConfigV2> scene_config_from_json(std::string_view json) noexcept;

[[nodiscard]] std::string engine_command_to_json(const EngineCommandV1& command);
// Best-effort extraction used by the IPC layer to reject a structurally valid
// command immediately even when the rest of its contract is invalid.
[[nodiscard]] ParseResult<std::uint32_t> engine_command_id_from_json(
    std::string_view json) noexcept;
[[nodiscard]] ParseResult<EngineCommandV1> engine_command_from_json(std::string_view json) noexcept;
[[nodiscard]] std::string engine_command_result_to_json(std::uint32_t command_id,
                                                        bool accepted,
                                                        bool persisted,
                                                        std::string_view error);
[[nodiscard]] std::string engine_status_to_json(const EngineStatusV1& status);

[[nodiscard]] bool validate_window_audio_config(const WindowAudioConfig& config,
                                                std::string& error) noexcept;
[[nodiscard]] std::string window_audio_config_to_json(const WindowAudioConfig& config);
[[nodiscard]] ParseResult<WindowAudioConfig> window_audio_config_from_json(
    std::string_view json) noexcept;
[[nodiscard]] std::string window_audio_status_to_json(
    const WindowAudioSnapshot& snapshot, const WindowAudioDiagnostics& diagnostics);

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path base_directory = default_base_directory());

    [[nodiscard]] bool save_scene(const SceneConfigV2& scene, std::string& error) const noexcept;
    [[nodiscard]] ParseResult<SceneConfigV2> load_scene() const noexcept;
    [[nodiscard]] bool save_window_audio_config(const WindowAudioConfig& config,
                                                std::string& error) const noexcept;
    [[nodiscard]] ParseResult<WindowAudioConfig> load_window_audio_config() const noexcept;
    [[nodiscard]] const std::filesystem::path& base_directory() const noexcept { return base_directory_; }
    [[nodiscard]] std::filesystem::path scene_path() const { return base_directory_ / "scene-v2.json"; }
    [[nodiscard]] std::filesystem::path legacy_scene_path() const { return base_directory_ / "scene-v1.json"; }
    [[nodiscard]] std::filesystem::path window_audio_path() const {
        return base_directory_ / "window-spatialization-v1.json";
    }

    [[nodiscard]] static std::filesystem::path default_base_directory();

private:
    std::filesystem::path base_directory_;
};

} // namespace sound_spatializer
