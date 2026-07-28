// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QMutex>
#include <QString>

#include <atomic>
#include <deque>

namespace sdr::app {

class ApplicationLogModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString formattedText READ formattedText NOTIFY formattedTextChanged)
    Q_PROPERTY(int minimumSeverity READ minimumSeverity WRITE setMinimumSeverity NOTIFY minimumSeverityChanged)
    Q_PROPERTY(int entryCount READ entryCount NOTIFY entryCountChanged)

public:
    enum Severity {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
    };
    Q_ENUM(Severity)

    enum Role {
        SequenceRole = Qt::UserRole + 1,
        TimestampRole,
        SeverityRole,
        SeverityNameRole,
        SourceRole,
        MessageRole,
        FormattedRole,
    };

    static constexpr qsizetype maximumEntries = 5'000;

    explicit ApplicationLogModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QString formattedText() const;
    [[nodiscard]] int minimumSeverity() const noexcept;
    [[nodiscard]] int entryCount() const noexcept;

    void post(Severity severity, QString source, QString message) noexcept;

public slots:
    void clear();
    void setMinimumSeverity(int severity);
    [[nodiscard]] QString copyAllText() const;

signals:
    void formattedTextChanged();
    void minimumSeverityChanged();
    void entryCountChanged();

private slots:
    void drainPending();

private:
    struct Entry {
        quint64 sequence = 0;
        QDateTime timestamp;
        Severity severity = Info;
        QString source;
        QString message;
    };

    [[nodiscard]] static QString sanitize(QString text);
    [[nodiscard]] static QString severityName(Severity severity);
    [[nodiscard]] static QString format(const Entry& entry);
    void rebuildVisibleEntries();

    std::atomic<quint64> m_nextSequence{1};
    std::atomic_bool m_drainQueued{false};
    QMutex m_pendingMutex;
    std::deque<Entry> m_pending;
    QVector<Entry> m_entries;
    QVector<int> m_visibleEntries;
    QVector<QString> m_visibleFormattedEntries;
    QString m_formattedText;
    int m_minimumSeverity = Debug;
};

}  // namespace sdr::app
