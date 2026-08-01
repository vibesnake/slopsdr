// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"
#include "RecordedAudioSource.hpp"
#include "SpectrumFramePacing.hpp"

#include <memory>
#include <filesystem>

namespace sdr::dsp {

// A separate, non-RF backend for ordinary WAVE audio.  It deliberately does
// not derive from WidebandIqSource and never constructs an RF demodulation
// path.
class RecordedAudioBackend final : public radio::ReceiverBackend
{
public:
    explicit RecordedAudioBackend(
        std::filesystem::path path,
        SpectrumDisplayConfiguration spectrumConfiguration = {});
    ~RecordedAudioBackend() override;

    RecordedAudioBackend(const RecordedAudioBackend&) = delete;
    RecordedAudioBackend& operator=(const RecordedAudioBackend&) = delete;

    [[nodiscard]] const radio::ReceiverLimits& limits() const noexcept override;
    [[nodiscard]] const radio::ReceiverCapabilities& capabilities() const noexcept override;
    [[nodiscard]] radio::ReceiverSourceCapabilities sourceCapabilities() const noexcept override;
    [[nodiscard]] radio::RecordingTransportState recordingTransport() const noexcept override;
    [[nodiscard]] std::optional<std::string> takePlaybackEnd() override;
    [[nodiscard]] radio::OperationResult setPlaybackPaused(bool paused) override;
    [[nodiscard]] radio::OperationResult restartPlayback() override;
    [[nodiscard]] radio::OperationResult seekPlayback(std::uint64_t frame) override;
    [[nodiscard]] const radio::ReceiverState& state() const noexcept override;
    [[nodiscard]] std::uint64_t effectiveSampleRate() const noexcept override;
    [[nodiscard]] std::optional<radio::SpectrumFrame> takeLatestSpectrumFrame() override;
    [[nodiscard]] std::vector<radio::SpectrumFrame> takePendingSpectrumFrames(
        std::size_t maximumFrames) override;
    [[nodiscard]] radio::SpectrumProcessingMetrics spectrumProcessingMetrics() const override;
    [[nodiscard]] std::size_t spectrumFftSize() const noexcept override;
    [[nodiscard]] std::size_t requestedSpectrumFftSize() const noexcept override;
    [[nodiscard]] radio::OperationResult setSpectrumFftSize(std::size_t fftSize) override;
    [[nodiscard]] std::uint32_t spectrumFramesPerSecond() const noexcept override;
    [[nodiscard]] radio::OperationResult setSpectrumFramesPerSecond(
        std::uint32_t framesPerSecond) override;
    [[nodiscard]] std::vector<float> takeStereoAudioSamples(
        std::size_t maximumFrames) override;
    void clearAudioSamples() override;
    [[nodiscard]] std::uint64_t audioProducedSamples() const override;
    [[nodiscard]] std::uint64_t audioDroppedSamples() const override;
    [[nodiscard]] std::size_t audioBufferedSampleCount() const override;
    [[nodiscard]] std::optional<radio::OperationResult> takeRuntimeError() override;

    [[nodiscard]] radio::OperationResult startReception() override;
    [[nodiscard]] radio::OperationResult stopReception() override;
    [[nodiscard]] radio::OperationResult setCenterFrequency(std::uint64_t) override;
    [[nodiscard]] radio::OperationResult setListeningFrequency(std::uint64_t) override;
    [[nodiscard]] radio::OperationResult tuneListeningFrequency(double) override;
    [[nodiscard]] radio::OperationResult shiftCenterFrequency(std::int64_t) override;
    [[nodiscard]] radio::OperationResult setSampleRate(std::uint64_t) override;
    [[nodiscard]] radio::OperationResult setFilterWidth(std::uint64_t) override;
    [[nodiscard]] radio::OperationResult setGain(double) override;
    [[nodiscard]] radio::OperationResult setPpmCorrection(double) override;
    [[nodiscard]] radio::OperationResult setDemodulationMode(radio::DemodulationMode) override;
    [[nodiscard]] radio::OperationResult setSquelchLevel(double) override;
    [[nodiscard]] radio::OperationResult enableManualSquelch() override;
    [[nodiscard]] radio::OperationResult disableSquelch() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sdr::dsp
