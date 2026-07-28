// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioDspPlan.hpp"
#include "AudioOutputService.hpp"
#include "AudioSampleBuffer.hpp"

#include <QtTest>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t audioFrameBytes = 2 * sizeof(std::int16_t);

struct FakeAudioTrace {
    std::vector<sdr::platform::AudioOutputDevice> devices{
        {"audio:default", "Test speakers", true},
        {"audio:second", "Second output", false},
    };
    sdr::platform::AudioSinkOpenResult openResult;
    std::vector<std::string> openedIdentifiers;
    std::vector<std::byte> writtenBytes;
    std::optional<std::string> runtimeError;
    std::size_t writableFrames = 0;
    std::size_t partialWriteLimitBytes = std::numeric_limits<std::size_t>::max();
    std::size_t sinkCapacity = 0;
    std::size_t queuedBytes = 0;
    std::uint64_t platformUnderrunEvents = 0;
    std::uint64_t consumedFrames = 0;
    unsigned playbackRemainder = 0;
    int closes = 0;
    int playbackStarts = 0;
    bool playbackStarted = false;
};

class FakeAudioSink final : public sdr::platform::AudioSinkBackend
{
public:
    explicit FakeAudioSink(std::shared_ptr<FakeAudioTrace> trace)
        : m_trace(std::move(trace))
    {
    }

    [[nodiscard]] std::vector<sdr::platform::AudioOutputDevice> devices()
        override
    {
        return m_trace->devices;
    }

    [[nodiscard]] sdr::platform::AudioSinkOpenResult open(
        const std::string& identifier, std::uint32_t sampleRate) override
    {
        m_trace->openedIdentifiers.push_back(identifier);
        if (sampleRate != sdr::radio::receiverAudioSampleRate) {
            return {
                sdr::platform::AudioSinkOpenError::UnsupportedFormat,
                "Unexpected sample rate",
            };
        }
        return m_trace->openResult;
    }

    [[nodiscard]] sdr::platform::AudioSinkOpenResult startPlayback() override
    {
        ++m_trace->playbackStarts;
        m_trace->playbackStarted = m_trace->openResult.succeeded();
        return m_trace->openResult;
    }

    void close() noexcept override
    {
        ++m_trace->closes;
        m_trace->playbackStarted = false;
        m_trace->queuedBytes = 0;
    }

    [[nodiscard]] std::size_t writableFrames() const noexcept override
    {
        if (m_trace->sinkCapacity > 0) {
            return (m_trace->sinkCapacity * audioFrameBytes -
                    m_trace->queuedBytes) /
                   audioFrameBytes;
        }
        return m_trace->writableFrames;
    }

    [[nodiscard]] std::size_t bufferedFrames() const noexcept override
    {
        return m_trace->sinkCapacity > 0
                   ? m_trace->queuedBytes / audioFrameBytes
                   : 0;
    }

    [[nodiscard]] std::size_t write(
        std::span<const std::byte> bytes) override
    {
        const std::size_t writtenBytes = std::min(
            bytes.size(), m_trace->partialWriteLimitBytes);
        m_trace->writtenBytes.insert(
            m_trace->writtenBytes.end(), bytes.begin(),
            bytes.begin() + static_cast<std::ptrdiff_t>(writtenBytes));
        if (m_trace->sinkCapacity > 0) {
            m_trace->queuedBytes += writtenBytes;
        } else {
            m_trace->writableFrames = 0;
        }
        return writtenBytes;
    }

    [[nodiscard]] std::uint64_t takePlatformUnderrunEvents() override
    {
        return std::exchange(m_trace->platformUnderrunEvents, 0);
    }

    [[nodiscard]] std::optional<std::string> takeRuntimeError() override
    {
        return std::exchange(m_trace->runtimeError, std::nullopt);
    }

private:
    std::shared_ptr<FakeAudioTrace> m_trace;
};

std::pair<std::unique_ptr<sdr::platform::AudioOutputService>,
          std::shared_ptr<FakeAudioTrace>>
makeService(std::size_t capacity = sdr::radio::defaultAudioBufferCapacity)
{
    auto trace = std::make_shared<FakeAudioTrace>();
    auto service = std::make_unique<sdr::platform::AudioOutputService>(
        std::make_unique<FakeAudioSink>(trace), capacity);
    return {std::move(service), std::move(trace)};
}

void advancePlayback(FakeAudioTrace& trace, unsigned milliseconds)
{
    const unsigned numerator =
        trace.playbackRemainder + milliseconds * sdr::radio::receiverAudioSampleRate;
    const std::size_t requested = numerator / 1'000U;
    trace.playbackRemainder = numerator % 1'000U;
    trace.consumedFrames += requested;
    const std::size_t requestedBytes = requested * audioFrameBytes;
    if (trace.queuedBytes < requestedBytes) {
        ++trace.platformUnderrunEvents;
        trace.queuedBytes = 0;
        return;
    }
    trace.queuedBytes -= requestedBytes;
}

}  // namespace

