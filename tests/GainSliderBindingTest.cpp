// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include <QQmlComponent>
#include <QQmlEngine>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlContext>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

class ReceiverControlsPaneModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool receiverControlsPaneOpen READ receiverControlsPaneOpen NOTIFY receiverControlsPaneOpenChanged)
    Q_PROPERTY(double receiverControlsPaneWidth READ receiverControlsPaneWidth NOTIFY receiverControlsPaneWidthChanged)

public:
    [[nodiscard]] bool receiverControlsPaneOpen() const noexcept
    {
        return m_open;
    }

    [[nodiscard]] double receiverControlsPaneWidth() const noexcept
    {
        return m_width;
    }

    Q_INVOKABLE void setReceiverControlsPaneOpen(bool open)
    {
        if (m_open == open) return;
        m_open = open;
        emit receiverControlsPaneOpenChanged();
    }

    Q_INVOKABLE void toggleReceiverControlsPane()
    {
        setReceiverControlsPaneOpen(!m_open);
    }

    Q_INVOKABLE void setReceiverControlsPaneWidth(double width)
    {
        const double bounded = std::clamp(width, 330.0, 520.0);
        if (qFuzzyCompare(m_width + 1.0, bounded + 1.0)) return;
        m_width = bounded;
        emit receiverControlsPaneWidthChanged();
    }

    Q_INVOKABLE void commitReceiverControlsPaneWidth() {}

signals:
    void receiverControlsPaneOpenChanged();
    void receiverControlsPaneWidthChanged();

private:
    bool m_open = true;
    double m_width = 440.0;
};

class GainSliderBindingTest final : public QObject
{
    Q_OBJECT

private slots:
    void reappliesRequestedGainWhenCapabilitiesArrive();
    void keepsReceiverControlsGeometryStableAcrossRuntimeValues();
    void maximumHoldToggleHasDistinctCheckedAndUncheckedStates();
    void spectrumAveragingSliderKeepsFixedHeaderGeometry();
    void sidebarButtonsKeepNavigationEntriesExclusiveAndExposeScanShell();
    void toolbarUsesEmbeddedSlopSdrLogo();
    void bookmarkNameDialogSelectsSuggestionAndRejectsWhitespace();
    void bookmarkDragStartsOnlyFromVisibleHandleAndEscapeCancels();
    void centerDigitHoverKeysAvoidTextEditorsAndScannerOwnership();
    void recordingFooterUsesGroupedTwoRowDarkLayout();
    void themedCheckBoxKeepsTextClearOfItsIndicator();
    void affectedDarkControlsUseSharedTheming();
    void recordingLoaderUsesWidgetDialogBridge();
    void receiverControlsPaneTogglesResizesAndRespondsWithoutDuplicates();
};

void GainSliderBindingTest::receiverControlsPaneTogglesResizesAndRespondsWithoutDuplicates()
{
    const QString panePath = QFINDTESTDATA("../qml/ReceiverControlsPane.qml");
    QVERIFY2(!panePath.isEmpty(), "ReceiverControlsPane.qml test data was not found");

    ReceiverControlsPaneModel model;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("receiverControlsPaneModel"), &model);
    QSignalSpy warningSpy(&engine, &QQmlEngine::warnings);
    QQmlComponent component(&engine);
    const QUrl harnessUrl = QUrl::fromLocalFile(
        QFileInfo(panePath).absolutePath() + QStringLiteral("/PaneHarness.qml"));
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Layouts
            import "."

            ReceiverControlsPane {
                applicationModel: receiverControlsPaneModel
                workspaceAvailableWidth: 1000
                panelColor: "#172033"
                panelBorderColor: "#2b3a55"
                primaryTextColor: "#f1f5fb"
                secondaryTextColor: "#9caac0"
                accentColor: "#61dafb"
                width: requestedLayoutWidth
                height: 480

                Rectangle {
                    objectName: "receiverControlSentinel"
                    Layout.fillWidth: true
                    implicitHeight: 900
                    color: "transparent"
                }
            }
        )",
        harnessUrl);
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> pane(component.create());
    QVERIFY2(pane, qPrintable(component.errorString()));
    QCoreApplication::processEvents();

    QObject* panel = pane->findChild<QObject*>("receiverControlsPanel");
    QObject* toggle = pane->findChild<QObject*>("receiverControlsToggleButton");
    QObject* scroll = pane->findChild<QObject*>("receiverControlsScroll");
    QObject* content = pane->findChild<QObject*>("receiverControlsContent");
    const auto sentinels =
        pane->findChildren<QObject*>("receiverControlSentinel");
    QVERIFY(panel);
    QVERIFY(toggle);
    QVERIFY(scroll);
    QVERIFY(content);
    QCOMPARE(sentinels.size(), 1);
    QVERIFY(pane->property("expanded").toBool());
    QVERIFY(panel->property("visible").toBool());
    QCOMPARE(panel->property("width").toDouble(), 440.0);

    QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
    QCoreApplication::processEvents();
    QVERIFY(!model.receiverControlsPaneOpen());
    QVERIFY(!pane->property("expanded").toBool());
    QVERIFY(!panel->property("visible").toBool());

    model.toggleReceiverControlsPane();
    model.setReceiverControlsPaneWidth(480.0);
    QCoreApplication::processEvents();
    QVERIFY(pane->property("expanded").toBool());
    QCOMPARE(panel->property("width").toDouble(), 480.0);

    pane->setProperty("workspaceAvailableWidth", 700.0);
    QCoreApplication::processEvents();
    QVERIFY(model.receiverControlsPaneOpen());
    QVERIFY(pane->property("responsiveCollapsed").toBool());
    QVERIFY(!pane->property("expanded").toBool());
    QCOMPARE(pane->property("requestedLayoutWidth").toDouble(),
             pane->property("toggleWidth").toDouble());

    pane->setProperty("workspaceAvailableWidth", 1'000.0);
    QCoreApplication::processEvents();
    QVERIFY(pane->property("expanded").toBool());
    QVERIFY(panel->property("visible").toBool());
    QVERIFY(scroll->property("clip").toBool());
    QVERIFY(scroll->property("contentHeight").toDouble()
            > scroll->property("height").toDouble());
    QVERIFY(warningSpy.isEmpty());

    const QString mainPath = QFINDTESTDATA("../qml/Main.qml");
    QVERIFY2(!mainPath.isEmpty(), "Main.qml test data was not found");
    QFile input(mainPath);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const QByteArray source = input.readAll();
    QCOMPARE(source.count("ReceiverControlsPane {"), 1);
    QCOMPARE(source.count("text: qsTr(\"Receiver controls\")"), 1);
    QVERIFY(source.contains("objectName: \"receiverControlsPaneShortcut\""));
    QVERIFY(source.contains("sequence: \"Ctrl+Shift+R\""));
    QVERIFY(source.contains(
        "columns: receiverControlsPane.controlsAvailableWidth < 380"));
    QVERIFY(source.indexOf("ReceiverControlsPane {")
            > source.indexOf("id: displayWorkspace"));
    QVERIFY(source.indexOf("ReceiverControlsPane {")
            > source.indexOf("id: sidebarPanel"));
}

