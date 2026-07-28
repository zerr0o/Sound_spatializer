#include "sound_spatializer/audio_backend.hpp"

#if defined(_WIN32)

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <windows.h>
#include <wrl/client.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

#include "SoundSpatializerDriverContract.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace sound_spatializer {
namespace {

using Microsoft::WRL::ComPtr;

static_assert(kStereoChannelMask == KSAUDIO_SPEAKER_STEREO);
static_assert(kSurround51BackChannelMask == KSAUDIO_SPEAKER_5POINT1);
static_assert(kSurround51ChannelMask == KSAUDIO_SPEAKER_5POINT1_SURROUND);

[[nodiscard]] std::string hresult_message(std::string_view operation, HRESULT result);

[[nodiscard]] constexpr bool should_fallback_to_legacy_after_period_query(HRESULT result) noexcept {
    return result == E_NOINTERFACE || result == E_NOTIMPL;
}

[[nodiscard]] constexpr bool should_fallback_to_legacy_after_period_initialize(HRESULT result) noexcept {
    // InitializeSharedAudioStream only documents EVENTCALLBACK as a supported
    // stream flag. LOOPBACK is therefore attempted for low latency, but a fresh
    // IAudioClient is opened for the documented legacy Initialize path when an
    // audio stack rejects the flag or its requested periodicity.
    return result == E_NOINTERFACE || result == E_NOTIMPL || result == E_INVALIDARG ||
           result == AUDCLNT_E_INVALID_STREAM_FLAG || result == AUDCLNT_E_ENGINE_PERIODICITY_LOCKED ||
           result == AUDCLNT_E_INVALID_DEVICE_PERIOD;
}

class ScopedFlushDenormalsToZero {
public:
    ScopedFlushDenormalsToZero() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
        original_mxcsr_ = _mm_getcsr();
        _mm_setcsr(realtime_mxcsr_with_ftz_daz(original_mxcsr_));
#endif
    }

    ~ScopedFlushDenormalsToZero() {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_setcsr(original_mxcsr_);
#endif
    }

    ScopedFlushDenormalsToZero(const ScopedFlushDenormalsToZero&) = delete;
    ScopedFlushDenormalsToZero& operator=(const ScopedFlushDenormalsToZero&) = delete;

private:
#if defined(_M_X64) || defined(__x86_64__)
    unsigned int original_mxcsr_{};
#endif
};

[[nodiscard]] std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), size) != size) return {};
    return result;
}

[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), size, nullptr, nullptr) != size) return {};
    return result;
}

[[nodiscard]] std::uint32_t read_uint32_property(IMMDevice* device, const DEVPROPKEY& key) noexcept {
    if (device == nullptr) return 0;
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return 0;
    PROPVARIANT value{};
    PropVariantInit(&value);
    const PROPERTYKEY property_key{key.fmtid, key.pid};
    const HRESULT result = properties->GetValue(property_key, &value);
    const std::uint32_t parsed = SUCCEEDED(result) && value.vt == VT_UI4 ? value.ulVal : 0;
    PropVariantClear(&value);
    return parsed;
}

[[nodiscard]] AudioEndpointSelection discover_capture_endpoint(IMMDeviceEnumerator* enumerator,
                                                               CaptureProvider provider,
                                                               std::string_view explicit_endpoint_id,
                                                               std::string_view native_test_override_id) {
    if (enumerator == nullptr) return {{}, "MMDevice enumerator is unavailable"};
    ComPtr<IMMDeviceCollection> collection;
    HRESULT result = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(result)) return {{}, hresult_message("enumerate active render endpoints", result)};
    UINT count = 0;
    result = collection->GetCount(&count);
    if (FAILED(result)) return {{}, hresult_message("count active render endpoints", result)};
    std::vector<AudioEndpointDescriptor> endpoints;
    endpoints.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || id == nullptr) continue;
        AudioEndpointDescriptor descriptor{};
        descriptor.id = wide_to_utf8(id);
        descriptor.active = true;
        CoTaskMemFree(id);
        descriptor.endpoint_marker = read_uint32_property(device.Get(), DEVPKEY_SoundSpatializer_EndpointMarker);
        descriptor.contract_version = read_uint32_property(device.Get(), DEVPKEY_SoundSpatializer_ContractVersion);
        endpoints.push_back(std::move(descriptor));
    }
    return select_capture_render_endpoint(endpoints, provider, explicit_endpoint_id,
                                          native_test_override_id);
}

[[nodiscard]] bool is_float_48k(const WAVEFORMATEX* format, WORD channels) noexcept {
    if (format == nullptr || format->nChannels != channels || format->nSamplesPerSec != kSampleRate ||
        format->wBitsPerSample != 32 ||
        format->nBlockAlign != static_cast<WORD>(channels * sizeof(float))) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }
    return false;
}

[[nodiscard]] WAVEFORMATEXTENSIBLE canonical_float_48k(WORD channels, DWORD channel_mask) noexcept {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = kSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channel_mask;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

[[nodiscard]] WAVEFORMATEXTENSIBLE canonical_stereo_float_48k() noexcept {
    return canonical_float_48k(2, KSAUDIO_SPEAKER_STEREO);
}

[[nodiscard]] std::string surround_5_1_format_error(std::string_view detail) {
    std::string result = "the selected capture endpoint is not configured for usable 5.1 audio";
    if (!detail.empty()) {
        result += " (";
        result += detail;
        result += ')';
    }
    result += ". Open mmsys.cpl > Playback > CABLE Input > Configure > 5.1, "
              "then restart applications that produce audio";
    return result;
}

[[nodiscard]] bool validate_surround_5_1_mix_format(const WAVEFORMATEX* format,
                                                     DWORD& channel_mask,
                                                     std::string& error) {
    channel_mask = 0;
    if (format == nullptr) {
        error = surround_5_1_format_error("the endpoint returned no mix format");
        return false;
    }
    if (format->nChannels != 6) {
        char detail[80]{};
        std::snprintf(detail, sizeof(detail), "Windows currently exposes %u channels, expected 6",
                      static_cast<unsigned int>(format->nChannels));
        error = surround_5_1_format_error(detail);
        return false;
    }
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < 22) {
        error = surround_5_1_format_error("six-channel PCM requires WAVEFORMATEXTENSIBLE");
        return false;
    }

    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    channel_mask = extensible->dwChannelMask;
    if (!is_supported_surround_5_1_channel_mask(channel_mask)) {
        char detail[80]{};
        std::snprintf(detail, sizeof(detail), "unsupported Windows channel mask 0x%08lX",
                      static_cast<unsigned long>(channel_mask));
        error = surround_5_1_format_error(detail);
        return false;
    }

    const bool ieee_float = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    const bool integer_pcm = IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    const bool supported_container =
        (ieee_float && format->wBitsPerSample == 32) ||
        (integer_pcm && (format->wBitsPerSample == 16 || format->wBitsPerSample == 24 ||
                         format->wBitsPerSample == 32));
    if (!supported_container) {
        error = surround_5_1_format_error("the mix format is not uncompressed PCM or float32");
        return false;
    }
    const WORD expected_block_align =
        static_cast<WORD>(format->nChannels * (format->wBitsPerSample / 8U));
    if (format->nBlockAlign != expected_block_align ||
        extensible->Samples.wValidBitsPerSample > format->wBitsPerSample) {
        error = surround_5_1_format_error("the endpoint returned an inconsistent PCM container");
        return false;
    }
    error.clear();
    return true;
}

