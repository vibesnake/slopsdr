// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "RecordedAudioBackend.hpp"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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
std::filesystem::path writeTone(
    const QTemporaryDir& directory, std::uint32_t rate = 8'000,
    std::size_t frames = 4'096, const char* name = "audio-without-wav-extension.capture")
{
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
    const auto path = std::filesystem::path(directory.path().toStdString()) / name;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return path;
}

std::vector<sdr::radio::SpectrumFrame> waitForFrames(
    sdr::dsp::RecordedAudioBackend& backend, std::size_t count,
    int timeoutMilliseconds = 1'500)
{
    QElapsedTimer wait;
    wait.start();
    std::vector<sdr::radio::SpectrumFrame> frames;
    while (frames.size() < count && wait.elapsed() < timeoutMilliseconds) {
        auto pending = backend.takePendingSpectrumFrames(count - frames.size());
        frames.insert(
            frames.end(), std::make_move_iterator(pending.begin()),
            std::make_move_iterator(pending.end()));
        if (frames.size() < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return frames;
}

class RecordedAudioBackendTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesStereoFramesAndPublishesAudioSpectrum();
    void capsRequestedFftAndPublishesOverlappingFrames();
    void selectsEffectiveAudioFftSizes();
    void preservesFractionalAudioHopCadence();
    void pausesWithoutFramesAndRestartsWithFreshFrames();
    void seeksWithoutChangingPlaybackStateAndResetsDelivery();
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
    QCOMPARE(backend.requestedSpectrumFftSize(), std::size_t{4'096});
    QCOMPARE(backend.spectrumFftSize(), std::size_t{1'024});
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

void RecordedAudioBackendTest::capsRequestedFftAndPublishesOverlappingFrames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = writeTone(directory, 48'000, 96'000, "48k.capture");
    sdr::dsp::RecordedAudioBackend backend(path);
    QVERIFY(backend.setSpectrumFftSize(65'536).succeeded());
    QCOMPARE(backend.requestedSpectrumFftSize(), std::size_t{65'536});
    QCOMPARE(backend.spectrumFftSize(), std::size_t{4'096});
    const auto metrics = backend.spectrumProcessingMetrics();
    QCOMPARE(metrics.fftSize, std::size_t{4'096});
    QCOMPARE(metrics.targetFramesPerSecond, 60.0);
    QCOMPARE(metrics.achievableFramesPerSecond, 60.0);
    QCOMPARE(metrics.hopSize, 800.0);
    QCOMPARE(metrics.overlapPercentage, 80.46875);
    QCOMPARE(metrics.hertzPerBin, 24'000.0 / 4'096.0);

    QVERIFY(backend.startReception().succeeded());
    const auto frames = waitForFrames(backend, 8);
    QCOMPARE(frames.size(), std::size_t{8});
    for (std::size_t index = 1; index < frames.size(); ++index) {
        QVERIFY(frames[index].sequence > frames[index - 1U].sequence);
        QVERIFY(frames[index].timestampNanoseconds >
                frames[index - 1U].timestampNanoseconds);
        QCOMPARE(frames[index].fftSize, std::size_t{4'096});
    }
    QVERIFY(backend.stopReception().succeeded());

    sdr::dsp::RecordedAudioBackend largerRequest(path);
    QVERIFY(largerRequest.setSpectrumFftSize(262'144).succeeded());
    QCOMPARE(largerRequest.requestedSpectrumFftSize(), std::size_t{262'144});
    QCOMPARE(largerRequest.spectrumFftSize(), std::size_t{4'096});
}

void RecordedAudioBackendTest::selectsEffectiveAudioFftSizes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const std::array cases{
        std::pair{8'000U, std::size_t{1'024}},
        std::pair{44'100U, std::size_t{4'096}},
        std::pair{48'000U, std::size_t{4'096}},
        std::pair{96'000U, std::size_t{4'096}},
    };
    for (const auto& [rate, expectedFftSize] : cases) {
        const auto path = writeTone(directory, rate, rate, (std::to_string(rate) + ".capture").c_str());
        sdr::dsp::RecordedAudioBackend backend(path);
        QVERIFY(backend.setSpectrumFftSize(262'144).succeeded());
        QCOMPARE(backend.requestedSpectrumFftSize(), std::size_t{262'144});
        QCOMPARE(backend.spectrumFftSize(), expectedFftSize);
    }
}

void RecordedAudioBackendTest::preservesFractionalAudioHopCadence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::dsp::RecordedAudioBackend backend(
        writeTone(directory, 44'100, 88'200, "fractional.capture"));
    QVERIFY(backend.setSpectrumFramesPerSecond(59).succeeded());
    QVERIFY(backend.startReception().succeeded());
    const auto frames = waitForFrames(backend, 8);
    QCOMPARE(frames.size(), std::size_t{8});
    std::vector<std::uint64_t> intervals;
    for (std::size_t index = 1; index < frames.size(); ++index) {
        intervals.push_back(
            frames[index].timestampNanoseconds - frames[index - 1U].timestampNanoseconds);
    }
    QVERIFY(std::adjacent_find(intervals.begin(), intervals.end(),
        [] (std::uint64_t first, std::uint64_t second) { return first != second; }) !=
        intervals.end());
    QVERIFY(backend.stopReception().succeeded());
}

void RecordedAudioBackendTest::pausesWithoutFramesAndRestartsWithFreshFrames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::dsp::RecordedAudioBackend backend(
        writeTone(directory, 48'000, 144'000, "pause.capture"));
    QVERIFY(backend.startReception().succeeded());
    const auto beforePause = waitForFrames(backend, 4);
    QCOMPARE(beforePause.size(), std::size_t{4});
    QVERIFY(backend.setPlaybackPaused(true).succeeded());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    static_cast<void>(backend.takePendingSpectrumFrames(64));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    QVERIFY(backend.takePendingSpectrumFrames(64).empty());

    QVERIFY(backend.setPlaybackPaused(false).succeeded());
    const auto afterResume = waitForFrames(backend, 3);
    QCOMPARE(afterResume.size(), std::size_t{3});
    QVERIFY(afterResume.front().sequence > beforePause.back().sequence);
    QVERIFY(afterResume.front().timestampNanoseconds >
            beforePause.back().timestampNanoseconds);

    QVERIFY(backend.restartPlayback().succeeded());
    QVERIFY(backend.takePendingSpectrumFrames(64).empty());
    const auto afterRestart = waitForFrames(backend, 2);
    QCOMPARE(afterRestart.size(), std::size_t{2});
    QVERIFY(afterRestart.front().sequence > afterResume.back().sequence);
    QVERIFY(afterRestart.front().timestampNanoseconds >
            afterResume.back().timestampNanoseconds);
    QVERIFY(backend.stopReception().succeeded());
}

void RecordedAudioBackendTest::seeksWithoutChangingPlaybackStateAndResetsDelivery()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    sdr::dsp::RecordedAudioBackend backend(
        writeTone(directory, 48'000, 240'000, "seek.capture"));
    const auto initial = backend.recordingTransport();
    QVERIFY(initial.canSeek);
    QCOMPARE(initial.totalSamples, std::uint64_t{240'000});

    // Seeking while stopped selects the next start point without starting the
    // reader.  Out-of-range targets clamp to the exact EOF frame.
    QVERIFY(backend.seekPlayback(120'000).succeeded());
    QCOMPARE(backend.recordingTransport().state, sdr::radio::RecordingPlaybackState::Stopped);
    QCOMPARE(backend.recordingTransport().positionSamples, std::uint64_t{120'000});
    QVERIFY(backend.seekPlayback(999'999).succeeded());
    QCOMPARE(backend.recordingTransport().state, sdr::radio::RecordingPlaybackState::Ended);
    QCOMPARE(backend.recordingTransport().positionSamples, std::uint64_t{240'000});
    QVERIFY(backend.seekPlayback(0).succeeded());

    QVERIFY(backend.startReception().succeeded());
    QVERIFY(!waitForFrames(backend, 2).empty());
    QVERIFY(backend.seekPlayback(96'000).succeeded());
    QCOMPARE(backend.recordingTransport().state, sdr::radio::RecordingPlaybackState::Playing);
    const auto afterPlayingSeek = waitForFrames(backend, 2);
    QCOMPARE(afterPlayingSeek.size(), std::size_t{2});

    QVERIFY(backend.setPlaybackPaused(true).succeeded());
    QVERIFY(backend.seekPlayback(144'000).succeeded());
    QCOMPARE(backend.recordingTransport().state, sdr::radio::RecordingPlaybackState::Paused);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    QVERIFY(backend.takePendingSpectrumFrames(64).empty());
    QVERIFY(backend.setPlaybackPaused(false).succeeded());
    QVERIFY(!waitForFrames(backend, 1).empty());
    QVERIFY(backend.stopReception().succeeded());
}

}  // namespace

QTEST_GUILESS_MAIN(RecordedAudioBackendTest)
#include "RecordedAudioBackendTest.moc"
