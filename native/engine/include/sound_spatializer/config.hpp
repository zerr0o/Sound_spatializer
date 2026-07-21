#pragma once

#include "sound_spatializer/types.hpp"

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

[[nodiscard]] bool validate_scene_config(const SceneConfigV1& scene, std::string& error) noexcept;
[[nodiscard]] std::string scene_config_to_json(const SceneConfigV1& scene);
[[nodiscard]] ParseResult<SceneConfigV1> scene_config_from_json(std::string_view json) noexcept;

[[nodiscard]] std::string engine_command_to_json(const EngineCommandV1& command);
[[nodiscard]] ParseResult<EngineCommandV1> engine_command_from_json(std::string_view json) noexcept;
[[nodiscard]] std::string engine_status_to_json(const EngineStatusV1& status);

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path base_directory = default_base_directory());

    [[nodiscard]] bool save_scene(const SceneConfigV1& scene, std::string& error) const noexcept;
    [[nodiscard]] ParseResult<SceneConfigV1> load_scene() const noexcept;
    [[nodiscard]] const std::filesystem::path& base_directory() const noexcept { return base_directory_; }
    [[nodiscard]] std::filesystem::path scene_path() const { return base_directory_ / "scene-v1.json"; }

    [[nodiscard]] static std::filesystem::path default_base_directory();

private:
    std::filesystem::path base_directory_;
};

} // namespace sound_spatializer

