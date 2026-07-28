// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sdr::devices {

inline constexpr std::uint64_t ppmCalibrationSettlingNanoseconds =
    5'000'000'000ULL;
inline constexpr std::uint64_t ppmCalibrationWindowNanoseconds =
    10'000'000'000ULL;
inline constexpr double maximumPpmCalibrationWindowSpreadPpm = 20.0;
inline constexpr int maximumSupportedAutomaticPpmCorrection = 100;

enum class PpmCalibrationPhase {
    Settling,
    MeasuringFirst,
    MeasuringSecond,
    MeasuringThird,
    ReadyToApply,
    Failed,
};

struct PpmCalibrationWindow {
    std::uint64_t complexSamples = 0;
    std::uint64_t elapsedNanoseconds = 0;
    double measuredPpm = 0.0;
};

class PpmCalibrationEstimator final
{
public:
    explicit PpmCalibrationEstimator(std::uint64_t requestedSampleRate);

    void start(std::uint64_t timestampNanoseconds);
    void ingest(
        std::span<const std::uint8_t> counterBytes,
        std::uint64_t timestampNanoseconds,
        bool streamReportedDroppedData = false);

    [[nodiscard]] PpmCalibrationPhase phase() const noexcept;
    [[nodiscard]] int progressPercent() const noexcept;
    [[nodiscard]] const std::vector<PpmCalibrationWindow>& windows() const noexcept;
    [[nodiscard]] std::optional<int> correctionPpm() const noexcept;
    [[nodiscard]] const std::string& failureMessage() const noexcept;

    [[nodiscard]] static std::optional<double> calculateMeasuredPpm(
        std::uint64_t complexSamples,
        std::uint64_t elapsedNanoseconds,
        std::uint64_t requestedSampleRate) noexcept;
    [[nodiscard]] static int driverCorrectionForMeasuredPpm(
        double measuredPpm) noexcept;

private:
    void fail(std::string message);
    void finishWindow(std::uint64_t timestampNanoseconds);
    void finishMeasurement();
    void resetWindow(std::uint64_t timestampNanoseconds);
    void addWindowObservation(std::uint64_t timestampNanoseconds);
    [[nodiscard]] std::optional<double> windowMeasuredPpm() const noexcept;
    [[nodiscard]] std::uint64_t phaseElapsedNanoseconds() const noexcept;

    std::uint64_t m_requestedSampleRate = 0;
    PpmCalibrationPhase m_phase = PpmCalibrationPhase::Failed;
    std::uint64_t m_phaseStartedNanoseconds = 0;
    std::uint64_t m_lastTimestampNanoseconds = 0;
    std::uint64_t m_windowComplexSamples = 0;
    std::uint64_t m_windowObservationCount = 0;
    long double m_windowSumTimeSeconds = 0.0L;
    long double m_windowSumSamples = 0.0L;
    long double m_windowSumTimeSquared = 0.0L;
    long double m_windowSumTimeSamples = 0.0L;
    std::optional<std::uint8_t> m_previousCounterByte;
    bool m_settlingCounterInitialized = false;
    std::vector<PpmCalibrationWindow> m_windows;
    std::optional<int> m_correctionPpm;
    std::string m_failureMessage;
};

}  // namespace sdr::devices
