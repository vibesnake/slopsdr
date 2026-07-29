// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "ReceiverStateModel.hpp"

#include <cstdint>

namespace sdr::radio {

struct MockReceiverConfiguration {
    bool ppmCorrectionSupported = true;
    bool startSucceeds = true;
    bool stopSucceeds = true;
    bool squelchOpen = false;
};

class MockReceiverBackend final : public ReceiverBackend
{
public:
    explicit MockReceiverBackend(
        MockReceiverConfiguration configuration = {},
        ReceiverLimits limits = {});

    [[nodiscard]] const ReceiverLimits& limits() const noexcept override;
    [[nodiscard]] const ReceiverCapabilities& capabilities() const noexcept override;
    [[nodiscard]] const ReceiverState& state() const noexcept override;
    [[nodiscard]] std::uint64_t tuningGeneration() const noexcept override;
    [[nodiscard]] bool squelchOpen() const noexcept override;
    [[nodiscard]] std::optional<SpectrumFrame> takeLatestSpectrumFrame() override;
    [[nodiscard]] SpectrumProcessingMetrics spectrumProcessingMetrics()
        const override;
    [[nodiscard]] std::size_t spectrumFftSize() const noexcept override;
    [[nodiscard]] OperationResult setSpectrumFftSize(std::size_t fftSize) override;
    [[nodiscard]] std::uint32_t spectrumFramesPerSecond() const noexcept override;
    [[nodiscard]] OperationResult setSpectrumFramesPerSecond(
        std::uint32_t framesPerSecond) override;

    [[nodiscard]] OperationResult startReception() override;
    [[nodiscard]] OperationResult stopReception() override;
    [[nodiscard]] OperationResult setCenterFrequency(std::uint64_t frequency) override;
    [[nodiscard]] OperationResult setListeningFrequency(std::uint64_t frequency) override;
    [[nodiscard]] OperationResult tuneListeningFrequency(
        double normalizedPosition) override;
    [[nodiscard]] OperationResult shiftCenterFrequency(
        std::int64_t requestedStep) override;
    [[nodiscard]] OperationResult setSampleRate(std::uint64_t sampleRate) override;
    [[nodiscard]] OperationResult setFilterWidth(std::uint64_t filterWidth) override;
    [[nodiscard]] OperationResult setGain(double gainDb) override;
    [[nodiscard]] OperationResult setPpmCorrection(double ppmCorrection) override;
    [[nodiscard]] OperationResult setDemodulationMode(DemodulationMode mode) override;
    [[nodiscard]] OperationResult setSquelchLevel(double squelchLevelDb) override;
    [[nodiscard]] OperationResult enableManualSquelch() override;
    [[nodiscard]] OperationResult enableAutomaticSquelch() override;
    [[nodiscard]] OperationResult disableSquelch() override;

private:
    [[nodiscard]] OperationResult finish(OperationResult result);
    void publishSyntheticSpectrumFrame(bool force = true);

    ReceiverStateModel m_model;
    MockReceiverConfiguration m_configuration;
    ReceiverCapabilities m_capabilities;
    SpectrumFrameQueue m_spectrumFrames{2};
    std::uint64_t m_nextFrameSequence = 1;
    std::uint64_t m_lastFrameTimestampNanoseconds = 0;
    std::uint64_t m_tuningGeneration = 0;
    std::size_t m_spectrumFftSize = 4'096;
    std::uint32_t m_spectrumFramesPerSecond = 60;
};

}  // namespace sdr::radio