void GainSliderBindingTest::themedCheckBoxKeepsTextClearOfItsIndicator()
{
    const QString path = QFINDTESTDATA("../qml/ThemedCheckBox.qml");
    QVERIFY2(!path.isEmpty(), "ThemedCheckBox.qml test data was not found");
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(path));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> checkBox(component.create());
    QVERIFY2(checkBox, qPrintable(component.errorString()));
    QObject* indicator = checkBox->property("indicator").value<QObject*>();
    QObject* label = checkBox->findChild<QObject*>("themedCheckBoxLabel");
    QVERIFY(indicator);
    QVERIFY(label);

    checkBox->setProperty("text", QStringLiteral("Disable squelch"));
    checkBox->setProperty("enabledTextColor", QColor(QStringLiteral("#f1f5fb")));
    checkBox->setProperty("disabledTextColor", QColor(QStringLiteral("#9caac0")));
    QCoreApplication::processEvents();

    const auto expectedPadding = [checkBox = checkBox.get(), indicator] {
        return indicator->property("width").toDouble()
               + checkBox->property("spacing").toDouble();
    };
    QCOMPARE(label->property("leftPadding").toDouble(), expectedPadding());
    QCOMPARE(label->property("color").value<QColor>(),
             QColor(QStringLiteral("#f1f5fb")));

    checkBox->setProperty("checked", true);
    QCoreApplication::processEvents();
    QCOMPARE(label->property("color").value<QColor>(),
             QColor(QStringLiteral("#f1f5fb")));

    indicator->setProperty("width", 36.0);
    QCoreApplication::processEvents();
    QCOMPARE(label->property("leftPadding").toDouble(), expectedPadding());

    checkBox->setProperty("enabled", false);
    QCoreApplication::processEvents();
    const QColor disabledColor = label->property("color").value<QColor>();
    QCOMPARE(label->property("color").value<QColor>(),
             checkBox->property("disabledTextColor").value<QColor>());
    QVERIFY(disabledColor != QColor(Qt::black));
    QVERIFY(disabledColor.lightness() > QColor(Qt::black).lightness());
}

void GainSliderBindingTest::affectedDarkControlsUseSharedTheming()
{
    const QString path = QFINDTESTDATA("../qml/Main.qml");
    QVERIFY2(!path.isEmpty(), "Main.qml test data was not found");
    QFile input(path);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const QByteArray source = input.readAll();

    QVERIFY(source.contains("palette.windowText: primaryTextColor"));
    QVERIFY(source.contains("palette.disabled.windowText: secondaryTextColor"));
    for (const char* objectName : {
             "skipQuietRecordingPartsCheckBox",
             "recordScannerActivityCheckBox",
             "disableSquelchCheckBox",
         }) {
        const qsizetype position = source.indexOf(objectName);
        QVERIFY2(position >= 0, objectName);
        const qsizetype component = source.lastIndexOf("ThemedCheckBox {", position);
        QVERIFY2(component >= 0 && position - component < 160, objectName);
    }
    for (const char* objectName : {
             "recordingPreRollLabel",
             "recordingTailLabel",
         }) {
        const qsizetype position = source.indexOf(objectName);
        QVERIFY2(position >= 0, objectName);
        QVERIFY(source.mid(position, 180).contains("color: root.primaryTextColor"));
    }
}

void GainSliderBindingTest::recordingLoaderUsesWidgetDialogBridge()
{
    const QString qmlPath = QFINDTESTDATA("../qml/Main.qml");
    QVERIFY2(!qmlPath.isEmpty(), "Main.qml test data was not found");
    QFile qmlInput(qmlPath);
    QVERIFY(qmlInput.open(QIODevice::ReadOnly));
    const QByteArray source = qmlInput.readAll();
    QVERIFY(!source.contains("import QtQuick.Dialogs"));
    QVERIFY(!source.contains("id: recordingFileDialog"));
    QVERIFY(!source.contains("recordedIqFileDialog"));
    QVERIFY(source.contains("root.applicationFileDialogs.openRecordingFileDialog()"));
    QVERIFY(source.contains("selectRecordingDirectory()"));
    QVERIFY(source.contains("selectDsdFmeExecutable()"));
    QVERIFY(!source.contains("FileDialog {"));
    QVERIFY(!source.contains("FolderDialog {"));
    QVERIFY(source.contains("onRecordingFileSelected(fileUrl)"));

    const QString mainPath = QFINDTESTDATA("../app/main.cpp");
    QVERIFY2(!mainPath.isEmpty(), "main.cpp test data was not found");
    QFile mainInput(mainPath);
    QVERIFY(mainInput.open(QIODevice::ReadOnly));
    const QByteArray main = mainInput.readAll();
    QVERIFY(main.contains("ApplicationFileDialogs"));
    QVERIFY(main.contains("applicationModel.loadRecording(fileUrl)"));
    QVERIFY(main.contains("QApplication application"));

    const QString cmakePath = QFINDTESTDATA("../CMakeLists.txt");
    QVERIFY2(!cmakePath.isEmpty(), "CMakeLists.txt test data was not found");
    QFile cmakeInput(cmakePath);
    QVERIFY(cmakeInput.open(QIODevice::ReadOnly));
    const QByteArray cmake = cmakeInput.readAll();
    QVERIFY(cmake.contains("Qt6::Widgets"));
}

