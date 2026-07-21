#include "sound_spatializer/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <variant>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sound_spatializer {
namespace {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data{};
};

class JsonParser {
public:
    explicit JsonParser(std::string_view source) : source_(source) {}

    JsonValue parse() {
        JsonValue result = parse_value();
        skip_whitespace();
        if (position_ != source_.size()) {
            fail("unexpected trailing data");
        }
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(position_));
    }

    void skip_whitespace() noexcept {
        while (position_ < source_.size() &&
               (source_[position_] == ' ' || source_[position_] == '\n' || source_[position_] == '\r' ||
                source_[position_] == '\t')) {
            ++position_;
        }
    }

    bool consume(char expected) noexcept {
        skip_whitespace();
        if (position_ < source_.size() && source_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue parse_value() {
        skip_whitespace();
        if (position_ >= source_.size()) {
            fail("expected a JSON value");
        }
        switch (source_[position_]) {
        case '{': return JsonValue{parse_object()};
        case '[': return JsonValue{parse_array()};
        case '"': return JsonValue{parse_string()};
        case 't': parse_literal("true"); return JsonValue{true};
        case 'f': parse_literal("false"); return JsonValue{false};
        case 'n': parse_literal("null"); return JsonValue{nullptr};
        default:
            if (source_[position_] == '-' || (source_[position_] >= '0' && source_[position_] <= '9')) {
                return JsonValue{parse_number()};
            }
            fail("invalid JSON value");
        }
    }

    JsonValue::Object parse_object() {
        if (!consume('{')) {
            fail("expected object");
        }
        JsonValue::Object object;
        if (consume('}')) {
            return object;
        }
        for (;;) {
            skip_whitespace();
            if (position_ >= source_.size() || source_[position_] != '"') {
                fail("expected an object key");
            }
            std::string key = parse_string();
            if (!consume(':')) {
                fail("expected ':' after object key");
            }
            if (!object.emplace(std::move(key), parse_value()).second) {
                fail("duplicate object key");
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                fail("expected ',' between object members");
            }
        }
        return object;
    }

    JsonValue::Array parse_array() {
        if (!consume('[')) {
            fail("expected array");
        }
        JsonValue::Array array;
        if (consume(']')) {
            return array;
        }
        for (;;) {
            array.push_back(parse_value());
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                fail("expected ',' between array elements");
            }
        }
        return array;
    }

    std::string parse_string() {
        if (!consume('"')) {
            fail("expected string");
        }
        std::string result;
        while (position_ < source_.size()) {
            const char character = source_[position_++];
            if (character == '"') {
                return result;
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                fail("control character in string");
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            if (position_ >= source_.size()) {
                fail("unterminated escape");
            }
            const char escaped = source_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > source_.size()) {
                    fail("short unicode escape");
                }
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = source_[position_++];
                    codepoint <<= 4U;
                    if (digit >= '0' && digit <= '9') codepoint |= static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') codepoint |= static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') codepoint |= static_cast<unsigned>(digit - 'A' + 10);
                    else fail("invalid unicode escape");
                }
                if (codepoint <= 0x7FU) result.push_back(static_cast<char>(codepoint));
                else if (codepoint <= 0x7FFU) {
                    result.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
                    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
                } else {
                    result.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
                    result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
                    result.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
                }
                break;
            }
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    double parse_number() {
        const std::size_t begin = position_;
        if (source_[position_] == '-') ++position_;
        if (position_ >= source_.size()) fail("invalid number");
        if (source_[position_] == '0') ++position_;
        else {
            if (source_[position_] < '1' || source_[position_] > '9') fail("invalid number");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        if (position_ < source_.size() && source_[position_] == '.') {
            ++position_;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9') fail("invalid fraction");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) ++position_;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9') fail("invalid exponent");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        double result = 0.0;
        const auto conversion = std::from_chars(source_.data() + begin, source_.data() + position_, result);
        if (conversion.ec != std::errc{} || !std::isfinite(result)) fail("number is outside the supported range");
        return result;
    }

    void parse_literal(std::string_view literal) {
        if (source_.substr(position_, literal.size()) != literal) fail("invalid literal");
        position_ += literal.size();
    }

    std::string_view source_;
    std::size_t position_{};
};

[[nodiscard]] const JsonValue::Object& object(const JsonValue& value, std::string_view context) {
    if (const auto* result = std::get_if<JsonValue::Object>(&value.data)) return *result;
    throw std::runtime_error(std::string(context) + " must be an object");
}

[[nodiscard]] const JsonValue::Array& array(const JsonValue& value, std::string_view context) {
    if (const auto* result = std::get_if<JsonValue::Array>(&value.data)) return *result;
    throw std::runtime_error(std::string(context) + " must be an array");
}

[[nodiscard]] const JsonValue& field(const JsonValue::Object& value, std::string_view name) {
    const auto iterator = value.find(name);
    if (iterator == value.end()) throw std::runtime_error("missing required field '" + std::string(name) + "'");
    return iterator->second;
}

[[nodiscard]] const JsonValue* optional_field(const JsonValue::Object& value, std::string_view name) noexcept {
    const auto iterator = value.find(name);
    return iterator == value.end() ? nullptr : &iterator->second;
}

