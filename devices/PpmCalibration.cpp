// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "PpmCalibration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace sdr::devices {
namespace {

bool measuring(PpmCalibrationPhase phase) noexcept
{
    return phase == PpmCalibrationPhase::MeasuringFirst ||
           phase == PpmCalibrationPhase::MeasuringSecond ||
           phase == PpmCalibrationPhase::MeasuringThird;
}

}  // namespace

PpmCalibrationEstimator::PpmCalibrationEstimator(
    std::uint64_t requestedSampleRate)
    : m_requestedSampleRate(requestedSampleRate)
{
    if (requestedSampleRate == 0) {
        m_failureMessage = "Requested sample rate is invalid";
    }
}

void PpmCalibrationEstimator::start(std::uint64_t timestampNanoseconds)
{
    m_windows.clear();
    m_correctionPpm.reset();
    m_previousCounterByte.reset();
    m_settlingCounterInitialized = false;
    m_windowComplexSamples = 0;
    if (m_requestedSampleRate == 0 || timestampNanoseconds == 0) {
        fail("Calibration timing or requested sample rate is invalid");
        return;
    }
    m_failureMessage.clear();
    m_phase = PpmCalibrationPhase::Settling;
    m_phaseStartedNanoseconds = timestampNanoseconds;
    m_lastTimestampNanoseconds = timestampNanoseconds;
}

void PpmCalibrationEstimator::ingest(
    std::span<const std::uint8_t> counterBytes,
    std::uint64_t timestampNanoseconds,
    bool streamReportedDroppedData)
{
    if (m_phase == PpmCalibrationPhase::Failed ||
        m_phase == PpmCalibrationPhase::ReadyToApply) {
        return;
    }
    if (timestampNanoseconds <= m_lastTimestampNanoseconds) {
        fail("Calibration monotonic timing is invalid");
        return;
    }
    m_lastTimestampNanoseconds = timestampNanoseconds;
    if (streamReportedDroppedData) {
        fail("The SDR stream reported dropped calibration data");
        return;
    }
    if (counterBytes.empty() || counterBytes.size() % 2 != 0) {
        fail("The RTL-SDR test counter returned an incomplete complex sample");
        return;
    }

    if (m_phase == PpmCalibrationPhase::Settling) {
        // RTL2832 test mode can reset its counter once while the first USB
        // transfer is being established. The settling interval deliberately
        // permits that single reset in the first buffer, then enforces strict
        // continuity even before measurement begins.
        std::size_t discontinuities = 0;
        for (const std::uint8_t byte : counterBytes) {
            if (m_previousCounterByte.has_value() &&
                byte !=
                    static_cast<std::uint8_t>(*m_previousCounterByte + 1U)) {
                ++discontinuities;
            }
            m_previousCounterByte = byte;
        }
        if ((m_settlingCounterInitialized && discontinuities != 0) ||
            discontinuities > 1) {
            fail("RTL-SDR test-counter discontinuity detected");
            return;
        }
        m_settlingCounterInitialized = true;
        if (phaseElapsedNanoseconds() >= ppmCalibrationSettlingNanoseconds) {
            m_phase = PpmCalibrationPhase::MeasuringFirst;
            resetWindow(timestampNanoseconds);
        }
        return;
    }

    if (!measuring(m_phase)) {
        return;
    }
    for (const std::uint8_t byte : counterBytes) {
        if (m_previousCounterByte.has_value() &&
            byte != static_cast<std::uint8_t>(*m_previousCounterByte + 1U)) {
            fail("RTL-SDR test-counter discontinuity detected");
            return;
        }
        m_previousCounterByte = byte;
    }
    const std::uint64_t complexSamples =
        static_cast<std::uint64_t>(counterBytes.size() / 2);
    if (complexSamples >
        std::numeric_limits<std::uint64_t>::max() - m_windowComplexSamples) {
        fail("Calibration sample count overflowed");
        return;
    }
    m_windowComplexSamples += complexSamples;
    addWindowObservation(timestampNanoseconds);
    if (phaseElapsedNanoseconds() >= ppmCalibrationWindowNanoseconds) {
        finishWindow(timestampNanoseconds);
    }
}

PpmCalibrationPhase PpmCalibrationEstimator::phase() const noexcept
{
    return m_phase;
}

