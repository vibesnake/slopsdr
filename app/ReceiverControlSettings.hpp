// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include "DemodulatorRegistry.hpp"
#include "ReceiverBackend.hpp"

#include <QMetaType>
#include <QSettings>
#include <QString>

#include <cmath>
#include <cstdint>
#include <optional>

namespace sdr::app {

inline constexpr auto receiverCenterFrequencySettingsKey =
    "receiver/centerFrequencyHz";
inline constexpr auto receiverListeningFrequencySettingsKey =
    "receiver/listeningFrequencyHz";
inline constexpr auto receiverDemodulationModeSettingsKey =
    "receiver/demodulationMode";
inline constexpr auto receiverGainSettingsKey = "receiver/gainDb";
inline constexpr auto receiverSquelchThresholdSettingsKey =
    "receiver/squelchThresholdDb";
inline constexpr auto receiverSquelchDisabledSettingsKey =
    "receiver/squelchDisabled";

struct ReceiverControlSettings {
    std::uint64_t centerFrequency = 100'000'000;
    std::uint64_t listeningFrequency = 100'000'000;
    radio::DemodulationMode demodulationMode = radio::DemodulationMode::Am;
    double squelchThresholdDb = -80.0;
    bool squelchDisabled = false;
    std::optional<double> requestedGainDb;

    [[nodiscard]] radio::ReceiverState receiverState(
        std::uint64_t sampleRate) const noexcept
    {
        radio::ReceiverState state;
        state.centerFrequency = centerFrequency;
        state.listeningFrequency = listeningFrequency;
        state.sampleRate = sampleRate;
        state.demodulationMode = demodulationMode;
        const auto filterRange = radio::filterWidthRange(
            demodulationMode, sampleRate);
        if (!filterRange.contains(state.filterWidth)) {
            state.filterWidth = filterRange.preferred;
        }
        state.squelchLevelDb = squelchThresholdDb;
        state.manualSquelchLevelDb = squelchThresholdDb;
        state.squelchMode = squelchDisabled
                                ? radio::SquelchMode::Disabled
                                : radio::SquelchMode::Manual;
        return state;
    }
};

[[nodiscard]] inline std::optional<bool> strictSettingsBoolean(
    const QVariant& value)
{
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool();
    }
    const QString text = value.toString().trimmed().toLower();
    if (text == QLatin1String("true") || text == QLatin1String("1")) {
        return true;
    }
    if (text == QLatin1String("false") || text == QLatin1String("0")) {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] inline ReceiverControlSettings loadReceiverControlSettings(
    QSettings& settings,
    std::uint64_t sampleRate)
{
    ReceiverControlSettings restored;
    const radio::ReceiverLimits limits;
    const std::uint64_t validatedSampleRate =
        limits.sampleRate.contains(sampleRate) ? sampleRate : 2'000'000;
    const auto centerRange =
        radio::validCenterFrequencyRange(limits, validatedSampleRate);
    bool valid = false;
    const std::uint64_t savedCenter =
        settings.value(receiverCenterFrequencySettingsKey).toULongLong(&valid);
    if (valid && centerRange.contains(savedCenter)) {
        restored.centerFrequency = savedCenter;
    }

    valid = false;
    const std::uint64_t savedListening =
        settings.value(receiverListeningFrequencySettingsKey).toULongLong(&valid);
    if (valid && radio::availablePassband(
                     restored.centerFrequency,
                     validatedSampleRate).contains(savedListening)) {
        restored.listeningFrequency = savedListening;
    }

    const QVariant savedDemodulator =
        settings.value(receiverDemodulationModeSettingsKey);
    const auto stableId = savedDemodulator.toString().trimmed().toStdString();
    const auto* descriptor = radio::DemodulatorRegistry::findById(stableId);
    if (!descriptor) {
        valid = false;
        const int legacyMode = savedDemodulator.toInt(&valid);
        if (valid && legacyMode >= static_cast<int>(radio::DemodulationMode::Am) &&
            legacyMode <= static_cast<int>(radio::DemodulationMode::Lsb)) {
            descriptor = radio::DemodulatorRegistry::findByMode(
                static_cast<radio::DemodulationMode>(legacyMode));
            if (descriptor) {
                settings.setValue(
                    receiverDemodulationModeSettingsKey,
                    QString::fromLatin1(
                        descriptor->id.data(),
                        static_cast<qsizetype>(descriptor->id.size())));
            }
        }
    }
    if (descriptor) {
        const auto mode = descriptor->mode;
        const auto range = radio::filterWidthRange(mode, validatedSampleRate);
        if (range.maximum >= range.minimum) {
            restored.demodulationMode = mode;
        }
    }

    valid = false;
    const double savedGain =
        settings.value(receiverGainSettingsKey).toDouble(&valid);
    if (valid && std::isfinite(savedGain) &&
        savedGain >= limits.minimumGainDb && savedGain <= limits.maximumGainDb) {
        restored.requestedGainDb = savedGain;
    }

    valid = false;
    const double savedSquelch =
        settings.value(receiverSquelchThresholdSettingsKey).toDouble(&valid);
    if (valid && std::isfinite(savedSquelch) &&
        savedSquelch >= limits.minimumSquelchDb &&
        savedSquelch <= limits.maximumSquelchDb) {
        restored.squelchThresholdDb = savedSquelch;
    }

    if (settings.contains(receiverSquelchDisabledSettingsKey)) {
        restored.squelchDisabled = strictSettingsBoolean(
                                       settings.value(
                                           receiverSquelchDisabledSettingsKey))
                                       .value_or(false);
    }
    return restored;
}

inline void saveReceiverControlSettings(
    QSettings& settings,
    const ReceiverControlSettings& controls)
{
    settings.setValue(
        receiverCenterFrequencySettingsKey,
        static_cast<qulonglong>(controls.centerFrequency));
    settings.setValue(
        receiverListeningFrequencySettingsKey,
        static_cast<qulonglong>(controls.listeningFrequency));
    if (const auto* descriptor =
            radio::DemodulatorRegistry::findByMode(controls.demodulationMode)) {
        settings.setValue(
            receiverDemodulationModeSettingsKey,
            QString::fromLatin1(
                descriptor->id.data(),
                static_cast<qsizetype>(descriptor->id.size())));
    }
    settings.setValue(
        receiverSquelchThresholdSettingsKey, controls.squelchThresholdDb);
    settings.setValue(
        receiverSquelchDisabledSettingsKey, controls.squelchDisabled);
}

}  // namespace sdr::app
