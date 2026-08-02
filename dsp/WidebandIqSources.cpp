// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WidebandIqSources.hpp"

#include "DeviceController.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

namespace sdr::dsp {
namespace {

constexpr double syntheticToneOffsetHz = 100'000.0;
constexpr double syntheticToneAmplitude = 0.5;
constexpr double twoPi = 6.28318530717958647692;
constexpr std::size_t recordedIqReadBlockSamples = 4'096;
constexpr std::size_t cf32BytesPerSample = sizeof(float) * 2;
constexpr std::uintmax_t maximumRecordedIqSidecarBytes = 64 * 1024;

[[nodiscard]] float decodeLittleEndianFloat(const unsigned char* bytes) noexcept
{
    const auto bits = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return std::bit_cast<float>(bits);
}

radio::WidebandIqSourceOperationResult operation(
    const devices::DeviceOperationResult& result)
{
    return {
        .succeeded = result.succeeded(),
        .stateChanged = result.stateChanged,
        .message = result.message,
    };
}

radio::WidebandIqReadStatus readStatus(devices::DeviceReadStatus status) noexcept
{
    switch (status) {
    case devices::DeviceReadStatus::Samples:
        return radio::WidebandIqReadStatus::Samples;
    case devices::DeviceReadStatus::Timeout:
        return radio::WidebandIqReadStatus::Timeout;
    case devices::DeviceReadStatus::Stopped:
        return radio::WidebandIqReadStatus::Stopped;
    case devices::DeviceReadStatus::Disconnected:
        return radio::WidebandIqReadStatus::Disconnected;
    case devices::DeviceReadStatus::Failed:
        return radio::WidebandIqReadStatus::Failed;
    }
    return radio::WidebandIqReadStatus::Failed;
}

}  // namespace

namespace {

enum class JsonType { Null, Boolean, Number, String, Array, Object };

struct JsonValue {
    JsonValue() = default;
    JsonValue(JsonType valueType, std::string valueText = {})
        : type(valueType)
        , text(std::move(valueText))
    {
    }

    JsonType type = JsonType::Null;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

class JsonParser final
{
public:
    explicit JsonParser(const std::string& input)
        : m_input(input)
    {
    }

    [[nodiscard]] JsonValue parseDocument()
    {
        auto value = parseValue(0);
        skipWhitespace();
        if (m_position != m_input.size()) fail("unexpected trailing content");
        return value;
    }

private:
    static constexpr std::size_t maximumDepth = 16;

    [[noreturn]] void fail(const std::string& reason) const
    {
        throw std::invalid_argument("Recorded IQ sidecar JSON is malformed: " + reason);
    }

    void skipWhitespace()
    {
        while (m_position < m_input.size()) {
            const char value = m_input[m_position];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++m_position;
        }
    }

    [[nodiscard]] char take()
    {
        if (m_position == m_input.size()) fail("unexpected end of input");
        return m_input[m_position++];
    }

    void expect(char expected)
    {
        if (take() != expected) fail(std::string("expected '") + expected + "'");
    }

    [[nodiscard]] JsonValue parseValue(std::size_t depth)
    {
        if (depth > maximumDepth) fail("nesting exceeds the 16-level limit");
        skipWhitespace();
        if (m_position == m_input.size()) fail("unexpected end of input");
        switch (m_input[m_position]) {
        case '{': return parseObject(depth + 1);
        case '[': return parseArray(depth + 1);
        case '"': return {JsonType::String, parseString()};
        case 't': return parseLiteral("true", JsonType::Boolean);
        case 'f': return parseLiteral("false", JsonType::Boolean);
        case 'n': return parseLiteral("null", JsonType::Null);
        default:
            if (m_input[m_position] == '-' ||
                (m_input[m_position] >= '0' && m_input[m_position] <= '9')) {
                return {JsonType::Number, parseNumber()};
            }
            fail("expected a JSON value");
        }
    }

