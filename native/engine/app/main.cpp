#include "sound_spatializer/config.hpp"
#include "sound_spatializer/engine.hpp"
#include "sound_spatializer/ipc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::atomic<bool> keep_running{true};

void signal_handler(int) { keep_running.store(false, std::memory_order_release); }

class RuntimeHandler final : public sound_spatializer::IpcMessageHandler {
public:
    RuntimeHandler(sound_spatializer::SpatialAudioEngine& engine, sound_spatializer::ConfigStore& config_store)
        : engine_(engine), config_store_(config_store) {}

    [[nodiscard]] CommandResult on_engine_command(
        const sound_spatializer::EngineCommandV1& command) override {
        std::string error;
        if (!engine_.execute_command(command, error)) {
            std::cerr << "Engine command rejected: " << error << '\n';
            return {false, false, std::move(error)};
        }
        bool persisted = true;
        // `start` can replace a requested exclusive mode with its effective
        // shared fallback, so it is scene-mutating for persistence purposes.
        if (command.type == sound_spatializer::EngineCommandType::start ||
            command.type == sound_spatializer::EngineCommandType::set_scene ||
            command.type == sound_spatializer::EngineCommandType::set_bypass ||
            command.type == sound_spatializer::EngineCommandType::set_output_device ||
            command.type == sound_spatializer::EngineCommandType::set_audio_mode ||
            command.type == sound_spatializer::EngineCommandType::calibrate_neutral ||
            command.type == sound_spatializer::EngineCommandType::set_hrtf ||
            command.type == sound_spatializer::EngineCommandType::set_headphone_eq ||
            command.type == sound_spatializer::EngineCommandType::set_audio_route) {
            if (!config_store_.save_scene(engine_.scene(), error)) {
                persisted = false;
                std::cerr << "Config save failed: " << error << '\n';
            }
        }
        if (command.type ==
            sound_spatializer::EngineCommandType::set_window_spatialization) {
            if (!config_store_.save_window_audio_config(
                    engine_.window_spatialization(), error)) {
                persisted = false;
                std::cerr << "Window spatialization config save failed: "
                          << error << '\n';
            }
        }
        return {true, persisted, persisted ? std::string{} : std::move(error)};
    }

    void on_head_pose(const sound_spatializer::HeadPoseSampleV1& pose) override { engine_.submit_head_pose(pose); }
    void on_ipc_error(std::string_view error) override { std::cerr << "IPC: " << error << '\n'; }

private:
    sound_spatializer::SpatialAudioEngine& engine_;
    sound_spatializer::ConfigStore& config_store_;
};

int run_engine(const std::vector<std::string>& arguments) {
    bool mock_audio = false;
    bool diagnostics_once = false;
    std::string virtual_endpoint_id;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument == "--mock-audio") mock_audio = true;
        else if (argument == "--diagnostics-once") diagnostics_once = true;
        else if (argument == "--autostart") {
            // The Windows GUI subsystem already keeps this launch silent. The flag is
            // explicit so startup registration remains stable and diagnosable.
        }
        else if (argument == "--virtual-endpoint" && index + 1 < arguments.size())
            virtual_endpoint_id = arguments[++index];
        else if (argument == "--help") {
            std::cout << "SoundSpatializer.Engine [--autostart] [--mock-audio] "
                         "[--virtual-endpoint <WASAPI id>] [--diagnostics-once]\n";
            return 0;
        }
    }

    sound_spatializer::SingleInstanceGuard instance;
    std::string instance_error;
    if (!instance.acquire(instance_error)) {
        if (!instance.already_running()) std::cerr << "Single-instance guard failed: " << instance_error << '\n';
        return instance.already_running() ? 0 : 1;
    }

    if (virtual_endpoint_id.empty()) {
        if (const char* environment_id = std::getenv("SOUND_SPATIALIZER_VIRTUAL_ENDPOINT_ID"))
            virtual_endpoint_id = environment_id;
    }

    auto backend = mock_audio ? std::unique_ptr<sound_spatializer::IAudioBackend>(
                                    std::make_unique<sound_spatializer::MockAudioBackend>())
                              : sound_spatializer::create_system_audio_backend();
    auto engine = std::make_unique<sound_spatializer::SpatialAudioEngine>(std::move(backend));
    engine->set_virtual_endpoint_id(std::move(virtual_endpoint_id));
    sound_spatializer::ConfigStore config_store;
    bool loaded_valid_scene = false;
    if (auto loaded = config_store.load_scene(); loaded) {
        std::string error;
        if (!engine->set_scene(*loaded.value, error)) std::cerr << "Stored scene rejected: " << error << '\n';
        else loaded_valid_scene = true;
    }
    if (auto loaded = config_store.load_window_audio_config(); loaded) {
        std::string error;
        if (!engine->set_window_spatialization(*loaded.value, error))
            std::cerr << "Stored window spatialization config rejected: "
                      << error << '\n';
    }

    if (diagnostics_once) {
        std::cout << sound_spatializer::engine_status_to_json(engine->status()) << '\n';
        return 0;
    }

    if (!mock_audio && loaded_valid_scene && engine->scene().audio.output_device_id) {
        const sound_spatializer::AudioMode stored_mode = engine->scene().audio.mode;
        std::string startup_error;
        if (!engine->start_audio(startup_error))
            std::cerr << "Automatic audio start failed (engine remains available): " << startup_error << '\n';
        else if (engine->scene().audio.mode != stored_mode) {
            std::string save_error;
            if (!config_store.save_scene(engine->scene(), save_error))
                std::cerr << "Audio fallback config save failed: " << save_error << '\n';
        }
    }

    RuntimeHandler handler(*engine, config_store);
    sound_spatializer::NamedPipeServer ipc(handler);
    std::string error;
    if (!ipc.start(error)) {
        std::cerr << "IPC server could not start: " << error << '\n';
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    while (keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::string publish_error;
        (void)ipc.publish_status(engine->status(), publish_error); // no connected UI is a normal state
    }
    ipc.stop();
    engine->stop_audio();
    return 0;
}

#if defined(_WIN32)

std::string utf8_from_wide(const wchar_t* input) {
    if (input == nullptr || *input == L'\0') return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, output.data(), required, nullptr, nullptr) == 0)
        return {};
    output.pop_back();
    return output;
}

void attach_parent_console_for_diagnostics(const std::vector<std::string>& arguments) {
    if (std::find(arguments.begin(), arguments.end(), "--autostart") != arguments.end()) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* ignored = nullptr;
    (void)freopen_s(&ignored, "CONOUT$", "w", stdout);
    (void)freopen_s(&ignored, "CONOUT$", "w", stderr);
    (void)freopen_s(&ignored, "CONIN$", "r", stdin);
}

#endif

} // namespace

#if defined(_WIN32)

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argument_count = 0;
    wchar_t** wide_arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (wide_arguments == nullptr) return 1;
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argument_count));
    for (int index = 0; index < argument_count; ++index) arguments.push_back(utf8_from_wide(wide_arguments[index]));
    LocalFree(wide_arguments);
    attach_parent_console_for_diagnostics(arguments);
    return run_engine(arguments);
}

#else

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    return run_engine(arguments);
}

#endif
