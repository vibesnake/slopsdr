// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumFrame.hpp"

#include <stdexcept>
#include <utility>

namespace sdr::radio {

bool hasConsistentMetadata(const SpectrumFrame& frame) noexcept
{
    return frame.sequence > 0 && frame.sampleRate > 0 && frame.fftSize > 1 &&
           frame.normalizedMagnitudes.size() == frame.fftSize;
}

std::uint64_t captureSpan(const SpectrumFrame& frame) noexcept
{
    return frame.captureSpan == 0 ? frame.sampleRate : frame.captureSpan;
}

bool isCompatibleSpectrumFrame(
    const SpectrumFrame& frame,
    std::uint64_t sampleRate,
    std::size_t fftSize) noexcept
{
    return hasConsistentMetadata(frame) &&
           frame.sampleRate == sampleRate &&
           captureSpan(frame) == sampleRate &&
           frame.fftSize == fftSize;
}

bool isCurrentTuningFrame(
    const SpectrumFrame& frame,
    std::uint64_t centerFrequency,
    std::uint64_t sampleRate,
    std::uint64_t tuningGeneration) noexcept
{
    return isCompatibleSpectrumFrame(
               frame, sampleRate, frame.fftSize) &&
           frame.centerFrequency == centerFrequency &&
           frame.tuningGeneration == tuningGeneration;
}

SpectrumFrameQueue::SpectrumFrameQueue(std::size_t capacity)
    : m_capacity(capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Spectrum frame queue capacity must be positive");
    }
}

void SpectrumFrameQueue::push(SpectrumFrame frame)
{
    std::lock_guard lock(m_mutex);
    if (m_frames.size() == m_capacity) {
        m_frames.pop_front();
        ++m_droppedFrameCount;
    }
    m_frames.push_back(std::move(frame));
}

void SpectrumFrameQueue::clear()
{
    std::lock_guard lock(m_mutex);
    m_frames.clear();
}

std::optional<SpectrumFrame> SpectrumFrameQueue::takeOldest()
{
    std::lock_guard lock(m_mutex);
    if (m_frames.empty()) {
        return std::nullopt;
    }
    SpectrumFrame oldest = std::move(m_frames.front());
    m_frames.pop_front();
    return oldest;
}

std::optional<SpectrumFrame> SpectrumFrameQueue::takeLatest()
{
    std::lock_guard lock(m_mutex);
    if (m_frames.empty()) {
        return std::nullopt;
    }

    SpectrumFrame latest = std::move(m_frames.back());
    m_droppedFrameCount += static_cast<std::uint64_t>(m_frames.size() - 1);
    m_frames.clear();
    return latest;
}

std::size_t SpectrumFrameQueue::size() const
{
    std::lock_guard lock(m_mutex);
    return m_frames.size();
}

std::size_t SpectrumFrameQueue::capacity() const noexcept
{
    return m_capacity;
}

std::uint64_t SpectrumFrameQueue::droppedFrameCount() const
{
    std::lock_guard lock(m_mutex);
    return m_droppedFrameCount;
}

}  // namespace sdr::radio