int PpmCalibrationEstimator::progressPercent() const noexcept
{
    constexpr std::uint64_t maximumMeasurementNanoseconds =
        ppmCalibrationSettlingNanoseconds + 3 * ppmCalibrationWindowNanoseconds;
    if (m_phase == PpmCalibrationPhase::ReadyToApply) {
        return 95;
    }
    if (m_phase == PpmCalibrationPhase::Failed) {
        return 0;
    }

    std::uint64_t completedNanoseconds = 0;
    switch (m_phase) {
    case PpmCalibrationPhase::Settling:
        completedNanoseconds = std::min(
            phaseElapsedNanoseconds(), ppmCalibrationSettlingNanoseconds);
        break;
    case PpmCalibrationPhase::MeasuringFirst:
        completedNanoseconds =
            ppmCalibrationSettlingNanoseconds +
            std::min(phaseElapsedNanoseconds(), ppmCalibrationWindowNanoseconds);
        break;
    case PpmCalibrationPhase::MeasuringSecond:
        completedNanoseconds =
            ppmCalibrationSettlingNanoseconds + ppmCalibrationWindowNanoseconds +
            std::min(phaseElapsedNanoseconds(), ppmCalibrationWindowNanoseconds);
        break;
    case PpmCalibrationPhase::MeasuringThird:
        completedNanoseconds =
            ppmCalibrationSettlingNanoseconds +
            2 * ppmCalibrationWindowNanoseconds +
            std::min(phaseElapsedNanoseconds(), ppmCalibrationWindowNanoseconds);
        break;
    case PpmCalibrationPhase::ReadyToApply:
    case PpmCalibrationPhase::Failed:
        break;
    }
    return static_cast<int>(
        std::min<std::uint64_t>(
            94, completedNanoseconds * 94 / maximumMeasurementNanoseconds));
}

const std::vector<PpmCalibrationWindow>&
PpmCalibrationEstimator::windows() const noexcept
{
    return m_windows;
}

std::optional<int> PpmCalibrationEstimator::correctionPpm() const noexcept
{
    return m_correctionPpm;
}

const std::string& PpmCalibrationEstimator::failureMessage() const noexcept
{
    return m_failureMessage;
}

std::optional<double> PpmCalibrationEstimator::calculateMeasuredPpm(
    std::uint64_t complexSamples,
    std::uint64_t elapsedNanoseconds,
    std::uint64_t requestedSampleRate) noexcept
{
    if (complexSamples == 0 || elapsedNanoseconds == 0 ||
        requestedSampleRate == 0) {
        return std::nullopt;
    }
    const long double measuredSampleRate =
        static_cast<long double>(complexSamples) * 1'000'000'000.0L /
        static_cast<long double>(elapsedNanoseconds);
    const long double ppm =
        1'000'000.0L *
        (measuredSampleRate / static_cast<long double>(requestedSampleRate) -
         1.0L);
    if (!std::isfinite(static_cast<double>(ppm))) {
        return std::nullopt;
    }
    return static_cast<double>(ppm);
}

int PpmCalibrationEstimator::driverCorrectionForMeasuredPpm(
    double measuredPpm) noexcept
{
    return std::isfinite(measuredPpm)
               ? static_cast<int>(std::llround(measuredPpm))
               : 0;
}

void PpmCalibrationEstimator::fail(std::string message)
{
    m_phase = PpmCalibrationPhase::Failed;
    m_correctionPpm.reset();
    m_failureMessage = std::move(message);
}

void PpmCalibrationEstimator::finishWindow(
    std::uint64_t timestampNanoseconds)
{
    const std::uint64_t elapsedNanoseconds =
        timestampNanoseconds - m_phaseStartedNanoseconds;
    const auto measuredPpm = windowMeasuredPpm();
    if (!measuredPpm.has_value()) {
        fail("Calibration window timing is invalid");
        return;
    }
    m_windows.push_back({
        m_windowComplexSamples,
        elapsedNanoseconds,
        *measuredPpm,
    });

    if (m_windows.size() == 2 &&
        driverCorrectionForMeasuredPpm(m_windows[0].measuredPpm) ==
            driverCorrectionForMeasuredPpm(m_windows[1].measuredPpm)) {
        finishMeasurement();
        return;
    }
    if (m_windows.size() == 3) {
        finishMeasurement();
        return;
    }

    m_phase = m_windows.size() == 1
                  ? PpmCalibrationPhase::MeasuringSecond
                  : PpmCalibrationPhase::MeasuringThird;
    resetWindow(timestampNanoseconds);
}

