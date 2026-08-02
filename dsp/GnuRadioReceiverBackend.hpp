// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverBackend.hpp"
#include "SpectrumFramePacing.hpp"

#include <chrono>
#include <memory>
#include <span>
#include <vector>

namespace sdr::devices {
class DeviceController;
}

namespace sdr::dsp {

class GnuRadioReceiverBackend final : public radio::ReceiverBackend
{
public:
    GnuRadioReceiverBackend();
    explicit GnuRadioReceiverBackend(
        SpectrumDisplayConfiguration spectrumConfiguration,
        bool verboseDspMetrics = false);
    explicit GnuRadioReceiverBackend(
        std::unique_ptr<devices::DeviceController> explicitlySelectedDevice,
        SpectrumDisplayConfiguration spectrumConfiguration = {},
        bool verboseDspMetrics = false);
    explicit GnuRadioReceiverBackend(
        radio::RecordedIqSourceConfiguration recordedSource,
        SpectrumDisplayConfiguration spectrumConfiguration = {},
        bool verboseDspMetrics = false);
    ~GnuRadioReceiverBackend() override;

    GnuRadioReceiverBackend(const GnuRadioReceiverBackend&) = delete;
    GnuRadioReceiverBackend& operator=(const GnuRadioReceiverBackend&) = delete;

    [[nodiscard]] const radio::ReceiverLimits& limits() const noexcept override;
    [[nodiscard]] const radio::ReceiverCapabilities& capabilities()
        const noexcept override;
    [[nodiscard]] radio::ReceiverSourceCapabilities sourceCapabilities()
        const noexcept override;
    [[nodiscard]] radio::RecordingTransportState recordingTransport() const noexcept override;
    [[nodiscard]] radio::OperationResult setPlaybackPaused(bool paused) override;
    [[nodiscard]] radio::OperationResult restartPlayback() override;
    [[nodiscard]] radio::OperationResult seekPlayback(std::uint64_t sample) override;
    [[nodiscard]] const radio::ReceiverState& state() const noexcept override;
    [[nodiscard]] std::uint64_t effectiveSampleRate() const noexcept override;
    [[nodiscard]] std::uint64_t tuningGeneration() const noexcept override;
    [[nodiscard]] bool squelchOpen() const noexcept override;
    [[nodiscard]] std::optional<double> squelchSignalStrengthDb()
        const noexcept override;
    [[nodiscard]] std::optional<radio::SpectrumFrame> takeLatestSpectrumFrame() override;
    [[nodiscard]] std::vector<radio::SpectrumFrame> takePendingSpectrumFrames(
        std::size_t maximumFrames) override;
    [[nodiscard]] radio::SpectrumProcessingMetrics spectrumProcessingMetrics()
        const override;
    [[nodiscard]] std::size_t spectrumFftSize() const noexcept override;
    [[nodiscard]] std::size_t requestedSpectrumFftSize() const noexcept override;
    [[nodiscard]] radio::OperationResult setSpectrumFftSize(
        std::size_t fftSize) override;
    [[nodiscard]] std::uint32_t spectrumFramesPerSecond() const noexcept override;
    [[nodiscard]] radio::OperationResult setSpectrumFramesPerSecond(
        std::uint32_t framesPerSecond) override;
    [[nodiscard]] std::vector<float> takeAudioSamples(
        std::size_t maximumSamples) override;
    void clearAudioSamples() override;
    [[nodiscard]] std::uint64_t audioProducedSamples() const override;
    [[nodiscard]] std::uint64_t audioDroppedSamples() const override;
    void setFullBandwidthIqCaptureEnabled(bool enabled) override;
    [[nodiscard]] std::vector<std::complex<float>> takeFullBandwidthIqSamples(
        std::size_t maximumSamples) override;
    void clearFullBandwidthIqSamples() override;
    [[nodiscard]] std::uint64_t fullBandwidthIqDroppedSamples() const override;
    [[nodiscard]] std::size_t audioBufferedSampleCount() const override;
    [[nodiscard]] std::vector<float> takeDecoderInputSamples(
        std::size_t maximumSamples) override;
    void clearDecoderInputSamples() override;
    [[nodiscard]] std::uint64_t decoderInputDroppedSamples() const override;

    [[nodiscard]] radio::OperationResult startReception() override;
    [[nodiscard]] radio::OperationResult stopReception() override;
    [[nodiscard]] radio::OperationResult setCenterFrequency(
        std::uint64_t frequency) override;
    [[nodiscard]] radio::OperationResult setListeningFrequency(
        std::uint64_t frequency) override;
    [[nodiscard]] radio::OperationResult tuneListeningFrequency(
        double normalizedPosition) override;
    [[nodiscard]] radio::OperationResult shiftCenterFrequency(
        std::int64_t requestedStep) override;
    [[nodiscard]] radio::OperationResult setSampleRate(
        std::uint64_t sampleRate) override;
    [[nodiscard]] radio::OperationResult setFilterWidth(
        std::uint64_t filterWidth) override;
    [[nodiscard]] radio::OperationResult setGain(double gainDb) override;
    [[nodiscard]] radio::OperationResult setPpmCorrection(
        double ppmCorrection) override;
    [[nodiscard]] radio::OperationResult setDemodulationMode(
        radio::DemodulationMode mode) override;
    [[nodiscard]] radio::OperationResult setSquelchLevel(
        double squelchLevelDb) override;
    [[nodiscard]] radio::OperationResult enableManualSquelch() override;
    [[nodiscard]] radio::OperationResult disableSquelch() override;

    [[nodiscard]] double frequencyTranslationOffsetHz() const noexcept;
    [[nodiscard]] bool usesHardwareSource() const noexcept;
    [[nodiscard]] std::optional<radio::OperationResult> takeRuntimeError() override;
    [[nodiscard]] radio::OperationResult beginPpmCalibration() override;
    [[nodiscard]] radio::PpmCalibrationReadResult readPpmCalibrationBytes(
        std::span<std::uint8_t> bytes,
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] radio::OperationResult endPpmCalibration() override;
    [[nodiscard]] radio::OperationResult
    resumeReceptionAfterPpmCalibration() override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sdr::dsp
