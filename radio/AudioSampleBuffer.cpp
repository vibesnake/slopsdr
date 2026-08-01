// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioSampleBuffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace sdr::radio {

AudioSampleBuffer::AudioSampleBuffer(std::size_t capacity)
    : m_storage(capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Audio sample buffer capacity must be positive");
    }
}

AudioBufferWriteResult AudioSampleBuffer::push(std::span<const float> samples)
{
    if (samples.empty()) {
        return {};
    }

    std::scoped_lock lock(m_mutex);
    m_totalProducedSamples += samples.size();
    AudioBufferWriteResult result;
    if (samples.size() >= m_storage.size()) {
        result.acceptedSamples = m_storage.size();
        result.droppedSamples = m_size + samples.size() - m_storage.size();
        const auto retained = samples.last(m_storage.size());
        std::ranges::copy(retained, m_storage.begin());
        m_readIndex = 0;
        m_size = m_storage.size();
        m_totalDroppedSamples += result.droppedSamples;
        return result;
    }

    const std::size_t available = m_storage.size() - m_size;
    result.droppedSamples = samples.size() > available
                                ? samples.size() - available
                                : 0;
    if (result.droppedSamples > 0) {
        m_readIndex = (m_readIndex + result.droppedSamples) % m_storage.size();
        m_size -= result.droppedSamples;
    }

    const std::size_t writeIndex = (m_readIndex + m_size) % m_storage.size();
    const std::size_t firstCount =
        std::min(samples.size(), m_storage.size() - writeIndex);
    std::copy_n(samples.data(), firstCount, m_storage.data() + writeIndex);
    std::copy_n(
        samples.data() + firstCount,
        samples.size() - firstCount,
        m_storage.data());
    m_size += samples.size();
    result.acceptedSamples = samples.size();
    m_totalDroppedSamples += result.droppedSamples;
    return result;
}

std::vector<float> AudioSampleBuffer::take(std::size_t maximumSamples)
{
    std::scoped_lock lock(m_mutex);
    const std::size_t count = std::min(maximumSamples, m_size);
    std::vector<float> samples(count);
    if (count == 0) {
        return samples;
    }

    const std::size_t firstCount =
        std::min(count, m_storage.size() - m_readIndex);
    std::copy_n(m_storage.data() + m_readIndex, firstCount, samples.data());
    std::copy_n(
        m_storage.data(),
        count - firstCount,
        samples.data() + firstCount);
    m_readIndex = (m_readIndex + count) % m_storage.size();
    m_size -= count;
    return samples;
}

void AudioSampleBuffer::clear()
{
    std::scoped_lock lock(m_mutex);
    m_readIndex = 0;
    m_size = 0;
}

StereoAudioSampleBuffer::StereoAudioSampleBuffer(std::size_t frameCapacity)
    : m_storage(std::max<std::size_t>(std::size_t{1}, frameCapacity) * 2U)
{
}

AudioBufferWriteResult StereoAudioSampleBuffer::push(
    std::span<const float> interleavedSamples)
{
    const std::size_t incomingFrames = interleavedSamples.size() / 2U;
    if (incomingFrames == 0) return {};
    interleavedSamples = interleavedSamples.first(incomingFrames * 2U);
    std::scoped_lock lock(m_mutex);
    const std::size_t capacityFrames = capacity();
    const std::size_t acceptedFrames = std::min(incomingFrames, capacityFrames);
    const std::size_t sourceOffsetFrames = incomingFrames - acceptedFrames;
    const std::size_t overflowFrames = m_sizeFrames + acceptedFrames > capacityFrames
                                           ? m_sizeFrames + acceptedFrames - capacityFrames
                                           : 0;
    m_readFrame = (m_readFrame + overflowFrames) % capacityFrames;
    m_sizeFrames -= overflowFrames;
    const std::size_t writeFrame = (m_readFrame + m_sizeFrames) % capacityFrames;
    for (std::size_t frame = 0; frame < acceptedFrames; ++frame) {
        const std::size_t destination = ((writeFrame + frame) % capacityFrames) * 2U;
        const std::size_t source = (sourceOffsetFrames + frame) * 2U;
        m_storage[destination] = interleavedSamples[source];
        m_storage[destination + 1U] = interleavedSamples[source + 1U];
    }
    m_sizeFrames += acceptedFrames;
    m_totalProducedFrames += incomingFrames;
    m_totalDroppedFrames += sourceOffsetFrames + overflowFrames;
    return {.acceptedSamples = acceptedFrames, .droppedSamples = sourceOffsetFrames + overflowFrames};
}