void reject_unknown_fields(const JsonValue::Object& value, std::initializer_list<std::string_view> allowed,
                           std::string_view context) {
    for (const auto& [name, ignored] : value) {
        (void)ignored;
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw std::runtime_error(std::string(context) + " contains unknown field '" + name + "'");
    }
}

[[nodiscard]] double number(const JsonValue& value, std::string_view context) {
    if (const auto* result = std::get_if<double>(&value.data)) return *result;
    throw std::runtime_error(std::string(context) + " must be a number");
}

[[nodiscard]] std::uint32_t unsigned_integer(const JsonValue& value, std::string_view context) {
    const double parsed = number(value, context);
    if (parsed < 0.0 || parsed > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(parsed) != parsed) {
        throw std::runtime_error(std::string(context) + " must be an unsigned integer");
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] bool boolean(const JsonValue& value, std::string_view context) {
    if (const auto* result = std::get_if<bool>(&value.data)) return *result;
    throw std::runtime_error(std::string(context) + " must be a boolean");
}

[[nodiscard]] const std::string& string(const JsonValue& value, std::string_view context) {
    if (const auto* result = std::get_if<std::string>(&value.data)) return *result;
    throw std::runtime_error(std::string(context) + " must be a string");
}

[[nodiscard]] std::optional<std::string> nullable_string(const JsonValue& value, std::string_view context) {
    if (std::holds_alternative<std::nullptr_t>(value.data)) return std::nullopt;
    return string(value, context);
}

[[nodiscard]] Vec3f vec3(const JsonValue& value, std::string_view context) {
    const auto& values = array(value, context);
    if (values.size() != 3) throw std::runtime_error(std::string(context) + " must contain 3 numbers");
    return {static_cast<float>(number(values[0], context)), static_cast<float>(number(values[1], context)),
            static_cast<float>(number(values[2], context))};
}

[[nodiscard]] Quaternionf quaternion(const JsonValue& value, std::string_view context) {
    const auto& values = array(value, context);
    if (values.size() != 4) throw std::runtime_error(std::string(context) + " must contain 4 numbers");
    return {static_cast<float>(number(values[0], context)), static_cast<float>(number(values[1], context)),
            static_cast<float>(number(values[2], context)), static_cast<float>(number(values[3], context))};
}

[[nodiscard]] MaterialBands bands(const JsonValue& value, std::string_view context) {
    const Vec3f parsed = vec3(value, context);
    return {parsed.x, parsed.y, parsed.z};
}

void append_escaped(std::ostringstream& output, std::string_view value) {
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
}

void append_optional_string(std::ostringstream& output, const std::optional<std::string>& value) {
    if (value) append_escaped(output, *value);
    else output << "null";
}

void append_vec3(std::ostringstream& output, const Vec3f& value) {
    output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

void append_quaternion(std::ostringstream& output, const Quaternionf& value) {
    output << '[' << value.w << ',' << value.x << ',' << value.y << ',' << value.z << ']';
}

void append_bands(std::ostringstream& output, const MaterialBands& value) {
    output << '[' << value.low << ',' << value.mid << ',' << value.high << ']';
}

[[nodiscard]] std::string_view audio_mode_name(AudioMode mode) noexcept {
    switch (mode) {
    case AudioMode::shared_low_latency: return "shared-low-latency";
    case AudioMode::exclusive_pro: return "exclusive-pro";
    case AudioMode::compatibility_256: return "compatibility";
    }
    return "shared-low-latency";
}

[[nodiscard]] AudioMode parse_audio_mode(std::string_view value) {
    if (value == "shared-low-latency") return AudioMode::shared_low_latency;
    if (value == "exclusive-pro") return AudioMode::exclusive_pro;
    if (value == "compatibility") return AudioMode::compatibility_256;
    throw std::runtime_error("unknown audio mode");
}

[[nodiscard]] std::string_view capture_provider_name(CaptureProvider provider) noexcept {
    switch (provider) {
    case CaptureProvider::native_driver: return "native-driver";
    case CaptureProvider::external_render: return "external-render";
    }
    return "native-driver";
}

[[nodiscard]] CaptureProvider parse_capture_provider(std::string_view value) {
    if (value == "native-driver") return CaptureProvider::native_driver;
    if (value == "external-render") return CaptureProvider::external_render;
    throw std::runtime_error("unknown capture provider");
}

[[nodiscard]] bool endpoint_ids_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

[[nodiscard]] std::string_view biquad_type_name(BiquadType type) noexcept {
    switch (type) {
    case BiquadType::peaking: return "peaking";
    case BiquadType::low_shelf: return "low-shelf";
    case BiquadType::high_shelf: return "high-shelf";
    case BiquadType::high_pass: return "high-pass";
    case BiquadType::low_pass: return "low-pass";
    }
    return "peaking";
}

[[nodiscard]] BiquadType parse_biquad_type(std::string_view value) {
    if (value == "peaking") return BiquadType::peaking;
    if (value == "low-shelf") return BiquadType::low_shelf;
    if (value == "high-shelf") return BiquadType::high_shelf;
    if (value == "high-pass") return BiquadType::high_pass;
    if (value == "low-pass") return BiquadType::low_pass;
    throw std::runtime_error("unknown biquad type");
}

[[nodiscard]] std::string_view command_type_name(EngineCommandType type) noexcept {
    switch (type) {
    case EngineCommandType::start: return "start";
    case EngineCommandType::stop: return "stop";
    case EngineCommandType::set_bypass: return "set-bypass";
    case EngineCommandType::set_output_device: return "set-output-device";
    case EngineCommandType::set_audio_mode: return "set-audio-mode";
    case EngineCommandType::calibrate_neutral: return "calibrate-neutral-pose";
    case EngineCommandType::set_scene: return "set-scene";
    case EngineCommandType::set_hrtf: return "set-hrtf";
    case EngineCommandType::set_headphone_eq: return "set-headphone-eq";
    case EngineCommandType::set_audio_route: return "set-audio-route";
    }
    return "start";
}

[[nodiscard]] EngineCommandType parse_command_type(std::string_view value) {
    for (std::uint32_t raw = 0; raw <= static_cast<std::uint32_t>(EngineCommandType::set_audio_route); ++raw) {
        const auto type = static_cast<EngineCommandType>(raw);
        if (command_type_name(type) == value) return type;
    }
    throw std::runtime_error("unknown engine command type");
}

[[nodiscard]] SceneConfigV1 parse_scene_value(const JsonValue& root) {
    const auto& root_object = object(root, "scene");
    reject_unknown_fields(root_object, {"schemaVersion", "audio", "tracking", "listener", "speakers", "hrtf",
                                        "headphoneEq", "room"}, "scene");
    SceneConfigV1 scene{};
    scene.schema_version = unsigned_integer(field(root_object, "schemaVersion"), "schemaVersion");

    const auto& audio = object(field(root_object, "audio"), "audio");
    reject_unknown_fields(audio, {"captureProvider", "captureEndpointId", "outputDeviceId", "mode",
                                  "sampleRate", "bufferFrames", "bypass", "masterGainDb", "roomMix"}, "audio");
    if (const JsonValue* provider = optional_field(audio, "captureProvider"))
        scene.audio.capture_provider = parse_capture_provider(string(*provider, "audio.captureProvider"));
    if (const JsonValue* endpoint = optional_field(audio, "captureEndpointId"))
        scene.audio.capture_endpoint_id = nullable_string(*endpoint, "audio.captureEndpointId");
    scene.audio.output_device_id = nullable_string(field(audio, "outputDeviceId"), "audio.outputDeviceId");
    scene.audio.mode = parse_audio_mode(string(field(audio, "mode"), "audio.mode"));
    scene.audio.sample_rate = unsigned_integer(field(audio, "sampleRate"), "audio.sampleRate");
    scene.audio.buffer_frames = unsigned_integer(field(audio, "bufferFrames"), "audio.bufferFrames");
    scene.audio.bypass = boolean(field(audio, "bypass"), "audio.bypass");
    scene.audio.master_gain_db = static_cast<float>(number(field(audio, "masterGainDb"), "audio.masterGainDb"));
    scene.audio.room_mix = static_cast<float>(number(field(audio, "roomMix"), "audio.roomMix"));

    const auto& tracking = object(field(root_object, "tracking"), "tracking");
    reject_unknown_fields(tracking, {"enabled", "cameraDeviceId", "minimumFps", "predictionLimitMs"}, "tracking");
    scene.tracking.enabled = boolean(field(tracking, "enabled"), "tracking.enabled");
    scene.tracking.camera_device_id = nullable_string(field(tracking, "cameraDeviceId"), "tracking.cameraDeviceId");
    scene.tracking.minimum_fps = unsigned_integer(field(tracking, "minimumFps"), "tracking.minimumFps");
    scene.tracking.prediction_limit_ms = static_cast<float>(number(field(tracking, "predictionLimitMs"), "tracking.predictionLimitMs"));

    const auto& listener = object(field(root_object, "listener"), "listener");
    reject_unknown_fields(listener, {"positionM", "neutralOrientation"}, "listener");
    scene.listener.position_m = vec3(field(listener, "positionM"), "listener.positionM");
    scene.listener.neutral_orientation = quaternion(field(listener, "neutralOrientation"), "listener.neutralOrientation");

    const auto& speakers = array(field(root_object, "speakers"), "speakers");
    if (speakers.size() != 2) throw std::runtime_error("speakers must contain exactly two entries");
    for (std::size_t index = 0; index < speakers.size(); ++index) {
        const auto& speaker = object(speakers[index], "speaker");
        reject_unknown_fields(speaker, {"channel", "positionM", "gainDb"}, "speaker");
        const std::string& channel = string(field(speaker, "channel"), "speaker.channel");
        scene.speakers[index].channel = channel == "left" ? SpeakerChannel::left
                                        : channel == "right" ? SpeakerChannel::right
                                                             : throw std::runtime_error("speaker.channel must be left or right");
        scene.speakers[index].position_m = vec3(field(speaker, "positionM"), "speaker.positionM");
        scene.speakers[index].gain_db = static_cast<float>(number(field(speaker, "gainDb"), "speaker.gainDb"));
    }

    const auto& hrtf = object(field(root_object, "hrtf"), "hrtf");
    reject_unknown_fields(hrtf, {"profileId", "sofaPath"}, "hrtf");
    scene.hrtf.profile_id = string(field(hrtf, "profileId"), "hrtf.profileId");
    scene.hrtf.sofa_path = nullable_string(field(hrtf, "sofaPath"), "hrtf.sofaPath");

    const auto& headphone_eq = object(field(root_object, "headphoneEq"), "headphoneEq");
    reject_unknown_fields(headphone_eq, {"enabled", "preampDb", "filters"}, "headphoneEq");
    scene.headphone_eq.enabled = boolean(field(headphone_eq, "enabled"), "headphoneEq.enabled");
    scene.headphone_eq.preamp_db = static_cast<float>(number(field(headphone_eq, "preampDb"), "headphoneEq.preampDb"));
    const auto& filters = array(field(headphone_eq, "filters"), "headphoneEq.filters");
    scene.headphone_eq.sections.clear();
    scene.headphone_eq.sections.reserve(filters.size());
    for (const auto& filter_value : filters) {
        const auto& filter = object(filter_value, "headphoneEq.filter");
        reject_unknown_fields(filter, {"type", "frequencyHz", "q", "gainDb"}, "headphoneEq.filter");
        scene.headphone_eq.sections.push_back({
            parse_biquad_type(string(field(filter, "type"), "filter.type")),
            static_cast<float>(number(field(filter, "frequencyHz"), "filter.frequencyHz")),
            static_cast<float>(number(field(filter, "q"), "filter.q")),
            static_cast<float>(number(field(filter, "gainDb"), "filter.gainDb")),
        });
    }

    const auto& room = object(field(root_object, "room"), "room");
    reject_unknown_fields(room, {"enabled", "dimensionsM", "surfaces", "earlyReflectionOrder",
                                 "earlyReflectionLimitMs", "lateReverbEnabled"}, "room");
    scene.room.enabled = boolean(field(room, "enabled"), "room.enabled");
    scene.room.dimensions_m = vec3(field(room, "dimensionsM"), "room.dimensionsM");
    scene.room.reflection_order = unsigned_integer(field(room, "earlyReflectionOrder"), "room.earlyReflectionOrder");
    scene.room.early_window_ms = static_cast<float>(number(field(room, "earlyReflectionLimitMs"), "room.earlyReflectionLimitMs"));
    scene.room.late_reverb_enabled = boolean(field(room, "lateReverbEnabled"), "room.lateReverbEnabled");
    const auto& surfaces = array(field(room, "surfaces"), "room.surfaces");
    if (surfaces.size() != scene.room.surfaces.size()) throw std::runtime_error("room.surfaces must contain six entries");
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        const auto& surface = object(surfaces[index], "room.surface");
        reject_unknown_fields(surface, {"materialId", "absorption", "diffusion"}, "room.surface");
        scene.room.surfaces[index].material_id = string(field(surface, "materialId"), "surface.materialId");
        scene.room.surfaces[index].absorption = bands(field(surface, "absorption"), "surface.absorption");
        scene.room.surfaces[index].diffusion = bands(field(surface, "diffusion"), "surface.diffusion");
    }
    return scene;
}

} // namespace

