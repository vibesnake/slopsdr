// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include <QQmlComponent>
#include <QQmlEngine>
#include <QCoreApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

#include <cmath>
#include <memory>

class GainSliderBindingTest final : public QObject
{
    Q_OBJECT

private slots:
    void reappliesRequestedGainWhenCapabilitiesArrive();
    void keepsFilterTextVisibleAndPlacesGainStatusBelowSlider();
    void maximumHoldToggleHasDistinctCheckedAndUncheckedStates();
    void sidebarButtonsKeepNavigationEntriesExclusiveAndExposeScanShell();
    void toolbarUsesEmbeddedSlopSdrLogo();
    void bookmarkNameDialogSelectsSuggestionAndRejectsWhitespace();
    void bookmarkDragStartsOnlyFromVisibleHandleAndEscapeCancels();
};

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

void GainSliderBindingTest::keepsFilterTextVisibleAndPlacesGainStatusBelowSlider()
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
                property var filterWidthOptions: ["8.33 kHz", "12.50 kHz", "25 kHz", "Custom…"]
                property string formattedFilterWidth: "12.50 kHz"

                GridLayout {
                    id: controls
                    objectName: "receiverControls"
                    width: parent.width
                    columns: root.dense ? 1 : 3
                    columnSpacing: 8

                    ColumnLayout {
                        TextMetrics {
                            id: filterWidthTextMetrics
                            font: filterWidthSelector.font
                            text: filterWidthSelector.widestAvailableLabel
                        }
                        ComboBox {
                            id: filterWidthSelector
                            objectName: "filterWidthSelector"
                            readonly property string widestAvailableLabel: {
                                var widest = root.formattedFilterWidth
                                for (var index = 0; index < root.filterWidthOptions.length; ++index) {
                                    var label = root.filterWidthOptions[index]
                                    if (label === "Custom…")
                                        label = "Custom · " + root.formattedFilterWidth
                                    if (label.length > widest.length)
                                        widest = label
                                }
                                return widest
                            }
                            readonly property real minimumDisplayWidth: Math.ceil(
                                filterWidthTextMetrics.advanceWidth
                                + leftPadding + rightPadding + implicitIndicatorWidth + 8)
                            Layout.fillWidth: true
                            Layout.minimumWidth: minimumDisplayWidth
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
                            text: "Requested: 20.0 dB · Effective: 19.7 dB"
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
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
    QVERIFY(filter->width() >= filter->property("minimumDisplayWidth").toDouble());
    QCOMPARE(filter->property("displayText").toString(), QStringLiteral("12.50 kHz"));
    QVERIFY(status->mapToItem(root, QPointF()).y() >=
             slider->mapToItem(root, QPointF(0.0, slider->height())).y());
    QVERIFY(status->width() >= slider->width());

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
                                enabled: false
                                model: ["Current passband"]
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
    QCOMPARE(scanType->property("enabled").toBool(), false);
    QCOMPARE(scanType->property("currentText").toString(),
             QStringLiteral("Current passband"));
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
             "scanStateDisplay",
             "scanStatusMessage",
             "scanValidationError",
         }) {
        QVERIFY2(object->findChild<QObject*>(objectName), objectName);
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
                        text: "v0.10.3-alpha.2"
                    }

                    Text {
                        objectName: "releaseDateLabel"
                        text: "2026-07-29"
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

QTEST_MAIN(GainSliderBindingTest)

#include "GainSliderBindingTest.moc"