std::vector<float> StereoAudioSampleBuffer::take(std::size_t maximumFrames)
{
    std::scoped_lock lock(m_mutex);
    const std::size_t frames = std::min(maximumFrames, m_sizeFrames);
    std::vector<float> result(frames * 2U);
    const std::size_t capacityFrames = capacity();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t source = ((m_readFrame + frame) % capacityFrames) * 2U;
        result[frame * 2U] = m_storage[source];
        result[frame * 2U + 1U] = m_storage[source + 1U];
    }
    m_readFrame = (m_readFrame + frames) % capacityFrames;
    m_sizeFrames -= frames;
    return result;
}

void StereoAudioSampleBuffer::clear()
{
    std::scoped_lock lock(m_mutex);
    m_readFrame = 0;
    m_sizeFrames = 0;
}

std::size_t StereoAudioSampleBuffer::size() const
{
    std::scoped_lock lock(m_mutex);
    return m_sizeFrames;
}

std::size_t StereoAudioSampleBuffer::capacity() const noexcept { return m_storage.size() / 2U; }
std::uint64_t StereoAudioSampleBuffer::totalProducedFrames() const
{
    std::scoped_lock lock(m_mutex);
    return m_totalProducedFrames;
}
std::uint64_t StereoAudioSampleBuffer::totalDroppedFrames() const
{
    std::scoped_lock lock(m_mutex);
    return m_totalDroppedFrames;
}

std::size_t AudioSampleBuffer::size() const
{
    std::scoped_lock lock(m_mutex);
    return m_size;
}

std::size_t AudioSampleBuffer::capacity() const noexcept
{
    return m_storage.size();
}

ComplexSampleBuffer::ComplexSampleBuffer(std::size_t capacity)
    : m_storage(capacity)
{
    if (capacity == 0) {
        throw std::invalid_argument("Complex sample buffer capacity must be positive");
    }
}

void ComplexSampleBuffer::push(std::span<const std::complex<float>> samples)
{
    if (samples.empty()) return;
    std::scoped_lock lock(m_mutex);
    if (!m_enabled) return;
    if (samples.size() >= m_storage.size()) {
        m_totalDroppedSamples += m_size + samples.size() - m_storage.size();
        std::ranges::copy(samples.last(m_storage.size()), m_storage.begin());
        m_readIndex = 0;
        m_size = m_storage.size();
        return;
    }
    const std::size_t available = m_storage.size() - m_size;
    const std::size_t dropped = samples.size() > available ? samples.size() - available : 0;
    if (dropped > 0) {
        m_readIndex = (m_readIndex + dropped) % m_storage.size();
        m_size -= dropped;
        m_totalDroppedSamples += dropped;
    }
    const std::size_t writeIndex = (m_readIndex + m_size) % m_storage.size();
    const std::size_t first = std::min(samples.size(), m_storage.size() - writeIndex);
    std::copy_n(samples.data(), first, m_storage.data() + writeIndex);
    std::copy_n(samples.data() + first, samples.size() - first, m_storage.data());
    m_size += samples.size();
}

std::vector<std::complex<float>> ComplexSampleBuffer::take(std::size_t maximumSamples)
{
    std::scoped_lock lock(m_mutex);
    const std::size_t count = std::min(maximumSamples, m_size);
    std::vector<std::complex<float>> samples(count);
    if (count == 0) return samples;
    const std::size_t first = std::min(count, m_storage.size() - m_readIndex);
    std::copy_n(m_storage.data() + m_readIndex, first, samples.data());
    std::copy_n(m_storage.data(), count - first, samples.data() + first);
    m_readIndex = (m_readIndex + count) % m_storage.size();
    m_size -= count;
    return samples;
}

void ComplexSampleBuffer::clear()
{
    std::scoped_lock lock(m_mutex);
    m_readIndex = 0;
    m_size = 0;
}

void ComplexSampleBuffer::setEnabled(bool enabled) noexcept
{
    std::scoped_lock lock(m_mutex);
    m_enabled = enabled;
}

std::uint64_t ComplexSampleBuffer::totalDroppedSamples() const
{
    std::scoped_lock lock(m_mutex);
    return m_totalDroppedSamples;
}

std::uint64_t AudioSampleBuffer::totalProducedSamples() const
{
    std::scoped_lock lock(m_mutex);
    return m_totalProducedSamples;
}

std::uint64_t AudioSampleBuffer::totalDroppedSamples() const
{
    std::scoped_lock lock(m_mutex);
    return m_totalDroppedSamples;
}

}  // namespace sdr::radio
