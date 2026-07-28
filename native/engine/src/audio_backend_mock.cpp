#include "sound_spatializer/audio_backend.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace sound_spatializer {
namespace {

[[nodiscard]] bool endpoint_ids_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

} // namespace

bool validate_audio_backend_config(const AudioBackendConfig& config, std::string& error) noexcept {
    if (config.capture_provider != CaptureProvider::native_driver &&
        config.capture_provider != CaptureProvider::external_render) {
        error = "unsupported capture provider";
        return false;
    }
    if (config.input_layout != InputLayout::stereo &&
        config.input_layout != InputLayout::surround_5_1) {
        error = "unsupported input layout";
        return false;
    }
    if (config.input_layout == InputLayout::surround_5_1 &&
        config.capture_provider != CaptureProvider::external_render) {
        error = "5.1 capture requires an external render endpoint such as a 5.1-configured VB-CABLE";
        return false;
    }
    if (config.capture_provider == CaptureProvider::external_render) {
        if (!config.native_test_override_endpoint_id.empty()) {
            error = "the native test endpoint override is not allowed with an external capture provider";
            return false;
        }
        if (config.capture_endpoint_id.empty()) {
            error = "external render capture requires an explicit active endpoint id";
            return false;
        }
        if (config.physical_output_endpoint_id.empty()) {
            error = "external render capture requires an explicit physical output endpoint";
            return false;
        }
    } else if (!config.capture_endpoint_id.empty()) {
        error = "native driver capture is selected by its vendor marker, not by a persisted endpoint id";
        return false;
    }

    const std::string_view selected_capture = config.capture_provider == CaptureProvider::external_render
                                                  ? std::string_view(config.capture_endpoint_id)
                                                  : std::string_view(config.native_test_override_endpoint_id);
    if (!selected_capture.empty() && !config.physical_output_endpoint_id.empty() &&
        endpoint_ids_equal(selected_capture, config.physical_output_endpoint_id)) {
        error = "the capture endpoint cannot be selected as the physical output (audio loop prevented)";
        return false;
    }
    error.clear();
    return true;
}

AudioEndpointSelection select_capture_render_endpoint(std::span<const AudioEndpointDescriptor> endpoints,
                                                       CaptureProvider provider,
                                                       std::string_view explicit_endpoint_id,
                                                       std::string_view native_test_override_id) {
    if (provider != CaptureProvider::native_driver && provider != CaptureProvider::external_render)
        return {{}, "unsupported capture provider"};
    if (!native_test_override_id.empty()) {
        if (provider != CaptureProvider::native_driver)
            return {{}, "the native test endpoint override is only valid with the native capture provider"};
        const auto iterator = std::find_if(endpoints.begin(), endpoints.end(), [native_test_override_id](const auto& endpoint) {
            return endpoint.active && endpoint_ids_equal(endpoint.id, native_test_override_id);
        });
        if (iterator == endpoints.end()) return {{}, "the explicit native test endpoint id is not active"};
        return {iterator->id, {}};
    }

    if (provider == CaptureProvider::external_render) {
        if (explicit_endpoint_id.empty())
            return {{}, "external render capture requires an explicit endpoint id"};
        const auto iterator = std::find_if(endpoints.begin(), endpoints.end(), [explicit_endpoint_id](const auto& endpoint) {
            return endpoint.active && endpoint_ids_equal(endpoint.id, explicit_endpoint_id);
        });
        if (iterator == endpoints.end()) return {{}, "the external render capture endpoint is not active"};
        if (iterator->endpoint_marker == 1)
            return {{}, "the external render capture endpoint carries the Sound Spatializer marker"};
        return {iterator->id, {}};
    }

    if (!explicit_endpoint_id.empty())
        return {{}, "native driver capture does not accept a persisted endpoint id"};
    const AudioEndpointDescriptor* selected = nullptr;
    for (const auto& endpoint : endpoints) {
        if (!endpoint.active || endpoint.endpoint_marker != 1 || endpoint.contract_version != 1) continue;
        if (selected != nullptr) {
            return {{}, "multiple active Sound Spatializer endpoints advertise contract version 1"};
        }
        selected = &endpoint;
    }
    if (selected == nullptr) {
        return {{}, "no active Sound Spatializer endpoint advertises the vendor marker and contract version 1"};
    }
    return {selected->id, {}};
}