    [[nodiscard]] JsonValue parseObject(std::size_t depth)
    {
        expect('{');
        JsonValue result;
        result.type = JsonType::Object;
        std::unordered_set<std::string> keys;
        skipWhitespace();
        if (m_position < m_input.size() && m_input[m_position] == '}') {
            ++m_position;
            return result;
        }
        while (true) {
            skipWhitespace();
            if (m_position == m_input.size() || m_input[m_position] != '"') {
                fail("object keys must be strings");
            }
            const std::string key = parseString();
            if (!keys.insert(key).second) {
                fail("duplicate object key '" + key + "'");
            }
            skipWhitespace();
            expect(':');
            result.object.emplace_back(key, parseValue(depth));
            skipWhitespace();
            const char delimiter = take();
            if (delimiter == '}') return result;
            if (delimiter != ',') fail("expected ',' or '}' after an object value");
        }
    }

    [[nodiscard]] JsonValue parseArray(std::size_t depth)
    {
        expect('[');
        JsonValue result;
        result.type = JsonType::Array;
        skipWhitespace();
        if (m_position < m_input.size() && m_input[m_position] == ']') {
            ++m_position;
            return result;
        }
        while (true) {
            result.array.push_back(parseValue(depth));
            skipWhitespace();
            const char delimiter = take();
            if (delimiter == ']') return result;
            if (delimiter != ',') fail("expected ',' or ']' after an array value");
        }
    }

    [[nodiscard]] JsonValue parseLiteral(const char* literal, JsonType type)
    {
        const std::string_view value(literal);
        if (m_input.compare(m_position, value.size(), value) != 0) {
            fail("invalid literal");
        }
        m_position += value.size();
        return {type};
    }

    [[nodiscard]] std::string parseNumber()
    {
        const std::size_t start = m_position;
        if (m_input[m_position] == '-') ++m_position;
        if (m_position == m_input.size()) fail("incomplete number");
        if (m_input[m_position] == '0') {
            ++m_position;
            if (m_position < m_input.size() && m_input[m_position] >= '0' &&
                m_input[m_position] <= '9') {
                fail("numbers cannot have leading zeroes");
            }
        } else {
            if (m_input[m_position] < '1' || m_input[m_position] > '9') {
                fail("invalid number");
            }
            do {
                ++m_position;
            } while (m_position < m_input.size() && m_input[m_position] >= '0' &&
                     m_input[m_position] <= '9');
        }
        if (m_position < m_input.size() && m_input[m_position] == '.') {
            ++m_position;
            const std::size_t fractionStart = m_position;
            while (m_position < m_input.size() && m_input[m_position] >= '0' &&
                   m_input[m_position] <= '9') {
                ++m_position;
            }
            if (m_position == fractionStart) fail("fraction is missing digits");
        }
        if (m_position < m_input.size() &&
            (m_input[m_position] == 'e' || m_input[m_position] == 'E')) {
            ++m_position;
            if (m_position < m_input.size() &&
                (m_input[m_position] == '+' || m_input[m_position] == '-')) {
                ++m_position;
            }
            const std::size_t exponentStart = m_position;
            while (m_position < m_input.size() && m_input[m_position] >= '0' &&
                   m_input[m_position] <= '9') {
                ++m_position;
            }
            if (m_position == exponentStart) fail("exponent is missing digits");
        }
        return m_input.substr(start, m_position - start);
    }

    [[nodiscard]] static unsigned hexDigit(char value)
    {
        if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
        throw std::invalid_argument("Recorded IQ sidecar JSON is malformed: invalid unicode escape");
    }

    [[nodiscard]] std::uint32_t parseUnicodeUnit()
    {
        if (m_input.size() - m_position < 4) fail("incomplete unicode escape");
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            result = (result << 4U) | hexDigit(m_input[m_position++]);
        }
        return result;
    }