bool validate_scene_config(const SceneConfigV1& scene, std::string& error) noexcept {
    const auto fail = [&error](std::string message) {
        error = std::move(message);
        return false;
    };
    const auto finite_vec3 = [](const Vec3f& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (scene.schema_version != 1) return fail("unsupported scene schema version");
    if (scene.audio.output_device_id && scene.audio.output_device_id->empty())
        return fail("physical output endpoint id cannot be empty");
    switch (scene.audio.capture_provider) {
    case CaptureProvider::native_driver:
        if (scene.audio.capture_endpoint_id)
            return fail("native driver capture is selected by its vendor marker, not by endpoint id");
        break;
    case CaptureProvider::external_render:
        if (!scene.audio.capture_endpoint_id || scene.audio.capture_endpoint_id->empty())
            return fail("external render capture requires an explicit endpoint id");
        if (!scene.audio.output_device_id || scene.audio.output_device_id->empty())
            return fail("external render capture requires an explicit physical output endpoint");
        if (endpoint_ids_equal(*scene.audio.capture_endpoint_id, *scene.audio.output_device_id))
            return fail("capture and physical output endpoints must be different");
        break;
    default: return fail("unsupported capture provider");
    }
    if (scene.audio.sample_rate != kSampleRate) return fail("only 48000 Hz is supported");
    if (scene.audio.buffer_frames != 64 && scene.audio.buffer_frames != 128 && scene.audio.buffer_frames != 256)
        return fail("audio buffer must be 64, 128, or 256 frames");
    if (!std::isfinite(scene.audio.master_gain_db) || scene.audio.master_gain_db < -60.0F || scene.audio.master_gain_db > 6.0F)
        return fail("master gain is outside [-60, 6] dB");
    if (!std::isfinite(scene.audio.room_mix) || scene.audio.room_mix < 0.0F || scene.audio.room_mix > 1.0F)
        return fail("room mix is outside [0, 1]");
    if (scene.tracking.minimum_fps < 30 || scene.tracking.minimum_fps > 240) return fail("tracking FPS is outside [30, 240]");
    if (!std::isfinite(scene.tracking.prediction_limit_ms) || scene.tracking.prediction_limit_ms < 0.0F ||
        scene.tracking.prediction_limit_ms > 20.0F)
        return fail("prediction limit is outside [0, 20] ms");
    if (scene.speakers[0].channel != SpeakerChannel::left || scene.speakers[1].channel != SpeakerChannel::right)
        return fail("speakers must be ordered left then right");
    for (const auto& speaker : scene.speakers) {
        if (!finite_vec3(speaker.position_m) || !std::isfinite(speaker.gain_db) ||
            speaker.gain_db < -60.0F || speaker.gain_db > 12.0F)
            return fail("speaker position or gain is invalid");
    }
    if (!finite_vec3(scene.listener.position_m)) return fail("listener position is invalid");
    const Quaternionf q = scene.listener.neutral_orientation;
    const float q_length = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (q_length < 0.8F || q_length > 1.2F || !std::isfinite(q_length)) return fail("listener quaternion is invalid");
    if (scene.hrtf.profile_id.empty()) return fail("HRTF profile id cannot be empty");
    if (scene.headphone_eq.sections.size() > kMaximumEqSections) return fail("too many headphone EQ filters");
    if (!std::isfinite(scene.headphone_eq.preamp_db) || scene.headphone_eq.preamp_db < -24.0F ||
        scene.headphone_eq.preamp_db > 0.0F)
        return fail("headphone EQ preamp is outside [-24, 0] dB");
    for (const auto& filter : scene.headphone_eq.sections) {
        if (!std::isfinite(filter.frequency_hz) || !std::isfinite(filter.q) || !std::isfinite(filter.gain_db) ||
            filter.frequency_hz < 10.0F || filter.frequency_hz > 24'000.0F || filter.q <= 0.0F || filter.q > 30.0F ||
            filter.gain_db < -24.0F || filter.gain_db > 24.0F) return fail("headphone EQ filter is invalid");
    }
    if (!finite_vec3(scene.room.dimensions_m) || scene.room.dimensions_m.x <= 0.0F ||
        scene.room.dimensions_m.y <= 0.0F || scene.room.dimensions_m.z <= 0.0F)
        return fail("room dimensions must be positive");
    if (scene.room.reflection_order > 2 || !std::isfinite(scene.room.early_window_ms) ||
        scene.room.early_window_ms < 0.0F || scene.room.early_window_ms > 80.0F)
        return fail("early reflection settings are invalid");
    if (scene.room.enabled) {
        const auto inside_room = [&scene](const Vec3f& position) noexcept {
            return position.x >= -scene.room.dimensions_m.x * 0.5F &&
                   position.x <= scene.room.dimensions_m.x * 0.5F && position.y >= 0.0F &&
                   position.y <= scene.room.dimensions_m.y &&
                   position.z >= -scene.room.dimensions_m.z * 0.5F &&
                   position.z <= scene.room.dimensions_m.z * 0.5F;
        };
        if (!inside_room(scene.listener.position_m))
            return fail("listener must be inside the enabled rectangular room");
        for (const auto& speaker : scene.speakers)
            if (!inside_room(speaker.position_m))
                return fail("both speakers must be inside the enabled rectangular room");
    }
    for (const auto& surface : scene.room.surfaces) {
        if (surface.material_id.empty() ||
            !std::isfinite(surface.absorption.low) || !std::isfinite(surface.absorption.mid) ||
            !std::isfinite(surface.absorption.high) || !std::isfinite(surface.diffusion.low) ||
            !std::isfinite(surface.diffusion.mid) || !std::isfinite(surface.diffusion.high) ||
            surface.absorption.low < 0.0F || surface.absorption.low > 1.0F ||
            surface.absorption.mid < 0.0F || surface.absorption.mid > 1.0F ||
            surface.absorption.high < 0.0F || surface.absorption.high > 1.0F ||
            surface.diffusion.low < 0.0F || surface.diffusion.low > 1.0F ||
            surface.diffusion.mid < 0.0F || surface.diffusion.mid > 1.0F ||
            surface.diffusion.high < 0.0F || surface.diffusion.high > 1.0F)
            return fail("room surface material is invalid");
    }
    error.clear();
    return true;
}

std::string scene_config_to_json(const SceneConfigV1& scene) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "{\"schemaVersion\":" << scene.schema_version << ",\"audio\":{\"captureProvider\":";
    append_escaped(output, capture_provider_name(scene.audio.capture_provider));
    output << ",\"captureEndpointId\":";
    append_optional_string(output, scene.audio.capture_endpoint_id);
    output << ",\"outputDeviceId\":";
    append_optional_string(output, scene.audio.output_device_id);
    output << ",\"mode\":"; append_escaped(output, audio_mode_name(scene.audio.mode));
    output << ",\"sampleRate\":" << scene.audio.sample_rate << ",\"bufferFrames\":" << scene.audio.buffer_frames
           << ",\"bypass\":" << (scene.audio.bypass ? "true" : "false")
           << ",\"masterGainDb\":" << scene.audio.master_gain_db << ",\"roomMix\":" << scene.audio.room_mix << '}';
    output << ",\"tracking\":{\"enabled\":" << (scene.tracking.enabled ? "true" : "false") << ",\"cameraDeviceId\":";
    append_optional_string(output, scene.tracking.camera_device_id);
    output << ",\"minimumFps\":" << scene.tracking.minimum_fps << ",\"predictionLimitMs\":"
           << scene.tracking.prediction_limit_ms << '}';
    output << ",\"listener\":{\"positionM\":"; append_vec3(output, scene.listener.position_m);
    output << ",\"neutralOrientation\":"; append_quaternion(output, scene.listener.neutral_orientation); output << '}';
    output << ",\"speakers\":[";
    for (std::size_t index = 0; index < scene.speakers.size(); ++index) {
        if (index != 0) output << ',';
        const auto& speaker = scene.speakers[index];
        output << "{\"channel\":"; append_escaped(output, speaker.channel == SpeakerChannel::left ? "left" : "right");
        output << ",\"positionM\":"; append_vec3(output, speaker.position_m);
        output << ",\"gainDb\":" << speaker.gain_db << '}';
    }
    output << "],\"hrtf\":{\"profileId\":"; append_escaped(output, scene.hrtf.profile_id);
    output << ",\"sofaPath\":"; append_optional_string(output, scene.hrtf.sofa_path); output << '}';
    output << ",\"headphoneEq\":{\"enabled\":" << (scene.headphone_eq.enabled ? "true" : "false")
           << ",\"preampDb\":" << scene.headphone_eq.preamp_db << ",\"filters\":[";
    for (std::size_t index = 0; index < scene.headphone_eq.sections.size(); ++index) {
        if (index != 0) output << ',';
        const auto& filter = scene.headphone_eq.sections[index];
        output << "{\"type\":"; append_escaped(output, biquad_type_name(filter.type));
        output << ",\"frequencyHz\":" << filter.frequency_hz << ",\"q\":" << filter.q << ",\"gainDb\":" << filter.gain_db << '}';
    }
    output << "]},\"room\":{\"enabled\":" << (scene.room.enabled ? "true" : "false") << ",\"dimensionsM\":";
    append_vec3(output, scene.room.dimensions_m); output << ",\"surfaces\":[";
    for (std::size_t index = 0; index < scene.room.surfaces.size(); ++index) {
        if (index != 0) output << ',';
        const auto& surface = scene.room.surfaces[index];
        output << "{\"materialId\":"; append_escaped(output, surface.material_id); output << ",\"absorption\":";
        append_bands(output, surface.absorption); output << ",\"diffusion\":"; append_bands(output, surface.diffusion);
        output << '}';
    }
    output << "],\"earlyReflectionOrder\":" << scene.room.reflection_order
           << ",\"earlyReflectionLimitMs\":" << scene.room.early_window_ms
           << ",\"lateReverbEnabled\":" << (scene.room.late_reverb_enabled ? "true" : "false") << "}}";
    return output.str();
}