class AudioOutputTest final : public QObject
{
    Q_OBJECT

private slots:
    void convertsReceiverRatesExactly();
    void derivesSafeModeSpecificChannelRates();
    void keepsBufferBoundedAndDropsOldestSamples();
    void reportsServiceOverflowAndFlushesOnModeBoundary();
    void insertsSilenceForContiguousUnderruns();
    void appliesVolumeAndMuteToPcm();
    void duplicatesAnalogMonoAndPreservesDecodedStereo();
    void sustainsRateAcrossJitterWithPartialWrites();
    void prefillDelaysPlaybackAndFlushDiscardsOldModeAudio();
    void managesLifecycleFailuresAndDisappearingDevices();
};

void AudioOutputTest::convertsReceiverRatesExactly()
{
    const auto twoMegahertz = sdr::dsp::makeAudioRateConversion(2'000'000);
    QCOMPARE(twoMegahertz.outputSampleRate, std::uint32_t{48'000});
    QCOMPARE(twoMegahertz.interpolation, 3U);
    QCOMPARE(twoMegahertz.decimation, 125U);

    const auto oneMegahertz = sdr::dsp::makeAudioRateConversion(1'000'000);
    QCOMPARE(oneMegahertz.interpolation, 6U);
    QCOMPARE(oneMegahertz.decimation, 125U);

    const auto twoPointFourMegahertz =
        sdr::dsp::makeAudioRateConversion(2'400'000);
    QCOMPARE(twoPointFourMegahertz.interpolation, 1U);
    QCOMPARE(twoPointFourMegahertz.decimation, 50U);
}

void AudioOutputTest::derivesSafeModeSpecificChannelRates()
{
    struct Case {
        std::uint64_t sampleRate;
        sdr::radio::DemodulationMode mode;
        std::uint64_t filterWidth;
        unsigned expectedDecimation;
        std::uint32_t expectedOutputRate;
    };
    constexpr Case cases[]{
        {2'000'000, sdr::radio::DemodulationMode::Am, 12'500, 10, 200'000},
        {1'000'000, sdr::radio::DemodulationMode::Nfm, 25'000, 5, 200'000},
        {2'400'000, sdr::radio::DemodulationMode::Wfm, 180'000, 10, 240'000},
        {2'000'000, sdr::radio::DemodulationMode::Wfm, 250'000, 5, 400'000},
        {2'400'000, sdr::radio::DemodulationMode::Usb, 4'000, 12, 200'000},
        {1'000'000, sdr::radio::DemodulationMode::Lsb, 1'800, 5, 200'000},
        {2'400'000,
         sdr::radio::DemodulationMode::DigitalDecoderOutput,
         12'500,
         12,
         200'000},
    };

    for (const auto& testCase : cases) {
        const auto plan = sdr::dsp::makeChannelRatePlan(
            testCase.sampleRate, testCase.mode, testCase.filterWidth);
        QCOMPARE(plan.inputSampleRate, testCase.sampleRate);
        QCOMPARE(plan.decimation, testCase.expectedDecimation);
        QCOMPARE(plan.outputSampleRate, testCase.expectedOutputRate);
        QCOMPARE(
            static_cast<std::uint64_t>(plan.outputSampleRate) * plan.decimation,
            testCase.sampleRate);
        QVERIFY(static_cast<double>(plan.outputSampleRate) >=
                plan.minimumOutputSampleRate);
    }
}

void AudioOutputTest::keepsBufferBoundedAndDropsOldestSamples()
{
    sdr::radio::AudioSampleBuffer buffer(4);
    const std::vector<float> first{1.0F, 2.0F, 3.0F};
    QCOMPARE(buffer.push(first).droppedSamples, std::size_t{0});
    const std::vector<float> second{4.0F, 5.0F};
    QCOMPARE(buffer.push(second).droppedSamples, std::size_t{1});
    QCOMPARE(buffer.size(), std::size_t{4});
    QCOMPARE(buffer.take(10), std::vector<float>({2.0F, 3.0F, 4.0F, 5.0F}));

    const std::vector<float> oversized{6.0F, 7.0F, 8.0F, 9.0F, 10.0F};
    QCOMPARE(buffer.push(oversized).droppedSamples, std::size_t{1});
    QCOMPARE(buffer.take(4), std::vector<float>({7.0F, 8.0F, 9.0F, 10.0F}));
    QCOMPARE(buffer.totalProducedSamples(), std::uint64_t{10});
}

void AudioOutputTest::reportsServiceOverflowAndFlushesOnModeBoundary()
{
    auto [service, trace] = makeService(4);
    service->refreshDevices();
    QVERIFY(service->start());
    const std::vector<float> samples{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    service->enqueue(samples);
    QCOMPARE(service->bufferedSampleCount(), std::size_t{4});
    QCOMPARE(service->availableBufferCapacity(), std::size_t{0});
    QCOMPARE(service->state().overflowEvents, std::uint64_t{1});
    QCOMPARE(service->state().droppedSamples, std::uint64_t{2});
    const std::string firstDiagnostic = service->state().statusText;
    service->reportUpstreamOverflow(3);
    QCOMPARE(service->state().overflowEvents, std::uint64_t{2});
    QCOMPARE(service->state().droppedSamples, std::uint64_t{5});
    QCOMPARE(service->state().statusText, firstDiagnostic);

    service->flush();
    QCOMPARE(service->bufferedSampleCount(), std::size_t{0});
    QCOMPARE(service->availableBufferCapacity(), std::size_t{4});
    QVERIFY(trace->writtenBytes.empty());
}

void AudioOutputTest::insertsSilenceForContiguousUnderruns()
{
    auto [service, trace] = makeService();
    service->refreshDevices();
    QVERIFY(service->start());
    service->enqueue(std::vector<float>(1'440, 0.0F));
    trace->writableFrames = 1'440;
    service->process();
    trace->writableFrames = 4;
    service->process();
    QCOMPARE(service->state().underrunEvents, std::uint64_t{1});
    QVERIFY(!trace->writtenBytes.empty());

    trace->writableFrames = 4;
    service->process();
    QCOMPARE(service->state().underrunEvents, std::uint64_t{1});

    const std::vector<float> full{0.1F, 0.2F, 0.3F, 0.4F};
    service->enqueue(full);
    trace->writableFrames = 4;
    service->process();
    trace->writableFrames = 4;
    service->process();
    QCOMPARE(service->state().underrunEvents, std::uint64_t{2});

    ++trace->platformUnderrunEvents;
    service->process();
    QCOMPARE(service->state().platformUnderrunEvents, std::uint64_t{1});
}

void AudioOutputTest::appliesVolumeAndMuteToPcm()
{
    auto [service, trace] = makeService();
    service->refreshDevices();
    QVERIFY(service->start());
    QVERIFY(service->setVolumePercent(50));
    std::vector<float> samples(1'440, 1.0F);
    samples[1] = -1.0F;
    service->enqueue(samples);
    trace->writableFrames = 2;
    service->process();
    std::int16_t first = 0;
    std::int16_t second = 0;
    std::int16_t third = 0;
    std::int16_t fourth = 0;
    std::memcpy(&first, trace->writtenBytes.data(), sizeof(first));
    std::memcpy(&second, trace->writtenBytes.data() + sizeof(first), sizeof(second));
    QCOMPARE(first, std::int16_t{16'384});
    QCOMPARE(second, std::int16_t{16'384});
    std::memcpy(
        &third,
        trace->writtenBytes.data() + 2 * sizeof(std::int16_t),
        sizeof(third));
    std::memcpy(
        &fourth,
        trace->writtenBytes.data() + 3 * sizeof(std::int16_t),
        sizeof(fourth));
    QCOMPARE(third, std::int16_t{-16'384});
    QCOMPARE(fourth, std::int16_t{-16'384});

    service->setMuted(true);
    const std::size_t muteStart = trace->writtenBytes.size();
    service->enqueue(std::vector<float>(1'440, 1.0F));
    trace->writableFrames = 2;
    service->process();
    std::int16_t mutedFirst = 1;
    std::int16_t mutedSecond = 1;
    std::memcpy(
        &mutedFirst, trace->writtenBytes.data() + muteStart, sizeof(mutedFirst));
    std::memcpy(
        &mutedSecond,
        trace->writtenBytes.data() + muteStart + sizeof(mutedFirst),
        sizeof(mutedSecond));
    QCOMPARE(mutedFirst, std::int16_t{0});
    QCOMPARE(mutedSecond, std::int16_t{0});
    QVERIFY(service->state().muted);
    QCOMPARE(service->state().volumePercent, 50);
}

void AudioOutputTest::duplicatesAnalogMonoAndPreservesDecodedStereo()
{
    auto [service, trace] = makeService();
    service->refreshDevices();
    QVERIFY(service->start());

    std::vector<float> mono(1'440, 0.0F);
    mono[0] = 0.25F;
    service->enqueueMono(mono);
    trace->writableFrames = 1;
    service->process();

    std::int16_t left = 0;
    std::int16_t right = 0;
    std::memcpy(&left, trace->writtenBytes.data(), sizeof(left));
    std::memcpy(
        &right,
        trace->writtenBytes.data() + sizeof(left),
        sizeof(right));
    QCOMPARE(left, right);

    service->flush();
    trace->writtenBytes.clear();
    std::vector<float> stereo(1'440 * 2, 0.0F);
    stereo[0] = 0.25F;
    stereo[1] = -0.25F;
    service->enqueueStereo(stereo);
    trace->writableFrames = 1;
    service->process();
    std::memcpy(&left, trace->writtenBytes.data(), sizeof(left));
    std::memcpy(
        &right,
        trace->writtenBytes.data() + sizeof(left),
        sizeof(right));
    QVERIFY(left > 0);
    QVERIFY(right < 0);
}

void AudioOutputTest::sustainsRateAcrossJitterWithPartialWrites()
{
    auto [service, trace] = makeService();
    trace->sinkCapacity = 1'920;
    trace->partialWriteLimitBytes = 275;
    service->refreshDevices();
    QVERIFY(service->start());
    service->enqueue(std::vector<float>(1'920, 0.25F));
    service->process();

    constexpr unsigned intervals[]{5, 7, 4, 12, 5, 6, 3, 9, 5, 4};
    unsigned producerRemainder = 0;
    unsigned elapsedMilliseconds = 0;
    for (unsigned cycle = 0; cycle < 600; ++cycle) {
        const unsigned interval = intervals[cycle % std::size(intervals)];
        const unsigned numerator =
            producerRemainder + interval * sdr::radio::receiverAudioSampleRate;
        const std::size_t produced = numerator / 1'000U;
        producerRemainder = numerator % 1'000U;
        service->enqueue(std::vector<float>(produced, 0.25F));
        advancePlayback(*trace, interval);
        service->process();
        elapsedMilliseconds += interval;
    }

    const std::uint64_t requiredFrames =
        static_cast<std::uint64_t>(elapsedMilliseconds) *
        sdr::radio::receiverAudioSampleRate / 1'000U;
    QVERIFY(service->state().writtenSamples >= requiredFrames);
    QCOMPARE(service->state().underrunEvents, std::uint64_t{0});
    QCOMPARE(service->state().platformUnderrunEvents, std::uint64_t{0});
    QVERIFY(service->bufferedSampleCount() <=
            sdr::radio::defaultAudioBufferCapacity);
}

void AudioOutputTest::prefillDelaysPlaybackAndFlushDiscardsOldModeAudio()
{
    auto [service, trace] = makeService();
    trace->sinkCapacity = 1'920;
    service->refreshDevices();
    QVERIFY(service->start());

    service->enqueue(std::vector<float>(1'439, 0.25F));
    service->process();
    QCOMPARE(trace->playbackStarts, 0);
    QVERIFY(trace->writtenBytes.empty());

    service->enqueue(std::vector<float>(1, 0.25F));
    service->process();
    QCOMPARE(trace->playbackStarts, 1);
    QVERIFY(!trace->writtenBytes.empty());
    QCOMPARE(service->sinkBufferedSampleCount(), std::size_t{1'440});

    service->flush();
    QCOMPARE(service->bufferedSampleCount(), std::size_t{0});
    QCOMPARE(trace->queuedBytes, std::size_t{0});
    QCOMPARE(trace->playbackStarts, 1);

    trace->queuedBytes = 0;
    trace->writtenBytes.clear();
    service->enqueue(std::vector<float>(1'440, -0.25F));
    service->process();
    QCOMPARE(trace->playbackStarts, 2);
    QVERIFY(!trace->writtenBytes.empty());
}

void AudioOutputTest::managesLifecycleFailuresAndDisappearingDevices()
{
    auto [service, trace] = makeService();
    service->refreshDevices();
    QVERIFY(service->state().ready);
    QCOMPARE(
        service->state().selectedDeviceIdentifier,
        std::string("audio:default"));
    QVERIFY(service->start());
    QVERIFY(service->state().running);
    QCOMPARE(trace->openedIdentifiers.back(), std::string("audio:default"));
    service->stop();
    QVERIFY(!service->state().running);

    trace->openResult = {
        sdr::platform::AudioSinkOpenError::UnsupportedFormat,
        "Unsupported test format",
    };
    QVERIFY(!service->start());
    QVERIFY(service->state().statusText.find("Unsupported") != std::string::npos);

    trace->openResult = {};
    QVERIFY(service->start());
    trace->devices.clear();
    service->refreshDevices();
    QVERIFY(!service->state().running);
    QVERIFY(!service->state().ready);
    QVERIFY(service->state().statusText.find("disappeared") != std::string::npos);
}

QTEST_GUILESS_MAIN(AudioOutputTest)

#include "AudioOutputTest.moc"
