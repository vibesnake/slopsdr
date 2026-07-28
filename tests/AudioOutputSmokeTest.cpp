// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioOutputService.hpp"

#include <QtTest>

#include <cmath>
#include <numbers>
#include <vector>

class AudioOutputSmokeTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesAQuietToneToTheDefaultOutput();
};

void AudioOutputSmokeTest::writesAQuietToneToTheDefaultOutput()
{
    auto service = sdr::platform::makeQtAudioOutputService();
    service->refreshDevices();
    if (service->state().devices.empty()) {
        QSKIP("No system audio output is available");
    }
    QVERIFY2(service->start(), service->state().statusText.c_str());

    constexpr std::size_t chunkSize = 480;
    constexpr double frequencyHz = 440.0;
    constexpr float amplitude = 0.02F;
    std::vector<float> samples(chunkSize);
    std::size_t sampleIndex = 0;
    for (int chunk = 0; chunk < 50; ++chunk) {
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const double phase =
                2.0 * std::numbers::pi * frequencyHz *
                static_cast<double>(sampleIndex++) /
                static_cast<double>(sdr::radio::receiverAudioSampleRate);
            samples[index] = amplitude * static_cast<float>(std::sin(phase));
        }
        service->enqueue(samples);
        service->process();
        QTest::qWait(10);
        QVERIFY2(service->state().running, service->state().statusText.c_str());
    }

    QVERIFY(service->state().writtenSamples >= 1'440U);
    service->stop();
    QVERIFY(!service->state().running);
}

QTEST_GUILESS_MAIN(AudioOutputSmokeTest)

#include "AudioOutputSmokeTest.moc"