ParseResult<SceneConfigV1> scene_config_from_json(std::string_view json) noexcept {
    try {
        SceneConfigV1 scene = parse_scene_value(JsonParser(json).parse());
        std::string validation_error;
        if (!validate_scene_config(scene, validation_error)) return {std::nullopt, std::move(validation_error)};
        return {std::move(scene), {}};
    } catch (const std::exception& exception) {
        return {std::nullopt, exception.what()};
    } catch (...) {
        return {std::nullopt, "unknown JSON parsing error"};
    }
}

std::string engine_command_to_json(const EngineCommandV1& command) {
    std::ostringstream output;
    output << "{\"schemaVersion\":" << command.schema_version << ",\"type\":";
    append_escaped(output, command_type_name(command.type));
    switch (command.type) {
    case EngineCommandType::set_bypass: output << ",\"enabled\":" << (command.bool_value ? "true" : "false"); break;
    case EngineCommandType::set_output_device: output << ",\"deviceId\":"; append_escaped(output, command.string_value); break;
    case EngineCommandType::set_audio_mode: output << ",\"mode\":"; append_escaped(output, audio_mode_name(command.audio_mode)); break;
    case EngineCommandType::calibrate_neutral:
        output << ",\"quaternion\":"; append_quaternion(output, command.quaternion_value); break;
    case EngineCommandType::set_scene: output << ",\"scene\":" << scene_config_to_json(command.scene); break;
    case EngineCommandType::set_hrtf:
        output << ",\"profileId\":"; append_escaped(output, command.string_value);
        output << ",\"sofaPath\":"; append_optional_string(output, command.optional_string_value); break;
    case EngineCommandType::set_headphone_eq:
        output << ",\"eq\":{\"enabled\":" << (command.headphone_eq.enabled ? "true" : "false")
               << ",\"preampDb\":" << command.headphone_eq.preamp_db << ",\"profileName\":null,\"bands\":[";
        for (std::size_t index = 0; index < command.headphone_eq.sections.size(); ++index) {
            if (index != 0) output << ',';
            const auto& filter = command.headphone_eq.sections[index];
            output << "{\"id\":\"native-" << index << "\",\"enabled\":true,\"type\":";
            append_escaped(output, filter.type == BiquadType::peaking ? "peak" : biquad_type_name(filter.type));
            output << ",\"frequencyHz\":" << filter.frequency_hz << ",\"gainDb\":" << filter.gain_db
                   << ",\"q\":" << filter.q << '}';
        }
        output << "]}"; break;
    case EngineCommandType::set_audio_route:
        output << ",\"captureProvider\":";
        append_escaped(output, capture_provider_name(command.capture_provider));
        output << ",\"captureEndpointId\":";
        append_optional_string(output, command.capture_endpoint_id);
        output << ",\"outputDeviceId\":";
        append_optional_string(output, command.output_device_id);
        break;
    default: break;
    }
    output << '}';
    return output.str();
}