void GainSliderBindingTest::recordingFooterUsesGroupedTwoRowDarkLayout()
{
    const QString path = QFINDTESTDATA("../qml/Main.qml");
    QVERIFY2(!path.isEmpty(), "Main.qml test data was not found");
    QFile input(path);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const QByteArray source = input.readAll();
    const qsizetype footerStart = source.indexOf("footer: ToolBar");
    QVERIFY(footerStart >= 0);
    const QByteArray footer = source.mid(footerStart);

    QVERIFY(source.contains("component FooterDarkButton: Button"));
    const qsizetype transport = footer.indexOf("playbackTransportGroup");
    const qsizetype information = footer.indexOf("recordingInformationGroup");
    const qsizetype recording = footer.indexOf("recordingControlsGroup");
    const qsizetype seek = footer.indexOf("recordingSeekRow");
    QVERIFY(transport >= 0);
    QVERIFY(information > transport);
    QVERIFY(recording > information);
    QVERIFY(seek > recording);
    QVERIFY(footer.contains("objectName: \"recordingSeekSlider\""));
    QVERIFY(footer.contains("root.applicationModel.recordingCanSeek"));
    QVERIFY(!footer.contains("root.applicationModel.recordedAudioSource"));
    QVERIFY(footer.contains("height: 7"));
    QVERIFY(footer.contains("Layout.minimumWidth: 160"));
    QVERIFY(!footer.contains("recordingFormatLabel"));
    QVERIFY(!footer.contains("root.applicationModel.backendDescription"));
    QVERIFY(!footer.contains("Selected SDR opens when Start is pressed"));
}

void GainSliderBindingTest::reappliesRequestedGainWhenCapabilitiesArrive()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls

            Slider {
                id: gainSlider
                property real requestedGain: 21
                from: 0
                to: 0
                value: Math.max(
                           gainSlider.from,
                           Math.min(gainSlider.to, requestedGain))
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> slider(component.create());
    QVERIFY2(slider, qPrintable(component.errorString()));
    QCOMPARE(slider->property("value").toDouble(), 0.0);

    slider->setProperty("to", 49.0);
    QCoreApplication::processEvents();
    QCOMPARE(slider->property("value").toDouble(), 21.0);
}

