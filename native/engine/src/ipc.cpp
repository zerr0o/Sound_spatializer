#include "sound_spatializer/ipc.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>
#endif

namespace sound_spatializer {

std::vector<std::byte> encode_json_frame(std::string_view json) {
    if (json.size() > kMaximumJsonFrameBytes) {
        return {};
    }
    std::vector<std::byte> result(4 + json.size());
    const std::uint32_t length = static_cast<std::uint32_t>(json.size());
    result[0] = static_cast<std::byte>(length & 0xFFU);
    result[1] = static_cast<std::byte>((length >> 8U) & 0xFFU);
    result[2] = static_cast<std::byte>((length >> 16U) & 0xFFU);
    result[3] = static_cast<std::byte>((length >> 24U) & 0xFFU);
    std::memcpy(result.data() + 4, json.data(), json.size());
    return result;
}

bool JsonFrameDecoder::feed(std::span<const std::byte> bytes, std::vector<std::string>& completed_frames,
                            std::string& error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (length_bytes_received_ < length_bytes_.size()) {
            const std::size_t copied = std::min(length_bytes_.size() - length_bytes_received_, bytes.size() - offset);
            std::copy_n(bytes.data() + offset, copied, length_bytes_.data() + length_bytes_received_);
            length_bytes_received_ += copied;
            offset += copied;
            if (length_bytes_received_ != length_bytes_.size()) continue;
            expected_payload_bytes_ = static_cast<std::uint32_t>(std::to_integer<unsigned>(length_bytes_[0])) |
                                      (static_cast<std::uint32_t>(std::to_integer<unsigned>(length_bytes_[1])) << 8U) |
                                      (static_cast<std::uint32_t>(std::to_integer<unsigned>(length_bytes_[2])) << 16U) |
                                      (static_cast<std::uint32_t>(std::to_integer<unsigned>(length_bytes_[3])) << 24U);
            if (expected_payload_bytes_ == 0 || expected_payload_bytes_ > kMaximumJsonFrameBytes) {
                error = "invalid JSON frame length";
                reset();
                return false;
            }
            payload_.clear();
            payload_.reserve(expected_payload_bytes_);
        }
        const std::size_t copied = std::min<std::size_t>(expected_payload_bytes_ - payload_.size(), bytes.size() - offset);
        payload_.append(reinterpret_cast<const char*>(bytes.data() + offset), copied);
        offset += copied;
        if (payload_.size() == expected_payload_bytes_) {
            completed_frames.push_back(std::move(payload_));
            payload_.clear();
            expected_payload_bytes_ = 0;
            length_bytes_received_ = 0;
        }
    }
    error.clear();
    return true;
}

void JsonFrameDecoder::reset() noexcept {
    length_bytes_received_ = 0;
    expected_payload_bytes_ = 0;
    payload_.clear();
}

class NamedPipeServer::Implementation {
public:
    explicit Implementation(IpcMessageHandler& handler) : handler_(handler) {}
    ~Implementation() { stop(); }

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;
    [[nodiscard]] bool publish_status(const EngineStatusV1& status, std::string& error);
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
#if defined(_WIN32)
    void worker(std::stop_token stop_token);
    void pose_worker(std::stop_token stop_token);
    void dispatch_pose_packet(const HeadPosePacketV1& packet);
    void cancel_pipe_worker(std::jthread& thread, std::atomic<HANDLE>& pipe) noexcept;
    [[nodiscard]] bool read_exact(HANDLE pipe, void* destination, std::size_t size,
                                  std::stop_token stop_token) noexcept;
    [[nodiscard]] bool write_exact(HANDLE pipe, const void* source, std::size_t size) noexcept;
    [[nodiscard]] bool create_user_only_security(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor,
                                                  std::string& error) noexcept;

