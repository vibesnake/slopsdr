// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordedAudioSource.hpp"

#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <vector>

namespace {

void put16(std::vector<unsigned char>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
}
void put32(std::vector<unsigned char>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}
void appendChunk(std::vector<unsigned char>& file, const char (&id)[5],
    std::span<const unsigned char> data)
{
    file.insert(file.end(), id, id + 4);
    put32(file, static_cast<std::uint32_t>(data.size()));
    file.insert(file.end(), data.begin(), data.end());
    if (data.size() % 2U != 0U) file.push_back(0);
}
std::filesystem::path writeWave(const QTemporaryDir& directory,
    const QString& name, std::span<const unsigned char> fmt,
    std::span<const unsigned char> data, bool dataFirst = false,
    bool optionalChunk = false)
{
    std::vector<unsigned char> chunks;
    const std::array<unsigned char, 3> optional{1, 2, 3};
    if (dataFirst) appendChunk(chunks, "data", data);
    if (optionalChunk) appendChunk(chunks, "JUNK", optional);
    appendChunk(chunks, "fmt ", fmt);
    if (!dataFirst) appendChunk(chunks, "data", data);
    std::vector<unsigned char> file{'R', 'I', 'F', 'F'};
    put32(file, static_cast<std::uint32_t>(chunks.size() + 4U));
    file.insert(file.end(), {'W', 'A', 'V', 'E'});
    file.insert(file.end(), chunks.begin(), chunks.end());
    const auto path = std::filesystem::path(directory.path().toStdString()) / name.toStdString();
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return path;
}
std::vector<unsigned char> pcmFormat(std::uint16_t tag, std::uint16_t channels,
    std::uint32_t rate, std::uint16_t bits)
{
    const std::uint16_t bytes = static_cast<std::uint16_t>(bits / 8U);
    std::vector<unsigned char> fmt;
    put16(fmt, tag); put16(fmt, channels); put32(fmt, rate);
    put32(fmt, rate * channels * bytes); put16(fmt, channels * bytes); put16(fmt, bits);
    return fmt;
}

class RecordedAudioSourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void detectsAndDecodesPcmIndependentOfExtension();
    void decodesSupportedPcmWidths();
    void handlesFloatAndExtensibleFormat();
    void rejectsMalformedAndUnsupportedWaves();
    void pausesResumesAndReportsEndDeterministically();
    void seeksByDecodedFrameAndClampsTargets();
};

void RecordedAudioSourceTest::detectsAndDecodesPcmIndependentOfExtension()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::array<unsigned char, 8> data{0x00, 0x80, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x40};
    const auto path = writeWave(directory, "capture.data", pcmFormat(1, 2, 8'000, 16), data, true, true);
    QVERIFY(sdr::radio::RecordedAudioSource::hasWaveSignature(path));
    sdr::radio::RecordedAudioSource source(path);
    QCOMPARE(source.metadata().sampleRate, 8'000U);
    QCOMPARE(source.metadata().channelCount, 2U);
    QCOMPARE(source.metadata().frameCount, 2U);
    QVERIFY(source.start().succeeded);
    std::array<float, 4> decoded{};
    const auto result = source.read(decoded, std::chrono::milliseconds(1));
    QCOMPARE(result.status, sdr::radio::RecordedAudioReadStatus::Frames);
    QCOMPARE(result.frameCount, std::size_t{2});
    QVERIFY(decoded[0] <= -0.999F);
    QVERIFY(decoded[1] >= 0.999F);
    QCOMPARE(decoded[2], 0.0F);
    QVERIFY(decoded[3] > 0.49F && decoded[3] < 0.51F);
}

void RecordedAudioSourceTest::decodesSupportedPcmWidths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    struct Case { std::uint16_t bits; std::vector<unsigned char> data; };
    const std::array<Case, 4> cases{{
        {8, {0x00, 0x80, 0xff}},
        {16, {0x00, 0x80, 0x00, 0x00, 0xff, 0x7f}},
        {24, {0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0x7f}},
        {32, {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
              0xff, 0xff, 0xff, 0x7f}},
    }};
    for (const auto& test : cases) {
        const auto path = writeWave(directory, QStringLiteral("pcm%1.wav").arg(test.bits),
            pcmFormat(1, 1, 8'000, test.bits), test.data);
        sdr::radio::RecordedAudioSource source(path);
        QVERIFY(source.start().succeeded);
        std::array<float, 3> decoded{};
        QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).frameCount, std::size_t{3});
        QVERIFY(decoded[0] <= -0.999F);
        QCOMPARE(decoded[1], 0.0F);
        QVERIFY(decoded[2] >= 0.98F);
    }
}

void RecordedAudioSourceTest::handlesFloatAndExtensibleFormat()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    std::vector<unsigned char> fmt = pcmFormat(0xfffe, 1, 8'000, 32);
    put16(fmt, 22); put16(fmt, 32); put32(fmt, 0);
    put16(fmt, 3); put16(fmt, 0); // IEEE float Data1 plus high word
    fmt.insert(fmt.end(), {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                           0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71});
    const std::array<unsigned char, 8> data{0x00, 0x00, 0x00, 0x3f,
                                              0x00, 0x00, 0xc0, 0x7f};
    const auto path = writeWave(directory, "float.capture", fmt, data);
    sdr::radio::RecordedAudioSource source(path);
    QCOMPARE(source.metadata().encoding, sdr::radio::RecordedAudioEncoding::Float32);
    QVERIFY(source.start().succeeded);
    std::array<float, 2> decoded{};
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).frameCount, std::size_t{2});
    QCOMPARE(decoded[0], 0.5F);
    QCOMPARE(decoded[1], 0.0F);
}