ParseResult<EngineCommandV1> engine_command_from_json(std::string_view json) noexcept {
    try {
        const JsonValue root = JsonParser(json).parse();
        const auto& root_object = object(root, "command");
        EngineCommandV1 command{};
        command.schema_version = unsigned_integer(field(root_object, "schemaVersion"), "schemaVersion");
        if (command.schema_version != 1) throw std::runtime_error("unsupported command schema version");
        command.type = parse_command_type(string(field(root_object, "type"), "type"));
        switch (command.type) {
        case EngineCommandType::start:
        case EngineCommandType::stop:
            reject_unknown_fields(root_object, {"schemaVersion", "type"}, "command");
            break;
        case EngineCommandType::set_bypass:
            reject_unknown_fields(root_object, {"schemaVersion", "type", "enabled"}, "command");
            command.bool_value = boolean(field(root_object, "enabled"), "enabled"); break;
        case EngineCommandType::set_output_device:
            reject_unknown_fields(root_object, {"schemaVersion", "type", "deviceId"}, "command");
            command.string_value = string(field(root_object, "deviceId"), "deviceId"); break;
        case EngineCommandType::set_audio_mode:
            reject_unknown_fields(root_object, {"schemaVersion", "type", "mode"}, "command");
            command.audio_mode = parse_audio_mode(string(field(root_object, "mode"), "mode")); break;
        case EngineCommandType::calibrate_neutral:
        {
            reject_unknown_fields(root_object, {"schemaVersion", "type", "quaternion"}, "command");
            const Quaternionf parsed_quaternion = quaternion(field(root_object, "quaternion"), "quaternion");
            const float norm = std::sqrt(parsed_quaternion.w * parsed_quaternion.w + parsed_quaternion.x * parsed_quaternion.x +
                                         parsed_quaternion.y * parsed_quaternion.y + parsed_quaternion.z * parsed_quaternion.z);
            if (!std::isfinite(norm) || norm < 0.8F || norm > 1.2F)
                throw std::runtime_error("calibration quaternion must be finite and approximately unit length");
            command.quaternion_value = parsed_quaternion.normalized_value();
            break;
        }
        case EngineCommandType::set_scene:
            reject_unknown_fields(root_object, {"schemaVersion", "type", "scene"}, "command");
            command.scene = parse_scene_value(field(root_object, "scene")); break;
        case EngineCommandType::set_hrtf: {
            reject_unknown_fields(root_object, {"schemaVersion", "type", "profileId", "sofaPath"}, "command");
            command.string_value = string(field(root_object, "profileId"), "profileId");
            command.optional_string_value = nullable_string(field(root_object, "sofaPath"), "sofaPath");
            break;
        }
        case EngineCommandType::set_headphone_eq: {
            reject_unknown_fields(root_object, {"schemaVersion", "type", "eq"}, "command");
            const auto& eq = object(field(root_object, "eq"), "eq");
            reject_unknown_fields(eq, {"enabled", "preampDb", "profileName", "bands"}, "eq");
            command.headphone_eq.enabled = boolean(field(eq, "enabled"), "eq.enabled");
            if (const JsonValue* preamp = optional_field(eq, "preampDb"))
                command.headphone_eq.preamp_db = static_cast<float>(number(*preamp, "eq.preampDb"));
            const auto& bands_value = array(field(eq, "bands"), "eq.bands");
            for (const JsonValue& band_value : bands_value) {
                const auto& band = object(band_value, "eq.band");
                reject_unknown_fields(band, {"id", "enabled", "type", "frequencyHz", "gainDb", "q"}, "eq.band");
                if (const JsonValue* enabled = optional_field(band, "enabled"); enabled && !boolean(*enabled, "band.enabled"))
                    continue;
                const std::string& type_name = string(field(band, "type"), "band.type");
                const BiquadType type = type_name == "peak" ? BiquadType::peaking : parse_biquad_type(type_name);
                command.headphone_eq.sections.push_back({
                    type,
                    static_cast<float>(number(field(band, "frequencyHz"), "band.frequencyHz")),
                    static_cast<float>(number(field(band, "q"), "band.q")),
                    static_cast<float>(number(field(band, "gainDb"), "band.gainDb")),
                });
            }
            if (command.headphone_eq.sections.size() > kMaximumEqSections)
                throw std::runtime_error("eq contains more than 16 enabled bands");
            break;
        }
        case EngineCommandType::set_audio_route:
            reject_unknown_fields(root_object, {"schemaVersion", "type", "captureProvider",
                                                 "captureEndpointId", "outputDeviceId"}, "command");
            command.capture_provider = parse_capture_provider(
                string(field(root_object, "captureProvider"), "captureProvider"));
            command.capture_endpoint_id = nullable_string(
                field(root_object, "captureEndpointId"), "captureEndpointId");
            command.output_device_id = nullable_string(field(root_object, "outputDeviceId"), "outputDeviceId");
            break;
        }
        return {std::move(command), {}};
    } catch (const std::exception& exception) {
        return {std::nullopt, exception.what()};
    } catch (...) {
        return {std::nullopt, "unknown command JSON parsing error"};
    }
}