    std::jthread thread_{};
    std::jthread pose_thread_{};
    std::atomic<HANDLE> pipe_{INVALID_HANDLE_VALUE};
    std::atomic<HANDLE> pose_pipe_{INVALID_HANDLE_VALUE};
    std::mutex write_mutex_{};
    std::mutex pose_dispatch_mutex_{};
    std::uint64_t last_pose_sequence_{};
    bool have_pose_sequence_{};
#endif
    IpcMessageHandler& handler_;
    std::atomic<bool> running_{};
};

class SingleInstanceGuard::Implementation {
public:
    ~Implementation();
    [[nodiscard]] bool acquire(std::string& error, std::string_view instance_suffix);
    [[nodiscard]] bool acquired() const noexcept;
    [[nodiscard]] bool already_running() const noexcept { return already_running_; }

private:
#if defined(_WIN32)
    HANDLE handle_{};
#else
    bool held_{};
#endif
    bool already_running_{};
};

#if defined(_WIN32)

[[nodiscard]] bool current_user_sid_string(std::wstring& sid, std::string& error) noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = "OpenProcessToken failed: " + std::to_string(GetLastError());
        return false;
    }
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<std::byte> token_buffer(required);
    if (!GetTokenInformation(token, TokenUser, token_buffer.data(), required, &required)) {
        error = "GetTokenInformation failed: " + std::to_string(GetLastError());
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
    LPWSTR sid_string = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_string)) {
        error = "ConvertSidToStringSid failed: " + std::to_string(GetLastError());
        return false;
    }
    sid = sid_string;
    LocalFree(sid_string);
    return true;
}

[[nodiscard]] bool current_session_id(DWORD& session_id, std::string& error) noexcept {
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        error = "ProcessIdToSessionId failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

[[nodiscard]] bool current_pipe_name(std::wstring& name, std::string& error) noexcept {
    std::wstring sid;
    DWORD session_id = 0;
    if (!current_user_sid_string(sid, error) || !current_session_id(session_id, error)) return false;
    name = L"\\\\.\\pipe\\SoundSpatializer.Engine.v1." + sid + L"." + std::to_wstring(session_id);
    error.clear();
    return true;
}

[[nodiscard]] bool current_pose_pipe_name(std::wstring& name, std::string& error) noexcept {
    std::wstring sid;
    DWORD session_id = 0;
    if (!current_user_sid_string(sid, error) || !current_session_id(session_id, error)) return false;
    name = L"\\\\.\\pipe\\SoundSpatializer.Pose.v1." + sid + L"." + std::to_wstring(session_id);
    error.clear();
    return true;
}

bool NamedPipeServer::Implementation::create_user_only_security(SECURITY_ATTRIBUTES& attributes,
                                                                 PSECURITY_DESCRIPTOR& descriptor,
                                                                 std::string& error) noexcept {
    std::wstring sid;
    if (!current_user_sid_string(sid, error)) return false;
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error = "ConvertStringSecurityDescriptor failed: " + std::to_string(GetLastError());
        return false;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

bool NamedPipeServer::Implementation::start(std::string& error) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        error.clear();
        return true;
    }
    // A worker can report a fatal asynchronous setup error before its sibling
    // exits. A subsequent start must cancel and join both old threads before
    // replacing either jthread (whose move-assignment would otherwise wait on a
    // still-blocked ConnectNamedPipe without cancelling its native I/O).
    if (thread_.joinable() || pose_thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        stop();
        running_.store(true, std::memory_order_release);
    }
    {
        std::scoped_lock lock(pose_dispatch_mutex_);
        have_pose_sequence_ = false;
        last_pose_sequence_ = 0;
    }
    try {
        thread_ = std::jthread([this](std::stop_token token) { worker(token); });
        pose_thread_ = std::jthread([this](std::stop_token token) { pose_worker(token); });
    } catch (const std::exception& exception) {
        error = exception.what();
        stop();
        return false;
    }
    error.clear();
    return true;
}

void NamedPipeServer::Implementation::cancel_pipe_worker(std::jthread& thread,
                                                          std::atomic<HANDLE>& pipe_slot) noexcept {
    if (!thread.joinable()) return;
    thread.request_stop();
    const HANDLE pipe = pipe_slot.load(std::memory_order_acquire);
    if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
        DisconnectNamedPipe(pipe);
    }
    CancelSynchronousIo(thread.native_handle());
}

