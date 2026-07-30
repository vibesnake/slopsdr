// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <cstddef>
#include <cstdint>
#include <complex>
#include <mutex>
#include <span>
#include <vector>

namespace sdr::radio {

inline constexpr std::uint32_t receiverAudioSampleRate = 48'000;
inline constexpr std::size_t defaultAudioBufferCapacity =
    receiverAudioSampleRate / 20;

struct AudioBufferWriteResult {
    std::size_t acceptedSamples = 0;
    std::size_t droppedSamples = 0;
};

class AudioSampleBuffer final
{
public:
    explicit AudioSampleBuffer(std::size_t capacity = defaultAudioBufferCapacity);

    [[nodiscard]] AudioBufferWriteResult push(std::span<const float> samples);
    [[nodiscard]] std::vector<float> take(std::size_t maximumSamples);
    void clear();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::uint64_t totalProducedSamples() const;
    [[nodiscard]] std::uint64_t totalDroppedSamples() const;

private:
    mutable std::mutex m_mutex;
    std::vector<float> m_storage;
    std::size_t m_readIndex = 0;
    std::size_t m_size = 0;
    std::uint64_t m_totalProducedSamples = 0;
    std::uint64_t m_totalDroppedSamples = 0;
};

class ComplexSampleBuffer final
{
public:
    explicit ComplexSampleBuffer(std::size_t capacity);

    void push(std::span<const std::complex<float>> samples);
    [[nodiscard]] std::vector<std::complex<float>> take(std::size_t maximumSamples);
    void clear();
    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] std::uint64_t totalDroppedSamples() const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::complex<float>> m_storage;
    std::size_t m_readIndex = 0;
    std::size_t m_size = 0;
    std::uint64_t m_totalDroppedSamples = 0;
    bool m_enabled = false;
};

}  // namespace sdr::radio
