// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "AudioOutputService.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMediaDevices>
#include <QString>

#include <algorithm>
#include <memory>
#include <utility>

namespace sdr::platform {
namespace {

constexpr std::size_t audioFrameBytes = 2 * sizeof(std::int16_t);

std::string stableIdentifier(const QAudioDevice& device)
{
    return device.id()
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
        .toStdString();
}

QString audioErrorText(QAudio::Error error)
{
    switch (error) {
    case QAudio::NoError:
        return QStringLiteral("No audio error");
    case QAudio::OpenError:
        return QStringLiteral("The audio device could not be opened");
    case QAudio::IOError:
        return QStringLiteral("The audio device reported an I/O error");
    case QAudio::UnderrunError:
        return QStringLiteral("The platform audio sink underrun");
    case QAudio::FatalError:
        return QStringLiteral("The platform audio sink reported a fatal error");
    }
    return QStringLiteral("The audio device reported an unknown error");
}

class QtAudioSinkBackend final : public AudioSinkBackend
{
public:
    [[nodiscard]] std::vector<AudioOutputDevice> devices() override
    {
        const QAudioDevice preferred = QMediaDevices::defaultAudioOutput();
        std::vector<AudioOutputDevice> result;
        const auto outputs = QMediaDevices::audioOutputs();
        result.reserve(static_cast<std::size_t>(outputs.size()));
        for (const auto& output : outputs) {
            result.push_back({
                stableIdentifier(output),
                output.description().toStdString(),
                !preferred.isNull() && output.id() == preferred.id(),
            });
        }
        return result;
    }

    [[nodiscard]] AudioSinkOpenResult open(
        const std::string& identifier, std::uint32_t sampleRate) override
    {
        close();
        const auto outputs = QMediaDevices::audioOutputs();
        const auto selected = std::ranges::find_if(
            outputs, [&identifier](const QAudioDevice& device) {
                return stableIdentifier(device) == identifier;
            });
        if (selected == outputs.end()) {
            return {
                AudioSinkOpenError::NoDevice,
                "The selected audio output device is no longer available",
            };
        }

        QAudioFormat format;
        format.setSampleRate(static_cast<int>(sampleRate));
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        if (!selected->isFormatSupported(format)) {
            return {
                AudioSinkOpenError::UnsupportedFormat,
                "The selected audio device does not support 48 kHz stereo 16-bit PCM",
            };
        }

        m_sink = std::make_unique<QAudioSink>(*selected, format);
        m_sink->setBufferSize(
            static_cast<int>(sampleRate / 25U * audioFrameBytes));
        return {AudioSinkOpenError::None, "Audio output opened"};
    }

    [[nodiscard]] AudioSinkOpenResult startPlayback() override
    {
        if (!m_sink) {
            return {
                AudioSinkOpenError::OpenFailed,
                "Audio output was not opened before playback started",
            };
        }
        m_device = m_sink->start();
        if (!m_device || m_sink->error() != QAudio::NoError) {
            const QString error = m_sink
                                      ? audioErrorText(m_sink->error())
                                      : QStringLiteral("The audio device returned no output stream");
            close();
            return {AudioSinkOpenError::OpenFailed, error.toStdString()};
        }
        return {AudioSinkOpenError::None, "Audio output opened"};
    }

    void close() noexcept override
    {
        if (m_sink) {
            m_sink->stop();
        }
        m_device = nullptr;
        m_sink.reset();
        m_inPlatformUnderrun = false;
    }

    [[nodiscard]] std::size_t writableFrames() const noexcept override
    {
        if (!m_sink || !m_device) {
            return 0;
        }
        return static_cast<std::size_t>(
            std::max<qint64>(0, m_sink->bytesFree())) /
               audioFrameBytes;
    }

    [[nodiscard]] std::size_t bufferedFrames() const noexcept override
    {
        if (!m_sink || !m_device) {
            return 0;
        }
        const qint64 capacityBytes = std::max<qint64>(
            0, static_cast<qint64>(m_sink->bufferSize()));
        const qint64 bufferedBytes = std::max<qint64>(
            0, capacityBytes - std::max<qint64>(0, m_sink->bytesFree()));
        return static_cast<std::size_t>(bufferedBytes) / audioFrameBytes;
    }

    [[nodiscard]] std::size_t write(
        std::span<const std::byte> bytes) override
    {
        if (!m_device || bytes.empty()) {
            return 0;
        }
        const qint64 bytesWritten = m_device->write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<qint64>(bytes.size()));
        return bytesWritten > 0
                   ? static_cast<std::size_t>(bytesWritten)
                   : 0;
    }

    [[nodiscard]] std::uint64_t takePlatformUnderrunEvents() override
    {
        if (!m_sink) {
            return 0;
        }
        if (m_sink->error() == QAudio::UnderrunError) {
            if (!m_inPlatformUnderrun) {
                m_inPlatformUnderrun = true;
                return 1;
            }
            return 0;
        }
        m_inPlatformUnderrun = false;
        return 0;
    }

    [[nodiscard]] std::optional<std::string> takeRuntimeError() override
    {
        if (!m_sink || m_sink->error() == QAudio::NoError ||
            m_sink->error() == QAudio::UnderrunError) {
            return std::nullopt;
        }
        return audioErrorText(m_sink->error()).toStdString();
    }

private:
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice* m_device = nullptr;
    bool m_inPlatformUnderrun = false;
};

}  // namespace

std::unique_ptr<AudioOutputService> makeQtAudioOutputService()
{
    return std::make_unique<AudioOutputService>(
        std::make_unique<QtAudioSinkBackend>());
}

}  // namespace sdr::platform