[[nodiscard]] WAVEFORMATEXTENSIBLE canonical_stereo_pcm_s32_48k() noexcept {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = kSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(2U * sizeof(std::int32_t));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return format;
}

[[nodiscard]] WAVEFORMATEX legacy_stereo_32_bit_48k(WORD format_tag) noexcept {
    WAVEFORMATEX format{};
    format.wFormatTag = format_tag;
    format.nChannels = 2;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(2U * sizeof(std::int32_t));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
    return format;
}

[[nodiscard]] constexpr bool is_unsupported_format_result(HRESULT result) noexcept {
    return result == AUDCLNT_E_UNSUPPORTED_FORMAT || result == S_FALSE;
}

[[nodiscard]] std::string hresult_message(std::string_view operation, HRESULT result) {
    return std::string(operation) + " failed (HRESULT 0x" + [&] {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(result));
        return std::string(buffer);
    }() + ')';
}

enum class RuntimeAudioError : std::uint32_t {
    none,
    mmcss_unavailable,
    wait_failed,
    capture_packet_too_large,
    capture_io_failed,
    render_io_failed,
    device_invalidated,
    audio_service_stopped,
};

[[nodiscard]] RuntimeAudioError runtime_error_from_hresult(HRESULT result, bool capture) noexcept {
    if (result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_RESOURCES_INVALIDATED)
        return RuntimeAudioError::device_invalidated;
    if (result == AUDCLNT_E_SERVICE_NOT_RUNNING) return RuntimeAudioError::audio_service_stopped;
    return capture ? RuntimeAudioError::capture_io_failed : RuntimeAudioError::render_io_failed;
}

[[nodiscard]] std::string_view runtime_error_message(RuntimeAudioError error) noexcept {
    switch (error) {
    case RuntimeAudioError::none: return {};
    case RuntimeAudioError::mmcss_unavailable:
        return "MMCSS Pro Audio registration failed; real-time priority is degraded";
    case RuntimeAudioError::wait_failed: return "WASAPI event wait failed";
    case RuntimeAudioError::capture_packet_too_large:
        return "capture packet exceeded the preallocated WASAPI buffer";
    case RuntimeAudioError::capture_io_failed: return "WASAPI capture stream failed";
    case RuntimeAudioError::render_io_failed: return "WASAPI render stream failed";
    case RuntimeAudioError::device_invalidated: return "audio endpoint was invalidated or disconnected";
    case RuntimeAudioError::audio_service_stopped: return "Windows Audio service stopped";
    }
    return "unknown WASAPI runtime failure";
}

class WasapiAudioBackend final : public IAudioBackend {
public:
    ~WasapiAudioBackend() override { stop(); }

    [[nodiscard]] bool start(const AudioBackendConfig& config, IAudioProcessor& processor,
                             std::string& error) override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] AudioBackendDiagnostics diagnostics() const override;

private:
    void worker(std::stop_token stop_token);
    void finish_initialization(bool success, std::string error);

    AudioBackendConfig config_{};
    IAudioProcessor* processor_{};
    std::jthread thread_{};
    std::atomic<bool> running_{};
    std::atomic<HANDLE> capture_event_{nullptr};
    std::atomic<HANDLE> render_event_{nullptr};
    mutable std::mutex initialization_mutex_{};
    std::condition_variable initialization_condition_{};
    bool initialization_finished_{};
    bool initialization_succeeded_{};
    mutable std::mutex error_mutex_{};
    std::string last_error_{};
    std::string resolved_capture_endpoint_id_{};
    std::atomic<std::uint32_t> runtime_error_{static_cast<std::uint32_t>(RuntimeAudioError::none)};
    std::atomic<std::uint32_t> capture_state_{static_cast<std::uint32_t>(StreamState::stopped)};
    std::atomic<std::uint32_t> render_state_{static_cast<std::uint32_t>(StreamState::stopped)};
    std::atomic<std::uint32_t> capture_channels_{};
    std::atomic<std::uint32_t> capture_channel_mask_{};
    std::atomic<std::uint32_t> capture_rate_{};
    std::atomic<std::uint32_t> render_rate_{};
    std::atomic<std::uint32_t> render_sample_format_{
        static_cast<std::uint32_t>(AudioSampleFormat::unknown)};
    std::atomic<std::uint32_t> capture_period_{};
    std::atomic<std::uint32_t> render_period_{};
    std::atomic<std::uint32_t> fifo_fill_{};
    std::atomic<std::uint64_t> capture_overruns_{};
    std::atomic<std::uint64_t> render_underruns_{};
    std::atomic<float> callback_cpu_{};
    std::atomic<float> resample_ratio_{1.0F};
};