void NamedPipeServer::Implementation::stop() noexcept {
    running_.store(false, std::memory_order_release);
    cancel_pipe_worker(thread_, pipe_);
    cancel_pipe_worker(pose_thread_, pose_pipe_);
    if (thread_.joinable()) thread_.join();
    if (pose_thread_.joinable()) pose_thread_.join();
    pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
    pose_pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
}

bool NamedPipeServer::Implementation::read_exact(HANDLE pipe, void* destination, std::size_t size,
                                                  std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        DWORD available_bytes = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available_bytes, nullptr)) return false;
        if (pipe_has_complete_read(available_bytes, size)) break;
        Sleep(1);
    }
    if (stop_token.stop_requested()) return false;

    auto* bytes = static_cast<std::byte*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - offset, 64U * 1024U));
        if (!ReadFile(pipe, bytes + offset, chunk, &read, nullptr) || read == 0) return false;
        offset += read;
    }
    return true;
}

bool NamedPipeServer::Implementation::write_exact(HANDLE pipe, const void* source, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::byte*>(source);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - offset, 64U * 1024U));
        if (!WriteFile(pipe, bytes + offset, chunk, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

void NamedPipeServer::Implementation::dispatch_pose_packet(const HeadPosePacketV1& packet) {
    HeadPoseSampleV1 pose{};
    if (!from_packet(packet, pose)) {
        handler_.on_ipc_error("invalid HeadPosePacketV1");
        return;
    }

    // Both the legacy duplex pipe and the dedicated low-latency pipe remain
    // valid pose producers during migration. Serialize their final sequence
    // gate and callback so an older packet from one pipe can never overwrite a
    // newer packet already accepted from the other.
    std::scoped_lock lock(pose_dispatch_mutex_);
    if (!pose_sequence_is_newer(have_pose_sequence_, last_pose_sequence_, pose.sequence)) {
        handler_.on_ipc_error("non-monotonic HeadPosePacketV1 sequence");
        return;
    }
    have_pose_sequence_ = true;
    last_pose_sequence_ = pose.sequence;
    handler_.on_head_pose(pose);
}

void NamedPipeServer::Implementation::worker(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        SECURITY_ATTRIBUTES security{};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        std::string security_error;
        if (!create_user_only_security(security, descriptor, security_error)) {
            handler_.on_ipc_error(security_error);
            break;
        }
        std::wstring pipe_name;
        if (!current_pipe_name(pipe_name, security_error)) {
            LocalFree(descriptor);
            handler_.on_ipc_error(security_error);
            break;
        }
        const HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
                                              PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                              1, kMaximumJsonFrameBytes + 4, kMaximumJsonFrameBytes + 4, 0, &security);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            handler_.on_ipc_error("CreateNamedPipe failed: " + std::to_string(GetLastError()));
            break;
        }
        pipe_.store(pipe, std::memory_order_release);
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
            CloseHandle(pipe);
            if (!stop_token.stop_requested()) handler_.on_ipc_error("named pipe connection failed");
            continue;
        }

        while (!stop_token.stop_requested()) {
            std::uint32_t prefix = 0;
            if (!read_exact(pipe, &prefix, sizeof(prefix), stop_token)) break;
            if (prefix == kPosePacketMagic) {
                HeadPosePacketV1 packet{};
                packet.magic = prefix;
                if (!read_exact(pipe, reinterpret_cast<std::byte*>(&packet) + sizeof(prefix),
                                sizeof(packet) - sizeof(prefix), stop_token)) break;
                dispatch_pose_packet(packet);
                continue;
            }
            if (prefix == 0 || prefix > kMaximumJsonFrameBytes) {
                handler_.on_ipc_error("invalid JSON frame length");
                break;
            }
            std::string json(prefix, '\0');
            if (!read_exact(pipe, json.data(), json.size(), stop_token)) break;
            const ParseResult<std::uint32_t> command_id =
                engine_command_id_from_json(json);
            ParseResult<EngineCommandV1> command = engine_command_from_json(json);
            if (!command) {
                handler_.on_ipc_error(command.error);
                if (command_id && *command_id.value != 0U) {
                    const std::vector<std::byte> result_frame =
                        encode_json_frame(engine_command_result_to_json(
                            *command_id.value, false, false, command.error));
                    if (result_frame.empty()) {
                        handler_.on_ipc_error(
                            "command rejection JSON is too large");
                        continue;
                    }
                    std::scoped_lock lock(write_mutex_);
                    if (!write_exact(pipe, result_frame.data(),
                                     result_frame.size()))
                        break;
                }
                continue;
            }

            const IpcMessageHandler::CommandResult command_result =
                handler_.on_engine_command(*command.value);
            // Legacy desktop builds omit commandId and expect every JSON frame
            // to be an EngineStatusV1. Keep their one-way command semantics.
            if (command.value->command_id == 0U) continue;
            const std::vector<std::byte> result_frame = encode_json_frame(
                engine_command_result_to_json(
                    command.value->command_id, command_result.accepted,
                    command_result.persisted, command_result.error));
            if (result_frame.empty()) {
                handler_.on_ipc_error("command result JSON is too large");
                continue;
            }
            std::scoped_lock lock(write_mutex_);
            if (!write_exact(pipe, result_frame.data(), result_frame.size())) break;
        }
        DisconnectNamedPipe(pipe);
        pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        CloseHandle(pipe);
    }
    running_.store(false, std::memory_order_release);
}

