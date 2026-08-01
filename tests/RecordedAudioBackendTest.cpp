// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordedAudioBackend.hpp"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <thread>
#include <vector>

namespace {

void put16(std::vector<unsigned char>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
}
void put32(std::vector<unsigned char>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
}
std::filesystem::path writeTone(const QTemporaryDir& directory)
{
    constexpr std::uint32_t rate = 8'000;
    constexpr std::size_t frames = 4'096;
    std::vector<unsigned char> data;
    data.reserve(frames * 2U);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int16_t value = frame % 8U < 4U ? 12'000 : -12'000;
        put16(data, static_cast<std::uint16_t>(value));
    }
    std::vector<unsigned char> file{'R', 'I', 'F', 'F'};
    put32(file, static_cast<std::uint32_t>(36U + data.size()));
    file.insert(file.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(file, 16); put16(file, 1); put16(file, 1); put32(file, rate);
    put32(file, rate * 2U); put16(file, 2); put16(file, 16);
    file.insert(file.end(), {'d', 'a', 't', 'a'}); put32(file, static_cast<std::uint32_t>(data.size()));
    file.insert(file.end(), data.begin(), data.end());
    const auto path = std::filesystem::path(directory.path().toStdString()) / "audio-without-wav-extension.capture";
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return path;
}

class RecordedAudioBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesStereoFramesAndPublishesAudioSpectrum();
};

void RecordedAudioBackendTest::preservesStereoFramesAndPublishesAudioSpectrum()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::dsp::RecordedAudioBackend backend(writeTone(directory));
    const auto capabilities = backend.sourceCapabilities();
    QCOMPARE(capabilities.kind, sdr::radio::ReceiverSourceKind::RecordedAudio);
    QVERIFY(!capabilities.rfControlsSupported);
    QVERIFY(!capabilities.scannerSupported);
    QVERIFY(!capabilities.iqRecordingSupported);
    QVERIFY(backend.startReception().succeeded());

    QElapsedTimer wait;
    wait.start();
    while (backend.audioBufferedSampleCount() == 0 && wait.elapsed() < 1'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto audio = backend.takeStereoAudioSamples(128);
    QVERIFY(!audio.empty());
    QCOMPARE(audio.size() % 2U, std::size_t{0});
    QCOMPARE(audio[0], audio[1]);

    wait.restart();
    std::optional<sdr::radio::SpectrumFrame> frame;
    while (!(frame = backend.takeLatestSpectrumFrame()) && wait.elapsed() < 1'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    QVERIFY(frame.has_value());
    QCOMPARE(frame->sampleRate, 4'000U);
    QCOMPARE(frame->centerFrequency, 2'000U);
    QCOMPARE(frame->captureSpan, 4'000U);

    wait.restart();
    while (!backend.takePlaybackEnd().has_value() && wait.elapsed() < 1'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    QCOMPARE(backend.recordingTransport().state, sdr::radio::RecordingPlaybackState::Ended);
    QVERIFY(backend.restartPlayback().succeeded());
    QVERIFY(backend.stopReception().succeeded());
    QCOMPARE(backend.recordingTransport().positionSamples, 0U);
}

}  // namespace

QTEST_GUILESS_MAIN(RecordedAudioBackendTest)
#include "RecordedAudioBackendTest.moc"