bool WasapiAudioBackend::start(const AudioBackendConfig& config, IAudioProcessor& processor, std::string& error) {
    if (running()) {
        error.clear();
        return true;
    }
    if (!validate_audio_backend_config(config, error)) return false;
    config_ = config;
    processor_ = &processor;
    runtime_error_.store(static_cast<std::uint32_t>(RuntimeAudioError::none), std::memory_order_release);
    render_sample_format_.store(static_cast<std::uint32_t>(AudioSampleFormat::unknown),
                                std::memory_order_release);
    {
        std::scoped_lock lock(error_mutex_);
        last_error_.clear();
        resolved_capture_endpoint_id_.clear();
    }
    {
        std::scoped_lock lock(initialization_mutex_);
        initialization_finished_ = false;
        initialization_succeeded_ = false;
    }
    capture_state_.store(static_cast<std::uint32_t>(StreamState::starting), std::memory_order_release);
    render_state_.store(static_cast<std::uint32_t>(StreamState::starting), std::memory_order_release);
    running_.store(true, std::memory_order_release);
    try {
        thread_ = std::jthread([this](std::stop_token token) { worker(token); });
    } catch (const std::exception& exception) {
        running_.store(false, std::memory_order_release);
        error = exception.what();
        return false;
    }
    std::unique_lock lock(initialization_mutex_);
    if (!initialization_condition_.wait_for(lock, std::chrono::seconds(5), [this] { return initialization_finished_; })) {
        lock.unlock();
        constexpr std::string_view timeout_error = "WASAPI initialization timed out; audio worker cancellation is pending";
        {
            std::scoped_lock error_lock(error_mutex_);
            last_error_ = timeout_error;
        }
        capture_state_.store(static_cast<std::uint32_t>(StreamState::failed), std::memory_order_release);
        render_state_.store(static_cast<std::uint32_t>(StreamState::failed), std::memory_order_release);
        stop();
        error = timeout_error;
        return false;
    }
    if (!initialization_succeeded_) {
        error = last_error_;
        lock.unlock();
        stop();
        return false;
    }
    error.clear();
    return true;
}

void WasapiAudioBackend::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.request_stop();
        if (const HANDLE event = capture_event_.load(std::memory_order_acquire); event != nullptr) SetEvent(event);
        if (const HANDLE event = render_event_.load(std::memory_order_acquire); event != nullptr) SetEvent(event);
        thread_.join();
    }
    processor_ = nullptr;
    render_sample_format_.store(static_cast<std::uint32_t>(AudioSampleFormat::unknown),
                                std::memory_order_release);
    if (static_cast<StreamState>(capture_state_.load(std::memory_order_acquire)) != StreamState::failed)
        capture_state_.store(static_cast<std::uint32_t>(StreamState::stopped), std::memory_order_release);
    if (static_cast<StreamState>(render_state_.load(std::memory_order_acquire)) != StreamState::failed)
        render_state_.store(static_cast<std::uint32_t>(StreamState::stopped), std::memory_order_release);
}

AudioBackendDiagnostics WasapiAudioBackend::diagnostics() const {
    AudioBackendDiagnostics result{};
    result.capture_state = static_cast<StreamState>(capture_state_.load(std::memory_order_acquire));
    result.render_state = static_cast<StreamState>(render_state_.load(std::memory_order_acquire));
    result.capture_channels = capture_channels_.load(std::memory_order_relaxed);
    result.capture_channel_mask = capture_channel_mask_.load(std::memory_order_relaxed);
    result.capture_sample_rate = capture_rate_.load(std::memory_order_relaxed);
    result.render_sample_rate = render_rate_.load(std::memory_order_relaxed);
    result.render_sample_format = static_cast<AudioSampleFormat>(
        render_sample_format_.load(std::memory_order_acquire));
    result.capture_period_frames = capture_period_.load(std::memory_order_relaxed);
    result.render_period_frames = render_period_.load(std::memory_order_relaxed);
    result.fifo_fill_frames = fifo_fill_.load(std::memory_order_relaxed);
    result.capture_overruns = capture_overruns_.load(std::memory_order_relaxed);
    result.render_underruns = render_underruns_.load(std::memory_order_relaxed);
    result.callback_cpu_percent = callback_cpu_.load(std::memory_order_relaxed);
    result.resample_ratio = resample_ratio_.load(std::memory_order_relaxed);
    const auto runtime_error = static_cast<RuntimeAudioError>(runtime_error_.load(std::memory_order_acquire));
    {
        std::scoped_lock lock(error_mutex_);
        result.capture_endpoint_id = resolved_capture_endpoint_id_;
        if (runtime_error == RuntimeAudioError::none)
            result.last_error = last_error_;
    }
    if (runtime_error != RuntimeAudioError::none)
        result.last_error = runtime_error_message(runtime_error);
    return result;
}

void WasapiAudioBackend::finish_initialization(bool success, std::string error) {
    {
        std::scoped_lock lock(initialization_mutex_);
        initialization_finished_ = true;
        initialization_succeeded_ = success;
        std::scoped_lock error_lock(error_mutex_);
        if (success) last_error_.clear();
        else last_error_ = std::move(error);
    }
    initialization_condition_.notify_all();
}

