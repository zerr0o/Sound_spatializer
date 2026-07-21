#pragma once

#include "sound_spatializer/config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sound_spatializer {

inline constexpr std::uint32_t kMaximumJsonFrameBytes = 1U << 20U;

[[nodiscard]] constexpr bool pipe_has_complete_read(std::uint32_t available_bytes,
                                                    std::size_t required_bytes) noexcept {
    return static_cast<std::size_t>(available_bytes) >= required_bytes;
}

[[nodiscard]] constexpr bool pose_sequence_is_newer(bool have_previous,
                                                     std::uint64_t previous,
                                                     std::uint64_t candidate) noexcept {
    return !have_previous || candidate > previous;
}

[[nodiscard]] std::vector<std::byte> encode_json_frame(std::string_view json);

class JsonFrameDecoder {
public:
    [[nodiscard]] bool feed(std::span<const std::byte> bytes, std::vector<std::string>& completed_frames,
                            std::string& error);
    void reset() noexcept;

private:
    std::array<std::byte, 4> length_bytes_{};
    std::size_t length_bytes_received_{};
    std::uint32_t expected_payload_bytes_{};
    std::string payload_{};
};

class IpcMessageHandler {
public:
    virtual ~IpcMessageHandler() = default;
    virtual void on_engine_command(const EngineCommandV1& command) = 0;
    virtual void on_head_pose(const HeadPoseSampleV1& pose) = 0;
    virtual void on_ipc_error(std::string_view error) = 0;
};

class NamedPipeServer {
public:
    explicit NamedPipeServer(IpcMessageHandler& handler);
    ~NamedPipeServer();

    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;
    [[nodiscard]] bool publish_status(const EngineStatusV1& status, std::string& error);
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] static std::string endpoint_name();
    [[nodiscard]] static std::string pose_endpoint_name();

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

class SingleInstanceGuard {
public:
    SingleInstanceGuard();
    ~SingleInstanceGuard();
    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool acquire(std::string& error, std::string_view instance_suffix = "Engine.v1");
    [[nodiscard]] bool acquired() const noexcept;
    [[nodiscard]] bool already_running() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace sound_spatializer