std::string engine_status_to_json(const EngineStatusV1& status) {
    const auto stream_name = [](StreamState state) -> std::string_view {
        switch (state) {
        case StreamState::stopped: return "stopped";
        case StreamState::starting: return "starting";
        case StreamState::running: return "running";
        case StreamState::degraded: return "degraded";
        case StreamState::failed: return "failed";
        }
        return "failed";
    };
    const auto tracking_name = [](TrackingState state) -> std::string_view {
        switch (state) {
        case TrackingState::unavailable: return "lost";
        case TrackingState::tracking: return "tracking";
        case TrackingState::held: return "held";
        case TrackingState::returning_to_neutral: return "returning-to-neutral";
        }
        return "lost";
    };
    const auto sample_format_name = [](AudioSampleFormat format) -> std::string_view {
        switch (format) {
        case AudioSampleFormat::unknown: return "unknown";
        case AudioSampleFormat::float32: return "float32";
        case AudioSampleFormat::pcm_s32: return "pcm-s32";
        }
        return "unknown";
    };
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "{\"schemaVersion\":" << status.schema_version << ",\"captureState\":";
    append_escaped(output, stream_name(status.capture_state));
    output << ",\"renderState\":"; append_escaped(output, stream_name(status.render_state));
    output << ",\"trackingState\":"; append_escaped(output, tracking_name(status.tracking_state));
    output << ",\"audioMode\":"; append_escaped(output, audio_mode_name(status.audio_mode));
    output << ",\"renderSampleFormat\":";
    append_escaped(output, sample_format_name(status.render_sample_format));
    output << ",\"captureSampleRate\":" << status.capture_sample_rate
           << ",\"renderSampleRate\":" << status.render_sample_rate
           << ",\"capturePeriodFrames\":" << status.capture_period_frames
           << ",\"renderPeriodFrames\":" << status.render_period_frames
           << ",\"fifoFillFrames\":" << status.fifo_fill_frames << ",\"xruns\":" << status.xruns
           << ",\"callbackCpuPercent\":" << status.callback_cpu_percent << ",\"trackingHz\":" << status.tracking_hz
           << ",\"latencyP50Ms\":" << status.latency_p50_ms << ",\"latencyP95Ms\":" << status.latency_p95_ms
           << ",\"resampleRatio\":" << status.current_resample_ratio
           << ",\"potentiallyBinaural\":" << (status.potentially_binaural ? "true" : "false")
           << ",\"lastError\":";
    append_escaped(output, status.last_error);
    output << '}';
    return output.str();
}

