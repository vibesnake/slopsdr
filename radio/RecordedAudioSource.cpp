// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordedAudioSource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sdr::radio {
namespace {

constexpr std::uint16_t waveFormatPcm = 1;
constexpr std::uint16_t waveFormatIeeeFloat = 3;
constexpr std::uint16_t waveFormatExtensible = 0xfffe;

[[nodiscard]] std::uint16_t little16(const unsigned char* bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1]) << 8U;
}

[[nodiscard]] std::uint32_t little32(const unsigned char* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
           static_cast<std::uint32_t>(bytes[1]) << 8U |
           static_cast<std::uint32_t>(bytes[2]) << 16U |
           static_cast<std::uint32_t>(bytes[3]) << 24U;
}

[[nodiscard]] bool equalsFour(const std::array<unsigned char, 4>& value,
    const char (&text)[5]) noexcept
{
    return std::equal(value.begin(), value.end(), text);
}

[[nodiscard]] bool supportedExtensibleSubtype(const unsigned char* bytes,
    std::uint16_t& resolvedFormat) noexcept
{
    // SubFormat is a GUID: Data1 contains the ordinary WAVE format tag and
    // the remaining bytes are 0000-0010-8000-00aa00389b71.
    static constexpr std::array<unsigned char, 12> suffix{
        0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
        0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
    if (!std::equal(suffix.begin(), suffix.end(), bytes + 4)) return false;
    const std::uint16_t format = little16(bytes);
    if (format != waveFormatPcm && format != waveFormatIeeeFloat) return false;
    resolvedFormat = format;
    return true;
}

[[nodiscard]] std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right,
    const char* message)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::invalid_argument(message);
    }
    return left + right;
}

}  // namespace

RecordedAudioSource::RecordedAudioSource(std::filesystem::path path)
    : m_path(std::move(path))
{
    parse();
}

RecordedAudioSource::~RecordedAudioSource()
{
    static_cast<void>(stop());
}

bool RecordedAudioSource::hasWaveSignature(const std::filesystem::path& path) noexcept
{
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    return input.gcount() == static_cast<std::streamsize>(header.size()) &&
           std::equal(header.begin(), header.begin() + 4, "RIFF") &&
           std::equal(header.begin() + 8, header.end(), "WAVE");
}

const RecordedAudioMetadata& RecordedAudioSource::metadata() const noexcept { return m_metadata; }
const std::filesystem::path& RecordedAudioSource::path() const noexcept { return m_path; }