void NamedPipeServer::Implementation::pose_worker(std::stop_token stop_token) {
    constexpr DWORD pose_pipe_buffer_bytes = static_cast<DWORD>(sizeof(HeadPosePacketV1) * 256U);
    while (!stop_token.stop_requested()) {
        SECURITY_ATTRIBUTES security{};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        std::string security_error;
        if (!create_user_only_security(security, descriptor, security_error)) {
            handler_.on_ipc_error(security_error);
            break;
        }
        std::wstring pipe_name;
        if (!current_pose_pipe_name(pipe_name, security_error)) {
            LocalFree(descriptor);
            handler_.on_ipc_error(security_error);
            break;
        }
        const HANDLE pipe = CreateNamedPipeW(
            pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 0, pose_pipe_buffer_bytes, 0, &security);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            handler_.on_ipc_error("CreateNamedPipe(pose) failed: " + std::to_string(GetLastError()));
            break;
        }
        pose_pipe_.store(pipe, std::memory_order_release);
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            pose_pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
            CloseHandle(pipe);
            if (!stop_token.stop_requested()) handler_.on_ipc_error("pose named pipe connection failed");
            continue;
        }

        while (!stop_token.stop_requested()) {
            HeadPosePacketV1 packet{};
            if (!read_exact(pipe, &packet, sizeof(packet), stop_token)) break;
            dispatch_pose_packet(packet);
        }
        pose_pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

bool NamedPipeServer::Implementation::publish_status(const EngineStatusV1& status, std::string& error) {
    const std::vector<std::byte> frame = encode_json_frame(engine_status_to_json(status));
    if (frame.empty()) {
        error = "status JSON is too large";
        return false;
    }
    std::scoped_lock lock(write_mutex_);
    const HANDLE pipe = pipe_.load(std::memory_order_acquire);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "no IPC client is connected";
        return false;
    }
    if (!write_exact(pipe, frame.data(), frame.size())) {
        error = "could not publish engine status";
        return false;
    }
    error.clear();
    return true;
}

SingleInstanceGuard::Implementation::~Implementation() {
    if (handle_ != nullptr) CloseHandle(handle_);
}