ConfigStore::ConfigStore(std::filesystem::path base_directory) : base_directory_(std::move(base_directory)) {}

std::filesystem::path ConfigStore::default_base_directory() {
#if defined(_WIN32)
    wchar_t* local_app_data = nullptr;
    std::size_t local_app_data_length = 0;
    if (_wdupenv_s(&local_app_data, &local_app_data_length, L"LOCALAPPDATA") == 0 && local_app_data != nullptr) {
        const std::filesystem::path result = std::filesystem::path(local_app_data) / L"SoundSpatializer";
        std::free(local_app_data);
        return result;
    }
    std::free(local_app_data);
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr) return std::filesystem::path(xdg) / "SoundSpatializer";
    if (const char* home = std::getenv("HOME"); home != nullptr) return std::filesystem::path(home) / ".config" / "SoundSpatializer";
#endif
    return std::filesystem::temp_directory_path() / "SoundSpatializer";
}

bool ConfigStore::save_scene(const SceneConfigV1& scene, std::string& error) const noexcept {
    try {
        if (!validate_scene_config(scene, error)) return false;
        std::filesystem::create_directories(base_directory_);
        const std::filesystem::path destination = scene_path();
        const std::filesystem::path temporary = destination.string() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) { error = "could not create temporary config file"; return false; }
            const std::string json = scene_config_to_json(scene);
            stream.write(json.data(), static_cast<std::streamsize>(json.size()));
            stream.flush();
            if (!stream) { error = "could not flush temporary config file"; return false; }
        }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "atomic config replacement failed with Win32 error " + std::to_string(GetLastError());
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
#else
        std::filesystem::rename(temporary, destination);
#endif
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "unknown config persistence error";
        return false;
    }
}

ParseResult<SceneConfigV1> ConfigStore::load_scene() const noexcept {
    try {
        std::ifstream stream(scene_path(), std::ios::binary);
        if (!stream) return {std::nullopt, "scene config does not exist"};
        std::ostringstream contents;
        contents << stream.rdbuf();
        if (!stream.good() && !stream.eof()) return {std::nullopt, "could not read scene config"};
        return scene_config_from_json(contents.str());
    } catch (const std::exception& exception) {
        return {std::nullopt, exception.what()};
    } catch (...) {
        return {std::nullopt, "unknown config loading error"};
    }
}

} // namespace sound_spatializer