void WasapiAudioBackend::worker(std::stop_token stop_token) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        finish_initialization(false, hresult_message("CoInitializeEx", com_result));
        running_.store(false, std::memory_order_release);
        return;
    }
    bool ever_initialized = false;
    std::uint32_t reconnect_attempt = 0;
    while (!stop_token.stop_requested() && running()) {
        bool initialized = false;
        ComPtr<IAudioClient> capture_client;
        ComPtr<IAudioClient3> render_client;
        HANDLE capture_event = nullptr;
        HANDLE render_event = nullptr;
        HANDLE mmcss_handle = nullptr;
        render_sample_format_.store(static_cast<std::uint32_t>(AudioSampleFormat::unknown),
                                    std::memory_order_release);
        capture_channels_.store(0U, std::memory_order_relaxed);
        capture_channel_mask_.store(0U, std::memory_order_relaxed);

        do {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) { finish_initialization(false, hresult_message("CoCreateInstance(MMDeviceEnumerator)", result)); break; }

        const AudioEndpointSelection capture_endpoint = discover_capture_endpoint(
            enumerator.Get(), config_.capture_provider, config_.capture_endpoint_id,
            config_.native_test_override_endpoint_id);
        if (!capture_endpoint) { finish_initialization(false, capture_endpoint.error); break; }
        {
            std::scoped_lock lock(error_mutex_);
            resolved_capture_endpoint_id_ = capture_endpoint.id;
        }
        ComPtr<IMMDevice> capture_device;
        const std::wstring capture_id = utf8_to_wide(capture_endpoint.id);
        if (capture_id.empty()) { finish_initialization(false, "selected capture endpoint id is not valid UTF-8"); break; }
        result = enumerator->GetDevice(capture_id.c_str(), &capture_device);
        if (FAILED(result)) { finish_initialization(false, hresult_message("IMMDeviceEnumerator::GetDevice(capture)", result)); break; }

        ComPtr<IMMDevice> render_device;
        if (config_.physical_output_endpoint_id.empty()) {
            result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &render_device);
        } else {
            const std::wstring render_id = utf8_to_wide(config_.physical_output_endpoint_id);
            if (render_id.empty()) { finish_initialization(false, "physical endpoint id is not valid UTF-8"); break; }
            result = enumerator->GetDevice(render_id.c_str(), &render_device);
        }
        if (FAILED(result)) { finish_initialization(false, hresult_message("select physical render endpoint", result)); break; }
        if (read_uint32_property(render_device.Get(), DEVPKEY_SoundSpatializer_EndpointMarker) == 1) {
            finish_initialization(false, "the selected physical output carries the Sound Spatializer marker (audio loop prevented)");
            break;
        }

        LPWSTR actual_capture_id = nullptr;
        LPWSTR actual_render_id = nullptr;
        capture_device->GetId(&actual_capture_id);
        render_device->GetId(&actual_render_id);
        const bool same_endpoint = actual_capture_id != nullptr && actual_render_id != nullptr &&
                                   _wcsicmp(actual_capture_id, actual_render_id) == 0;
        CoTaskMemFree(actual_capture_id);
        CoTaskMemFree(actual_render_id);
        if (same_endpoint) { finish_initialization(false, "physical output resolves to the virtual endpoint (audio loop prevented)"); break; }

        result = capture_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**>(capture_client.GetAddressOf()));
        if (FAILED(result)) { finish_initialization(false, hresult_message("activate loopback audio client", result)); break; }
        result = render_device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(render_client.GetAddressOf()));
        if (FAILED(result)) { finish_initialization(false, hresult_message("activate render IAudioClient3", result)); break; }

        WAVEFORMATEX* capture_format_raw = nullptr;
        WAVEFORMATEX* render_format_raw = nullptr;
        result = capture_client->GetMixFormat(&capture_format_raw);
        if (FAILED(result)) { finish_initialization(false, hresult_message("get loopback mix format", result)); break; }
        result = render_client->GetMixFormat(&render_format_raw);
        if (FAILED(result)) { CoTaskMemFree(capture_format_raw); finish_initialization(false, hresult_message("get render mix format", result)); break; }
        const bool surround_input = config_.input_layout == InputLayout::surround_5_1;
        const WORD capture_channels =
            static_cast<WORD>(expected_input_channel_count(config_.input_layout));
        DWORD capture_channel_mask = KSAUDIO_SPEAKER_STEREO;
        if (surround_input) {
            std::string format_error;
            if (!validate_surround_5_1_mix_format(capture_format_raw, capture_channel_mask,
                                                  format_error)) {
                CoTaskMemFree(capture_format_raw);
                CoTaskMemFree(render_format_raw);
                finish_initialization(false, std::move(format_error));
                break;
            }
        }
        const bool capture_mix_is_canonical =
            is_float_48k(capture_format_raw, capture_channels) &&
            (!surround_input ||
             reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(capture_format_raw)->dwChannelMask ==
                 capture_channel_mask);
        const bool render_mix_is_canonical = is_float_48k(render_format_raw, 2);
        const WAVEFORMATEXTENSIBLE canonical_format = canonical_stereo_float_48k();
        const WAVEFORMATEX* canonical_wave_format = &canonical_format.Format;
        const WAVEFORMATEXTENSIBLE canonical_capture_format =
            canonical_float_48k(capture_channels, capture_channel_mask);
        const WAVEFORMATEX* canonical_capture_wave_format = &canonical_capture_format.Format;
        const WAVEFORMATEX legacy_float_format = legacy_stereo_32_bit_48k(WAVE_FORMAT_IEEE_FLOAT);
        const WAVEFORMATEXTENSIBLE canonical_pcm_s32_format = canonical_stereo_pcm_s32_48k();
        const WAVEFORMATEX legacy_pcm_s32_format = legacy_stereo_32_bit_48k(WAVE_FORMAT_PCM);
        AudioSampleFormat effective_render_sample_format = AudioSampleFormat::float32;
        const WAVEFORMATEX* exclusive_wave_format = canonical_wave_format;

        capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (capture_event == nullptr || render_event == nullptr) {
            CoTaskMemFree(capture_format_raw); CoTaskMemFree(render_format_raw);
            finish_initialization(false, "could not create WASAPI event handles"); break;
        }
        capture_event_.store(capture_event, std::memory_order_release);
        render_event_.store(render_event, std::memory_order_release);

        constexpr DWORD legacy_stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
        constexpr DWORD period_stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        constexpr DWORD loopback_period_stream_flags = period_stream_flags | AUDCLNT_STREAMFLAGS_LOOPBACK;
        constexpr DWORD loopback_legacy_stream_flags = legacy_stream_flags | AUDCLNT_STREAMFLAGS_LOOPBACK;
        constexpr DWORD conversion_stream_flags =
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        ComPtr<IAudioClient3> capture_client3;
        UINT32 capture_selected_period = 0;
        auto initialize_legacy_loopback = [&](const WAVEFORMATEX* stream_format,
                                              DWORD additional_flags = 0U) noexcept -> HRESULT {
            // Re-activate after a failed InitializeSharedAudioStream attempt so
            // the fallback never relies on a partially initialized COM object.
            capture_client3.Reset();
            capture_client.Reset();
            HRESULT fallback_result = capture_device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(capture_client.GetAddressOf()));
            if (FAILED(fallback_result)) return fallback_result;
            return capture_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              loopback_legacy_stream_flags | additional_flags,
                                              0, 0, stream_format, nullptr);
        };

        if (!capture_mix_is_canonical) {
            // Keep the endpoint's channel count and speaker mask. In 5.1 mode
            // AUTOCONVERTPCM may normalize PCM/sample rate to float32/48 kHz,
            // but it must never be allowed to matrix a stereo endpoint up to
            // six channels or a six-channel endpoint down to stereo.
            result = initialize_legacy_loopback(canonical_capture_wave_format,
                                                conversion_stream_flags);
        } else {
            result = capture_client.As(&capture_client3);
        }
        if (capture_mix_is_canonical && SUCCEEDED(result)) {
            UINT32 default_period = 0;
            UINT32 fundamental_period = 0;
            UINT32 minimum_period = 0;
            UINT32 maximum_period = 0;
            result = capture_client3->GetSharedModeEnginePeriod(capture_format_raw, &default_period,
                                                                 &fundamental_period, &minimum_period,
                                                                 &maximum_period);
            if (SUCCEEDED(result)) {
                capture_selected_period = select_shared_audio_period_frames(
                    config_.mode, {default_period, fundamental_period, minimum_period, maximum_period});
                result = capture_selected_period == 0U
                             ? AUDCLNT_E_INVALID_DEVICE_PERIOD
                             : capture_client3->InitializeSharedAudioStream(
                                   loopback_period_stream_flags, capture_selected_period, capture_format_raw, nullptr);
                if (FAILED(result) && should_fallback_to_legacy_after_period_initialize(result)) {
                    capture_selected_period = 0U;
                    result = initialize_legacy_loopback(capture_format_raw);
                }
            } else if (should_fallback_to_legacy_after_period_query(result)) {
                result = initialize_legacy_loopback(capture_format_raw);
            }
        } else if (capture_mix_is_canonical && should_fallback_to_legacy_after_period_query(result)) {
            result = initialize_legacy_loopback(capture_format_raw);
        }
        if (SUCCEEDED(result)) result = capture_client->SetEventHandle(capture_event);
        if (FAILED(result)) {
            CoTaskMemFree(capture_format_raw); CoTaskMemFree(render_format_raw);
            finish_initialization(false, hresult_message("initialize event-driven loopback", result)); break;
        }

        UINT32 selected_period = config_.requested_buffer_frames;
        if (config_.mode == AudioMode::exclusive_pro) {
            struct ExclusiveFormatCandidate {
                const WAVEFORMATEX* wave_format;
                AudioSampleFormat sample_format;
                std::string_view query_name;
            };
            // Microsoft recommends trying both WAVEFORMATEXTENSIBLE and legacy
            // WAVEFORMATEX for stereo endpoints. Keep float ahead of integer
            // PCM so capable hardware retains the zero-copy render path.
            const std::array<ExclusiveFormatCandidate, 4> candidates{{
                {canonical_wave_format, AudioSampleFormat::float32,
                 "query exclusive extensible stereo float32/48 kHz support"},
                {&legacy_float_format, AudioSampleFormat::float32,
                 "query exclusive legacy stereo float32/48 kHz support"},
                {&canonical_pcm_s32_format.Format, AudioSampleFormat::pcm_s32,
                 "query exclusive extensible stereo signed PCM32/48 kHz support"},
                {&legacy_pcm_s32_format, AudioSampleFormat::pcm_s32,
                 "query exclusive legacy stereo signed PCM32/48 kHz support"},
            }};
            bool found_supported_format = false;
            std::string query_error;
            for (const ExclusiveFormatCandidate& candidate : candidates) {
                result = render_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                           candidate.wave_format, nullptr);
                if (result == S_OK) {
                    exclusive_wave_format = candidate.wave_format;
                    effective_render_sample_format = candidate.sample_format;
                    found_supported_format = true;
                    break;
                }
                if (!is_unsupported_format_result(result)) {
                    query_error = hresult_message(candidate.query_name, result);
                    break;
                }
            }
            if (!found_supported_format) {
                CoTaskMemFree(capture_format_raw); CoTaskMemFree(render_format_raw);
                finish_initialization(
                    false, query_error.empty()
                               ? "the physical endpoint supports neither exclusive stereo float32/48 kHz nor signed PCM32/48 kHz"
                               : std::move(query_error));
                break;
            }
            const auto frames_to_reference_time = [](UINT32 frames) noexcept -> REFERENCE_TIME {
                return static_cast<REFERENCE_TIME>(audio_frames_to_reference_time(frames));
            };
            REFERENCE_TIME requested_period = frames_to_reference_time(
                std::max<std::uint32_t>(64, config_.requested_buffer_frames));
            REFERENCE_TIME default_device_period = 0;
            REFERENCE_TIME minimum_device_period = 0;
            if (SUCCEEDED(render_client->GetDevicePeriod(&default_device_period, &minimum_device_period)) &&
                minimum_device_period > requested_period)
                requested_period = minimum_device_period;
            result = render_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, legacy_stream_flags,
                                               requested_period, requested_period,
                                               exclusive_wave_format, nullptr);
            if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                // WASAPI exposes the hardware-aligned frame count after this
                // specific failure. Reactivate the client before retrying, as
                // an IAudioClient permits Initialize only once even on several
                // audio stacks that returned an alignment hint.
                UINT32 aligned_frames = 0;
                const HRESULT size_result = render_client->GetBufferSize(&aligned_frames);
                render_client.Reset();
                HRESULT retry_result = render_device->Activate(
                    __uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(render_client.GetAddressOf()));
                if (SUCCEEDED(retry_result) && SUCCEEDED(size_result) && aligned_frames != 0U) {
                    const REFERENCE_TIME aligned_period = frames_to_reference_time(aligned_frames);
                    retry_result = render_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                             legacy_stream_flags,
                                                             aligned_period, aligned_period,
                                                             exclusive_wave_format, nullptr);
                } else if (SUCCEEDED(retry_result)) {
                    retry_result = FAILED(size_result) ? size_result : AUDCLNT_E_BUFFER_SIZE_ERROR;
                }
                result = retry_result;
            }
        } else {
            auto initialize_legacy_render = [&](const WAVEFORMATEX* stream_format,
                                                DWORD additional_flags = 0U) noexcept -> HRESULT {
                render_client.Reset();
                HRESULT fallback_result = render_device->Activate(
                    __uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(render_client.GetAddressOf()));
                if (FAILED(fallback_result)) return fallback_result;
                return render_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                 legacy_stream_flags | additional_flags,
                                                 0, 0, stream_format, nullptr);
            };

            if (!render_mix_is_canonical) {
                selected_period = 0U;
                result = initialize_legacy_render(canonical_wave_format, conversion_stream_flags);
            } else {
                UINT32 default_period = 0, fundamental_period = 0, minimum_period = 0, maximum_period = 0;
                result = render_client->GetSharedModeEnginePeriod(render_format_raw, &default_period,
                                                                  &fundamental_period, &minimum_period,
                                                                  &maximum_period);
                if (SUCCEEDED(result)) {
                    selected_period = select_shared_audio_period_frames(
                        config_.mode, {default_period, fundamental_period, minimum_period, maximum_period});
                    result = selected_period == 0U
                                 ? AUDCLNT_E_INVALID_DEVICE_PERIOD
                                 : render_client->InitializeSharedAudioStream(
                                       period_stream_flags, selected_period, render_format_raw, nullptr);
                    if (FAILED(result) && should_fallback_to_legacy_after_period_initialize(result)) {
                        selected_period = 0U;
                        result = initialize_legacy_render(render_format_raw);
                    }
                } else if (should_fallback_to_legacy_after_period_query(result)) {
                    selected_period = 0U;
                    result = initialize_legacy_render(render_format_raw);
                }
            }
        }
        CoTaskMemFree(capture_format_raw);
        CoTaskMemFree(render_format_raw);
        if (SUCCEEDED(result)) result = render_client->SetEventHandle(render_event);
        if (FAILED(result)) { finish_initialization(false, hresult_message("initialize event-driven renderer", result)); break; }

        ComPtr<IAudioCaptureClient> capture_service;
        ComPtr<IAudioRenderClient> render_service;
        ComPtr<IAudioClock> capture_clock;
        ComPtr<IAudioClock> render_clock;
        result = capture_client->GetService(IID_PPV_ARGS(&capture_service));
        if (SUCCEEDED(result)) result = render_client->GetService(IID_PPV_ARGS(&render_service));
        if (SUCCEEDED(result)) result = capture_client->GetService(IID_PPV_ARGS(&capture_clock));
        if (SUCCEEDED(result)) result = render_client->GetService(IID_PPV_ARGS(&render_clock));
        if (FAILED(result)) { finish_initialization(false, hresult_message("obtain WASAPI services", result)); break; }

        UINT32 capture_buffer_frames = 0;
        UINT32 render_buffer_frames = 0;
        result = capture_client->GetBufferSize(&capture_buffer_frames);
        if (SUCCEEDED(result)) result = render_client->GetBufferSize(&render_buffer_frames);
        if (SUCCEEDED(result) && (capture_buffer_frames == 0U || render_buffer_frames == 0U))
            result = AUDCLNT_E_BUFFER_SIZE_ERROR;
        if (FAILED(result)) {
            finish_initialization(false, hresult_message("query initialized WASAPI buffer sizes", result));
            break;
        }
        WAVEFORMATEX* current_capture_format = nullptr;
        UINT32 current_capture_period = capture_selected_period != 0U ? capture_selected_period : capture_buffer_frames;
        ComPtr<IAudioClient3> current_capture_client3;
        if (FAILED(capture_client.As(&current_capture_client3)) ||
            FAILED(current_capture_client3->GetCurrentSharedModeEnginePeriod(&current_capture_format,
                                                                              &current_capture_period))) {
            current_capture_period = capture_selected_period != 0U ? capture_selected_period : capture_buffer_frames;
        }
        CoTaskMemFree(current_capture_format);
        if (config_.mode == AudioMode::exclusive_pro) selected_period = render_buffer_frames;
        else {
            WAVEFORMATEX* current_render_format = nullptr;
            UINT32 current_render_period = 0;
            if (SUCCEEDED(render_client->GetCurrentSharedModeEnginePeriod(&current_render_format, &current_render_period)) &&
                current_render_period != 0U)
                selected_period = current_render_period;
            else
                selected_period = render_buffer_frames;
            CoTaskMemFree(current_render_format);
        }
        capture_channels_.store(capture_channels, std::memory_order_relaxed);
        capture_channel_mask_.store(capture_channel_mask, std::memory_order_relaxed);
        capture_rate_.store(kSampleRate, std::memory_order_relaxed);
        render_rate_.store(kSampleRate, std::memory_order_relaxed);
        capture_period_.store(std::max<UINT32>(1, current_capture_period), std::memory_order_relaxed);
        render_period_.store(std::max<UINT32>(1, selected_period), std::memory_order_relaxed);

        AsyncProgrammeResampler resampler(std::max<std::size_t>(8'192, capture_buffer_frames * 8ULL));
        ClockDriftEstimator clock_drift;
        UINT64 capture_clock_frequency = 0;
        UINT64 render_clock_frequency = 0;
        capture_clock->GetFrequency(&capture_clock_frequency);
        render_clock->GetFrequency(&render_clock_frequency);
        resampler.set_nominal_ratio(1.0F);
        // Start from the requested/render quantum rather than the endpoint's
        // full legacy loopback buffer. The target is refined from the first
        // actual packet below, so an 80 ms allocation does not become 80 ms of
        // deliberate FIFO latency when packets really arrive every few ms.
        resampler.set_target_fill(select_asrc_target_fill_frames(
            0U, selected_period, config_.requested_buffer_frames));
        std::vector<ProgrammeFrame> capture_work(std::max<UINT32>(capture_buffer_frames, 1));
        std::vector<ProgrammeFrame> render_input(std::max<UINT32>(render_buffer_frames, 1));
        std::vector<StereoFrame> render_output(
            effective_render_sample_format == AudioSampleFormat::pcm_s32
                ? std::max<UINT32>(render_buffer_frames, 1)
                : 0U);
        UINT32 minimum_observed_capture_packet = std::max<UINT32>(1U, current_capture_period);

        if (config_.mode == AudioMode::exclusive_pro) {
            // An event-driven exclusive stream owns the complete endpoint
            // buffer. Prime it before Start; subsequent events likewise require
            // exactly render_buffer_frames rather than a shared-mode padding
            // calculation.
            BYTE* initial_output = nullptr;
            result = render_service->GetBuffer(render_buffer_frames, &initial_output);
            if (SUCCEEDED(result))
                result = render_service->ReleaseBuffer(render_buffer_frames,
                                                       AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(result)) {
                finish_initialization(false, hresult_message("prime exclusive WASAPI render buffer", result));
                break;
            }
        }

        result = capture_client->Start();
        if (SUCCEEDED(result)) result = render_client->Start();
        if (FAILED(result)) { finish_initialization(false, hresult_message("start WASAPI streams", result)); break; }
        render_sample_format_.store(static_cast<std::uint32_t>(effective_render_sample_format),
                                    std::memory_order_release);

        initialized = true;
        ever_initialized = true;
        reconnect_attempt = 0;
        finish_initialization(true, {});
        DWORD mmcss_task_index = 0;
        mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);
        ScopedFlushDenormalsToZero denormal_mode{};
        const bool mmcss_degraded = mmcss_handle == nullptr;
        capture_state_.store(static_cast<std::uint32_t>(mmcss_degraded ? StreamState::degraded : StreamState::running),
                             std::memory_order_release);
        render_state_.store(static_cast<std::uint32_t>(mmcss_degraded ? StreamState::degraded : StreamState::running),
                            std::memory_order_release);
        runtime_error_.store(static_cast<std::uint32_t>(RuntimeAudioError::none), std::memory_order_release);
        if (mmcss_degraded)
            runtime_error_.store(static_cast<std::uint32_t>(RuntimeAudioError::mmcss_unavailable),
                                 std::memory_order_release);

        LARGE_INTEGER qpc_frequency{};
        QueryPerformanceFrequency(&qpc_frequency);
        HANDLE events[2]{render_event, capture_event};
        bool stream_failed = false;
        auto drain_capture_packets = [&](CaptureDrainBudget capture_budget) noexcept -> bool {
            while (capture_budget.processed_packets < capture_budget.maximum_packets &&
                   capture_budget.processed_frames < capture_budget.maximum_frames) {
                UINT32 packet_frames = 0;
                result = capture_service->GetNextPacketSize(&packet_frames);
                if (FAILED(result)) {
                    runtime_error_.store(static_cast<std::uint32_t>(runtime_error_from_hresult(result, true)),
                                         std::memory_order_release);
                    return false;
                }
                if (packet_frames == 0U) return true;
                if (packet_frames > capture_work.size()) {
                    runtime_error_.store(static_cast<std::uint32_t>(RuntimeAudioError::capture_packet_too_large),
                                         std::memory_order_release);
                    return false;
                }
                if (!capture_budget.can_process(packet_frames)) return true;

                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT64 device_position = 0;
                UINT64 qpc_position = 0;
                UINT32 frames = 0;
                result = capture_service->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
                if (FAILED(result)) {
                    runtime_error_.store(static_cast<std::uint32_t>(runtime_error_from_hresult(result, true)),
                                         std::memory_order_release);
                    return false;
                }
                if (frames > capture_work.size() || !capture_budget.try_account(frames)) {
                    (void)capture_service->ReleaseBuffer(frames);
                    runtime_error_.store(static_cast<std::uint32_t>(RuntimeAudioError::capture_packet_too_large),
                                         std::memory_order_release);
                    return false;
                }
                if (frames != 0U) {
                    minimum_observed_capture_packet =
                        std::min(minimum_observed_capture_packet, frames);
                    capture_period_.store(minimum_observed_capture_packet, std::memory_order_relaxed);
                    resampler.set_target_fill(select_asrc_target_fill_frames(
                        frames, selected_period, config_.requested_buffer_frames));
                }
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                    std::fill_n(capture_work.data(), frames, ProgrammeFrame{});
                else if (surround_input)
                    std::memcpy(capture_work.data(), data,
                                static_cast<std::size_t>(frames) * sizeof(ProgrammeFrame));
                else {
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        StereoFrame stereo{};
                        std::memcpy(&stereo, data + frame * sizeof(StereoFrame),
                                    sizeof(StereoFrame));
                        capture_work[frame] = {};
                        capture_work[frame].front_left = stereo.left;
                        capture_work[frame].front_right = stereo.right;
                    }
                }
                (void)resampler.push(capture_work.data(), frames);
                result = capture_service->ReleaseBuffer(frames);
                if (FAILED(result)) {
                    runtime_error_.store(static_cast<std::uint32_t>(runtime_error_from_hresult(result, true)),
                                         std::memory_order_release);
                    return false;
                }
            }
            return true;
        };
        while (!stop_token.stop_requested() && running()) {
            const DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, 100);
            if (wait_result == WAIT_TIMEOUT) continue;
            if (wait_result == WAIT_FAILED) {
                runtime_error_.store(static_cast<std::uint32_t>(RuntimeAudioError::wait_failed),
                                     std::memory_order_release);
                stream_failed = true;
                break;
            }
            if (stop_token.stop_requested() || !running()) break;

            // When capture alone wakes the worker, move one bounded packet into
            // the ASRC before rendering. This removes a full render-period wait
            // from the capture-to-output path while render remains first when
            // its own deadline event is already signalled.
            if (wait_result == WAIT_OBJECT_0 + 1U) {
                CaptureDrainBudget pre_render_budget = make_capture_drain_budget(capture_buffer_frames);
                pre_render_budget.maximum_packets = 1U;
                if (!drain_capture_packets(pre_render_budget)) {
                    stream_failed = true;
                    break;
                }
            }

            const bool render_event_signalled = wait_result == WAIT_OBJECT_0;
            if (!should_service_wasapi_render_on_wake(config_.mode, render_event_signalled)) {
                // A capture-only wake must never consume an exclusive render
                // buffer. Doing so violates event-driven exclusive semantics
                // and causes AUDCLNT_E_OUT_OF_ORDER on several drivers.
                if (!drain_capture_packets(make_capture_drain_budget(capture_buffer_frames))) {
                    stream_failed = true;
                    break;
                }
                continue;
            }

            // Rendering owns the deadline after the optional single-packet fast
            // path above; the remaining capture backlog is budgeted below so it
            // cannot starve the physical output.
            {
                UINT32 available = render_buffer_frames;
                if (config_.mode != AudioMode::exclusive_pro) {
                    UINT32 padding = 0;
                    result = render_client->GetCurrentPadding(&padding);
                    if (FAILED(result) || padding > render_buffer_frames) {
                        const RuntimeAudioError runtime_error = FAILED(result)
                                                                    ? runtime_error_from_hresult(result, false)
                                                                    : RuntimeAudioError::render_io_failed;
                        runtime_error_.store(static_cast<std::uint32_t>(runtime_error), std::memory_order_release);
                        stream_failed = true;
                        break;
                    }
                    available = render_buffer_frames - padding;
                }
                if (available != 0U) {
                    BYTE* output_bytes = nullptr;
                    result = render_service->GetBuffer(available, &output_bytes);
                    if (FAILED(result)) {
                        runtime_error_.store(static_cast<std::uint32_t>(runtime_error_from_hresult(result, false)),
                                             std::memory_order_release);
                        stream_failed = true;
                        break;
                    }
                    LARGE_INTEGER begin{}, end{};
                    QueryPerformanceCounter(&begin);
                    UINT64 capture_position = 0, capture_qpc = 0, render_position = 0, render_qpc = 0;
                    if (SUCCEEDED(capture_clock->GetPosition(&capture_position, &capture_qpc)) &&
                        SUCCEEDED(render_clock->GetPosition(&render_position, &render_qpc))) {
                        resampler.set_nominal_ratio(clock_drift.update(capture_position, capture_clock_frequency,
                                                                       capture_qpc, render_position,
                                                                       render_clock_frequency, render_qpc));
                    }
                    (void)resampler.render(render_input.data(), available, static_cast<float>(kSampleRate));
                    DWORD release_flags = 0;
                    if (effective_render_sample_format == AudioSampleFormat::pcm_s32) {
                        processor_->process_audio(render_input.data(), render_output.data(),
                                                  available, begin.QuadPart);
                        const bool all_silent = convert_stereo_float_to_pcm_s32(
                            render_output.data(), reinterpret_cast<std::int32_t*>(output_bytes), available);
                        if (all_silent) release_flags = AUDCLNT_BUFFERFLAGS_SILENT;
                    } else {
                        processor_->process_audio(render_input.data(), reinterpret_cast<StereoFrame*>(output_bytes),
                                                  available, begin.QuadPart);
                    }
                    QueryPerformanceCounter(&end);
                    result = render_service->ReleaseBuffer(available, release_flags);
                    if (FAILED(result)) {
                        runtime_error_.store(static_cast<std::uint32_t>(runtime_error_from_hresult(result, false)),
                                             std::memory_order_release);
                        stream_failed = true;
                        break;
                    }
                    const double elapsed = static_cast<double>(end.QuadPart - begin.QuadPart) /
                                           static_cast<double>(qpc_frequency.QuadPart);
                    const double period_seconds = static_cast<double>(available) / kSampleRate;
                    callback_cpu_.store(
                        static_cast<float>(100.0 * elapsed / std::max(period_seconds, 1.0e-6)),
                        std::memory_order_relaxed);
                    fifo_fill_.store(static_cast<std::uint32_t>(resampler.fill_frames()),
                                     std::memory_order_relaxed);
                    capture_overruns_.store(resampler.overruns(), std::memory_order_relaxed);
                    render_underruns_.store(resampler.underruns(), std::memory_order_relaxed);
                    resample_ratio_.store(resampler.current_ratio(), std::memory_order_relaxed);
                }
            }
            if (stream_failed) break;

            if (!drain_capture_packets(make_capture_drain_budget(capture_buffer_frames))) {
                stream_failed = true;
                break;
            }
        }
        render_client->Stop();
        capture_client->Stop();
        render_sample_format_.store(static_cast<std::uint32_t>(AudioSampleFormat::unknown),
                                    std::memory_order_release);
        if (mmcss_handle != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss_handle);
            mmcss_handle = nullptr;
        }
        if (stop_token.stop_requested() && !stream_failed) {
            capture_state_.store(static_cast<std::uint32_t>(StreamState::stopped), std::memory_order_release);
            render_state_.store(static_cast<std::uint32_t>(StreamState::stopped), std::memory_order_release);
        } else {
            capture_state_.store(static_cast<std::uint32_t>(StreamState::failed), std::memory_order_release);
            render_state_.store(static_cast<std::uint32_t>(StreamState::failed), std::memory_order_release);
        }
        } while (false);

        if (!initialized) {
            capture_state_.store(static_cast<std::uint32_t>(StreamState::failed), std::memory_order_release);
            render_state_.store(static_cast<std::uint32_t>(StreamState::failed), std::memory_order_release);
        }
        if (mmcss_handle != nullptr) AvRevertMmThreadCharacteristics(mmcss_handle);
        if (capture_event != nullptr) CloseHandle(capture_event);
        if (render_event != nullptr) CloseHandle(render_event);
        capture_event_.store(nullptr, std::memory_order_release);
        render_event_.store(nullptr, std::memory_order_release);

        const RuntimeAudioError runtime_error =
            static_cast<RuntimeAudioError>(runtime_error_.load(std::memory_order_acquire));
        const bool retry = ever_initialized && !stop_token.stop_requested() && running() &&
                           runtime_error != RuntimeAudioError::none &&
                           runtime_error != RuntimeAudioError::mmcss_unavailable;
        if (!retry) break;

        capture_state_.store(static_cast<std::uint32_t>(StreamState::starting), std::memory_order_release);
        render_state_.store(static_cast<std::uint32_t>(StreamState::starting), std::memory_order_release);
        const std::uint32_t backoff_ms = audio_reconnect_backoff_ms(reconnect_attempt++);
        for (std::uint32_t waited_ms = 0; waited_ms < backoff_ms && !stop_token.stop_requested() && running();
             waited_ms += 50U) {
            Sleep(std::min<std::uint32_t>(50U, backoff_ms - waited_ms));
        }
    }
    if (stop_token.stop_requested() || !running()) {
        capture_state_.store(static_cast<std::uint32_t>(StreamState::stopped), std::memory_order_release);
        render_state_.store(static_cast<std::uint32_t>(StreamState::stopped), std::memory_order_release);
    }
    running_.store(false, std::memory_order_release);
    CoUninitialize();
}

} // namespace

std::unique_ptr<IAudioBackend> create_wasapi_audio_backend() { return std::make_unique<WasapiAudioBackend>(); }

} // namespace sound_spatializer

#endif
