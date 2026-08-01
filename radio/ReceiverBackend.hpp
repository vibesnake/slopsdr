// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "AudioSampleBuffer.hpp"
#include "SpectrumFrame.hpp"
#include "WidebandIqSource.hpp"

#include <cstddef>
#include <chrono>
#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sdr::radio {

enum class DemodulationMode {
    Am,
    Nfm,
    Wfm,
    Usb,
    Lsb,
    DigitalDecoderOutput,
};

enum class SquelchMode {
    Disabled,
    Manual,
};

enum class ReceiverError {
    None,
    CenterFrequencyOutOfRange,
    ListeningFrequencyOutsidePassband,
    SampleRateOutOfRange,
    FilterWidthOutOfRange,
    GainOutOfRange,
    PpmCorrectionOutOfRange,
    PpmCorrectionUnsupported,
    SquelchLevelOutOfRange,
    SpectrumPositionOutOfRange,
    UnsupportedMode,
    BackendFailure,
};

struct FrequencyRange {
    std::uint64_t minimum = 0;
    std::uint64_t maximum = 0;

    [[nodiscard]] bool contains(std::uint64_t frequency) const noexcept
    {
        return frequency >= minimum && frequency <= maximum;
    }

    friend bool operator==(const FrequencyRange&, const FrequencyRange&) = default;
};

struct ReceiverLimits {
    FrequencyRange frequency{0, 9'999'999'999};
    // Hardware adapters set this only when a device explicitly supports
    // centers near an RF edge even though a complete capture passband would
    // extend past that edge.
    bool allowsPartialPassbandAtFrequencyEdges = false;
    FrequencyRange sampleRate{200'000, 10'000'000};
    double minimumGainDb = -10.0;
    double maximumGainDb = 100.0;
    double minimumPpmCorrection = -200.0;
    double maximumPpmCorrection = 200.0;
    double minimumSquelchDb = -160.0;
    double maximumSquelchDb = 0.0;
};

struct ReceiverCapabilities {
    bool ppmCorrectionSupported = false;
    bool automaticPpmCalibrationSupported = false;
};

struct FilterWidthRange {
    std::uint64_t minimum = 0;
    std::uint64_t maximum = 0;
    std::uint64_t preferred = 0;

    [[nodiscard]] bool contains(std::uint64_t width) const noexcept
    {
        return width >= minimum && width <= maximum;
    }
};

struct ReceiverState {
    std::uint64_t centerFrequency = 100'000'000;
    std::uint64_t listeningFrequency = 100'000'000;
    std::uint64_t sampleRate = 2'000'000;
    std::uint64_t filterWidth = 12'500;
    double gainDb = 0.0;
    double ppmCorrection = 0.0;
    DemodulationMode demodulationMode = DemodulationMode::Am;
    double squelchLevelDb = -80.0;
    double manualSquelchLevelDb = -80.0;
    SquelchMode squelchMode = SquelchMode::Manual;
    bool running = false;
};

struct OperationResult {
    ReceiverError error = ReceiverError::None;
    bool stateChanged = false;
    bool adjusted = false;
    std::string message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error == ReceiverError::None;
    }
};

enum class PpmCalibrationReadStatus {
    Bytes,
    Timeout,
    Stopped,
    Disconnected,
    Failed,
};

struct PpmCalibrationReadResult {
    PpmCalibrationReadStatus status = PpmCalibrationReadStatus::Failed;
    std::size_t byteCount = 0;
    bool droppedData = false;
    std::string message;
};

[[nodiscard]] inline FrequencyRange availablePassband(
    std::uint64_t centerFrequency, std::uint64_t sampleRate) noexcept
{
    const std::uint64_t lowerOffset = sampleRate / 2;
    const std::uint64_t upperOffset = sampleRate - lowerOffset;
    return {
        centerFrequency > lowerOffset ? centerFrequency - lowerOffset : 0,
        centerFrequency + upperOffset,
    };
}

[[nodiscard]] inline FrequencyRange availablePassband(
    const ReceiverState& state) noexcept
{
    return availablePassband(state.centerFrequency, state.sampleRate);
}

[[nodiscard]] inline FrequencyRange validCenterFrequencyRange(
    const ReceiverLimits& limits, std::uint64_t sampleRate) noexcept
{
    if (limits.allowsPartialPassbandAtFrequencyEdges) {
        return limits.frequency;
    }
    const std::uint64_t lowerOffset = sampleRate / 2;
    const std::uint64_t upperOffset = sampleRate - lowerOffset;
    return {
        limits.frequency.minimum + lowerOffset,
        limits.frequency.maximum - upperOffset,
    };
}

[[nodiscard]] FilterWidthRange filterWidthRange(
    DemodulationMode mode, std::uint64_t sampleRate) noexcept;

class ReceiverBackend
{
public:
    virtual ~ReceiverBackend() = default;