void RecordedAudioSource::parse()
{
    std::error_code error;
    const auto bytes = std::filesystem::file_size(m_path, error);
    if (error) throw std::invalid_argument("Recorded audio file cannot be read: " + error.message());
    if (bytes < 12) throw std::invalid_argument("WAV file is too small for a RIFF header");

    std::ifstream input(m_path, std::ios::binary);
    if (!input) throw std::invalid_argument("Recorded audio file cannot be opened");
    std::array<unsigned char, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        !std::equal(header.begin(), header.begin() + 4, "RIFF") ||
        !std::equal(header.begin() + 8, header.end(), "WAVE")) {
        throw std::invalid_argument("Recording is not a RIFF/WAVE audio file");
    }
    const std::uint64_t riffEnd = checkedAdd(8, little32(header.data() + 4),
        "WAV RIFF size overflows the file bounds");
    if (riffEnd > bytes || riffEnd < 12) {
        throw std::invalid_argument("WAV RIFF size exceeds the file bounds");
    }

    bool foundFormat = false;
    bool foundData = false;
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint64_t dataSize = 0;
    std::uint64_t offset = 12;
    while (offset < riffEnd) {
        if (riffEnd - offset < 8) {
            throw std::invalid_argument("WAV chunk header is truncated");
        }
        input.seekg(static_cast<std::streamoff>(offset));
        std::array<unsigned char, 8> chunkHeader{};
        input.read(reinterpret_cast<char*>(chunkHeader.data()), chunkHeader.size());
        if (input.gcount() != static_cast<std::streamsize>(chunkHeader.size())) {
            throw std::invalid_argument("WAV chunk header cannot be read");
        }
        std::array<unsigned char, 4> id{};
        std::copy_n(chunkHeader.begin(), id.size(), id.begin());
        const std::uint64_t size = little32(chunkHeader.data() + 4);
        const std::uint64_t dataOffset = offset + 8;
        const std::uint64_t dataEnd = checkedAdd(dataOffset, size,
            "WAV chunk size overflows the file bounds");
        if (dataEnd > riffEnd) {
            throw std::invalid_argument("WAV chunk exceeds the RIFF container bounds");
        }

        if (equalsFour(id, "fmt ")) {
            if (foundFormat) throw std::invalid_argument("WAV file contains multiple fmt chunks");
            if (size < 16) throw std::invalid_argument("WAV fmt chunk is too small");
            std::vector<unsigned char> fmt(static_cast<std::size_t>(size));
            input.seekg(static_cast<std::streamoff>(dataOffset));
            input.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (input.gcount() != static_cast<std::streamsize>(fmt.size())) {
                throw std::invalid_argument("WAV fmt chunk is truncated");
            }
            format = little16(fmt.data());
            channels = little16(fmt.data() + 2);
            sampleRate = little32(fmt.data() + 4);
            byteRate = little32(fmt.data() + 8);
            blockAlign = little16(fmt.data() + 12);
            bitsPerSample = little16(fmt.data() + 14);
            if (format == waveFormatExtensible) {
                if (size < 40 || little16(fmt.data() + 16) < 22 ||
                    !supportedExtensibleSubtype(fmt.data() + 24, format)) {
                    throw std::invalid_argument("WAV extensible subformat is unsupported");
                }
            }
            foundFormat = true;
        } else if (equalsFour(id, "data")) {
            if (foundData) throw std::invalid_argument("WAV file contains multiple data chunks");
            foundData = true;
            m_dataOffset = dataOffset;
            dataSize = size;
        }

        const std::uint64_t paddedEnd = checkedAdd(dataEnd, size & 1U,
            "WAV chunk alignment exceeds the file bounds");
        if (paddedEnd > riffEnd) throw std::invalid_argument("WAV chunk alignment exceeds the RIFF container bounds");
        offset = paddedEnd;
    }
    if (!foundFormat || !foundData) throw std::invalid_argument("WAV file requires one fmt chunk and one data chunk");
    if (channels == 0 || channels > 2 || sampleRate < 2) {
        throw std::invalid_argument("WAV audio must have a sample rate of at least 2 Hz and one or two channels");
    }
    const std::uint16_t bytesPerSample = static_cast<std::uint16_t>((bitsPerSample + 7U) / 8U);
    if (bitsPerSample % 8U != 0U || bytesPerSample == 0 ||
        blockAlign != channels * bytesPerSample ||
        byteRate != static_cast<std::uint64_t>(sampleRate) * blockAlign) {
        throw std::invalid_argument("WAV format has inconsistent frame alignment or byte rate");
    }

    RecordedAudioEncoding encoding;
    if (format == waveFormatPcm && bitsPerSample == 8) encoding = RecordedAudioEncoding::UnsignedPcm8;
    else if (format == waveFormatPcm && bitsPerSample == 16) encoding = RecordedAudioEncoding::SignedPcm16;
    else if (format == waveFormatPcm && bitsPerSample == 24) encoding = RecordedAudioEncoding::SignedPcm24;
    else if (format == waveFormatPcm && bitsPerSample == 32) encoding = RecordedAudioEncoding::SignedPcm32;
    else if (format == waveFormatIeeeFloat && bitsPerSample == 32) encoding = RecordedAudioEncoding::Float32;
    else throw std::invalid_argument("WAV codec is unsupported; use PCM 8/16/24/32-bit or IEEE float32 audio");

    if (dataSize == 0 || dataSize % blockAlign != 0U) {
        throw std::invalid_argument("WAV data chunk ends mid-frame or contains no audio frames");
    }
    m_metadata = {.sampleRate = sampleRate, .channelCount = channels,
                  .encoding = encoding, .frameCount = dataSize / blockAlign,
                  .bytesPerFrame = blockAlign};
}

RecordedAudioSourceOperationResult RecordedAudioSource::start()
{
    if (m_running.exchange(true)) return {true, false, "Recorded audio playback is already active"};
    {
        std::scoped_lock lock(m_fileMutex);
        m_file.open(m_path, std::ios::binary);
        if (!m_file) {
            m_running = false;
            return {false, false, "Recorded audio file cannot be opened for playback"};
        }
        m_file.seekg(static_cast<std::streamoff>(m_dataOffset +
            m_positionFrames.load() * m_metadata.bytesPerFrame));
    }
    m_ended = false;
    m_paused = false;
    m_resumeNeedsDeadline = false;
    m_nextDeadline = std::chrono::steady_clock::now();
    m_waitCondition.notify_all();
    return {true, true, "Recorded audio playback started"};
}

RecordedAudioSourceOperationResult RecordedAudioSource::seekFrames(std::uint64_t frame)
{
    const auto target = std::min(frame, m_metadata.frameCount);
    {
        std::scoped_lock lock(m_fileMutex);
        if (m_file.is_open()) {
            m_file.clear();
            m_file.seekg(static_cast<std::streamoff>(m_dataOffset +
                target * m_metadata.bytesPerFrame));
            if (!m_file) return {false, false, "Recorded audio seek could not reposition the WAV file"};
        }
    }
    m_positionFrames = target;
    m_ended = target == m_metadata.frameCount;
    m_resumeNeedsDeadline = true;
    m_waitCondition.notify_all();
    return {true, true, "Recorded audio playback position updated"};
}

