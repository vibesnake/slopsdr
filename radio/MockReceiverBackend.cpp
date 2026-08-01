// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "MockReceiverBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace sdr::radio {
MockReceiverBackend::MockReceiverBackend(
    MockReceiverConfiguration configuration,
    ReceiverLimits limits)
    : m_model(std::move(limits))
    , m_configuration(configuration)
    , m_capabilities{configuration.ppmCorrectionSupported}
{
    publishSyntheticSpectrumFrame();
}

const ReceiverLimits& MockReceiverBackend::limits() const noexcept
{
    return m_model.limits();
}

const ReceiverCapabilities& MockReceiverBackend::capabilities() const noexcept
{
    return m_capabilities;
}

ReceiverSourceCapabilities MockReceiverBackend::sourceCapabilities() const noexcept
{
    return {
        .kind = ReceiverSourceKind::Mock,
        .ppmCorrectionSupported = m_capabilities.ppmCorrectionSupported,
    };
}

const ReceiverState& MockReceiverBackend::state() const noexcept
{
    return m_model.state();
}

std::uint64_t MockReceiverBackend::tuningGeneration() const noexcept
{
    return m_tuningGeneration;
}

bool MockReceiverBackend::squelchOpen() const noexcept
{
    return m_configuration.squelchOpen;
}

std::optional<double> MockReceiverBackend::squelchSignalStrengthDb()
    const noexcept
{
    if (!state().running || !m_configuration.squelchSignalStrengthDb.has_value() ||
        !std::isfinite(*m_configuration.squelchSignalStrengthDb)) {
        return std::nullopt;
    }
    return m_configuration.squelchSignalStrengthDb;
}

std::optional<SpectrumFrame> MockReceiverBackend::takeLatestSpectrumFrame()
{
    if (state().running) {
        publishSyntheticSpectrumFrame(false);
    }
    return m_spectrumFrames.takeLatest();
}

SpectrumProcessingMetrics MockReceiverBackend::spectrumProcessingMetrics() const
{
    const double requestedHop = static_cast<double>(state().sampleRate) /
                                static_cast<double>(m_spectrumFramesPerSecond);
    const double hopSize = std::max(
        requestedHop, static_cast<double>(m_spectrumFftSize));
    const double effectiveFramesPerSecond = std::min(
        static_cast<double>(m_spectrumFramesPerSecond),
        static_cast<double>(state().sampleRate) /
            static_cast<double>(m_spectrumFftSize));
    return {
        .fftSize = m_spectrumFftSize,
        .queueDepth = m_spectrumFrames.size(),
        .effectiveSampleRate = static_cast<double>(state().sampleRate),
        .targetFramesPerSecond = static_cast<double>(m_spectrumFramesPerSecond),
        .achievableFramesPerSecond = effectiveFramesPerSecond,
        .hertzPerBin = static_cast<double>(state().sampleRate) /
                       static_cast<double>(m_spectrumFftSize),
        .hopSize = hopSize,
        .overlapPercentage = 100.0 * std::max(
            0.0, 1.0 - hopSize / static_cast<double>(m_spectrumFftSize)),
    };
}

std::uint32_t MockReceiverBackend::spectrumFramesPerSecond() const noexcept
{
    return m_spectrumFramesPerSecond;
}

OperationResult MockReceiverBackend::setSpectrumFramesPerSecond(
    std::uint32_t framesPerSecond)
{
    if (framesPerSecond == 0 || framesPerSecond > 240) {
        return {
            ReceiverError::BackendFailure,
            false,
            false,
            "Internal spectrum source cadence must be from 1 through 240 frames/s",
        };
    }
    if (m_spectrumFramesPerSecond == framesPerSecond) {
        return {
            ReceiverError::None,
            false,
            false,
            "Internal spectrum source cadence is unchanged",
        };
    }
    m_spectrumFramesPerSecond = framesPerSecond;
    m_spectrumFrames.clear();
    publishSyntheticSpectrumFrame();
    return {
        ReceiverError::None,
        true,
        false,
        "Internal spectrum source cadence changed",
    };
}

std::size_t MockReceiverBackend::spectrumFftSize() const noexcept
{
    return m_spectrumFftSize;
}

OperationResult MockReceiverBackend::setSpectrumFftSize(std::size_t fftSize)
{
    switch (fftSize) {
    case 1'024:
    case 2'048:
    case 4'096:
    case 8'192:
    case 16'384:
    case 32'768:
    case 65'536:
    case 131'072:
    case 262'144:
        break;
    default:
        return {
            ReceiverError::BackendFailure,
            false,
            false,
            "Unsupported spectrum FFT size",
        };
    }
    if (m_spectrumFftSize == fftSize) {
        return {ReceiverError::None, false, false, "Spectrum FFT size is unchanged"};
    }
    m_spectrumFftSize = fftSize;
    m_spectrumFrames.clear();
    publishSyntheticSpectrumFrame();
    return {ReceiverError::None, true, false, "Spectrum FFT size changed"};
}

OperationResult MockReceiverBackend::startReception()
{
    if (!m_configuration.startSucceeds) {
        return {
            ReceiverError::BackendFailure,
            false,
            false,
            "Mock receiver failed to start",
        };
    }
    auto result = m_model.startReception();
    result.message = result.stateChanged
                         ? "Mock reception started - no hardware is active"
                         : "Mock reception is already running";
    return finish(std::move(result));
}

