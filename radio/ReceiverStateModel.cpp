// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ReceiverStateModel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sdr::radio {
namespace {

FilterWidthRange unconstrainedFilterWidthRange(DemodulationMode mode) noexcept
{
    switch (mode) {
    case DemodulationMode::Am:
        return {3'000, 15'000, 10'000};
    case DemodulationMode::Nfm:
        return {5'000, 25'000, 12'500};
    case DemodulationMode::Wfm:
        return {100'000, 250'000, 180'000};
    case DemodulationMode::Usb:
    case DemodulationMode::Lsb:
        return {1'800, 4'000, 2'400};
    case DemodulationMode::DigitalDecoderOutput:
        return {5'000, 200'000, 12'500};
    }
    return {};
}

}  // namespace

std::optional<double> estimateOneShotSquelchThreshold(
    std::span<const double> signalStrengthSamplesDb,
    const ReceiverLimits& limits) noexcept
{
    std::vector<double> finiteSamples;
    finiteSamples.reserve(signalStrengthSamplesDb.size());
    for (const double sample : signalStrengthSamplesDb) {
        if (std::isfinite(sample)) {
            finiteSamples.push_back(sample);
        }
    }
    if (finiteSamples.empty()) {
        return std::nullopt;
    }

    std::ranges::sort(finiteSamples);
    const std::size_t middle = finiteSamples.size() / 2;
    const double measuredLevel = finiteSamples.size() % 2 == 0
                                     ? (finiteSamples[middle - 1] +
                                        finiteSamples[middle]) /
                                           2.0
                                     : finiteSamples[middle];
    return std::clamp(
        measuredLevel + 2.0,
        limits.minimumSquelchDb,
        limits.maximumSquelchDb);
}

ReceiverStateModel::ReceiverStateModel(ReceiverLimits limits)
    : m_limits(std::move(limits))
{
}

ReceiverStateModel::ReceiverStateModel(
    ReceiverLimits limits, ReceiverState initialState)
    : m_limits(std::move(limits))
    , m_state(std::move(initialState))
{
    if (!m_limits.sampleRate.contains(m_state.sampleRate)) {
        throw std::invalid_argument("Initial sample rate is outside the receiver limits");
    }
    if (!validCenterFrequencyRange(m_limits, m_state.sampleRate)
             .contains(m_state.centerFrequency)) {
        throw std::invalid_argument(
            "Initial center frequency cannot provide the complete passband");
    }
    if (!availablePassband(m_state).contains(m_state.listeningFrequency)) {
        throw std::invalid_argument(
            "Initial listening frequency is outside the receiver passband");
    }
    if (!filterWidthRange(m_state.demodulationMode, m_state.sampleRate)
             .contains(m_state.filterWidth)) {
        throw std::invalid_argument(
            "Initial filter width is unsupported at the receiver sample rate");
    }
}

FilterWidthRange filterWidthRange(
    DemodulationMode mode, std::uint64_t sampleRate) noexcept
{
    auto range = unconstrainedFilterWidthRange(mode);
    range.maximum = std::min(range.maximum, sampleRate);
    if (range.maximum < range.minimum) {
        range.preferred = 0;
        return range;
    }
    range.preferred = std::clamp(range.preferred, range.minimum, range.maximum);
    return range;
}

const ReceiverLimits& ReceiverStateModel::limits() const noexcept
{
    return m_limits;
}

const ReceiverState& ReceiverStateModel::state() const noexcept
{
    return m_state;
}

OperationResult ReceiverStateModel::startReception()
{
    if (m_state.running) {
        return success(false, "Reception is already running");
    }

    m_state.running = true;
    return success(true, "Reception started");
}

OperationResult ReceiverStateModel::stopReception()
{
    if (!m_state.running) {
        return success(false, "Reception is already stopped");
    }

    m_state.running = false;
    return success(true, "Reception stopped");
}

OperationResult ReceiverStateModel::setCenterFrequency(std::uint64_t frequency)
{
    const FrequencyRange centerRange =
        validCenterFrequencyRange(m_limits, m_state.sampleRate);
    if (!centerRange.contains(frequency)) {
        return failure(
            ReceiverError::CenterFrequencyOutOfRange,
            "Center frequency cannot provide the complete current passband");
    }

    const bool stateChanged = frequency != m_state.centerFrequency ||
                              frequency != m_state.listeningFrequency;

    m_state.centerFrequency = frequency;
    m_state.listeningFrequency = frequency;
    return success(stateChanged, "Center and listening frequency changed");
}

OperationResult ReceiverStateModel::setListeningFrequency(std::uint64_t frequency)
{
    const FrequencyRange passband = availablePassband(m_state);
    if (!passband.contains(frequency)) {
        return failure(
            ReceiverError::ListeningFrequencyOutsidePassband,
            "Listening frequency must remain inside the current passband");
    }

    const bool stateChanged = frequency != m_state.listeningFrequency;
    m_state.listeningFrequency = frequency;
    return success(stateChanged, "Listening frequency changed");
}

OperationResult ReceiverStateModel::tuneListeningFrequency(double normalizedPosition)
{
    if (!std::isfinite(normalizedPosition) || normalizedPosition < 0.0 ||
        normalizedPosition > 1.0) {
        return failure(
            ReceiverError::SpectrumPositionOutOfRange,
            "Displayed-spectrum position must be between 0 and 1");
    }

    const FrequencyRange passband = availablePassband(m_state);
    const auto passbandWidth = passband.maximum - passband.minimum;
    const auto offset = static_cast<std::uint64_t>(
        std::llround(normalizedPosition * static_cast<double>(passbandWidth)));
    return setListeningFrequency(passband.minimum + offset);
}

OperationResult ReceiverStateModel::shiftCenterFrequency(std::int64_t requestedStep)
{
    if (requestedStep == 0) {
        return success(false, "Center-frequency shift is zero");
    }

    const FrequencyRange centerRange =
        validCenterFrequencyRange(m_limits, m_state.sampleRate);
    std::uint64_t newCenterFrequency = m_state.centerFrequency;

    if (requestedStep > 0) {
        const auto availableStep = centerRange.maximum - m_state.centerFrequency;
        newCenterFrequency += std::min(
            static_cast<std::uint64_t>(requestedStep), availableStep);
    } else {
        const auto availableStep = m_state.centerFrequency - centerRange.minimum;
        if (requestedStep < -static_cast<std::int64_t>(availableStep)) {
            newCenterFrequency = centerRange.minimum;
        } else {
            newCenterFrequency -= static_cast<std::uint64_t>(-requestedStep);
        }
    }

    const std::int64_t actualStep = static_cast<std::int64_t>(newCenterFrequency) -
                                    static_cast<std::int64_t>(m_state.centerFrequency);
    const bool adjusted = actualStep != requestedStep;

    m_state.centerFrequency = newCenterFrequency;
    m_state.listeningFrequency = newCenterFrequency;
    return success(
        actualStep != 0,
        adjusted ? "Center-frequency shift was limited by the available range"
                 : "Center and listening frequency shifted",
        adjusted);
}

OperationResult ReceiverStateModel::setSampleRate(std::uint64_t sampleRate)
{
    if (!m_limits.sampleRate.contains(sampleRate)) {
        return failure(
            ReceiverError::SampleRateOutOfRange,
            "Sample rate is outside the backend limits");
    }
    const auto widthRange = filterWidthRange(m_state.demodulationMode, sampleRate);
    if (!widthRange.contains(m_state.filterWidth)) {
        return failure(
            ReceiverError::FilterWidthOutOfRange,
            "Sample rate cannot support the current mode and filter width");
    }

    const FrequencyRange centerRange = validCenterFrequencyRange(m_limits, sampleRate);
    if (!centerRange.contains(m_state.centerFrequency)) {
        return failure(
            ReceiverError::CenterFrequencyOutOfRange,
            "Sample rate cannot provide a complete passband at the current center frequency");
    }

    const FrequencyRange newPassband =
        availablePassband(m_state.centerFrequency, sampleRate);
    const std::uint64_t listeningFrequency = std::clamp(
        m_state.listeningFrequency, newPassband.minimum, newPassband.maximum);
    const bool listeningAdjusted = listeningFrequency != m_state.listeningFrequency;
    const bool stateChanged = sampleRate != m_state.sampleRate || listeningAdjusted;

    m_state.sampleRate = sampleRate;
    m_state.listeningFrequency = listeningFrequency;

    if (listeningAdjusted) {
        return success(
            stateChanged,
            "Sample rate changed; listening frequency moved to the nearest passband edge",
            true);
    }
    return success(stateChanged, "Sample rate changed");
}

OperationResult ReceiverStateModel::setFilterWidth(std::uint64_t filterWidth)
{
    const auto range = filterWidthRange(
        m_state.demodulationMode, m_state.sampleRate);
    if (!range.contains(filterWidth)) {
        return failure(
            ReceiverError::FilterWidthOutOfRange,
            "Filter width is outside the selected mode and current passband limits");
    }

    const bool stateChanged = filterWidth != m_state.filterWidth;
    m_state.filterWidth = filterWidth;
    return success(stateChanged, "Filter width changed");
}

OperationResult ReceiverStateModel::setGain(double gainDb)
{
    if (!std::isfinite(gainDb) || gainDb < m_limits.minimumGainDb ||
        gainDb > m_limits.maximumGainDb) {
        return failure(ReceiverError::GainOutOfRange, "Gain is outside the backend limits");
    }

    const bool stateChanged = gainDb != m_state.gainDb;
    m_state.gainDb = gainDb;
    return success(stateChanged, "Gain changed");
}

OperationResult ReceiverStateModel::setPpmCorrection(double ppmCorrection)
{
    if (!std::isfinite(ppmCorrection) ||
        ppmCorrection < m_limits.minimumPpmCorrection ||
        ppmCorrection > m_limits.maximumPpmCorrection) {
        return failure(
            ReceiverError::PpmCorrectionOutOfRange,
            "PPM correction must be between -200 and 200");
    }

    const bool stateChanged = ppmCorrection != m_state.ppmCorrection;
    m_state.ppmCorrection = ppmCorrection;
    return success(stateChanged, "PPM correction changed");
}

OperationResult ReceiverStateModel::setDemodulationMode(DemodulationMode mode)
{
    switch (mode) {
    case DemodulationMode::Am:
    case DemodulationMode::Nfm:
    case DemodulationMode::Wfm:
    case DemodulationMode::Usb:
    case DemodulationMode::Lsb:
    case DemodulationMode::DigitalDecoderOutput:
        break;
    default:
        return failure(ReceiverError::UnsupportedMode, "Demodulation mode is unsupported");
    }

    const auto range = filterWidthRange(mode, m_state.sampleRate);
    if (range.maximum < range.minimum) {
        return failure(
            ReceiverError::FilterWidthOutOfRange,
            "The current sample rate cannot support this demodulation mode");
    }

    const bool filterAdjusted =
        !range.contains(m_state.filterWidth) ||
        (mode == DemodulationMode::DigitalDecoderOutput &&
         m_state.demodulationMode != mode &&
         m_state.filterWidth != range.preferred);
    const bool stateChanged = mode != m_state.demodulationMode || filterAdjusted;
    m_state.demodulationMode = mode;
    if (filterAdjusted) {
        m_state.filterWidth = range.preferred;
    }
    return success(
        stateChanged,
        filterAdjusted
            ? "Demodulation mode changed; filter width set to the mode default"
            : (mode == DemodulationMode::DigitalDecoderOutput
                   ? "DMR/P25 decoder input selected"
                   : "Demodulation mode changed"),
        filterAdjusted);
}

OperationResult ReceiverStateModel::setSquelchLevel(double squelchLevelDb)
{
    if (!std::isfinite(squelchLevelDb) ||
        squelchLevelDb < m_limits.minimumSquelchDb ||
        squelchLevelDb > m_limits.maximumSquelchDb) {
        return failure(
            ReceiverError::SquelchLevelOutOfRange,
            "Squelch level is outside the backend limits");
    }

    const bool stateChanged = squelchLevelDb != m_state.squelchLevelDb ||
                              m_state.squelchMode != SquelchMode::Manual;
    m_state.squelchLevelDb = squelchLevelDb;
    m_state.manualSquelchLevelDb = squelchLevelDb;
    m_state.squelchMode = SquelchMode::Manual;
    return success(stateChanged, "Manual squelch level changed");
}

OperationResult ReceiverStateModel::enableManualSquelch()
{
    const bool stateChanged = m_state.squelchMode != SquelchMode::Manual ||
                              m_state.squelchLevelDb != m_state.manualSquelchLevelDb;
    m_state.squelchMode = SquelchMode::Manual;
    m_state.squelchLevelDb = m_state.manualSquelchLevelDb;
    return success(stateChanged, "Manual squelch enabled");
}

OperationResult ReceiverStateModel::disableSquelch()
{
    const bool stateChanged = m_state.squelchMode != SquelchMode::Disabled;
    m_state.squelchMode = SquelchMode::Disabled;
    return success(stateChanged, "Squelch disabled");
}

OperationResult ReceiverStateModel::success(
    bool stateChanged, std::string message, bool adjusted)
{
    return {ReceiverError::None, stateChanged, adjusted, std::move(message)};
}

OperationResult ReceiverStateModel::failure(ReceiverError error, std::string message)
{
    return {error, false, false, std::move(message)};
}

}  // namespace sdr::radio