RecordedAudioSourceOperationResult RecordedAudioSource::stop()
{
    const bool wasRunning = m_running.exchange(false);
    m_paused = false;
    m_resumeNeedsDeadline = false;
    {
        std::scoped_lock lock(m_fileMutex);
        if (m_file.is_open()) m_file.close();
    }
    m_positionFrames = 0;
    m_ended = false;
    m_waitCondition.notify_all();
    return {true, wasRunning, wasRunning ? "Recorded audio playback stopped" : "Recorded audio playback is already stopped"};
}

RecordedAudioReadResult RecordedAudioSource::read(
    std::span<float> samples, std::chrono::milliseconds timeout)
{
    if (samples.empty() || samples.size() % m_metadata.channelCount != 0U) {
        return {RecordedAudioReadStatus::Failed, 0, "Recorded audio source received an incomplete frame buffer"};
    }
    const auto waitUntilReady = [this, timeout] {
        std::unique_lock lock(m_waitMutex);
        return m_waitCondition.wait_for(lock, timeout, [this] {
            return !m_running.load() || !m_paused.load();
        });
    };
    while (m_paused) {
        if (!m_running) return {RecordedAudioReadStatus::Stopped, 0, "Recorded audio playback is stopped"};
        static_cast<void>(waitUntilReady());
        if (m_paused) return {RecordedAudioReadStatus::Timeout, 0, {}};
    }
    if (!m_running) return {RecordedAudioReadStatus::Stopped, 0, "Recorded audio playback is stopped"};
    if (m_resumeNeedsDeadline.exchange(false)) m_nextDeadline = std::chrono::steady_clock::now();
    if (m_positionFrames >= m_metadata.frameCount) {
        m_ended = true;
        return {RecordedAudioReadStatus::EndOfFile, 0, "Recorded audio playback reached end of file"};
    }
    {
        std::unique_lock lock(m_waitMutex);
        m_waitCondition.wait_until(lock, m_nextDeadline, [this] {
            return !m_running.load() || m_paused.load() || m_resumeNeedsDeadline.load();
        });
    }
    if (!m_running) return {RecordedAudioReadStatus::Stopped, 0, "Recorded audio playback is stopped"};
    if (m_paused) return {RecordedAudioReadStatus::Timeout, 0, {}};
    if (m_resumeNeedsDeadline.exchange(false)) m_nextDeadline = std::chrono::steady_clock::now();

    const auto remaining = m_metadata.frameCount - m_positionFrames.load();
    const auto capacity = samples.size() / m_metadata.channelCount;
    const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, remaining));
    std::vector<unsigned char> bytes(frames * m_metadata.bytesPerFrame);
    {
        std::scoped_lock lock(m_fileMutex);
        m_file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (m_file.gcount() != static_cast<std::streamsize>(bytes.size())) {
            return {RecordedAudioReadStatus::Failed, 0, "WAV data ended before its declared frame count"};
        }
    }
    const std::size_t bytesPerSample = m_metadata.bytesPerFrame / m_metadata.channelCount;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < m_metadata.channelCount; ++channel) {
            samples[frame * m_metadata.channelCount + channel] = decodeSample(
                bytes.data() + (frame * m_metadata.channelCount + channel) * bytesPerSample);
        }
    }
    m_positionFrames.fetch_add(frames);
    m_nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(static_cast<double>(frames) / m_metadata.sampleRate));
    return {RecordedAudioReadStatus::Frames, frames, {}};
}

void RecordedAudioSource::setPaused(bool paused) noexcept
{
    const bool wasPaused = m_paused.exchange(paused);
    if (wasPaused && !paused) m_resumeNeedsDeadline = true;
    m_waitCondition.notify_all();
}
bool RecordedAudioSource::paused() const noexcept { return m_paused; }
bool RecordedAudioSource::ended() const noexcept { return m_ended; }
std::uint64_t RecordedAudioSource::positionFrames() const noexcept { return m_positionFrames; }

float RecordedAudioSource::decodeSample(const unsigned char* bytes) const noexcept
{
    switch (m_metadata.encoding) {
    case RecordedAudioEncoding::UnsignedPcm8:
        return (static_cast<float>(bytes[0]) - 128.0F) / 128.0F;
    case RecordedAudioEncoding::SignedPcm16: {
        const auto value = static_cast<std::int16_t>(little16(bytes));
        return static_cast<float>(value) / 32768.0F;
    }
    case RecordedAudioEncoding::SignedPcm24: {
        std::int32_t value = static_cast<std::int32_t>(bytes[0]) |
                             static_cast<std::int32_t>(bytes[1]) << 8U |
                             static_cast<std::int32_t>(bytes[2]) << 16U;
        if ((value & 0x00800000) != 0) value |= static_cast<std::int32_t>(0xff000000);
        return static_cast<float>(value) / 8388608.0F;
    }
    case RecordedAudioEncoding::SignedPcm32: {
        const auto value = static_cast<std::int32_t>(little32(bytes));
        return static_cast<float>(value) / 2147483648.0F;
    }
    case RecordedAudioEncoding::Float32: {
        const float value = std::bit_cast<float>(little32(bytes));
        return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
    }
    }
    return 0.0F;
}

}  // namespace sdr::radio