bool SingleInstanceGuard::Implementation::acquire(std::string& error, std::string_view instance_suffix) {
    if (handle_ != nullptr) { error.clear(); return true; }
    std::wstring sid;
    if (!current_user_sid_string(sid, error)) return false;
    DWORD session_id = 0;
    if (!current_session_id(session_id, error)) return false;
    std::wstring suffix;
    suffix.reserve(instance_suffix.size());
    for (const char character : instance_suffix) {
        const unsigned char value = static_cast<unsigned char>(character);
        suffix.push_back((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                                 (value >= '0' && value <= '9') || value == '.' || value == '-'
                             ? static_cast<wchar_t>(value)
                             : L'_');
    }
    const std::wstring name = L"Local\\SoundSpatializer." + sid + L"." + std::to_wstring(session_id) + L"." + suffix;
    handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle_ == nullptr) {
        error = "CreateMutex failed: " + std::to_string(GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle_);
        handle_ = nullptr;
        already_running_ = true;
        error = "another Sound Spatializer engine instance is already running for this user";
        return false;
    }
    already_running_ = false;
    error.clear();
    return true;
}

bool SingleInstanceGuard::Implementation::acquired() const noexcept { return handle_ != nullptr; }

#else

bool NamedPipeServer::Implementation::start(std::string& error) {
    error = "Windows named pipes are unavailable on this platform";
    running_.store(false, std::memory_order_release);
    return false;
}
void NamedPipeServer::Implementation::stop() noexcept { running_.store(false, std::memory_order_release); }
bool NamedPipeServer::Implementation::publish_status(const EngineStatusV1&, std::string& error) {
    error = "Windows named pipes are unavailable on this platform";
    return false;
}

SingleInstanceGuard::Implementation::~Implementation() = default;
bool SingleInstanceGuard::Implementation::acquire(std::string& error, std::string_view) {
    if (held_) { error.clear(); return true; }
    held_ = true;
    already_running_ = false;
    error.clear();
    return true;
}
bool SingleInstanceGuard::Implementation::acquired() const noexcept { return held_; }

#endif

NamedPipeServer::NamedPipeServer(IpcMessageHandler& handler) : implementation_(std::make_unique<Implementation>(handler)) {}
NamedPipeServer::~NamedPipeServer() = default;
bool NamedPipeServer::start(std::string& error) { return implementation_->start(error); }
void NamedPipeServer::stop() noexcept { implementation_->stop(); }
bool NamedPipeServer::publish_status(const EngineStatusV1& status, std::string& error) {
    return implementation_->publish_status(status, error);
}
bool NamedPipeServer::running() const noexcept { return implementation_->running(); }
std::string NamedPipeServer::endpoint_name() {
#if defined(_WIN32)
    std::wstring wide_name;
    std::string error;
    if (!current_pipe_name(wide_name, error)) return {};
    std::string name;
    name.reserve(wide_name.size());
    for (const wchar_t character : wide_name) {
        if (character < 0 || character > 0x7F) return {};
        name.push_back(static_cast<char>(character));
    }
    return name;
#else
    return R"(\\.\pipe\SoundSpatializer.Engine.v1.local.0)";
#endif
}

std::string NamedPipeServer::pose_endpoint_name() {
#if defined(_WIN32)
    std::wstring wide_name;
    std::string error;
    if (!current_pose_pipe_name(wide_name, error)) return {};
    std::string name;
    name.reserve(wide_name.size());
    for (const wchar_t character : wide_name) {
        if (character < 0 || character > 0x7F) return {};
        name.push_back(static_cast<char>(character));
    }
    return name;
#else
    return R"(\\.\pipe\SoundSpatializer.Pose.v1.local.0)";
#endif
}

SingleInstanceGuard::SingleInstanceGuard() : implementation_(std::make_unique<Implementation>()) {}
SingleInstanceGuard::~SingleInstanceGuard() = default;
bool SingleInstanceGuard::acquire(std::string& error, std::string_view instance_suffix) {
    return implementation_->acquire(error, instance_suffix);
}
bool SingleInstanceGuard::acquired() const noexcept { return implementation_->acquired(); }
bool SingleInstanceGuard::already_running() const noexcept { return implementation_->already_running(); }

} // namespace sound_spatializer