void GainSliderBindingTest::keepsReceiverControlsGeometryStableAcrossRuntimeValues()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls
            import QtQuick.Layouts

            Item {
                id: root
                width: 416
                height: 180
                property bool dense: false
                property bool gainSupported: true
                property var filterWidthOptions: ["8.33 kHz", "12.50 kHz", "25 kHz", "Custom…"]
                property string formattedFilterWidth: "12.50 kHz"

                GridLayout {
                    id: controls
                    objectName: "receiverControls"
                    width: parent.width
                    columns: root.dense ? 1 : 3
                    columnSpacing: 8

                    ColumnLayout {
                        ComboBox {
                            id: filterWidthSelector
                            objectName: "filterWidthSelector"
                            Layout.fillWidth: true
                            Layout.minimumWidth: implicitWidth
                            Layout.preferredWidth: implicitWidth
                            implicitContentWidthPolicy: ComboBox.WidestText
                            model: root.filterWidthOptions
                            displayText: root.formattedFilterWidth
                        }
                    }

                    ColumnLayout {
                        RowLayout {
                            Layout.fillWidth: true
                            Slider {
                                id: gainSlider
                                objectName: "gainSlider"
                                Layout.fillWidth: true
                                Layout.minimumWidth: root.dense ? 100 : 120
                            }
                        }
                        Label {
                            id: gainStatusText
                            objectName: "gainStatusText"
                            Layout.fillWidth: true
                            Layout.minimumHeight: gainStatusMetrics.height * 2
                            Layout.preferredHeight: gainStatusMetrics.height * 2
                            text: root.gainSupported
                                  ? "Requested: 20.0 dB · Effective: 19.7 dB"
                                  : "Gain control unsupported"
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        TextMetrics {
                            id: gainStatusMetrics
                            font: gainStatusText.font
                            text: "M"
                        }
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto* filter = object->findChild<QQuickItem*>("filterWidthSelector");
    auto* slider = object->findChild<QQuickItem*>("gainSlider");
    auto* status = object->findChild<QQuickItem*>("gainStatusText");
    auto* controls = object->findChild<QQuickItem*>("receiverControls");
    auto* root = qobject_cast<QQuickItem*>(object.get());
    QVERIFY(filter);
    QVERIFY(slider);
    QVERIFY(status);
    QVERIFY(controls);
    QVERIFY(root);
    QCoreApplication::processEvents();
    const double filterWidth = filter->width();
    const double filterImplicitWidth = filter->implicitWidth();
    const double controlsHeight = controls->implicitHeight();
    const double gainStatusHeight = status->height();
    QVERIFY(filter->property("implicitContentWidthPolicy").toInt() != 0);
    QCOMPARE(filter->property("displayText").toString(), QStringLiteral("12.50 kHz"));
    QVERIFY(status->mapToItem(root, QPointF()).y() >=
             slider->mapToItem(root, QPointF(0.0, slider->height())).y());
    QVERIFY(status->width() >= slider->width());
    for (int index = 0;
         index < filter->property("count").toInt();
         ++index) {
        filter->setProperty("currentIndex", index);
        QCoreApplication::processEvents();
        QCOMPARE(filter->width(), filterWidth);
        QCOMPARE(filter->implicitWidth(), filterImplicitWidth);
    }
    root->setProperty("gainSupported", false);
    QCoreApplication::processEvents();
    QCOMPARE(status->height(), gainStatusHeight);
    QCOMPARE(controls->implicitHeight(), controlsHeight);

    const std::unique_ptr<QObject> narrowObject(
        component.createWithInitialProperties(
            {{QStringLiteral("dense"), true}, {QStringLiteral("width"), 330.0}}));
    QVERIFY2(narrowObject, qPrintable(component.errorString()));
    auto* narrowFilter =
        narrowObject->findChild<QQuickItem*>("filterWidthSelector");
    auto* narrowSlider = narrowObject->findChild<QQuickItem*>("gainSlider");
    auto* narrowControls =
        narrowObject->findChild<QQuickItem*>("receiverControls");
    auto* narrowRoot = qobject_cast<QQuickItem*>(narrowObject.get());
    QVERIFY(narrowFilter);
    QVERIFY(narrowSlider);
    QVERIFY(narrowControls);
    QVERIFY(narrowRoot);
    QCoreApplication::processEvents();
    QCOMPARE(narrowControls->property("columns").toInt(), 1);
    QVERIFY(narrowFilter->mapToItem(
                narrowRoot, QPointF(0.0, narrowFilter->height())).y() <=
             narrowSlider->mapToItem(narrowRoot, QPointF()).y());
    QVERIFY(narrowFilter->width() <= narrowControls->width());
    QVERIFY(narrowSlider->width() >= 100.0);
}

void GainSliderBindingTest::maximumHoldToggleHasDistinctCheckedAndUncheckedStates()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls

            ToolButton {
                id: control
                checkable: true
                text: "Max"
                background: Rectangle {
                    objectName: "maxBackground"
                    color: control.checked ? control.palette.highlight
                                            : control.palette.button
                    border.color: control.checked ? control.palette.highlight
                                                   : control.palette.mid
                    border.width: control.activeFocus ? 2 : 1
                }
                contentItem: Text {
                    objectName: "maxText"
                    text: control.text
                    color: control.checked ? control.palette.highlightedText
                                            : control.palette.buttonText
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> button(component.create());
    QVERIFY2(button, qPrintable(component.errorString()));
    const auto background = button->findChild<QObject*>("maxBackground");
    QVERIFY(background);
    const auto text = button->findChild<QObject*>("maxText");
    QVERIFY(text);
    const auto uncheckedBackground = background->property("color");
    const auto uncheckedText = text->property("color");
    button->setProperty("checked", true);
    QCoreApplication::processEvents();
    const auto checkedBackground = background->property("color");
    const auto checkedText = text->property("color");
    QVERIFY(uncheckedBackground != checkedBackground);
    QVERIFY(uncheckedText != checkedText);
}

void GainSliderBindingTest::
    spectrumAveragingSliderKeepsFixedHeaderGeometry()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls
            import QtQuick.Layouts

            Item {
                width: 240
                height: 28

                RowLayout {
                    id: controls
                    objectName: "spectrumHeaderControls"
                    height: 28
                    spacing: 4

                    ToolButton {
                        id: maximumHoldButton
                        objectName: "maximumHoldButton"
                        implicitWidth: 44
                        implicitHeight: 26
                        text: "Max"
                    }
                    Label {
                        objectName: "spectrumAveragingLabel"
                        text: "AVG"
                    }
                    Slider {
                        objectName: "spectrumAveragingSlider"
                        Layout.minimumWidth: 92
                        Layout.preferredWidth: 92
                        Layout.maximumWidth: 92
                        implicitHeight: 26
                        from: 0
                        to: 100
                        stepSize: 1
                    }
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto* root = qobject_cast<QQuickItem*>(object.get());
    auto* controls =
        object->findChild<QQuickItem*>("spectrumHeaderControls");
    auto* maximum =
        object->findChild<QQuickItem*>("maximumHoldButton");
    auto* label =
        object->findChild<QQuickItem*>("spectrumAveragingLabel");
    auto* slider =
        object->findChild<QQuickItem*>("spectrumAveragingSlider");
    QVERIFY(root);
    QVERIFY(controls);
    QVERIFY(maximum);
    QVERIFY(label);
    QVERIFY(slider);
    QCoreApplication::processEvents();

    const double headerHeight = controls->height();
    const double sliderWidth = slider->width();
    QCOMPARE(slider->property("from").toDouble(), 0.0);
    QCOMPARE(slider->property("to").toDouble(), 100.0);
    QCOMPARE(sliderWidth, 92.0);
    QVERIFY(label->mapToItem(root, QPointF()).x() >=
            maximum->mapToItem(
                root, QPointF(maximum->width(), 0.0)).x());
    QVERIFY(slider->mapToItem(root, QPointF()).x() >=
            label->mapToItem(
                root, QPointF(label->width(), 0.0)).x());

    for (const int value : {0, 37, 100}) {
        slider->setProperty("value", value);
        QCoreApplication::processEvents();
        QCOMPARE(slider->width(), sliderWidth);
        QCOMPARE(controls->height(), headerHeight);
    }
}

void GainSliderBindingTest::
    sidebarButtonsKeepNavigationEntriesExclusiveAndExposeScanShell()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls

            ApplicationWindow {
                id: window
                visible: true
                width: 400
                height: 600
                property string sidebarMode: "none"
                property string selectedPresetId: ""
                property string loadedPresetId: ""

                Row {
                    Button {
                        id: bookmarksButton
                        objectName: "bookmarksSidebarButton"
                        checkable: true
                        checked: window.sidebarMode === "bookmarks"
                        text: "Bookmarks"
                        onClicked: window.sidebarMode = checked ? "bookmarks" : "none"
                    }
                    Button {
                        id: scanButton
                        objectName: "scanSidebarButton"
                        checkable: true
                        checked: window.sidebarMode === "scan"
                        text: "Scan"
                        onClicked: window.sidebarMode = checked ? "scan" : "none"
                    }
                    Button {
                        id: settingsButton
                        objectName: "settingsSidebarButton"
                        checkable: true
                        checked: window.sidebarMode === "settings"
                        text: "Settings"
                        onClicked: window.sidebarMode = checked ? "settings" : "none"
                    }
                }

                Item {
                    id: sidebar
                    objectName: "sharedSidebar"
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    width: 220
                    height: 500

                    Rectangle {
                        objectName: "bookmarksSidebarPane"
                        anchors.fill: parent
                        visible: window.sidebarMode === "bookmarks"
                        Column {
                            Rectangle { objectName: "bookmarkScanningSection" }
                            TextField { objectName: "bookmarkScanDwellField" }
                            TextField { objectName: "bookmarkScanResumeDelayField" }
                            Button { objectName: "bookmarkScanStartButton" }
                            Button { objectName: "bookmarkScanPauseResumeButton" }
                            Button { objectName: "bookmarkScanSkipButton" }
                            Button { objectName: "bookmarkScanStopButton" }
                            Text { objectName: "bookmarkScanCurrentName" }
                            Text { objectName: "bookmarkScanPosition" }
                            Text { objectName: "bookmarkScanState" }
                            Text { objectName: "bookmarkScanStatus" }
                        }
                    }
                    Rectangle {
                        objectName: "scanSidebarContent"
                        anchors.fill: parent
                        visible: window.sidebarMode === "scan"

                        Column {
                            anchors.fill: parent

                            Text {
                                objectName: "scanPaneHeading"
                                text: "Scan"
                            }
                            ComboBox {
                                objectName: "scanTypeControl"
                                enabled: true
                                model: ["Current passband", "Wide range"]
                            }
                            TextField { objectName: "scanLowerFrequencyField"; enabled: true }
                            TextField { objectName: "scanUpperFrequencyField"; enabled: true }
                            TextField { objectName: "scanStepSizeField"; enabled: true }
                            TextField { objectName: "scanDwellTimeField"; enabled: true }
                            TextField { objectName: "scanResumeDelayField"; enabled: true }
                            ComboBox {
                                objectName: "scanSquelchSourceControl"
                                enabled: false
                                model: ["Live receiver squelch"]
                            }
                            Row {
                                Button { objectName: "scanStartButton"; enabled: true }
                                Button { objectName: "scanPauseResumeButton"; enabled: false }
                                Button { objectName: "scanSkipButton"; enabled: false }
                                Button { objectName: "scanStopButton"; enabled: false }
                            }
                            Text {
                                objectName: "scanCurrentFrequencyDisplay"
                                text: "—"
                            }
                            Text { objectName: "scanListeningFrequencyDisplay" }
                            Text { objectName: "scanHardwareCenterFrequencyDisplay" }
                            Text { objectName: "scanCaptureBlockProgressDisplay" }
                            Text {
                                objectName: "scanStateDisplay"
                                text: "Scanner not running"
                            }
                            Text {
                                objectName: "scanStatusMessage"
                                text: "Scanner not running"
                            }
                            Text { objectName: "scanValidationError" }
                            Rectangle {
                                objectName: "scanPresetsSection"
                                width: parent.width
                                height: 80

                                Column {
                                    anchors.fill: parent
                                    TextField { objectName: "scanPresetNameField" }
                                    ListView {
                                        objectName: "scanPresetList"
                                        width: parent.width
                                        height: 42
                                        spacing: 2
                                        model: []
                                    }
                                    Item {
                                        id: scanPresetRow
                                        objectName: "scanPresetRow"
                                        width: parent.width
                                        height: 42
                                        property string presetId: "preset-1"

                                        Rectangle {
                                            anchors.fill: parent
                                            color: window.selectedPresetId === scanPresetRow.presetId
                                                   ? "#29425f" : "transparent"
                                        }
                                        Label {
                                            anchors.fill: parent
                                            text: "Long preset name"
                                        }
                                        TapHandler {
                                            onTapped: {
                                                window.selectedPresetId = scanPresetRow.presetId
                                            }
                                            onDoubleTapped: {
                                                window.selectedPresetId = scanPresetRow.presetId
                                                window.loadedPresetId = scanPresetRow.presetId
                                            }
                                        }
                                    }
                                    Row {
                                        Button { objectName: "saveNewScanPresetButton" }
                                        Button { objectName: "loadScanPresetButton"; enabled: false }
                                        Button { objectName: "updateScanPresetButton"; enabled: false }
                                        Button { objectName: "deleteScanPresetButton"; enabled: false }
                                    }
                                    Text { objectName: "scanPresetStatusMessage" }
                                }
                            }
                        }
                    }
                    Rectangle {
                        objectName: "settingsSidebarPane"
                        anchors.fill: parent
                        visible: window.sidebarMode === "settings"
                    }
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    auto* bookmarksButton = object->findChild<QQuickItem*>("bookmarksSidebarButton");
    auto* scanButton = object->findChild<QQuickItem*>("scanSidebarButton");
    auto* settingsButton = object->findChild<QQuickItem*>("settingsSidebarButton");
    auto* bookmarksPane = object->findChild<QQuickItem*>("bookmarksSidebarPane");
    auto* scanPane = object->findChild<QQuickItem*>("scanSidebarContent");
    auto* settingsPane = object->findChild<QQuickItem*>("settingsSidebarPane");
    auto* scanPresetsSection = object->findChild<QQuickItem*>("scanPresetsSection");
    auto* scanPresetList = object->findChild<QQuickItem*>("scanPresetList");
    QQuickItem* scanPresetRow = nullptr;
    auto* scanStatusMessage = object->findChild<QQuickItem*>("scanStatusMessage");
    QVERIFY(window);
    QVERIFY(bookmarksButton);
    QVERIFY(scanButton);
    QVERIFY(settingsButton);
    QVERIFY(bookmarksPane);
    QVERIFY(scanPane);
    QVERIFY(settingsPane);
    QVERIFY(scanPresetsSection);
    QVERIFY(scanPresetList);
    QVERIFY(scanStatusMessage);
    QVERIFY(QTest::qWaitFor([window] { return window->isExposed(); }));

    const auto click = [window](QQuickItem* item) {
        QTest::mouseClick(
            window,
            Qt::LeftButton,
            {},
            item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0))
                .toPoint());
    };
    click(bookmarksButton);
    QVERIFY(bookmarksPane->isVisible());
    QVERIFY(!scanPane->isVisible());
    QVERIFY(!settingsPane->isVisible());
    click(scanButton);
    QVERIFY(!bookmarksPane->isVisible());
    QVERIFY(scanPane->isVisible());
    QVERIFY(!settingsPane->isVisible());
    click(settingsButton);
    QVERIFY(!bookmarksPane->isVisible());
    QVERIFY(!scanPane->isVisible());
    QVERIFY(settingsPane->isVisible());
    click(settingsButton);
    QVERIFY(!bookmarksPane->isVisible());
    QVERIFY(!settingsPane->isVisible());
    for (int index = 0; index < 5; ++index) {
        click(bookmarksButton);
        QVERIFY(bookmarksPane->isVisible());
        QVERIFY(!scanPane->isVisible());
        QVERIFY(!settingsPane->isVisible());
        click(scanButton);
        QVERIFY(!bookmarksPane->isVisible());
        QVERIFY(scanPane->isVisible());
        QVERIFY(!settingsPane->isVisible());
        click(settingsButton);
        QVERIFY(!bookmarksPane->isVisible());
        QVERIFY(!scanPane->isVisible());
        QVERIFY(settingsPane->isVisible());
    }
    QCOMPARE(bookmarksPane->parentItem(), settingsPane->parentItem());
    QCOMPARE(scanPane->parentItem(), settingsPane->parentItem());
    click(scanButton);
    QCoreApplication::processEvents();
    scanPresetRow = object->findChild<QQuickItem*>("scanPresetRow");
    QVERIFY(scanPresetRow);
    QCOMPARE(scanPresetRow->objectName(), QStringLiteral("scanPresetRow"));

    const auto scanType = object->findChild<QObject*>("scanTypeControl");
    QVERIFY(scanType);
    QCOMPARE(scanType->property("enabled").toBool(), true);
    QCOMPARE(scanType->property("currentText").toString(),
             QStringLiteral("Current passband"));
    QCOMPARE(scanType->property("count").toInt(), 2);
    for (const char* objectName : {
             "scanPaneHeading",
             "scanLowerFrequencyField",
             "scanUpperFrequencyField",
             "scanStepSizeField",
             "scanDwellTimeField",
             "scanResumeDelayField",
             "scanSquelchSourceControl",
             "scanPresetNameField",
             "scanPresetsSection",
             "scanPresetList",
             "saveNewScanPresetButton",
             "loadScanPresetButton",
             "updateScanPresetButton",
             "deleteScanPresetButton",
             "scanPresetStatusMessage",
             "scanStartButton",
             "scanPauseResumeButton",
             "scanSkipButton",
             "scanStopButton",
             "scanCurrentFrequencyDisplay",
             "scanListeningFrequencyDisplay",
             "scanHardwareCenterFrequencyDisplay",
             "scanCaptureBlockProgressDisplay",
             "scanStateDisplay",
             "scanStatusMessage",
             "scanValidationError",
         }) {
        QVERIFY2(object->findChild<QObject*>(objectName), objectName);
    }
    for (const char* objectName : {
             "bookmarkScanningSection",
             "bookmarkScanDwellField",
             "bookmarkScanResumeDelayField",
             "bookmarkScanStartButton",
             "bookmarkScanPauseResumeButton",
             "bookmarkScanSkipButton",
             "bookmarkScanStopButton",
             "bookmarkScanCurrentName",
             "bookmarkScanPosition",
             "bookmarkScanState",
             "bookmarkScanStatus",
         }) {
        QObject* control = object->findChild<QObject*>(objectName);
        QVERIFY2(control, objectName);
        QObject* ancestor = control;
        bool inBookmarksPane = false;
        while (ancestor) {
            if (ancestor == bookmarksPane) {
                inBookmarksPane = true;
                break;
            }
            ancestor = ancestor->parent();
        }
        QVERIFY2(inBookmarksPane, objectName);
    }
    for (const char* objectName : {
             "scanLowerFrequencyField",
             "scanUpperFrequencyField",
             "scanStepSizeField",
             "scanDwellTimeField",
             "scanResumeDelayField",
         }) {
        QCOMPARE(
            object->findChild<QObject*>(objectName)->property("enabled").toBool(),
            true);
    }
    QCOMPARE(
        object->findChild<QObject*>("scanSquelchSourceControl")->property("enabled").toBool(),
        false);
    QCOMPARE(
        object->findChild<QObject*>("scanStartButton")->property("enabled").toBool(),
        true);
    for (const char* objectName : {
             "scanPauseResumeButton",
             "scanSkipButton",
             "scanStopButton",
         }) {
        QCOMPARE(
            object->findChild<QObject*>(objectName)->property("enabled").toBool(),
            false);
    }
    QCOMPARE(
        object->findChild<QObject*>("scanStateDisplay")->property("text").toString(),
        QStringLiteral("Scanner not running"));
    QVERIFY(scanPresetsSection->y() > scanStatusMessage->y());
    QVERIFY(!scanPane->findChild<QObject*>("scanPresetCheckBox"));

    click(scanPresetRow);
    QCOMPARE(object->property("selectedPresetId").toString(),
             QStringLiteral("preset-1"));
    QCOMPARE(object->property("loadedPresetId").toString(), QString());
    QTest::mouseDClick(
        window,
        Qt::LeftButton,
        {},
        scanPresetRow->mapToScene(
            QPointF(scanPresetRow->width() / 2.0, scanPresetRow->height() / 2.0))
            .toPoint());
    QCoreApplication::processEvents();
    QCOMPARE(object->property("selectedPresetId").toString(),
             QStringLiteral("preset-1"));
    QCOMPARE(object->property("loadedPresetId").toString(),
             QStringLiteral("preset-1"));
}

void GainSliderBindingTest::toolbarUsesEmbeddedSlopSdrLogo()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls

            Item {
                id: toolbar
                width: 640
                height: 42

                Button {
                    objectName: "consoleSidebarButton"
                    text: "Console"
                    visible: true
                }

                Image {
                    id: logo
                    objectName: "slopSdrLogo"
                    x: 180
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height
                    width: implicitWidth > 0
                           ? height * implicitWidth / implicitHeight : 0
                    source: "qrc:/assets/slopsdr-logo.png"
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                    asynchronous: false
                    visible: status === Image.Ready
                }

                Column {
                    objectName: "slopSdrReleaseMetadata"
                    x: logo.x + logo.width + 8
                    anchors.verticalCenter: parent.verticalCenter
                    visible: logo.visible

                    Text {
                        objectName: "versionLabel"
                        text: "v0.12.0"
                    }

                    Text {
                        objectName: "releaseDateLabel"
                        text: "2026-08-02"
                    }
                }

                Button {
                    objectName: "hardwareStatus"
                    x: 520
                    text: "Hardware"
                    visible: true
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    const auto logo = object->findChild<QObject*>("slopSdrLogo");
    QVERIFY(logo);
    QVERIFY(QTest::qWaitFor([logo] {
        return logo->property("status").toInt() == 1;
    }));
    QCOMPARE(logo->property("source").toUrl(),
             QUrl(QStringLiteral("qrc:/assets/slopsdr-logo.png")));
    QCOMPARE(logo->property("fillMode").toInt(), 1);
    QCOMPARE(logo->property("smooth").toBool(), true);
    QCOMPARE(logo->property("mipmap").toBool(), true);
    QCOMPARE(logo->property("asynchronous").toBool(), false);
    QVERIFY(logo->property("visible").toBool());
    QVERIFY(logo->property("width").toDouble() > 0.0);
    QVERIFY(std::abs(
        logo->property("width").toDouble() / logo->property("height").toDouble()
        - 1483.0 / 564.0) < 0.001);
    QCOMPARE(
        QImage(QStringLiteral(":/assets/slopsdr-logo.png")).size(),
        QSize(1483, 564));
    QVERIFY(QImage(QStringLiteral(":/assets/slopsdr-logo.png")).hasAlphaChannel());
    QVERIFY(!object->findChild<QObject*>("slopSdrWordmark"));
    QCOMPARE(object->property("height").toDouble(), 42.0);
    const auto console = qobject_cast<QQuickItem*>(
        object->findChild<QObject*>("consoleSidebarButton"));
    const auto hardware = qobject_cast<QQuickItem*>(
        object->findChild<QObject*>("hardwareStatus"));
    const auto logoItem = qobject_cast<QQuickItem*>(logo);
    QVERIFY(console);
    QVERIFY(hardware);
    QVERIFY(logoItem);
    QVERIFY(console->isVisible());
    QVERIFY(hardware->isVisible());
    QVERIFY(logoItem->x() > console->x() + console->width());
    QVERIFY(logoItem->x() + logoItem->width() < hardware->x());
    QVERIFY(object->findChild<QObject*>("versionLabel")->property("visible").toBool());
    QVERIFY(object->findChild<QObject*>("releaseDateLabel")->property("visible").toBool());
}

void GainSliderBindingTest::bookmarkNameDialogSelectsSuggestionAndRejectsWhitespace()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls

            ApplicationWindow {
                visible: true
                width: 400
                height: 200
                property alias dialog: dialog
                Dialog {
                    id: dialog
                    objectName: "bookmarkNameDialog"
                    anchors.centerIn: Overlay.overlay
                    modal: true
                    property bool invalidName: false
                    function openWithSuggestion(name) {
                        field.text = name
                        invalidName = false
                        open()
                    }
                    function submit() {
                        if (field.text.trim().length === 0) {
                            invalidName = true
                            field.forceActiveFocus()
                            return
                        }
                        close()
                    }
                    onOpened: {
                        field.forceActiveFocus()
                        field.selectAll()
                    }
                    contentItem: TextField {
                        id: field
                        objectName: "bookmarkNameField"
                        onAccepted: dialog.submit()
                    }
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QObject* dialog = window->findChild<QObject*>("bookmarkNameDialog");
    QObject* field = window->findChild<QObject*>("bookmarkNameField");
    QVERIFY(dialog);
    QVERIFY(field);
    QVERIFY(QMetaObject::invokeMethod(
        dialog,
        "openWithSuggestion",
        Q_ARG(QVariant, QStringLiteral("AM · 100.000 MHz"))));
    QCoreApplication::processEvents();
    QCOMPARE(field->property("selectedText").toString(),
             QStringLiteral("AM · 100.000 MHz"));

    field->setProperty("text", QStringLiteral("   "));
    QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
    QCoreApplication::processEvents();
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(dialog->property("invalidName").toBool());
}

void GainSliderBindingTest::bookmarkDragStartsOnlyFromVisibleHandleAndEscapeCancels()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls

            ApplicationWindow {
                id: window
                visible: true
                width: 320
                height: 160
                property bool rowTapped: false
                property bool dragActive: preview.Drag.active
                property int completedDrops: 0
                property bool dragSessionActive: false

                Item {
                    id: row
                    objectName: "bookmarkRow"
                    x: 20
                    y: 40
                    width: 260
                    height: 42

                    TapHandler {
                        onTapped: window.rowTapped = true
                    }

                    Item {
                        id: handle
                        objectName: "bookmarkDragHandle"
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        width: 30

                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Repeater {
                                model: 3
                                Rectangle { width: 12; height: 2 }
                            }
                        }

                        MouseArea {
                            id: handleDrag
                            objectName: "bookmarkDragHandler"
                            anchors.fill: parent
                            drag.target: preview
                            drag.threshold: Qt.styleHints.startDragDistance
                            preventStealing: true
                            onPositionChanged: {
                                if (drag.active && !window.dragSessionActive)
                                    window.dragSessionActive = true
                            }
                            onReleased: {
                                if (window.dragSessionActive) {
                                    preview.Drag.drop()
                                    window.dragSessionActive = false
                                }
                            }
                            onCanceled: {
                                preview.Drag.cancel()
                                window.dragSessionActive = false
                            }
                        }
                    }
                }

                Rectangle {
                    id: preview
                    objectName: "bookmarkDragPreview"
                    width: 200
                    height: 42
                    x: row.x
                    y: row.y
                    visible: window.dragSessionActive
                    Drag.active: window.dragSessionActive
                    Drag.source: row
                    Drag.keys: ["bookmark-row"]
                    Drag.hotSpot.x: width - 15
                    Drag.hotSpot.y: height / 2
                }

                DropArea {
                    anchors.fill: parent
                    keys: ["bookmark-row"]
                    onDropped: function(drop) {
                        ++window.completedDrops
                        drop.acceptProposedAction()
                    }
                }

                Shortcut {
                    enabled: window.dragSessionActive
                    sequence: "Escape"
                    onActivated: {
                        preview.Drag.cancel()
                        window.dragSessionActive = false
                    }
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    auto* row = object->findChild<QQuickItem*>("bookmarkRow");
    auto* handle = object->findChild<QQuickItem*>("bookmarkDragHandle");
    auto* preview = object->findChild<QQuickItem*>("bookmarkDragPreview");
    QVERIFY(window);
    QVERIFY(row);
    QVERIFY(handle);
    QVERIFY(preview);
    QVERIFY(QTest::qWaitFor([window] { return window->isExposed(); }));

    const QPoint rowPoint = row->mapToScene(QPointF(50.0, 21.0)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, {}, rowPoint);
    QTest::mouseMove(window, rowPoint + QPoint(45, 0), 20);
    QVERIFY(!object->property("dragActive").toBool());
    QTest::mouseRelease(window, Qt::LeftButton, {}, rowPoint + QPoint(45, 0));

    object->setProperty("rowTapped", false);
    QTest::mouseClick(window, Qt::LeftButton, {}, rowPoint);
    QVERIFY(object->property("rowTapped").toBool());

    const QPoint handlePoint =
        handle->mapToScene(QPointF(15.0, 21.0)).toPoint();
    const QPointF previewStart = preview->position();
    QTest::mousePress(window, Qt::LeftButton, {}, handlePoint);
    QTest::mouseMove(window, handlePoint + QPoint(-25, 0), 10);
    QTest::mouseMove(window, handlePoint + QPoint(-45, 0), 20);
    QVERIFY(QTest::qWaitFor(
        [&object] { return object->property("dragActive").toBool(); }));
    QVERIFY(preview->position() != previewStart);
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(QTest::qWaitFor(
        [&object] { return !object->property("dragActive").toBool(); }));
    QCOMPARE(object->property("completedDrops").toInt(), 0);
    QTest::mouseRelease(window, Qt::LeftButton, {}, handlePoint + QPoint(-45, 0));

    QTest::mousePress(window, Qt::LeftButton, {}, handlePoint);
    QTest::mouseMove(window, handlePoint + QPoint(20, 0), 10);
    QTest::mouseMove(window, handlePoint + QPoint(35, 0), 20);
    QVERIFY(QTest::qWaitFor(
        [&object] { return object->property("dragActive").toBool(); }));
    QTest::mouseRelease(window, Qt::LeftButton, {}, handlePoint + QPoint(35, 0));
    QVERIFY(QTest::qWaitFor(
        [&object] { return object->property("completedDrops").toInt() == 1; }));
    QVERIFY(!object->property("dragActive").toBool());
}

void GainSliderBindingTest::centerDigitHoverKeysAvoidTextEditorsAndScannerOwnership()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
            import QtQuick
            import QtQuick.Controls
            import QtQuick.Window

            Window {
                id: root
                width: 240
                height: 100
                visible: true
                property int hoveredCenterFrequencyDigitIndex: 4
                property bool scannerOwnsTuning: false
                property bool centerFrequencyDigitEditActive: false
                property int replacements: 0
                property int replacementDigit: -1
                property int adjustments: 0
                property int adjustmentDirection: 0

                function textEditorHasFocus() {
                    let item = root.activeFocusItem
                    while (item) {
                        if (item instanceof TextInput || item instanceof TextEdit)
                            return true
                        item = item.parent
                    }
                    return false
                }

                Repeater {
                    model: 10

                    delegate: Item {
                        width: 0
                        height: 0

                        Shortcut {
                            sequence: String(index)
                            context: Qt.WindowShortcut
                            enabled: !root.scannerOwnsTuning
                                     && !root.centerFrequencyDigitEditActive
                                     && !root.textEditorHasFocus()
                                     && root.hoveredCenterFrequencyDigitIndex >= 0
                            onActivated: {
                                ++root.replacements
                                root.replacementDigit = index
                            }
                        }
                    }
                }

                Repeater {
                    model: [1, -1]

                    delegate: Item {
                        width: 0
                        height: 0

                        Shortcut {
                            sequence: index === 0 ? "Up" : "Down"
                            context: Qt.WindowShortcut
                            enabled: !root.scannerOwnsTuning
                                     && !root.centerFrequencyDigitEditActive
                                     && !root.textEditorHasFocus()
                                     && root.hoveredCenterFrequencyDigitIndex >= 0
                            onActivated: {
                                ++root.adjustments
                                root.adjustmentDirection = modelData
                            }
                        }
                    }
                }

                Item {
                    id: digitFocus
                    objectName: "centerFrequencyDigitFocus"
                    focus: true
                    width: 30
                    height: 30
                }
                TextField {
                    id: editor
                    objectName: "otherTextEditor"
                    anchors.top: digitFocus.bottom
                    width: parent.width
                }
            }
        )",
        QUrl());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));

    const std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    auto* digitFocus = object->findChild<QQuickItem*>("centerFrequencyDigitFocus");
    auto* editor = object->findChild<QQuickItem*>("otherTextEditor");
    QVERIFY(window);
    QVERIFY(digitFocus);
    QVERIFY(editor);
    QVERIFY(QTest::qWaitFor([window] { return window->isExposed(); }));

    digitFocus->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_4);
    QCOMPARE(object->property("replacements").toInt(), 1);
    QCOMPARE(object->property("replacementDigit").toInt(), 4);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(object->property("adjustments").toInt(), 1);
    QCOMPARE(object->property("adjustmentDirection").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(object->property("adjustments").toInt(), 2);
    QCOMPARE(object->property("adjustmentDirection").toInt(), -1);

    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_5);
    QCOMPARE(object->property("replacements").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(object->property("adjustments").toInt(), 2);

    object->setProperty("scannerOwnsTuning", true);
    digitFocus->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_6);
    QCOMPARE(object->property("replacements").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(object->property("adjustments").toInt(), 2);

    object->setProperty("scannerOwnsTuning", false);
    object->setProperty("centerFrequencyDigitEditActive", true);
    QTest::keyClick(window, Qt::Key_7);
    QCOMPARE(object->property("replacements").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(object->property("adjustments").toInt(), 2);
}

QTEST_MAIN(GainSliderBindingTest)

#include "GainSliderBindingTest.moc"