    static void appendUtf8(std::string& output, std::uint32_t codepoint)
    {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    [[nodiscard]] std::string parseString()
    {
        expect('"');
        std::string result;
        while (m_position < m_input.size()) {
            const char value = take();
            if (value == '"') return result;
            if (static_cast<unsigned char>(value) < 0x20U) {
                fail("strings cannot contain control characters");
            }
            if (value != '\\') {
                result.push_back(value);
                continue;
            }
            switch (take()) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = parseUnicodeUnit();
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (m_input.size() - m_position < 6 || take() != '\\' || take() != 'u') {
                        fail("high surrogate is missing its low surrogate");
                    }
                    const std::uint32_t lowSurrogate = parseUnicodeUnit();
                    if (lowSurrogate < 0xdc00U || lowSurrogate > 0xdfffU) {
                        fail("high surrogate is followed by an invalid low surrogate");
                    }
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                        (lowSurrogate - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    fail("low surrogate is not preceded by a high surrogate");
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    const std::string& m_input;
    std::size_t m_position = 0;
};

[[nodiscard]] const JsonValue* findObjectField(
    const JsonValue& object, const char* key) noexcept
{
    for (const auto& [name, value] : object.object) {
        if (name == key) return &value;
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::uint64_t> strictUnsigned(
    const JsonValue* value, const char* key, std::string& error)
{
    if (!value) {
        error = std::string("Recorded IQ sidecar is missing '") + key + "'";
        return std::nullopt;
    }
    if (value->type != JsonType::Number || value->text.empty()) {
        error = std::string("Recorded IQ sidecar field '") + key + "' must be an unsigned integer";
        return std::nullopt;
    }
    for (const char digit : value->text) {
        if (digit < '0' || digit > '9') {
            error = std::string("Recorded IQ sidecar field '") + key + "' must be an unsigned integer";
            return std::nullopt;
        }
    }
    std::uint64_t result = 0;
    const auto [end, parseError] = std::from_chars(
        value->text.data(), value->text.data() + value->text.size(), result);
    if (parseError != std::errc{} || end != value->text.data() + value->text.size()) {
        error = std::string("Recorded IQ sidecar field '") + key + "' is outside the unsigned 64-bit range";
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool strictString(
    const JsonValue* value, const char* key, const char* expected, std::string& error)
{
    if (!value) {
        error = std::string("Recorded IQ sidecar is missing '") + key + "'";
        return false;
    }
    if (value->type != JsonType::String) {
        error = std::string("Recorded IQ sidecar field '") + key + "' must be a string";
        return false;
    }
    if (value->text != expected) {
        error = std::string("Recorded IQ sidecar field '") + key + "' must be '" + expected + "'";
        return false;
    }
    return true;
}

struct RecordedIqSidecarMetadata {
    std::uint64_t centerFrequency = 0;
    std::uint64_t sampleRate = 0;
};

[[nodiscard]] std::optional<RecordedIqSidecarMetadata> readRecordedIqSidecar(
    const std::filesystem::path& sidecar, std::uint64_t sampleCount, std::string& error)
{
    std::error_code fileError;
    const auto size = std::filesystem::file_size(sidecar, fileError);
    if (fileError) {
        error = "Recorded IQ sidecar cannot be read: " + fileError.message();
        return std::nullopt;
    }
    if (size > maximumRecordedIqSidecarBytes) {
        error = "Recorded IQ sidecar exceeds the 64 KiB size limit";
        return std::nullopt;
    }
    std::ifstream input(sidecar, std::ios::binary);
    if (!input) {
        error = "Recorded IQ sidecar cannot be opened";
        return std::nullopt;
    }
    std::string json(static_cast<std::size_t>(size), '\0');
    input.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (input.gcount() != static_cast<std::streamsize>(json.size())) {
        error = "Recorded IQ sidecar could not be read completely";
        return std::nullopt;
    }

    try {
        const JsonValue document = JsonParser(json).parseDocument();
        if (document.type != JsonType::Object) {
            error = "Recorded IQ sidecar root must be a JSON object";
            return std::nullopt;
        }
        const auto center = strictUnsigned(
            findObjectField(document, "hardware_center_frequency_hz"),
            "hardware_center_frequency_hz", error);
        if (!center) return std::nullopt;
        const auto rate = strictUnsigned(
            findObjectField(document, "sample_rate_hz"), "sample_rate_hz", error);
        if (!rate) return std::nullopt;
        if (*center == 0 || *rate == 0) {
            error = "Recorded IQ sidecar center frequency and sample rate must be positive";
            return std::nullopt;
        }
        if (!strictString(findObjectField(document, "sample_format"), "sample_format",
                          "cf32_le", error) ||
            !strictString(findObjectField(document, "byte_order"), "byte_order",
                          "little-endian", error)) {
            return std::nullopt;
        }
        if (const auto* written = findObjectField(document, "written_sample_count")) {
            const auto writtenCount = strictUnsigned(
                written, "written_sample_count", error);
            if (!writtenCount) return std::nullopt;
            if (*writtenCount != sampleCount) {
                error = "Recorded IQ sidecar written_sample_count does not match the raw file";
                return std::nullopt;
            }
        }
        return RecordedIqSidecarMetadata{*center, *rate};
    } catch (const std::invalid_argument& parseError) {
        error = parseError.what();
        return std::nullopt;
    }
}

void validateRecordedConfiguration(radio::RecordedIqSourceConfiguration& configuration,
    radio::WidebandIqCaptureMetadata& metadata, std::uint64_t& sampleCount)
{
    namespace fs = std::filesystem;
    const fs::path rawPath(configuration.path);
    if (rawPath.extension() != ".raw") {
        throw std::invalid_argument("Recorded IQ source must select a .raw file");
    }
    std::error_code error;
    const auto bytes = fs::file_size(rawPath, error);
    if (error) throw std::invalid_argument("Recorded IQ file cannot be read: " + error.message());
    if (bytes == 0 || bytes % (sizeof(float) * 2) != 0) {
        throw std::invalid_argument("Recorded IQ file is truncated: cf32_le samples require eight bytes");
    }
    fs::path sidecar = rawPath;
    sidecar.replace_extension(".json");
    std::string sidecarError;
    if (fs::exists(sidecar, error)) {
        if (const auto sidecarMetadata =
                readRecordedIqSidecar(sidecar, bytes / 8, sidecarError)) {
            configuration.centerFrequency = sidecarMetadata->centerFrequency;
            configuration.sampleRate = sidecarMetadata->sampleRate;
            configuration.format = "cf32_le";
        }
    } else if (error) {
        sidecarError = "Recorded IQ sidecar cannot be inspected: " + error.message();
    }
    if (configuration.format != "cf32_le") {
        throw std::invalid_argument("Recorded IQ format is unsupported; only cf32_le is supported");
    }
    if (configuration.centerFrequency == 0 || configuration.sampleRate == 0) {
        std::string message = "Recorded IQ metadata is missing";
        if (!sidecarError.empty()) message += "; " + sidecarError;
        throw std::invalid_argument(message + "; enter a center frequency and sample rate");
    }
    metadata = {configuration.centerFrequency, configuration.sampleRate};
    sampleCount = bytes / 8;
}

}  // namespace

DeviceControllerIqSource::DeviceControllerIqSource(
    std::shared_ptr<devices::DeviceController> device,
    radio::WidebandIqCaptureMetadata metadata)
    : m_device(std::move(device))
    , m_metadata(metadata)
{
    if (!m_device || !m_device->selectedDevice().has_value()) {
        throw std::invalid_argument("Hardware IQ source requires a selected device");
    }
}

radio::ReceiverSourceCapabilities DeviceControllerIqSource::capabilities() const noexcept
{
    const auto& capabilities = m_device->selectedDevice()->capabilities;
    return {
        .kind = radio::ReceiverSourceKind::Hardware,
        .hardwareTuningSupported = true,
        .gainControlSupported = capabilities.gainSupported,
        .ppmCorrectionSupported = capabilities.ppmCorrectionSupported,
        .automaticPpmCalibrationSupported =
            capabilities.ppmCorrectionSupported && capabilities.rtlSdrTestModeSupported,
    };
}

radio::WidebandIqCaptureMetadata DeviceControllerIqSource::captureMetadata() const noexcept
{
    return m_metadata;
}

radio::WidebandIqSourceOperationResult DeviceControllerIqSource::start()
{
    return operation(m_device->startReceiveStream());
}

radio::WidebandIqSourceOperationResult DeviceControllerIqSource::stop()
{
    return operation(m_device->stopReceiveStream());
}

radio::WidebandIqReadResult DeviceControllerIqSource::read(
    std::span<std::complex<float>> samples,
    std::chrono::milliseconds timeout)
{
    const auto result = m_device->readReceiveSamples(samples, timeout);
    return {
        .status = readStatus(result.status),
        .sampleCount = result.sampleCount,
        .message = result.message,
    };
}

SyntheticIqSource::SyntheticIqSource(radio::WidebandIqCaptureMetadata metadata)
    : m_metadata(metadata)
{
    if (m_metadata.effectiveSampleRate == 0) {
        throw std::invalid_argument("Synthetic IQ source requires a positive sample rate");
    }
}

radio::ReceiverSourceCapabilities SyntheticIqSource::capabilities() const noexcept
{
    return {.kind = radio::ReceiverSourceKind::Synthetic};
}

radio::WidebandIqCaptureMetadata SyntheticIqSource::captureMetadata() const noexcept
{
    return m_metadata;
}

radio::WidebandIqSourceOperationResult SyntheticIqSource::start()
{
    if (m_running) {
        return {true, false, "Synthetic IQ source is already active"};
    }
    m_phase = 0.0;
    m_nextDeadline = std::chrono::steady_clock::now();
    m_running = true;
    return {true, true, "Synthetic IQ source started"};
}

radio::WidebandIqSourceOperationResult SyntheticIqSource::stop()
{
    if (!m_running) {
        return {true, false, "Synthetic IQ source is already stopped"};
    }
    m_running = false;
    return {true, true, "Synthetic IQ source stopped"};
}

radio::WidebandIqReadResult SyntheticIqSource::read(
    std::span<std::complex<float>> samples,
    std::chrono::milliseconds timeout)
{
    static_cast<void>(timeout);
    if (!m_running) {
        return {radio::WidebandIqReadStatus::Stopped, 0, "Synthetic IQ source is stopped"};
    }
    if (samples.empty()) {
        return {radio::WidebandIqReadStatus::Failed, 0, "Synthetic IQ source received an empty buffer"};
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_nextDeadline > now) {
        std::this_thread::sleep_until(m_nextDeadline);
    }
    const double phaseStep = twoPi * syntheticToneOffsetHz /
                             static_cast<double>(m_metadata.effectiveSampleRate);
    for (auto& sample : samples) {
        sample = std::complex<float>(
            static_cast<float>(syntheticToneAmplitude * std::cos(m_phase)),
            static_cast<float>(syntheticToneAmplitude * std::sin(m_phase)));
        m_phase = std::remainder(m_phase + phaseStep, twoPi);
    }
    const auto duration = std::chrono::duration<double>(
        static_cast<double>(samples.size()) /
        static_cast<double>(m_metadata.effectiveSampleRate));
    m_nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        duration);
    return {radio::WidebandIqReadStatus::Samples, samples.size(), {}};
}

RecordedIqSource::RecordedIqSource(radio::RecordedIqSourceConfiguration configuration)
    : m_configuration(resolveConfiguration(std::move(configuration)))
    , m_readBuffer(recordedIqReadBlockSamples * cf32BytesPerSample)
{
    validateRecordedConfiguration(m_configuration, m_metadata, m_sampleCount);
}

radio::RecordedIqSourceConfiguration RecordedIqSource::resolveConfiguration(
    radio::RecordedIqSourceConfiguration configuration)
{
    radio::WidebandIqCaptureMetadata ignoredMetadata;
    std::uint64_t ignoredSampleCount = 0;
    validateRecordedConfiguration(configuration, ignoredMetadata, ignoredSampleCount);
    return configuration;
}

radio::ReceiverSourceCapabilities RecordedIqSource::capabilities() const noexcept
{
    return {.kind = radio::ReceiverSourceKind::RecordedIq, .sampleRateChangeSupported = false};
}

radio::WidebandIqCaptureMetadata RecordedIqSource::captureMetadata() const noexcept { return m_metadata; }
std::uint64_t RecordedIqSource::sampleCount() const noexcept { return m_sampleCount; }

radio::WidebandIqSourceOperationResult RecordedIqSource::start()
{
    if (m_running) return {true, false, "Recorded IQ playback is already active"};
    m_file.open(m_configuration.path, std::ios::binary);
    if (!m_file) return {false, false, "Recorded IQ file cannot be opened for playback"};
    m_samplesRead = 0;
    m_nextDeadline = std::chrono::steady_clock::now();
    m_paused = false;
    m_ended = false;
    m_running = true;
    return {true, true, "Recorded IQ playback started"};
}

radio::WidebandIqSourceOperationResult RecordedIqSource::stop()
{
    if (!m_running) return {true, false, "Recorded IQ playback is already stopped"};
    m_running = false;
    m_paused = false;
    m_file.close();
    return {true, true, "Recorded IQ playback stopped"};
}

radio::WidebandIqReadResult RecordedIqSource::read(
    std::span<std::complex<float>> samples, std::chrono::milliseconds timeout)
{
    static_cast<void>(timeout);
    if (!m_running) return {radio::WidebandIqReadStatus::Stopped, 0, "Recorded IQ playback is stopped"};
    if (m_paused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return {radio::WidebandIqReadStatus::Timeout, 0, {}};
    }
    if (m_resumeNeedsDeadline.exchange(false)) {
        m_nextDeadline = std::chrono::steady_clock::now();
    }
    if (samples.empty()) return {radio::WidebandIqReadStatus::Failed, 0, "Recorded IQ source received an empty buffer"};
    const auto samplesRead = m_samplesRead.load();
    if (samplesRead == m_sampleCount) {
        m_ended = true;
        return {radio::WidebandIqReadStatus::EndOfFile, 0, "Recorded IQ playback reached end of file"};
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_nextDeadline > now) std::this_thread::sleep_until(m_nextDeadline);
    const auto count = static_cast<std::size_t>(std::min({
        static_cast<std::uint64_t>(samples.size()),
        m_sampleCount - samplesRead,
        static_cast<std::uint64_t>(recordedIqReadBlockSamples),
    }));
    const std::size_t byteCount = count * cf32BytesPerSample;
    m_file.read(reinterpret_cast<char*>(m_readBuffer.data()),
                static_cast<std::streamsize>(byteCount));
    const auto bytesRead = static_cast<std::size_t>(m_file.gcount());
    const std::size_t completeSamples = bytesRead / cf32BytesPerSample;
    for (std::size_t index = 0; index < completeSamples; ++index) {
        const auto* bytes = m_readBuffer.data() + index * cf32BytesPerSample;
        samples[index] = {
            decodeLittleEndianFloat(bytes),
            decodeLittleEndianFloat(bytes + sizeof(float)),
        };
    }
    if (bytesRead != byteCount) {
        return {radio::WidebandIqReadStatus::Failed, completeSamples,
                "Recorded IQ file ended mid-sample"};
    }
    m_samplesRead.fetch_add(count);
    m_nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(static_cast<double>(count) /
            static_cast<double>(m_metadata.effectiveSampleRate)));
    return {radio::WidebandIqReadStatus::Samples, count, {}};
}

std::uint64_t RecordedIqSource::positionSamples() const noexcept { return m_samplesRead.load(); }
bool RecordedIqSource::paused() const noexcept { return m_paused; }
bool RecordedIqSource::ended() const noexcept { return m_ended; }
void RecordedIqSource::setPaused(bool paused) noexcept
{
    const bool wasPaused = m_paused.exchange(paused);
    if (wasPaused && !paused) m_resumeNeedsDeadline = true;
}

}  // namespace sdr::dsp
