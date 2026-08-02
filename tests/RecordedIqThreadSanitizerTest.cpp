// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "WidebandIqSources.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

class TemporaryCapture final
{
public:
    TemporaryCapture()
    {
        static std::atomic_uint64_t nextIdentifier = 0;
        const auto identifier = nextIdentifier.fetch_add(1);
        m_path = std::filesystem::temp_directory_path() /
            ("slopsdr-recorded-iq-tsan-" + std::to_string(identifier) + ".raw");

        std::ofstream output(m_path, std::ios::binary);
        for (std::size_t index = 0; index < 8'192; ++index) {
            const std::array<float, 2> sample = {0.0F, 0.0F};
            output.write(reinterpret_cast<const char*>(sample.data()),
                         static_cast<std::streamsize>(sizeof(sample)));
        }
    }

    ~TemporaryCapture()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] bool waitFor(const std::atomic_bool& value, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!value.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

[[nodiscard]] sdr::radio::RecordedIqSourceConfiguration configurationFor(
    const std::filesystem::path& path)
{
    return {
        .path = path.string(),
        .centerFrequency = 100'000'000,
        .sampleRate = 1,
    };
}

}  // namespace

int main()
{
    TemporaryCapture capture;
    sdr::dsp::RecordedIqSource source(configurationFor(capture.path()));
    std::atomic_bool pollPosition = true;
    std::atomic_bool invalidPosition = false;
    std::thread transportPoller([&] {
        while (pollPosition.load()) {
            if (source.positionSamples() > source.sampleCount()) invalidPosition = true;
            static_cast<void>(source.paused());
            static_cast<void>(source.ended());
            std::this_thread::yield();
        }
    });

    const auto started = source.start();
    if (!started.succeeded) {
        std::cerr << started.message << '\n';
        pollPosition = false;
        transportPoller.join();
        return 1;
    }

    std::atomic_bool secondReadStarted = false;
    std::atomic_bool readerFinished = false;
    std::atomic<int> secondReadStatus =
        static_cast<int>(sdr::radio::WidebandIqReadStatus::Failed);
    std::thread reader([&] {
        std::array<std::complex<float>, 4'096> samples;
        const auto firstRead = source.read(samples, 0ms);
        if (firstRead.status != sdr::radio::WidebandIqReadStatus::Samples ||
            firstRead.sampleCount != samples.size()) {
            secondReadStatus = static_cast<int>(sdr::radio::WidebandIqReadStatus::Failed);
            readerFinished = true;
            return;
        }
        secondReadStarted = true;
        const auto secondRead = source.read(samples, 0ms);
        secondReadStatus = static_cast<int>(secondRead.status);
        readerFinished = true;
    });

    if (!waitFor(secondReadStarted, 1s)) {
        std::cerr << "The paced recorded-IQ read did not start\n";
        static_cast<void>(source.stop());
        reader.join();
        pollPosition = false;
        transportPoller.join();
        return 1;
    }
    if (!source.stop().succeeded || !waitFor(readerFinished, 1s)) {
        std::cerr << "Stopping a paced recorded-IQ read was not prompt\n";
        reader.join();
        pollPosition = false;
        transportPoller.join();
        return 1;
    }
    reader.join();
    if (secondReadStatus.load() != static_cast<int>(sdr::radio::WidebandIqReadStatus::Stopped)) {
        std::cerr << "The interrupted recorded-IQ read did not report stopped\n";
        pollPosition = false;
        transportPoller.join();
        return 1;
    }

    const auto restarted = source.start();
    if (!restarted.succeeded) {
        std::cerr << restarted.message << '\n';
        pollPosition = false;
        transportPoller.join();
        return 1;
    }
    std::array<std::complex<float>, 32> restartSamples;
    const auto restartedRead = source.read(restartSamples, 0ms);
    if (restartedRead.status != sdr::radio::WidebandIqReadStatus::Samples ||
        restartedRead.sampleCount != restartSamples.size() || source.positionSamples() != restartSamples.size()) {
        std::cerr << "Restarted recorded-IQ playback did not reset and read samples\n";
        static_cast<void>(source.stop());
        pollPosition = false;
        transportPoller.join();
        return 1;
    }
    static_cast<void>(source.stop());
    pollPosition = false;
    transportPoller.join();

    if (invalidPosition.load()) {
        std::cerr << "Transport observed an invalid recorded-IQ position\n";
        return 1;
    }
    return 0;
}