OperationResult MockReceiverBackend::stopReception()
{
    if (!m_configuration.stopSucceeds) {
        return {
            ReceiverError::BackendFailure,
            false,
            false,
            "Mock receiver failed to stop",
        };
    }
    auto result = m_model.stopReception();
    result.message = result.stateChanged ? "Mock reception stopped"
                                         : "Mock reception is already stopped";
    return finish(std::move(result));
}

OperationResult MockReceiverBackend::setCenterFrequency(std::uint64_t frequency)
{
    auto result = m_model.setCenterFrequency(frequency);
    if (result.succeeded() && result.stateChanged) {
        ++m_tuningGeneration;
    }
    return finish(std::move(result));
}

OperationResult MockReceiverBackend::setListeningFrequency(std::uint64_t frequency)
{
    return finish(m_model.setListeningFrequency(frequency));
}

OperationResult MockReceiverBackend::tuneListeningFrequency(double normalizedPosition)
{
    return finish(m_model.tuneListeningFrequency(normalizedPosition));
}

OperationResult MockReceiverBackend::shiftCenterFrequency(std::int64_t requestedStep)
{
    auto result = m_model.shiftCenterFrequency(requestedStep);
    if (result.succeeded() && result.stateChanged) {
        ++m_tuningGeneration;
    }
    return finish(std::move(result));
}

OperationResult MockReceiverBackend::setSampleRate(std::uint64_t sampleRate)
{
    return finish(m_model.setSampleRate(sampleRate));
}

OperationResult MockReceiverBackend::setFilterWidth(std::uint64_t filterWidth)
{
    return finish(m_model.setFilterWidth(filterWidth));
}

OperationResult MockReceiverBackend::setGain(double gainDb)
{
    return finish(m_model.setGain(gainDb));
}

OperationResult MockReceiverBackend::setPpmCorrection(double ppmCorrection)
{
    if (!m_capabilities.ppmCorrectionSupported) {
        return {
            ReceiverError::PpmCorrectionUnsupported,
            false,
            false,
            "PPM correction is unsupported by this receiver backend",
        };
    }
    return finish(m_model.setPpmCorrection(ppmCorrection));
}

OperationResult MockReceiverBackend::setDemodulationMode(DemodulationMode mode)
{
    return finish(m_model.setDemodulationMode(mode));
}

OperationResult MockReceiverBackend::setSquelchLevel(double squelchLevelDb)
{
    return finish(m_model.setSquelchLevel(squelchLevelDb));
}

OperationResult MockReceiverBackend::enableManualSquelch()
{
    return finish(m_model.enableManualSquelch());
}

OperationResult MockReceiverBackend::disableSquelch()
{
    return finish(m_model.disableSquelch());
}

OperationResult MockReceiverBackend::finish(OperationResult result)
{
    if (result.succeeded() && result.stateChanged) {
        publishSyntheticSpectrumFrame();
    }
    return result;
}

void MockReceiverBackend::publishSyntheticSpectrumFrame(bool force)
{
    const std::uint64_t timestampNanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const double effectiveFramesPerSecond = std::min(
        static_cast<double>(m_spectrumFramesPerSecond),
        static_cast<double>(state().sampleRate) /
            static_cast<double>(m_spectrumFftSize));
    const auto intervalNanoseconds = static_cast<std::uint64_t>(std::ceil(
        1'000'000'000.0 / effectiveFramesPerSecond));
    if (!force && m_lastFrameTimestampNanoseconds != 0 &&
        timestampNanoseconds - m_lastFrameTimestampNanoseconds <
            intervalNanoseconds) {
        return;
    }

    std::vector<float> magnitudes;
    magnitudes.reserve(m_spectrumFftSize);

    const double animationPhase =
        static_cast<double>(m_nextFrameSequence % m_spectrumFftSize) /
        static_cast<double>(m_spectrumFftSize);
    for (std::size_t bin = 0; bin < m_spectrumFftSize; ++bin) {
        const double position = static_cast<double>(bin) /
                                static_cast<double>(m_spectrumFftSize - 1);
        const double noise = 0.10 + 0.025 * std::sin(
            (position * 31.0 + animationPhase) * 6.283185307179586);
        const double mainPeak = 0.78 * std::exp(
            -std::pow((position - 0.57) / 0.018, 2.0));
        const double secondaryPeak = 0.48 * std::exp(
            -std::pow((position - 0.31) / 0.035, 2.0));
        magnitudes.push_back(static_cast<float>(
            std::clamp(noise + mainPeak + secondaryPeak, 0.0, 1.0)));
    }

    m_spectrumFrames.push({
        .sequence = m_nextFrameSequence++,
        .timestampNanoseconds = timestampNanoseconds,
        .centerFrequency = state().centerFrequency,
        .sampleRate = state().sampleRate,
        .captureSpan = state().sampleRate,
        .fftSize = m_spectrumFftSize,
        .tuningGeneration = m_tuningGeneration,
        .normalizedMagnitudes = std::move(magnitudes),
    });
    m_lastFrameTimestampNanoseconds = timestampNanoseconds;
}

}  // namespace sdr::radio