    [[nodiscard]] virtual const ReceiverLimits& limits() const noexcept = 0;
    [[nodiscard]] virtual const ReceiverCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual ReceiverSourceCapabilities sourceCapabilities() const noexcept
    {
        return {};
    }
    [[nodiscard]] virtual RecordingTransportState recordingTransport() const noexcept { return {}; }
    [[nodiscard]] virtual std::optional<std::string> takePlaybackEnd()
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual OperationResult setPlaybackPaused(bool)
    { return {ReceiverError::BackendFailure, false, false, "Recording playback is unavailable"}; }
    [[nodiscard]] virtual OperationResult restartPlayback()
    { return {ReceiverError::BackendFailure, false, false, "Recording playback is unavailable"}; }
    [[nodiscard]] virtual OperationResult seekPlayback(std::uint64_t)
    { return {ReceiverError::BackendFailure, false, false, "Recording seeking is unavailable"}; }
    [[nodiscard]] virtual const ReceiverState& state() const noexcept = 0;
    // The radio state records the requested rate. Hardware backends override
    // this when the driver confirms a distinct rate for DSP and display timing.
    [[nodiscard]] virtual std::uint64_t effectiveSampleRate() const noexcept
    {
        return state().sampleRate;
    }
    [[nodiscard]] virtual std::uint64_t tuningGeneration() const noexcept
    {
        return 0;
    }
    // This reflects the receiver's live squelch gate. It is intentionally
    // separate from the configured threshold and mode in ReceiverState.
    [[nodiscard]] virtual bool squelchOpen() const noexcept
    {
        return false;
    }
    // This is the smoothed post-channel-filter signal-strength measurement
    // used by the live power-squelch gate. It is absent until reception has
    // produced a valid measurement.
    [[nodiscard]] virtual std::optional<double> squelchSignalStrengthDb()
        const noexcept
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<SpectrumFrame> takeLatestSpectrumFrame() = 0;
    [[nodiscard]] virtual std::vector<SpectrumFrame> takePendingSpectrumFrames(
        std::size_t maximumFrames)
    {
        std::vector<SpectrumFrame> frames;
        if (maximumFrames == 0) {
            return frames;
        }
        if (auto frame = takeLatestSpectrumFrame()) {
            frames.push_back(std::move(*frame));
        }
        return frames;
    }
    [[nodiscard]] virtual SpectrumProcessingMetrics spectrumProcessingMetrics() const
    {
        return {};
    }
    [[nodiscard]] virtual std::size_t spectrumFftSize() const noexcept
    {
        return spectrumProcessingMetrics().fftSize;
    }
    [[nodiscard]] virtual std::size_t requestedSpectrumFftSize() const noexcept
    {
        return spectrumFftSize();
    }
    [[nodiscard]] virtual OperationResult setSpectrumFftSize(std::size_t fftSize)
    {
        static_cast<void>(fftSize);
        return {
            ReceiverError::BackendFailure,
            false,
            false,
            "Runtime spectrum FFT reconfiguration is unsupported",
        };
    }
    [[nodiscard]] virtual std::uint32_t spectrumFramesPerSecond() const noexcept
    {
        return 0;
    }
    [[nodiscard]] virtual OperationResult setSpectrumFramesPerSecond(
        std::uint32_t framesPerSecond)
    {
        static_cast<void>(framesPerSecond);
        return {
            ReceiverError::BackendFailure,
            false,
            false,
            "Runtime waterfall time-resolution reconfiguration is unsupported",
        };
    }
    [[nodiscard]] virtual std::vector<float> takeAudioSamples(
        std::size_t maximumSamples)
    {
        static_cast<void>(maximumSamples);
        return {};
    }
    // Recorded-audio adapters preserve the original channel layout through
    // this bounded frame handoff.  RF backends continue to provide mono audio
    // through takeAudioSamples().
    [[nodiscard]] virtual std::vector<float> takeStereoAudioSamples(
        std::size_t maximumFrames)
    {
        static_cast<void>(maximumFrames);
        return {};
    }
    virtual void clearAudioSamples() {}
    [[nodiscard]] virtual std::vector<float> takeDecoderInputSamples(
        std::size_t maximumSamples)
    {
        static_cast<void>(maximumSamples);
        return {};
    }
    virtual void clearDecoderInputSamples() {}
    [[nodiscard]] virtual std::uint64_t decoderInputDroppedSamples() const
    {
        return 0;
    }
    [[nodiscard]] virtual std::uint64_t audioProducedSamples() const
    {
        return 0;
    }
    [[nodiscard]] virtual std::uint64_t audioDroppedSamples() const
    {
        return 0;
    }
    virtual void setFullBandwidthIqCaptureEnabled(bool enabled)
    {
        static_cast<void>(enabled);
    }
    [[nodiscard]] virtual std::vector<std::complex<float>> takeFullBandwidthIqSamples(
        std::size_t maximumSamples)
    {
        static_cast<void>(maximumSamples);
        return {};
    }
    virtual void clearFullBandwidthIqSamples() {}
    [[nodiscard]] virtual std::uint64_t fullBandwidthIqDroppedSamples() const
    {
        return 0;
    }
    [[nodiscard]] virtual std::size_t audioBufferedSampleCount() const
    {
        return 0;
    }
    [[nodiscard]] virtual std::optional<OperationResult> takeRuntimeError()
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual OperationResult beginPpmCalibration()
    {
        return {
            ReceiverError::PpmCorrectionUnsupported,
            false,
            false,
            "Automatic PPM calibration is unsupported",
        };
    }
    [[nodiscard]] virtual PpmCalibrationReadResult readPpmCalibrationBytes(
        std::span<std::uint8_t>,
        std::chrono::milliseconds)
    {
        return {
            PpmCalibrationReadStatus::Stopped,
            0,
            false,
            "Automatic PPM calibration is stopped",
        };
    }
    [[nodiscard]] virtual OperationResult endPpmCalibration()
    {
        return {
            ReceiverError::None,
            false,
            false,
            "Automatic PPM calibration is stopped",
        };
    }
    [[nodiscard]] virtual OperationResult resumeReceptionAfterPpmCalibration()
    {
        return {
            ReceiverError::None,
            false,
            false,
            "Reception did not require restoration",
        };
    }

