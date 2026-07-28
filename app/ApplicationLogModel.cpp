// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationLogModel.hpp"

#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>

#include <algorithm>

namespace sdr::app {
namespace {

constexpr qsizetype maximumPendingEntries = ApplicationLogModel::maximumEntries;
constexpr qsizetype maximumEntriesPerDrain = 128;

}  // namespace

ApplicationLogModel::ApplicationLogModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ApplicationLogModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visibleEntries.size());
}

QVariant ApplicationLogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= m_visibleEntries.size()) {
        return {};
    }
    const Entry& entry = m_entries.at(m_visibleEntries.at(index.row()));
    switch (role) {
    case SequenceRole:
        return QVariant::fromValue(entry.sequence);
    case TimestampRole:
        return entry.timestamp;
    case SeverityRole:
        return static_cast<int>(entry.severity);
    case SeverityNameRole:
        return severityName(entry.severity);
    case SourceRole:
        return entry.source;
    case MessageRole:
        return entry.message;
    case FormattedRole:
        return format(entry);
    default:
        return {};
    }
}

QHash<int, QByteArray> ApplicationLogModel::roleNames() const
{
    return {
        {SequenceRole, "sequence"},
        {TimestampRole, "timestamp"},
        {SeverityRole, "severity"},
        {SeverityNameRole, "severityName"},
        {SourceRole, "source"},
        {MessageRole, "message"},
        {FormattedRole, "formatted"},
    };
}

QString ApplicationLogModel::formattedText() const
{
    return m_formattedText;
}

int ApplicationLogModel::minimumSeverity() const noexcept
{
    return m_minimumSeverity;
}

int ApplicationLogModel::entryCount() const noexcept
{
    return static_cast<int>(m_entries.size());
}

void ApplicationLogModel::post(
    Severity severity, QString source, QString message) noexcept
{
    Entry entry{
        .sequence = 0,
        .timestamp = QDateTime::currentDateTime(),
        .severity = severity,
        .source = sanitize(std::move(source)),
        .message = sanitize(std::move(message)),
    };
    if (entry.source.isEmpty()) {
        entry.source = QStringLiteral("Application");
    }
    entry.source.replace(QLatin1Char('\n'), QLatin1Char(' '));
    entry.source.replace(QLatin1Char('\t'), QLatin1Char(' '));
    if (entry.message.isEmpty()) {
        return;
    }

    if (!m_pendingMutex.tryLock()) {
        return;
    }
    entry.sequence = m_nextSequence.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<qsizetype>(m_pending.size()) >= maximumPendingEntries) {
        m_pending.pop_front();
    }
    m_pending.push_back(std::move(entry));
    m_pendingMutex.unlock();

    if (!m_drainQueued.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
            this, &ApplicationLogModel::drainPending, Qt::QueuedConnection);
    }
}

void ApplicationLogModel::clear()
{
    {
        QMutexLocker lock(&m_pendingMutex);
        m_pending.clear();
    }
    beginResetModel();
    const bool hadEntries = !m_entries.isEmpty();
    m_entries.clear();
    m_visibleEntries.clear();
    m_visibleFormattedEntries.clear();
    m_formattedText.clear();
    endResetModel();
    if (hadEntries) {
        emit entryCountChanged();
    }
    emit formattedTextChanged();
}

void ApplicationLogModel::setMinimumSeverity(int severity)
{
    const int bounded = std::clamp(
        severity, static_cast<int>(Debug), static_cast<int>(Error));
    if (m_minimumSeverity == bounded) {
        return;
    }
    m_minimumSeverity = bounded;
    rebuildVisibleEntries();
    emit minimumSeverityChanged();
}

QString ApplicationLogModel::copyAllText() const
{
    return m_formattedText;
}