void RecordedAudioSourceTest::rejectsMalformedAndUnsupportedWaves()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto unsupported = writeWave(directory, "compressed.wav", pcmFormat(6, 1, 8'000, 16), std::array<unsigned char, 2>{0, 0});
    QVERIFY_EXCEPTION_THROWN(
        [&] { [[maybe_unused]] sdr::radio::RecordedAudioSource source{unsupported}; }(),
        std::invalid_argument);
    const auto truncated = writeWave(directory, "truncated.wav", pcmFormat(1, 2, 8'000, 16), std::array<unsigned char, 3>{0, 0, 0});
    QVERIFY_EXCEPTION_THROWN(
        [&] { [[maybe_unused]] sdr::radio::RecordedAudioSource source{truncated}; }(),
        std::invalid_argument);
    const auto invalidRate = writeWave(directory, "invalid-rate.wav", pcmFormat(1, 1, 1, 16), std::array<unsigned char, 2>{0, 0});
    QVERIFY_EXCEPTION_THROWN(
        [&] { [[maybe_unused]] sdr::radio::RecordedAudioSource source{invalidRate}; }(),
        std::invalid_argument);
}

void RecordedAudioSourceTest::pausesResumesAndReportsEndDeterministically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = writeWave(directory, "timing.wav", pcmFormat(1, 1, 8'000, 16), std::array<unsigned char, 4>{0, 0, 0, 0});
    sdr::radio::RecordedAudioSource source(path);
    QVERIFY(source.start().succeeded);
    source.setPaused(true);
    std::array<float, 2> decoded{};
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).status,
             sdr::radio::RecordedAudioReadStatus::Timeout);
    QCOMPARE(source.positionFrames(), 0U);
    source.setPaused(false);
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).frameCount, std::size_t{2});
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).status,
             sdr::radio::RecordedAudioReadStatus::EndOfFile);
    QVERIFY(source.ended());
    QCOMPARE(source.stop().succeeded, true);
    QCOMPARE(source.positionFrames(), 0U);
}

void RecordedAudioSourceTest::seeksByDecodedFrameAndClampsTargets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // Four 16-bit frames: -1, -0.5, 0, +0.5.  Seeking is validated against
    // decoded frame boundaries rather than a wall-clock approximation.
    const std::array<unsigned char, 8> data{
        0x00, 0x80, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x40};
    sdr::radio::RecordedAudioSource source(writeWave(
        directory, "seek.wav", pcmFormat(1, 1, 8'000, 16), data));
    QCOMPARE(source.metadata().frameCount, 4U);
    QVERIFY(source.start().succeeded);
    QVERIFY(source.seekFrames(2).succeeded);
    QCOMPARE(source.positionFrames(), 2U);
    std::array<float, 1> decoded{};
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).frameCount,
             std::size_t{1});
    QCOMPARE(decoded[0], 0.0F);

    QVERIFY(source.seekFrames(3).succeeded);
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).frameCount,
             std::size_t{1});
    QVERIFY(decoded[0] > 0.49F && decoded[0] < 0.51F);
    QVERIFY(source.seekFrames(99).succeeded);
    QCOMPARE(source.positionFrames(), 4U);
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).status,
             sdr::radio::RecordedAudioReadStatus::EndOfFile);
    QVERIFY(source.ended());

    // A stopped source retains an explicitly selected frame for its next
    // start; Stop itself remains the documented rewind operation.
    QVERIFY(source.stop().succeeded);
    QVERIFY(source.seekFrames(1).succeeded);
    QVERIFY(source.start().succeeded);
    QCOMPARE(source.read(decoded, std::chrono::milliseconds(1)).frameCount,
             std::size_t{1});
    QVERIFY(decoded[0] < -0.49F && decoded[0] > -0.51F);
}

}  // namespace

QTEST_GUILESS_MAIN(RecordedAudioSourceTest)
#include "RecordedAudioSourceTest.moc"
