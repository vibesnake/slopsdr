// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "ApplicationLogModel.hpp"

#include <QSignalSpy>
#include <QtTest>

#include <thread>
#include <vector>

class ApplicationLogModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void preservesStructuredEntriesAndFilters();
    void boundsAndClearsHistory();
    void sanitizesExternalTextAndCoalescesProducerWork();
};

void ApplicationLogModelTest::preservesStructuredEntriesAndFilters()
{
    sdr::app::ApplicationLogModel model;
    model.post(
        sdr::app::ApplicationLogModel::Debug,
        QStringLiteral("DSP"),
        QStringLiteral("debug"));
    model.post(
        sdr::app::ApplicationLogModel::Warning,
        QStringLiteral("Audio"),
        QStringLiteral("underrun\ncontinued"));
    model.post(
        sdr::app::ApplicationLogModel::Error,
        QStringLiteral("SDR"),
        QStringLiteral("device failed"));
    QCoreApplication::processEvents();
    QCOMPARE(model.entryCount(), 3);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(
        model.data(model.index(0), sdr::app::ApplicationLogModel::SequenceRole)
            .toULongLong(),
        quint64{1});
    QCOMPARE(
        model.data(model.index(1), sdr::app::ApplicationLogModel::SourceRole)
            .toString(),
        QStringLiteral("Audio"));
    QVERIFY(
        model.data(model.index(1), sdr::app::ApplicationLogModel::TimestampRole)
            .toDateTime()
            .isValid());
    QVERIFY(model.formattedText().contains(
        QStringLiteral("[Warning] [Audio] underrun")));

    model.setMinimumSeverity(sdr::app::ApplicationLogModel::Warning);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(!model.copyAllText().contains(QStringLiteral("debug")));
    QVERIFY(model.copyAllText().contains(QStringLiteral("device failed")));
}

void ApplicationLogModelTest::boundsAndClearsHistory()
{
    sdr::app::ApplicationLogModel model;
    QSignalSpy modelResets(&model, &QAbstractItemModel::modelReset);
    for (qsizetype index = 0;
         index < sdr::app::ApplicationLogModel::maximumEntries + 50;
         ++index) {
        model.post(
            sdr::app::ApplicationLogModel::Info,
            QStringLiteral("Test"),
            QString::number(index));
        if ((index % 100) == 0) {
            QCoreApplication::processEvents();
        }
    }
    QCoreApplication::processEvents();
    QCOMPARE(
        model.entryCount(),
        static_cast<int>(sdr::app::ApplicationLogModel::maximumEntries));
    QVERIFY(
        model.data(model.index(0), sdr::app::ApplicationLogModel::SequenceRole)
            .toULongLong() > 1);
    QCOMPARE(
        model.data(model.index(0), sdr::app::ApplicationLogModel::MessageRole)
            .toString(),
        QStringLiteral("50"));
    QCOMPARE(
        model.data(
                 model.index(model.rowCount() - 1),
                 sdr::app::ApplicationLogModel::MessageRole)
            .toString(),
        QStringLiteral("5049"));
    QCOMPARE(modelResets.count(), 0);
    model.clear();
    QCOMPARE(modelResets.count(), 1);
    QCOMPARE(model.entryCount(), 0);
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.formattedText().isEmpty());
}

void ApplicationLogModelTest::sanitizesExternalTextAndCoalescesProducerWork()
{
    sdr::app::ApplicationLogModel model;
    QSignalSpy updates(&model, &sdr::app::ApplicationLogModel::formattedTextChanged);
    std::vector<std::thread> producers;
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&model, producer] {
            for (int index = 0; index < 500; ++index) {
                model.post(
                    sdr::app::ApplicationLogModel::Info,
                    QStringLiteral("Worker"),
                    QStringLiteral("\x1b[31mproducer %1 %2\x1b[0m")
                        .arg(producer)
                        .arg(index));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    QCoreApplication::processEvents();
    QVERIFY(model.entryCount() > 0);
    QVERIFY(model.entryCount() <= 2'000);
    QVERIFY(!model.formattedText().contains(QLatin1Char('\x1b')));
    QVERIFY(updates.count() < model.entryCount());
    quint64 previousSequence = 0;
    for (int row = 0; row < model.rowCount(); ++row) {
        const quint64 sequence =
            model.data(
                     model.index(row),
                     sdr::app::ApplicationLogModel::SequenceRole)
                .toULongLong();
        QVERIFY(sequence > previousSequence);
        previousSequence = sequence;
    }
}

QTEST_GUILESS_MAIN(ApplicationLogModelTest)

#include "ApplicationLogModelTest.moc"