void ApplicationLogModel::drainPending()
{
    std::deque<Entry> pending;
    {
        QMutexLocker lock(&m_pendingMutex);
        const qsizetype entryCount = std::min(
            static_cast<qsizetype>(m_pending.size()), maximumEntriesPerDrain);
        for (qsizetype index = 0; index < entryCount; ++index) {
            pending.push_back(std::move(m_pending.front()));
            m_pending.pop_front();
        }
    }
    m_drainQueued.store(false, std::memory_order_release);

    if (!pending.empty()) {
        const qsizetype oldCount = m_entries.size();
        m_entries.reserve(maximumEntries);
        const qsizetype removeCount = std::max<qsizetype>(
            oldCount + static_cast<qsizetype>(pending.size()) - maximumEntries,
            0);
        bool formattedTextWasChanged = false;
        if (removeCount > 0) {
            qsizetype visibleRemoveCount = 0;
            while (visibleRemoveCount < m_visibleEntries.size() &&
                   m_visibleEntries.at(visibleRemoveCount) < removeCount) {
                ++visibleRemoveCount;
            }
            if (visibleRemoveCount > 0) {
                beginRemoveRows(
                    QModelIndex(), 0, static_cast<int>(visibleRemoveCount - 1));
                if (visibleRemoveCount == m_visibleFormattedEntries.size()) {
                    m_formattedText.clear();
                } else {
                    qsizetype removeCharacters = 0;
                    for (qsizetype index = 0;
                         index < visibleRemoveCount;
                         ++index) {
                        removeCharacters +=
                            m_visibleFormattedEntries.at(index).size() + 1;
                    }
                    m_formattedText.remove(0, removeCharacters);
                }
                m_visibleEntries.remove(0, visibleRemoveCount);
                m_visibleFormattedEntries.remove(0, visibleRemoveCount);
                formattedTextWasChanged = true;
            }
            m_entries.remove(0, removeCount);
            for (int& visibleEntry : m_visibleEntries) {
                visibleEntry -= static_cast<int>(removeCount);
            }
            if (visibleRemoveCount > 0) {
                endRemoveRows();
            }
        }

        const qsizetype firstNewEntry = m_entries.size();
        const qsizetype visibleAddCount = static_cast<qsizetype>(
            std::ranges::count_if(pending, [this](const Entry& entry) {
                return static_cast<int>(entry.severity) >= m_minimumSeverity;
            }));
        const qsizetype firstNewVisibleEntry = m_visibleEntries.size();
        if (visibleAddCount > 0) {
            beginInsertRows(
                QModelIndex(),
                static_cast<int>(firstNewVisibleEntry),
                static_cast<int>(firstNewVisibleEntry + visibleAddCount - 1));
        }
        for (Entry& entry : pending) {
            m_entries.push_back(std::move(entry));
        }
        for (qsizetype index = firstNewEntry;
             index < m_entries.size();
             ++index) {
            const Entry& entry = m_entries.at(index);
            if (static_cast<int>(entry.severity) < m_minimumSeverity) {
                continue;
            }
            const QString formatted = format(entry);
            m_visibleEntries.push_back(static_cast<int>(index));
            m_visibleFormattedEntries.push_back(formatted);
            if (!m_formattedText.isEmpty()) {
                m_formattedText.append(QLatin1Char('\n'));
            }
            m_formattedText.append(formatted);
        }
        if (visibleAddCount > 0) {
            endInsertRows();
            formattedTextWasChanged = true;
        }
        if (m_entries.size() != oldCount) {
            emit entryCountChanged();
        }
        if (formattedTextWasChanged) {
            emit formattedTextChanged();
        }
    }

    bool morePending = false;
    {
        QMutexLocker lock(&m_pendingMutex);
        morePending = !m_pending.empty();
    }
    if (morePending &&
        !m_drainQueued.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
            this, &ApplicationLogModel::drainPending, Qt::QueuedConnection);
    }
}

QString ApplicationLogModel::sanitize(QString text)
{
    static const QRegularExpression terminalSequence(
        QStringLiteral(
            R"((?:\x1B\][^\x07\x1B]*(?:\x07|\x1B\\))|(?:\x1B\[[0-?]*[ -/]*[@-~])|(?:\x1B[@-_]))"));
    text.remove(terminalSequence);
    for (qsizetype index = 0; index < text.size(); ++index) {
        const ushort code = text.at(index).unicode();
        if ((code < 0x20U && code != '\n' && code != '\t') ||
            code == 0x7fU) {
            text[index] = QLatin1Char(' ');
        }
    }
    return text.trimmed();
}

QString ApplicationLogModel::severityName(Severity severity)
{
    switch (severity) {
    case Debug:
        return QStringLiteral("Debug");
    case Info:
        return QStringLiteral("Info");
    case Warning:
        return QStringLiteral("Warning");
    case Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Info");
}

QString ApplicationLogModel::format(const Entry& entry)
{
    const QString prefix = QStringLiteral("%1  [%2] [%3] ")
                               .arg(
                                   entry.timestamp.toString(
                                       QStringLiteral("HH:mm:ss.zzz")),
                                   severityName(entry.severity),
                                   entry.source);
    QString message = entry.message;
    message.replace(
        QLatin1Char('\n'),
        QStringLiteral("\n%1").arg(QString(prefix.size(), QLatin1Char(' '))));
    return prefix + message;
}

void ApplicationLogModel::rebuildVisibleEntries()
{
    beginResetModel();
    m_visibleEntries.clear();
    m_visibleFormattedEntries.clear();
    for (qsizetype index = 0; index < m_entries.size(); ++index) {
        const Entry& entry = m_entries.at(index);
        if (static_cast<int>(entry.severity) < m_minimumSeverity) {
            continue;
        }
        m_visibleEntries.push_back(static_cast<int>(index));
        m_visibleFormattedEntries.push_back(format(entry));
    }
    m_formattedText = QStringList(
                          m_visibleFormattedEntries.cbegin(),
                          m_visibleFormattedEntries.cend())
                          .join(QLatin1Char('\n'));
    endResetModel();
    emit formattedTextChanged();
}

}  // namespace sdr::app