    [[nodiscard]] virtual OperationResult startReception() = 0;
    [[nodiscard]] virtual OperationResult stopReception() = 0;
    [[nodiscard]] virtual OperationResult setCenterFrequency(
        std::uint64_t frequency) = 0;
    [[nodiscard]] virtual OperationResult setListeningFrequency(
        std::uint64_t frequency) = 0;
    [[nodiscard]] virtual OperationResult tuneListeningFrequency(
        double normalizedPosition) = 0;
    [[nodiscard]] virtual OperationResult shiftCenterFrequency(
        std::int64_t requestedStep) = 0;
    [[nodiscard]] virtual OperationResult setSampleRate(std::uint64_t sampleRate) = 0;
    [[nodiscard]] virtual OperationResult setFilterWidth(std::uint64_t filterWidth) = 0;
    [[nodiscard]] virtual OperationResult setGain(double gainDb) = 0;
    [[nodiscard]] virtual OperationResult setPpmCorrection(double ppmCorrection) = 0;
    [[nodiscard]] virtual OperationResult setDemodulationMode(DemodulationMode mode) = 0;
    [[nodiscard]] virtual OperationResult setSquelchLevel(double squelchLevelDb) = 0;
    [[nodiscard]] virtual OperationResult enableManualSquelch() = 0;
    [[nodiscard]] virtual OperationResult disableSquelch() = 0;
};

}  // namespace sdr::radio