bool MockAudioBackend::start(const AudioBackendConfig& config, IAudioProcessor& processor, std::string& error) {
    if (running_) {
        error.clear();
        return true;
    }
    if (!validate_audio_backend_config(config, error)) return false;
    if (config.requested_buffer_frames == 0) {
        error = "mock audio buffer size cannot be zero";
        return false;
    }
    processor_ = &processor;
    diagnostics_ = {};
    diagnostics_.capture_state = StreamState::running;
    diagnostics_.render_state = StreamState::running;
    diagnostics_.capture_channels = expected_input_channel_count(config.input_layout);
    diagnostics_.capture_channel_mask =
        config.input_layout == InputLayout::surround_5_1 ? kSurround51ChannelMask : kStereoChannelMask;
    diagnostics_.capture_sample_rate = kSampleRate;
    diagnostics_.render_sample_rate = kSampleRate;
    diagnostics_.render_sample_format = AudioSampleFormat::float32;
    diagnostics_.capture_endpoint_id =
        config.capture_provider == CaptureProvider::external_render
            ? config.capture_endpoint_id
            : (!config.native_test_override_endpoint_id.empty()
                   ? config.native_test_override_endpoint_id
                   : "mock-native-render-endpoint");
    diagnostics_.capture_period_frames = config.requested_buffer_frames;
    diagnostics_.render_period_frames = config.requested_buffer_frames;
    running_ = true;
    error.clear();
    return true;
}

void MockAudioBackend::stop() noexcept {
    running_ = false;
    processor_ = nullptr;
    diagnostics_.capture_state = StreamState::stopped;
    diagnostics_.render_state = StreamState::stopped;
    diagnostics_.render_sample_format = AudioSampleFormat::unknown;
}

void MockAudioBackend::process_block(const ProgrammeFrame* input, StereoFrame* output, std::size_t frame_count,
                                     std::int64_t render_qpc) noexcept {
    if (!running_ || processor_ == nullptr || output == nullptr) {
        if (output != nullptr) std::fill_n(output, frame_count, StereoFrame{});
        return;
    }
    processor_->process_audio(input, output, frame_count, render_qpc);
}

void MockAudioBackend::process_block(const StereoFrame* input, StereoFrame* output, std::size_t frame_count,
                                     std::int64_t render_qpc) noexcept {
    if (!running_ || processor_ == nullptr || output == nullptr) {
        if (output != nullptr) std::fill_n(output, frame_count, StereoFrame{});
        return;
    }

    constexpr std::size_t kConversionChunkFrames = 2'048;
    std::array<ProgrammeFrame, kConversionChunkFrames> programme{};
    std::size_t offset = 0;
    while (offset < frame_count) {
        const std::size_t count = std::min(kConversionChunkFrames, frame_count - offset);
        for (std::size_t frame = 0; frame < count; ++frame) {
            programme[frame] = {};
            if (input != nullptr) {
                programme[frame].front_left = input[offset + frame].left;
                programme[frame].front_right = input[offset + frame].right;
            }
        }
        processor_->process_audio(programme.data(), output + offset, count, render_qpc);
        offset += count;
    }
}

#if defined(_WIN32)
std::unique_ptr<IAudioBackend> create_wasapi_audio_backend();
#endif

std::unique_ptr<IAudioBackend> create_system_audio_backend() {
#if defined(_WIN32)
    return create_wasapi_audio_backend();
#else
    return std::make_unique<MockAudioBackend>();
#endif
}

} // namespace sound_spatializer