void PpmCalibrationEstimator::finishMeasurement()
{
    if (m_windows.size() < 2 || m_windows.size() > 3) {
        fail("Calibration did not produce enough valid measurement windows");
        return;
    }
    const auto [minimum, maximum] = std::minmax_element(
        m_windows.begin(),
        m_windows.end(),
        [](const PpmCalibrationWindow& left, const PpmCalibrationWindow& right) {
            return left.measuredPpm < right.measuredPpm;
        });
    if (maximum->measuredPpm - minimum->measuredPpm >
        maximumPpmCalibrationWindowSpreadPpm) {
        fail("Calibration windows exceed the maximum stable PPM spread");
        return;
    }

    double selectedPpm = m_windows.front().measuredPpm;
    if (m_windows.size() == 3) {
        std::array<double, 3> values{
            m_windows[0].measuredPpm,
            m_windows[1].measuredPpm,
            m_windows[2].measuredPpm,
        };
        std::ranges::sort(values);
        selectedPpm = values[1];
    }
    const int correction = driverCorrectionForMeasuredPpm(selectedPpm);
    if (std::abs(correction) > maximumSupportedAutomaticPpmCorrection) {
        fail("Measured correction is outside the conservative supported PPM range");
        return;
    }
    m_correctionPpm = correction;
    m_phase = PpmCalibrationPhase::ReadyToApply;
}

void PpmCalibrationEstimator::resetWindow(
    std::uint64_t timestampNanoseconds)
{
    m_phaseStartedNanoseconds = timestampNanoseconds;
    m_windowComplexSamples = 0;
    // Include the synchronized window origin. Subsequent observations use
    // cumulative sample count versus monotonic time, allowing a least-squares
    // slope to suppress USB callback and worker scheduling jitter.
    m_windowObservationCount = 1;
    m_windowSumTimeSeconds = 0.0L;
    m_windowSumSamples = 0.0L;
    m_windowSumTimeSquared = 0.0L;
    m_windowSumTimeSamples = 0.0L;
}

void PpmCalibrationEstimator::addWindowObservation(
    std::uint64_t timestampNanoseconds)
{
    const long double timeSeconds =
        static_cast<long double>(
            timestampNanoseconds - m_phaseStartedNanoseconds) /
        1'000'000'000.0L;
    const long double samples =
        static_cast<long double>(m_windowComplexSamples);
    ++m_windowObservationCount;
    m_windowSumTimeSeconds += timeSeconds;
    m_windowSumSamples += samples;
    m_windowSumTimeSquared += timeSeconds * timeSeconds;
    m_windowSumTimeSamples += timeSeconds * samples;
}

std::optional<double> PpmCalibrationEstimator::windowMeasuredPpm()
    const noexcept
{
    if (m_windowObservationCount < 2 || m_requestedSampleRate == 0) {
        return std::nullopt;
    }
    const long double observations =
        static_cast<long double>(m_windowObservationCount);
    const long double denominator =
        observations * m_windowSumTimeSquared -
        m_windowSumTimeSeconds * m_windowSumTimeSeconds;
    if (!(denominator > 0.0L)) {
        return std::nullopt;
    }
    const long double measuredSampleRate =
        (observations * m_windowSumTimeSamples -
         m_windowSumTimeSeconds * m_windowSumSamples) /
        denominator;
    const long double ppm =
        1'000'000.0L *
        (measuredSampleRate /
             static_cast<long double>(m_requestedSampleRate) -
         1.0L);
    if (!std::isfinite(static_cast<double>(ppm))) {
        return std::nullopt;
    }
    return static_cast<double>(ppm);
}

std::uint64_t PpmCalibrationEstimator::phaseElapsedNanoseconds() const noexcept
{
    return m_lastTimestampNanoseconds >= m_phaseStartedNanoseconds
               ? m_lastTimestampNanoseconds - m_phaseStartedNanoseconds
               : 0;
}

}  // namespace sdr::devices
