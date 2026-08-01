// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <QtCore/qtypes.h>

namespace sdr::platform::bookmarkLimits {

// These limits keep a user-local bookmark file comfortably larger than normal
// use while bounding parse time, memory, and preserved future-format payloads.
inline constexpr qsizetype maximumFileBytes = 4 * 1024 * 1024;
inline constexpr qsizetype maximumNodeCount = 4'096;
inline constexpr int maximumTreeDepth = 64;
inline constexpr qsizetype maximumNameUtf8Bytes = 256;
inline constexpr qsizetype maximumDemodulatorIdUtf8Bytes = 128;
inline constexpr qsizetype maximumJsonPayloadBytes = 64 * 1024;

}  // namespace sdr::platform::bookmarkLimits
