// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: root

    required property var applicationModel
    required property var applicationFileDialogs
    property string applicationVersion
    property string applicationReleaseDate

    readonly property bool denseLayout: width < 1000 || height < 760
    readonly property int controlHeight: denseLayout ? 32 : 40
    readonly property color backgroundColor: "#0c1220"
    readonly property color panelColor: "#172033"
    readonly property color panelBorderColor: "#2b3a55"
    readonly property color primaryTextColor: "#f1f5fb"
    readonly property color secondaryTextColor: "#9caac0"
    readonly property color centerColor: "#61dafb"
    readonly property color listeningColor: "#f6ad55"
    palette.windowText: primaryTextColor
    palette.disabled.windowText: secondaryTextColor
    property bool bookmarkDragActive: false
    property real bookmarkDragListY: -1
    property bool scanPresetDragActive: false
    property real scanPresetDragListY: -1
    property int hoveredCenterFrequencyDigitIndex: -1
    property int focusedCenterFrequencyDigitIndex: -1
    property url selectedRecordingUrl: ""
    readonly property string sidebarModeNone: "none"
    readonly property string sidebarModeBookmarks: "bookmarks"
    readonly property string sidebarModeScan: "scan"
    readonly property string sidebarModeSettings: "settings"

    component FooterDarkButton: Button {
        id: footerButton
        property bool stateActive: false
        implicitHeight: 30
        implicitWidth: Math.max(30, footerButtonText.implicitWidth + 18)
        leftPadding: 9
        rightPadding: 9
        activeFocusOnTab: true

        background: Rectangle {
            radius: 4
            color: !footerButton.enabled ? "#141d2d"
                   : footerButton.down ? "#314968"
                   : footerButton.checked || footerButton.stateActive ? "#274b63"
                   : footerButton.hovered ? "#25364f"
                   : "#1a2639"
            border.color: footerButton.activeFocus ? root.centerColor
                          : footerButton.checked || footerButton.stateActive ? "#4eb8d8"
                          : footerButton.hovered ? "#526882"
                          : "#34445d"
            border.width: footerButton.activeFocus ? 2 : 1
        }

        contentItem: Text {
            id: footerButtonText
            text: footerButton.text
            color: !footerButton.enabled ? "#647187"
                   : footerButton.checked || footerButton.stateActive ? "#dff7ff"
                   : root.primaryTextColor
            font: footerButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Dialog {
        id: recordedIqMetadataDialog
        title: qsTr("Recorded IQ metadata")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: ColumnLayout {
            spacing: 8
            Label { text: qsTr("A valid adjacent JSON sidecar is loaded automatically. Enter these only when it is missing or invalid."); wrapMode: Text.WordWrap }
            TextField { id: recordedCenterField; placeholderText: qsTr("Center frequency (Hz)"); inputMethodHints: Qt.ImhDigitsOnly }
            TextField { id: recordedRateField; placeholderText: qsTr("Sample rate (samples/s)"); inputMethodHints: Qt.ImhDigitsOnly }
            Label { text: qsTr("Format: cf32_le · little-endian") }
        }
        onAccepted: root.applicationModel.selectRecordedIqSource(
                        root.selectedRecordingUrl, Number(recordedCenterField.text), Number(recordedRateField.text))
    }
    readonly property string sidebarModeConsole: "console"

    width: 1180
    height: 720
    minimumWidth: applicationModel.sidebarMode !== sidebarModeNone ? 1040 : 760
    minimumHeight: 460
    visible: true
    title: qsTr("slopSDR")
    color: backgroundColor

    onClosing: Qt.quit()

    function textEditorHasFocus() {
        let item = root.activeFocusItem
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit)
                return true
            item = item.parent
        }
        return false
    }

    function centerFrequencyDigitHasFocus() {
        return root.focusedCenterFrequencyDigitIndex >= 0
    }

    function formatRecordingFrames(frames) {
        const rate = applicationModel.recordingSampleRate
        const seconds = rate > 0 ? Math.floor(frames / rate) : 0
        const minutes = Math.floor(seconds / 60)
        const secondsPart = String(seconds % 60).padStart(2, "0")
        if (seconds >= 3600) {
            return Math.floor(minutes / 60) + ":" +
                   String(minutes % 60).padStart(2, "0") + ":" + secondsPart
        }
        return minutes + ":" + secondsPart
    }

    function openRecordingFileDialog() {
        root.applicationFileDialogs.openRecordingFileDialog()
    }

    function clearCenterFrequencyDigitFocus() {
        centerDigit0.focus = false
        centerDigit1.focus = false
        centerDigit2.focus = false
        centerDigit3.focus = false
        centerDigit4.focus = false
        centerDigit5.focus = false
        centerDigit6.focus = false
        centerDigit7.focus = false
        centerDigit8.focus = false
        centerDigit9.focus = false
        root.focusedCenterFrequencyDigitIndex = -1
    }

    Connections {
        target: root.applicationModel
        function onCenterFrequencyDigitEditChanged() {
            if (!root.applicationModel.centerFrequencyDigitEditActive)
                root.clearCenterFrequencyDigitFocus()
        }
        function onRecordingPlaybackChanged() {
            if (root.applicationModel.recordedIqMetadataRequired)
                recordedIqMetadataDialog.open()
        }
    }

    Connections {
        target: root.applicationFileDialogs
        function onRecordingFileSelected(fileUrl) {
            root.selectedRecordingUrl = fileUrl
        }
    }

    Repeater {
        model: 10

        delegate: Item {
            width: 0
            height: 0

            Shortcut {
                sequence: String(index)
                context: Qt.WindowShortcut
                enabled: !root.applicationModel.scannerOwnsTuning
                         && !root.applicationModel.centerFrequencyDigitEditActive
                         && !root.textEditorHasFocus()
                         && root.hoveredCenterFrequencyDigitIndex >= 0
                onActivated: root.applicationModel.replaceHoveredCenterFrequencyDigit(
                                 root.hoveredCenterFrequencyDigitIndex, index)
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
                enabled: !root.applicationModel.scannerOwnsTuning
                         && !root.applicationModel.centerFrequencyDigitEditActive
                         && !root.textEditorHasFocus()
                         && !root.centerFrequencyDigitHasFocus()
                         && root.hoveredCenterFrequencyDigitIndex >= 0
                onActivated: root.applicationModel.adjustCenterFrequencyDigit(
                                 root.hoveredCenterFrequencyDigitIndex, modelData)
            }
        }
    }

    Shortcut {
        sequence: StandardKey.Quit
        onActivated: Qt.quit()
    }

    Shortcut {
        objectName: "receiverControlsPaneShortcut"
        sequence: "Ctrl+Shift+R"
        context: Qt.WindowShortcut
        onActivated: root.applicationModel.toggleReceiverControlsPane()
    }

    component FrequencyPane: Rectangle {
        id: pane

        required property var applicationModel
        required property string heading
        required property string detail
        property bool waterfallInteraction: false
        readonly property bool historyFitsMemoryBudget:
            displaySurface.historyConfigurationFitsMemoryBudget
        readonly property real retainedHistoryCapacitySeconds:
            displaySurface.retainedHistoryCapacitySeconds
        readonly property int storedHistoryBins:
            displaySurface.storedHistoryBins
        property alias waterfallAggregation: displaySurface.waterfallAggregation
        readonly property real devicePixel: 1.0 / Math.max(1.0, Screen.devicePixelRatio)

        function frequencyLabelX(frequency, labelWidth) {
            return Math.max(0, Math.min(
                                displaySurface.width - labelWidth,
                                displaySurface.xForFrequency(frequency)
                                - labelWidth / 2.0))
        }

        radius: 8
        color: "#111a2b"
        border.color: "#2b3a55"
        border.width: 1
        clip: true

        ToolButton {
            id: spectrumPauseButton
            objectName: "spectrumPauseButton"
            anchors.left: parent.left
            anchors.verticalCenter: waterfallTitle.verticalCenter
            anchors.leftMargin: 8
            implicitWidth: 28
            implicitHeight: 28
            visible: !pane.waterfallInteraction
            checkable: true
            checked: displaySurface.paused
            text: checked ? "▶" : "⏸"
            Accessible.name: checked
                             ? qsTr("Resume spectrum")
                             : qsTr("Pause spectrum")
            Accessible.description: qsTr(
                "Pause or resume the spectrum display")
            onToggled: displaySurface.paused = checked
            ToolTip.visible: hovered
            ToolTip.text: checked
                           ? qsTr("Resume spectrum")
                           : qsTr("Pause spectrum")

            background: Rectangle {
                radius: 3
                color: spectrumPauseButton.checked
                           ? root.centerColor
                           : (spectrumPauseButton.pressed
                              ? root.panelBorderColor : root.panelColor)
                border.color: spectrumPauseButton.checked
                                  ? root.centerColor : root.panelBorderColor
                border.width: spectrumPauseButton.activeFocus ? 2 : 1
            }

            contentItem: Text {
                text: spectrumPauseButton.text
                color: spectrumPauseButton.checked
                           ? root.backgroundColor : root.primaryTextColor
                font.pixelSize: 16
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        ToolButton {
            id: waterfallPauseButton
            objectName: "waterfallPauseButton"
            anchors.left: parent.left
            anchors.verticalCenter: waterfallTitle.verticalCenter
            anchors.leftMargin: 8
            implicitWidth: 28
            implicitHeight: 28
            visible: pane.waterfallInteraction
            checkable: true
            checked: displaySurface.paused
            text: checked ? "▶" : "⏸"
            Accessible.name: checked
                             ? qsTr("Resume waterfall")
                             : qsTr("Pause waterfall")
            Accessible.description: qsTr(
                "Pause or resume the waterfall display")
            onToggled: displaySurface.paused = checked
            ToolTip.visible: hovered
            ToolTip.text: checked
                           ? qsTr("Resume waterfall")
                           : qsTr("Pause waterfall")

            background: Rectangle {
                radius: 3
                color: waterfallPauseButton.checked
                           ? root.centerColor
                           : (waterfallPauseButton.pressed
                              ? root.panelBorderColor : root.panelColor)
                border.color: waterfallPauseButton.checked
                                  ? root.centerColor : root.panelBorderColor
                border.width: waterfallPauseButton.activeFocus ? 2 : 1
            }

            contentItem: Text {
                text: waterfallPauseButton.text
                color: waterfallPauseButton.checked
                           ? root.backgroundColor : root.primaryTextColor
                font.pixelSize: 16
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }

        Label {
            id: waterfallTitle
            anchors.left: pane.waterfallInteraction
                         ? waterfallPauseButton.right
                         : spectrumPauseButton.right
            anchors.top: parent.top
            anchors.leftMargin: 6
            anchors.topMargin: 12
            text: pane.heading
            color: "#f1f5fb"
            font.bold: true
            font.pixelSize: 16
        }

        RowLayout {
            id: spectrumHoldControls
            anchors.left: waterfallTitle.right
            anchors.verticalCenter: waterfallTitle.verticalCenter
            anchors.leftMargin: 8
            visible: !pane.waterfallInteraction
            spacing: 4

            ToolButton {
                id: maximumHoldButton
                objectName: "maximumHoldButton"
                implicitWidth: 44
                implicitHeight: 26
                text: qsTr("Max")
                checkable: true
                checked: displaySurface.maximumHoldEnabled
                Accessible.name: qsTr("Maximum spectrum hold")
                Accessible.description: qsTr("Show the maximum captured spectrum trace")
                onToggled: displaySurface.maximumHoldEnabled = checked

                background: Rectangle {
                    radius: 3
                    color: !maximumHoldButton.enabled
                               ? maximumHoldButton.palette.mid
                               : maximumHoldButton.pressed
                                   ? maximumHoldButton.palette.dark
                               : maximumHoldButton.checked
                                   ? maximumHoldButton.palette.highlight
                                   : maximumHoldButton.hovered
                                       ? maximumHoldButton.palette.alternateBase
                                       : maximumHoldButton.palette.button
                    border.color: maximumHoldButton.checked
                                      ? maximumHoldButton.palette.highlight
                                      : maximumHoldButton.palette.mid
                    border.width: maximumHoldButton.activeFocus ? 2 : 1
                }

                contentItem: Text {
                    text: maximumHoldButton.text
                    color: maximumHoldButton.checked
                               ? maximumHoldButton.palette.highlightedText
                               : maximumHoldButton.palette.buttonText
                    font: maximumHoldButton.font
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Label {
                id: spectrumAveragingLabel
                objectName: "spectrumAveragingLabel"
                text: qsTr("AVG")
                color: root.secondaryTextColor
                font.pixelSize: 11
                font.bold: true
                Accessible.name: qsTr("Spectrum averaging")
            }

            Slider {
                id: spectrumAveragingSlider
                objectName: "spectrumAveragingSlider"
                Layout.minimumWidth: 92
                Layout.preferredWidth: 92
                Layout.maximumWidth: 92
                implicitHeight: 26
                from: 0
                to: 100
                stepSize: 1
                snapMode: Slider.SnapAlways
                live: true
                value: displaySurface.spectrumAveragingStrength
                Accessible.name: qsTr("Spectrum averaging strength")
                Accessible.description: qsTr(
                    "Minimum disables averaging; maximum gives the strongest smoothing. This affects only the spectrum trace.")
                onMoved: displaySurface.spectrumAveragingStrength =
                             Math.round(value)
                ToolTip.visible: hovered || pressed
                ToolTip.text: value === 0
                              ? qsTr("AVG 0: off; spectrum trace only")
                              : qsTr("AVG %1: stronger values steady only the spectrum trace")
                                    .arg(Math.round(value))
            }
        }

        RowLayout {
            id: waterfallRangeControls
            anchors.left: waterfallTitle.right
            anchors.right: waterfallHeaderControls.left
            anchors.verticalCenter: waterfallTitle.verticalCenter
            anchors.leftMargin: 10
            anchors.rightMargin: 8
            height: 28
            visible: pane.waterfallInteraction
            spacing: 6

            Label {
                text: qsTr("WF dB")
                color: "#aab7ca"
                font.pixelSize: 11
                font.bold: true
            }

            RangeSlider {
                id: waterfallDbRange
                Layout.fillWidth: true
                Layout.minimumWidth: 56
                Layout.preferredWidth: 150
                implicitHeight: 28
                from: -140
                to: 0
                stepSize: 1
                first.value: displaySurface.waterfallMinimumDbfs
                second.value: displaySurface.waterfallMaximumDbfs
                activeFocusOnTab: true
                Accessible.name: qsTr("Waterfall dB range")
                Accessible.description: qsTr("Set the minimum and maximum waterfall color levels in dBFS")

                first.onMoved: displaySurface.waterfallMinimumDbfs = Math.round(first.value)
                second.onMoved: displaySurface.waterfallMaximumDbfs = Math.round(second.value)

                background: Rectangle {
                    x: waterfallDbRange.leftPadding
                    y: waterfallDbRange.topPadding + (waterfallDbRange.availableHeight - height) / 2
                    width: waterfallDbRange.availableWidth
                    height: 4
                    radius: 2
                    color: "#33435f"

                    Rectangle {
                        x: waterfallDbRange.first.visualPosition * parent.width
                        width: (waterfallDbRange.second.visualPosition
                                - waterfallDbRange.first.visualPosition) * parent.width
                        height: parent.height
                        radius: parent.radius
                        color: "#61dafb"
                    }
                }

                first.handle: Rectangle {
                    x: waterfallDbRange.leftPadding
                       + waterfallDbRange.first.visualPosition
                         * (waterfallDbRange.availableWidth - width)
                    y: waterfallDbRange.topPadding
                       + (waterfallDbRange.availableHeight - height) / 2
                    width: 12
                    height: 12
                    radius: width / 2
                    color: waterfallDbRange.first.pressed ? "#f1f5fb" : "#aab7ca"
                    border.color: "#61dafb"
                    Accessible.name: qsTr("Waterfall minimum dBFS")

                    HoverHandler { id: minimumHandleHover }
                    ToolTip.visible: minimumHandleHover.hovered
                                     || waterfallDbRange.first.pressed
                    ToolTip.text: qsTr("Waterfall minimum dBFS")
                }

                second.handle: Rectangle {
                    x: waterfallDbRange.leftPadding
                       + waterfallDbRange.second.visualPosition
                         * (waterfallDbRange.availableWidth - width)
                    y: waterfallDbRange.topPadding
                       + (waterfallDbRange.availableHeight - height) / 2
                    width: 12
                    height: 12
                    radius: width / 2
                    color: waterfallDbRange.second.pressed ? "#f1f5fb" : "#aab7ca"
                    border.color: "#61dafb"
                    Accessible.name: qsTr("Waterfall maximum dBFS")

                    HoverHandler { id: maximumHandleHover }
                    ToolTip.visible: maximumHandleHover.hovered
                                     || waterfallDbRange.second.pressed
                    ToolTip.text: qsTr("Waterfall maximum dBFS")
                }
            }

            Label {
                Layout.minimumWidth: 76
                text: "%1  —  %2".arg(Math.round(displaySurface.waterfallMinimumDbfs))
                                  .arg(Math.round(displaySurface.waterfallMaximumDbfs))
                color: "#d8e1f0"
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
            }
        }

        Label {
            anchors.left: spectrumHoldControls.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 8
            anchors.rightMargin: 12
            anchors.topMargin: 12
            visible: !pane.waterfallInteraction
            text: pane.detail
            color: "#8190a8"
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            font.pixelSize: 10
        }

        RowLayout {
            id: waterfallHeaderControls
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 10
            anchors.topMargin: 4
            visible: pane.waterfallInteraction
            spacing: 5

            Label {
                id: zoomIndicator
                Layout.minimumWidth: 95
                Layout.preferredWidth: 95
                Layout.maximumWidth: 95
                Layout.preferredHeight: 28
                text: qsTr("%1%").arg(pane.applicationModel.displayZoomPercentage)
                color: "#d8e1f0"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                Accessible.name: qsTr("Spectrum and waterfall zoom")

                HoverHandler { id: zoomIndicatorHover }
                ToolTip.visible: zoomIndicatorHover.hovered
                ToolTip.text: qsTr("Spectrum and waterfall zoom")
            }

            Label {
                visible: pane.width >= 650
                text: qsTr("Spectrum step (Hz)")
                color: "#8190a8"
                font.pixelSize: 10
            }

            SpinBox {
                implicitWidth: 105
                implicitHeight: 28
                from: 1
                to: 1000000000
                value: Number(pane.applicationModel.tuningWheelStep)
                editable: true
                Accessible.description: qsTr("Normal spectrum wheel center-frequency step")
                onValueModified: pane.applicationModel.setTuningWheelStep(value)
            }
        }

        SpectrumWaterfallView {
            id: displaySurface

            anchors.left: pane.left
            anchors.right: pane.right
            anchors.top: pane.top
            anchors.bottom: frequencyScale.top
            anchors.topMargin: 34
            applicationModel: pane.applicationModel
            waterfall: pane.waterfallInteraction
            historyMemoryBudgetBytes: pane.applicationModel.waterfallHistoryMemoryBudgetBytes
            effectiveRowsPerSecond: pane.applicationModel.effectiveWaterfallRowsPerSecond
            visibleHistorySeconds: pane.applicationModel.visibleWaterfallHistorySeconds
            filterWidthAdjustmentActive: filterWidthHintTimer.running
        }

        Timer {
            id: filterWidthHintTimer
            interval: 1000
            repeat: false
        }

        Connections {
            target: pane.applicationModel
            function onFilterWidthChanged() {
                filterWidthHintTimer.restart()
            }
        }

        Label {
            id: filterWidthHint
            readonly property real lowerX: displaySurface.xForFrequency(
                                             pane.applicationModel.filterLowerFrequency)
            readonly property real upperX: displaySurface.xForFrequency(
                                             pane.applicationModel.filterUpperFrequency)
            readonly property real midpoint: (lowerX + upperX) / 2
            visible: filterWidthHintTimer.running
                     && pane.applicationModel.rfControlsSupported
            enabled: false
            z: 5
            text: (Number(pane.applicationModel.filterWidth) / 1000).toFixed(
                      Number(pane.applicationModel.filterWidth) % 1000 === 0 ? 0 : 2)
                  + qsTr(" kHz")
            x: displaySurface.x + (upperX - lowerX >= implicitWidth + 10
                                   ? midpoint - implicitWidth / 2
                                   : Math.min(displaySurface.width - implicitWidth,
                                              Math.max(0, upperX + 6)))
            y: displaySurface.y + 6
            padding: 3
            color: "#f6dc72"
            font.pixelSize: 10
            font.bold: true
            background: Rectangle {
                color: "#d9000000"
                border.color: "#f6dc72"
                border.width: pane.devicePixel
                radius: 2
            }
            Accessible.ignored: true
        }

        Item {
            id: amplitudeScale

            anchors.left: pane.left
            anchors.top: displaySurface.top
            anchors.bottom: displaySurface.bottom
            width: displaySurface.recommendedAmplitudeScaleMargin(
                       pane.width, Screen.devicePixelRatio)
            visible: !pane.waterfallInteraction
            z: 3
            Accessible.ignored: true

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.leftMargin: 3
                anchors.topMargin: 2
                width: dbfsTitle.implicitWidth + 6
                height: dbfsTitle.implicitHeight + 2
                radius: 2
                color: "#b3111a2b"

                Label {
                    id: dbfsTitle
                    anchors.centerIn: parent
                    text: qsTr("dBFS")
                    color: "#aab7ca"
                    font.pixelSize: 9
                    font.bold: true
                }
            }

            Repeater {
                model: displaySurface.minorDbfsTicks

                delegate: Rectangle {
                    x: amplitudeScale.width - width
                    y: displaySurface.yForDbfs(modelData, displaySurface.height) - height / 2
                    width: 4
                    height: pane.devicePixel
                    color: "#61728e"
                    opacity: 0.58
                }
            }

            Repeater {
                model: displaySurface.majorDbfsTicks

                delegate: Item {
                    width: amplitudeScale.width
                    height: 14
                    y: Math.max(0, Math.min(
                                    amplitudeScale.height - height,
                                    displaySurface.yForDbfs(modelData, displaySurface.height) - height / 2))

                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7
                        height: pane.devicePixel
                        color: "#8190a8"
                        opacity: 0.85
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.rightMargin: 9
                        width: dbfsLabel.implicitWidth + 5
                        height: dbfsLabel.implicitHeight + 1
                        radius: 2
                        color: "#a6111a2b"

                        Label {
                            id: dbfsLabel
                            anchors.centerIn: parent
                            text: Number(modelData).toLocaleString(Qt.locale(), "f", 0)
                            color: "#b7c3d4"
                            font.pixelSize: 10
                        }
                    }
                }
            }
        }

        Item {
            id: noiseFloorReference

            x: displaySurface.x
            y: displaySurface.y + displaySurface.yForDbfs(
                   displaySurface.noiseFloorDbfs, displaySurface.height) - height / 2
            width: displaySurface.width
            height: 12
            visible: !pane.waterfallInteraction
                     && pane.applicationModel.receiverRunning
                     && displaySurface.noiseFloorAvailable
            z: 2

            Repeater {
                model: 28

                delegate: Rectangle {
                    x: index * noiseFloorReference.width / 28
                    y: 5
                    width: noiseFloorReference.width / 56
                    height: pane.devicePixel
                    color: "#9caac0"
                    opacity: 0.34
                }
            }

            Label {
                anchors.right: parent.right
                anchors.rightMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                text: Number(displaySurface.noiseFloorDbfs).toLocaleString(
                          Qt.locale(), "f", 1) + qsTr(" dBFS")
                color: "#9caac0"
                font.pixelSize: 10
                opacity: 0.82
            }
        }

        Item {
            id: frequencyScale

            anchors.left: displaySurface.left
            anchors.right: displaySurface.right
            anchors.bottom: parent.bottom
            height: 24

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: pane.frequencyLabelX(
                       pane.applicationModel.visibleLowerFrequency, width)
                text: Number(pane.applicationModel.visibleLowerFrequency).toLocaleString(
                          Qt.locale(), "f", 0)
                color: "#8190a8"
                font.pixelSize: 10
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: pane.frequencyLabelX(
                       pane.applicationModel.visibleCenterFrequency, width)
                text: Number(pane.applicationModel.visibleCenterFrequency).toLocaleString(
                          Qt.locale(), "f", 0)
                color: "#61dafb"
                font.pixelSize: 10
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: pane.frequencyLabelX(
                       pane.applicationModel.visibleUpperFrequency, width)
                text: Number(pane.applicationModel.visibleUpperFrequency).toLocaleString(
                          Qt.locale(), "f", 0)
                color: "#8190a8"
                font.pixelSize: 10
            }
        }

        MouseArea {
            anchors.fill: displaySurface
            enabled: true
            acceptedButtons: pane.waterfallInteraction
                             && !pane.applicationModel.scannerOwnsTuning
                             ? Qt.LeftButton : Qt.NoButton
            preventStealing: true
            cursorShape: pane.waterfallInteraction
                         && !pane.applicationModel.scannerOwnsTuning
                         ? Qt.CrossCursor : Qt.ArrowCursor

            onClicked: function(mouse) {
                if (pane.waterfallInteraction)
                    pane.applicationModel.selectListeningFrequencyAt(mouse.x, width)
            }

            onWheel: function(wheel) {
                const scannerBlocksTuning =
                        pane.applicationModel.scannerOwnsTuning
                        && ((wheel.modifiers & Qt.ShiftModifier)
                            || (!pane.waterfallInteraction
                                && !(wheel.modifiers & Qt.ControlModifier)))
                if (!scannerBlocksTuning) {
                    pane.applicationModel.handleFrequencyWheelWithDeltas(
                        pane.waterfallInteraction,
                        wheel.angleDelta.y,
                        wheel.pixelDelta.y,
                        wheel.modifiers)
                }
                wheel.accepted = true
            }
        }
    }

    component FrequencyDigit: Rectangle {
        id: frequencyDigit

        required property var applicationModel
        required property int digitIndex
        required property string digitText
        required property bool dense
        required property color digitColor
        required property color outlineColor
        property Item previousDigit
        property Item nextDigit
        signal completeEntryRequested()

        readonly property bool editActive:
            applicationModel.centerFrequencyDigitEditActive
        readonly property bool activeEditDigit: editActive
            && applicationModel.centerFrequencyDigitEditIndex === digitIndex
        readonly property bool editableInSession: editActive
            && digitIndex >= applicationModel.centerFrequencyDigitEditStartIndex

        implicitWidth: dense ? 25 : 31
        implicitHeight: dense ? 36 : 42
        enabled: !applicationModel.scannerOwnsTuning
        opacity: enabled ? 1.0 : 0.55
        radius: 4
        color: activeEditDigit ? "#29425f" : (activeFocus ? "#29425f" : "#0a1020")
        border.color: activeEditDigit || activeFocus ? digitColor : outlineColor
        border.width: activeEditDigit || activeFocus ? 2 : 1
        activeFocusOnTab: true
        objectName: "centerFrequencyDigit" + digitIndex

        onActiveFocusChanged: {
            if (activeFocus) {
                root.focusedCenterFrequencyDigitIndex = digitIndex
            } else if (root.focusedCenterFrequencyDigitIndex === digitIndex) {
                root.focusedCenterFrequencyDigitIndex = -1
            }
        }

        Accessible.role: Accessible.SpinBox
        Accessible.name: qsTr("Center frequency digit %1 of 10").arg(digitIndex + 1)
        Accessible.description: qsTr(
            "Left-click begins sequential digit editing; right-click zeros this and following digits; hover and type a digit for immediate replacement; Up and wheel adjust; touch the upper or lower half to adjust")

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 1
            text: "▴"
            color: frequencyDigit.digitColor
            opacity: 0.55
            font.pixelSize: 7
        }

        Label {
            anchors.centerIn: parent
            text: frequencyDigit.digitText
            color: frequencyDigit.digitColor
            opacity: frequencyDigit.editActive
                     ? (frequencyDigit.editableInSession ? 1.0 : 0.65) : 1.0
            font.bold: true
            font.family: "monospace"
            font.pixelSize: frequencyDigit.dense ? 20 : 25
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            height: 2
            visible: frequencyDigit.editableInSession
            color: frequencyDigit.digitColor
            opacity: frequencyDigit.activeEditDigit ? 1.0 : 0.45
        }

        Keys.onPressed: function(event) {
            if (!frequencyDigit.editActive
                    || event.key < Qt.Key_0 || event.key > Qt.Key_9) {
                return
            }
            frequencyDigit.applicationModel.replaceCenterFrequencyDigitInEdit(
                        event.key - Qt.Key_0)
            event.accepted = true
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            text: "▾"
            color: frequencyDigit.digitColor
            opacity: 0.55
            font.pixelSize: 7
        }

        Keys.onUpPressed: function(event) {
            if (frequencyDigit.editActive) {
                event.accepted = true
                return
            }
            frequencyDigit.applicationModel.adjustCenterFrequencyDigit(
                frequencyDigit.digitIndex, 1)
            event.accepted = true
        }
        Keys.onDownPressed: function(event) {
            if (frequencyDigit.editActive) {
                event.accepted = true
                return
            }
            frequencyDigit.applicationModel.adjustCenterFrequencyDigit(
                frequencyDigit.digitIndex, -1)
            event.accepted = true
        }
        Keys.onDeletePressed: function(event) {
            const wasEditing = frequencyDigit.editActive
            frequencyDigit.applicationModel.zeroCenterFrequencyFromDigit(
                frequencyDigit.digitIndex)
            if (wasEditing)
                root.clearCenterFrequencyDigitFocus()
            event.accepted = true
        }
        Keys.onLeftPressed: function(event) {
            if (frequencyDigit.editActive) {
                event.accepted = true
                return
            }
            if (frequencyDigit.previousDigit) {
                frequencyDigit.previousDigit.forceActiveFocus()
            }
            event.accepted = true
        }
        Keys.onRightPressed: function(event) {
            if (frequencyDigit.editActive) {
                event.accepted = true
                return
            }
            if (frequencyDigit.nextDigit) {
                frequencyDigit.nextDigit.forceActiveFocus()
            }
            event.accepted = true
        }
        Keys.onReturnPressed: function(event) {
            if (frequencyDigit.editActive) {
                if (frequencyDigit.applicationModel.commitCenterFrequencyDigitEdit())
                    root.clearCenterFrequencyDigitFocus()
            } else {
                frequencyDigit.completeEntryRequested()
            }
            event.accepted = true
        }
        Keys.onEnterPressed: function(event) {
            if (frequencyDigit.editActive) {
                if (frequencyDigit.applicationModel.commitCenterFrequencyDigitEdit())
                    root.clearCenterFrequencyDigitFocus()
            } else {
                frequencyDigit.completeEntryRequested()
            }
            event.accepted = true
        }
        Keys.onEscapePressed: function(event) {
            if (!frequencyDigit.editActive)
                return
            frequencyDigit.applicationModel.cancelCenterFrequencyDigitEdit()
            root.clearCenterFrequencyDigitFocus()
            event.accepted = true
        }

        TapHandler {
            acceptedDevices: PointerDevice.TouchScreen
            onTapped: function(eventPoint) {
                if (frequencyDigit.editActive) {
                    frequencyDigit.applicationModel.cancelCenterFrequencyDigitEdit()
                    root.clearCenterFrequencyDigitFocus()
                }
                frequencyDigit.forceActiveFocus()
                frequencyDigit.applicationModel.adjustCenterFrequencyDigit(
                    frequencyDigit.digitIndex,
                    eventPoint.position.y < frequencyDigit.height / 2 ? 1 : -1)
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor

            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton) {
                    const wasEditing = frequencyDigit.editActive
                    frequencyDigit.applicationModel.zeroCenterFrequencyFromDigit(
                        frequencyDigit.digitIndex)
                    if (wasEditing)
                        root.clearCenterFrequencyDigitFocus()
                    return
                }
                if (frequencyDigit.applicationModel.beginCenterFrequencyDigitEdit(
                        frequencyDigit.digitIndex))
                    frequencyDigit.forceActiveFocus()
            }
            onContainsMouseChanged: {
                if (containsMouse) {
                    root.hoveredCenterFrequencyDigitIndex = frequencyDigit.digitIndex
                } else if (root.hoveredCenterFrequencyDigitIndex
                           === frequencyDigit.digitIndex) {
                    root.hoveredCenterFrequencyDigitIndex = -1
                }
            }
            onWheel: function(wheel) {
                if (frequencyDigit.editActive) {
                    frequencyDigit.applicationModel.cancelCenterFrequencyDigitEdit()
                    root.clearCenterFrequencyDigitFocus()
                }
                frequencyDigit.applicationModel.adjustCenterFrequencyDigit(
                    frequencyDigit.digitIndex, wheel.angleDelta.y)
                wheel.accepted = true
            }
        }
    }

    Dialog {
        id: completeFrequencyDialog

        function openForEntry() {
            if (root.applicationModel.scannerOwnsTuning)
                return
            completeFrequencyField.text = String(root.applicationModel.centerFrequency)
            open()
        }

        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        title: qsTr("Set center frequency")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape

        onOpened: {
            completeFrequencyField.forceActiveFocus()
            completeFrequencyField.selectAll()
        }
        onAccepted: root.applicationModel.setCenterFrequencyText(
                        completeFrequencyField.text)

        contentItem: TextField {
            id: completeFrequencyField

            placeholderText: qsTr("Frequency in Hz")
            inputMethodHints: Qt.ImhDigitsOnly
            maximumLength: 20
            Accessible.name: qsTr("Complete center frequency in hertz")
            onAccepted: completeFrequencyDialog.accept()
        }
    }
    Dialog {
        id: bookmarkGroupDialog
        property int editingRow: -1
        function openForAdd() {
            editingRow = -1
            bookmarkGroupName.text = qsTr("New group")
            open()
        }
        function openForEdit(row, details) {
            editingRow = row
            bookmarkGroupName.text = details.name
            open()
        }
        anchors.centerIn: Overlay.overlay
        modal: true
        title: editingRow < 0 ? qsTr("Add Group") : qsTr("Edit Group")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { bookmarkGroupName.forceActiveFocus(); bookmarkGroupName.selectAll() }
        onAccepted: {
            if (editingRow < 0) {
                root.applicationModel.bookmarkModel.addGroup(
                    bookmarkList.currentIndex, bookmarkGroupName.text)
            } else {
                root.applicationModel.renameBookmarkGroup(
                    editingRow, bookmarkGroupName.text)
            }
        }
        contentItem: TextField {
            id: bookmarkGroupName
            placeholderText: qsTr("Group name")
            Accessible.name: qsTr("Bookmark group name")
        }
    }

    Dialog {
        id: deleteScanPresetDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 360
        title: qsTr("Delete scanner preset")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        onAccepted: root.applicationModel.deleteSelectedScanPreset()

        contentItem: Label {
            width: 300
            text: qsTr("Delete scanner preset “%1”? This cannot be undone.")
                      .arg(root.applicationModel.selectedScanPresetName)
            color: root.primaryTextColor
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: bookmarkEditDialog
        property int editingRow: -1
        property var originalDetails: ({})
        function openFor(row, details) {
            editingRow = row
            originalDetails = details
            bookmarkNameField.text = details.name
            bookmarkFrequencyField.text = String(details.listeningFrequency)
            bookmarkGainField.text = String(details.requestedGain)
            bookmarkFilterLowField.text = String(details.filterLowHz)
            bookmarkFilterHighField.text = String(details.filterHighHz)
            bookmarkSquelchField.text = String(details.squelchThreshold)
            bookmarkSquelchEnabled.checked = details.squelchEnabled
            let options = root.applicationModel.bookmarkDemodulators.slice()
            let found = false
            for (let i = 0; i < options.length; ++i) {
                if (options[i].id === details.demodulatorId) {
                    found = true
                    break
                }
            }
            if (!found) {
                options.unshift({"id": details.demodulatorId,
                                 "name": qsTr("Unavailable · ") + details.demodulatorId})
            }
            bookmarkModeBox.model = options
            for (let i = 0; i < options.length; ++i) {
                if (options[i].id === details.demodulatorId) {
                    bookmarkModeBox.currentIndex = i
                    break
                }
            }
            open()
        }
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 430
        title: qsTr("Edit Bookmark")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.applicationModel.editBookmark(editingRow, {
            "name": bookmarkNameField.text,
            "listeningFrequency": Number(bookmarkFrequencyField.text),
            "requestedGain": Number(bookmarkGainField.text),
            "demodulatorId": bookmarkModeBox.currentValue,
            "filterLowHz": Number(bookmarkFilterLowField.text),
            "filterHighHz": Number(bookmarkFilterHighField.text),
            "squelchThreshold": Number(bookmarkSquelchField.text),
            "squelchEnabled": bookmarkSquelchEnabled.checked,
            "modeSpecificSettings": originalDetails.modeSpecificSettings,
            "scannerIncluded": originalDetails.scannerIncluded
        })
        contentItem: GridLayout {
            columns: 2
            columnSpacing: 10
            rowSpacing: 8
            Label { text: qsTr("Name"); color: root.primaryTextColor }
            TextField { id: bookmarkNameField; Layout.fillWidth: true }
            Label { text: qsTr("Frequency (Hz)"); color: root.primaryTextColor }
            TextField { id: bookmarkFrequencyField; Layout.fillWidth: true; inputMethodHints: Qt.ImhDigitsOnly }
            Label { text: qsTr("Requested gain (dB)"); color: root.primaryTextColor }
            TextField { id: bookmarkGainField; Layout.fillWidth: true }
            Label { text: qsTr("Mode"); color: root.primaryTextColor }
            ComboBox { id: bookmarkModeBox; Layout.fillWidth: true; textRole: "name"; valueRole: "id" }
            Label { text: qsTr("Filter low (Hz)"); color: root.primaryTextColor }
            TextField { id: bookmarkFilterLowField; Layout.fillWidth: true }
            Label { text: qsTr("Filter high (Hz)"); color: root.primaryTextColor }
            TextField { id: bookmarkFilterHighField; Layout.fillWidth: true }
            Label { text: qsTr("Squelch threshold (dB)"); color: root.primaryTextColor }
            TextField { id: bookmarkSquelchField; Layout.fillWidth: true }
            Label { text: qsTr("Squelch enabled"); color: root.primaryTextColor }
            CheckBox { id: bookmarkSquelchEnabled }
        }
    }

    Dialog {
        id: bookmarkNameDialog
        objectName: "bookmarkNameDialog"
        property bool confirmed: false

        function openForCurrentBookmark(parentRow) {
            const suggestion = root.applicationModel.beginAddCurrentBookmark(
                                   parentRow)
            if (suggestion.length === 0)
                return
            confirmed = false
            addBookmarkNameField.text = suggestion
            bookmarkNameError.visible = false
            open()
        }

        function submit() {
            const trimmed = addBookmarkNameField.text.trim()
            if (trimmed.length === 0) {
                bookmarkNameError.visible = true
                addBookmarkNameField.forceActiveFocus()
                return
            }
            if (root.applicationModel.confirmAddCurrentBookmark(trimmed)) {
                confirmed = true
                close()
            }
        }

        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        title: qsTr("Add Bookmark")
        closePolicy: Popup.CloseOnEscape
        onOpened: {
            addBookmarkNameField.forceActiveFocus()
            addBookmarkNameField.selectAll()
        }
        onClosed: {
            if (!confirmed)
                root.applicationModel.cancelAddCurrentBookmark()
        }

        contentItem: ColumnLayout {
            spacing: 6
            TextField {
                id: addBookmarkNameField
                objectName: "addBookmarkNameField"
                Layout.preferredWidth: 320
                Accessible.name: qsTr("Bookmark name")
                onAccepted: bookmarkNameDialog.submit()
                onTextChanged: bookmarkNameError.visible = false
            }
            Label {
                id: bookmarkNameError
                Layout.fillWidth: true
                visible: false
                text: qsTr("Bookmark name cannot be empty")
                color: "#f6ad55"
            }
        }

        footer: DialogButtonBox {
            standardButtons: DialogButtonBox.Ok | DialogButtonBox.Cancel
            onAccepted: bookmarkNameDialog.submit()
            onRejected: bookmarkNameDialog.close()
        }
    }

    Dialog {
        id: removeBookmarkDialog
        property int removingRow: -1
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 390
        title: qsTr("Remove Group")
        standardButtons: Dialog.Yes | Dialog.Cancel
        contentItem: Label {
            text: qsTr("Remove this group and all of its descendants?")
            color: root.primaryTextColor
            wrapMode: Text.WordWrap
        }
        onAccepted: {
            root.applicationModel.bookmarkModel.removeItem(removingRow)
            bookmarkList.currentIndex = -1
        }
    }

    Dialog {
        id: customCaptureBandwidthDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Custom capture bandwidth")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { captureBandwidthField.forceActiveFocus(); captureBandwidthField.selectAll() }
        onAccepted: root.applicationModel.setCaptureBandwidthText(captureBandwidthField.text)
        contentItem: TextField {
            id: captureBandwidthField
            text: (Number(root.applicationModel.requestedCaptureBandwidth) / 1000000).toFixed(3)
            placeholderText: qsTr("MS/s or samples/s")
            Accessible.name: qsTr("Custom capture bandwidth")
        }
    }

    Dialog {
        id: customFilterWidthDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Custom filter width")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { filterWidthField.forceActiveFocus(); filterWidthField.selectAll() }
        onAccepted: root.applicationModel.setFilterWidthText(filterWidthField.text)
        contentItem: TextField {
            id: filterWidthField
            text: (Number(root.applicationModel.filterWidth) / 1000).toFixed(2)
            placeholderText: qsTr("kHz or Hz")
            Accessible.name: qsTr("Custom filter width")
        }
    }

    Dialog {
        id: customWaterfallHistoryDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Custom visible waterfall history")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: {
            customWaterfallHistoryField.value = root.applicationModel.visibleWaterfallHistorySeconds
            customWaterfallHistoryField.forceActiveFocus()
        }
        onAccepted: root.applicationModel.setVisibleWaterfallHistorySeconds(
                        customWaterfallHistoryField.value)
        contentItem: SpinBox {
            id: customWaterfallHistoryField
            from: 1
            to: 300
            value: root.applicationModel.visibleWaterfallHistorySeconds
            editable: true
            Accessible.name: qsTr("Custom visible history seconds")
        }
    }

    header: ToolBar {
        implicitHeight: root.denseLayout ? 78 : 88
        background: Rectangle {
            color: "#111a2b"
            border.color: root.panelBorderColor
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: root.denseLayout ? 10 : 16
            anchors.rightMargin: root.denseLayout ? 10 : 16
            spacing: root.denseLayout ? 10 : 20

            ColumnLayout {
                spacing: 2

                Label {
                    text: root.applicationModel.centerFrequencyDigitEditActive
                          ? qsTr("CENTER FREQUENCY — edit highlighted digits · Enter apply · Esc cancel")
                          : qsTr("CENTER FREQUENCY — scroll/tap digits · click to edit")
                    color: root.secondaryTextColor
                    font.bold: true
                    font.pixelSize: root.denseLayout ? 9 : 10
                }

                RowLayout {
                    spacing: 2

                    FrequencyDigit {
                        id: centerDigit0
                        applicationModel: root.applicationModel
                        digitIndex: 0
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(0)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        nextDigit: centerDigit1
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    Label { text: " "; color: root.secondaryTextColor }
                    FrequencyDigit {
                        id: centerDigit1
                        applicationModel: root.applicationModel
                        digitIndex: 1
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(1)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit0
                        nextDigit: centerDigit2
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    FrequencyDigit {
                        id: centerDigit2
                        applicationModel: root.applicationModel
                        digitIndex: 2
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(2)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit1
                        nextDigit: centerDigit3
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    FrequencyDigit {
                        id: centerDigit3
                        applicationModel: root.applicationModel
                        digitIndex: 3
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(3)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit2
                        nextDigit: centerDigit4
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    Label { text: " "; color: root.secondaryTextColor }
                    FrequencyDigit {
                        id: centerDigit4
                        applicationModel: root.applicationModel
                        digitIndex: 4
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(4)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit3
                        nextDigit: centerDigit5
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    FrequencyDigit {
                        id: centerDigit5
                        applicationModel: root.applicationModel
                        digitIndex: 5
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(5)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit4
                        nextDigit: centerDigit6
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    FrequencyDigit {
                        id: centerDigit6
                        applicationModel: root.applicationModel
                        digitIndex: 6
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(6)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit5
                        nextDigit: centerDigit7
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    Label { text: " "; color: root.secondaryTextColor }
                    FrequencyDigit {
                        id: centerDigit7
                        applicationModel: root.applicationModel
                        digitIndex: 7
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(7)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit6
                        nextDigit: centerDigit8
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    FrequencyDigit {
                        id: centerDigit8
                        applicationModel: root.applicationModel
                        digitIndex: 8
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(8)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit7
                        nextDigit: centerDigit9
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }
                    FrequencyDigit {
                        id: centerDigit9
                        applicationModel: root.applicationModel
                        digitIndex: 9
                        digitText: root.applicationModel.centerFrequencyDigits.charAt(9)
                        dense: root.denseLayout
                        digitColor: root.centerColor
                        outlineColor: root.panelBorderColor
                        previousDigit: centerDigit8
                        onCompleteEntryRequested: completeFrequencyDialog.openForEntry()
                    }

                    Label {
                        text: qsTr("Hz")
                        color: root.centerColor
                        font.bold: true
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                Layout.topMargin: 10
                Layout.bottomMargin: 10
                color: root.panelBorderColor
            }

            ColumnLayout {
                Layout.minimumWidth: root.denseLayout ? 150 : 190
                spacing: 3

                Label {
                    text: qsTr("LISTENING FREQUENCY")
                    color: root.secondaryTextColor
                    font.bold: true
                    font.pixelSize: 10
                }

                Label {
                    text: Number(root.applicationModel.listeningFrequency).toLocaleString(
                              Qt.locale(), "f", 0) + qsTr(" Hz")
                    color: root.listeningColor
                    font.bold: true
                    font.pixelSize: root.denseLayout ? 19 : 24
                }

                Label {
                    text: qsTr("Click or tap the waterfall to select")
                    color: root.secondaryTextColor
                    font.pixelSize: 9
                }

            }

            Button {
                id: bookmarksSidebarButton
                objectName: "bookmarksSidebarButton"
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: root.controlHeight
                checkable: true
                checked: root.applicationModel.sidebarMode
                         === root.sidebarModeBookmarks
                text: qsTr("Bookmarks")
                Accessible.name: qsTr("Show bookmarks")
                Accessible.description: qsTr(
                    "Open or close the bookmark manager panel")
                onClicked: root.applicationModel.setSidebarMode(
                               checked ? root.sidebarModeBookmarks
                                       : root.sidebarModeNone)
                background: Rectangle {
                    radius: 4
                    color: bookmarksSidebarButton.checked ? "#29425f"
                                                          : (bookmarksSidebarButton.pressed
                                                             ? "#22324b" : "#172033")
                    border.color: bookmarksSidebarButton.checked
                                      ? root.centerColor : root.panelBorderColor
                    border.width: bookmarksSidebarButton.activeFocus ? 2 : 1
                }
                contentItem: Text {
                    text: bookmarksSidebarButton.text
                    color: root.primaryTextColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font: bookmarksSidebarButton.font
                }
            }

            Button {
                id: scanSidebarButton
                objectName: "scanSidebarButton"
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: root.controlHeight
                checkable: true
                checked: root.applicationModel.sidebarMode
                         === root.sidebarModeScan
                text: qsTr("Scan")
                Accessible.name: qsTr("Show scan pane")
                Accessible.description: qsTr(
                    "Open or close the scanner pane")
                onClicked: root.applicationModel.setSidebarMode(
                               checked ? root.sidebarModeScan
                                       : root.sidebarModeNone)
                background: Rectangle {
                    radius: 4
                    color: scanSidebarButton.checked ? "#29425f"
                                                     : (scanSidebarButton.pressed
                                                        ? "#22324b" : "#172033")
                    border.color: scanSidebarButton.checked
                                      ? root.centerColor : root.panelBorderColor
                    border.width: scanSidebarButton.activeFocus ? 2 : 1
                }
                contentItem: Text {
                    text: scanSidebarButton.text
                    color: root.primaryTextColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font: scanSidebarButton.font
                }
            }

            Button {
                id: settingsSidebarButton
                objectName: "settingsSidebarButton"
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: root.controlHeight
                checkable: true
                checked: root.applicationModel.sidebarMode
                         === root.sidebarModeSettings
                text: qsTr("Settings")
                Accessible.name: qsTr("Show settings")
                Accessible.description: qsTr(
                    "Open or close the settings panel")
                onClicked: root.applicationModel.setSidebarMode(
                               checked ? root.sidebarModeSettings
                                       : root.sidebarModeNone)
                background: Rectangle {
                    radius: 4
                    color: settingsSidebarButton.checked ? "#29425f"
                                                         : (settingsSidebarButton.pressed
                                                            ? "#22324b" : "#172033")
                    border.color: settingsSidebarButton.checked
                                      ? root.centerColor : root.panelBorderColor
                    border.width: settingsSidebarButton.activeFocus ? 2 : 1
                }
                contentItem: Text {
                    text: settingsSidebarButton.text
                    color: root.primaryTextColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font: settingsSidebarButton.font
                }
            }

            Button {
                id: consoleSidebarButton
                objectName: "consoleSidebarButton"
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: root.controlHeight
                checkable: true
                checked: root.applicationModel.sidebarMode
                         === root.sidebarModeConsole
                text: qsTr("Console")
                Accessible.name: qsTr("Show application console")
                Accessible.description: qsTr(
                    "Open or close the read-only application console")
                onClicked: root.applicationModel.setSidebarMode(
                               checked ? root.sidebarModeConsole
                                       : root.sidebarModeNone)
                background: Rectangle {
                    radius: 4
                    color: consoleSidebarButton.checked ? "#29425f"
                                                        : (consoleSidebarButton.pressed
                                                           ? "#22324b" : "#172033")
                    border.color: consoleSidebarButton.checked
                                      ? root.centerColor : root.panelBorderColor
                    border.width: consoleSidebarButton.activeFocus ? 2 : 1
                }
                contentItem: Text {
                    text: consoleSidebarButton.text
                    color: root.primaryTextColor
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font: consoleSidebarButton.font
                }
            }

            Item {
                id: logoArea
                objectName: "slopSdrLogoArea"
                Layout.fillWidth: true
                Layout.fillHeight: true

                readonly property real metadataGap: root.denseLayout ? 5 : 8
                readonly property real metadataWidth:
                    Math.max(versionLabel.implicitWidth,
                             releaseDateLabel.implicitWidth)
                readonly property real minimumLogoWidth:
                    root.denseLayout ? 56 : 72
                readonly property bool metadataVisible:
                    width >= minimumLogoWidth + metadataGap + metadataWidth
                readonly property real naturalLogoWidth:
                    slopSdrLogo.implicitHeight > 0
                    ? height * slopSdrLogo.implicitWidth
                      / slopSdrLogo.implicitHeight : 0
                readonly property real logoWidth:
                    Math.min(naturalLogoWidth,
                             Math.max(0, width
                                         - (metadataVisible
                                            ? metadataGap + metadataWidth : 0)))
                readonly property bool logoVisible:
                    slopSdrLogo.implicitWidth > 0
                    && logoWidth >= minimumLogoWidth
                readonly property real contentWidth:
                    (logoVisible ? logoWidth : 0)
                    + (metadataVisible && logoVisible
                       ? metadataGap + metadataWidth : 0)

                Image {
                    id: slopSdrLogo
                    objectName: "slopSdrLogo"
                    x: (parent.width - parent.contentWidth) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.logoVisible ? parent.logoWidth : 0
                    height: implicitWidth > 0 ? width * implicitHeight / implicitWidth
                                              : 0
                    visible: parent.logoVisible && status === Image.Ready
                    source: "qrc:/assets/slopsdr-logo.png"
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    mipmap: true
                    asynchronous: false
                }

                Column {
                    id: metadataBlock
                    objectName: "slopSdrReleaseMetadata"
                    x: slopSdrLogo.x + slopSdrLogo.width + parent.metadataGap
                    y: (parent.height - height) / 2
                    width: parent.metadataWidth
                    spacing: 1
                    visible: parent.metadataVisible && parent.logoVisible
                    enabled: false

                    Label {
                        id: versionLabel
                        text: root.applicationVersion.length > 0
                              ? qsTr("v%1").arg(root.applicationVersion) : ""
                        color: root.primaryTextColor
                        font.bold: true
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    Label {
                        id: releaseDateLabel
                        text: root.applicationReleaseDate
                        color: root.secondaryTextColor
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }
            }

            Label {
                visible: !root.denseLayout
                text: root.applicationModel.mockMode
                      ? qsTr("MOCK BACKEND\nNO SDR HARDWARE")
                      : (root.applicationModel.backendReady
                         ? qsTr("HARDWARE DEVICE READY")
                         : qsTr("HARDWARE MODE\nNO DEVICE SELECTED"))
                color: root.secondaryTextColor
                horizontalAlignment: Text.AlignRight
                font.bold: true
                font.pixelSize: 10
            }
        }
    }

    RowLayout {
        id: mainLayout
        anchors.fill: parent
        anchors.margins: root.denseLayout ? 6 : 10
        spacing: root.denseLayout ? 6 : 10

        Rectangle {
            id: sidebarPanel

            visible: root.applicationModel.sidebarMode !== root.sidebarModeNone
            Layout.fillHeight: true
            Layout.preferredWidth: root.applicationModel.sidebarMode
                                 === root.sidebarModeScan
                                 ? root.applicationModel.scanPanelWidth
                                 : (root.applicationModel.sidebarMode
                                    === root.sidebarModeSettings
                                    ? root.applicationModel.settingsPanelWidth
                                    : (root.applicationModel.sidebarMode
                                       === root.sidebarModeConsole
                                       ? root.applicationModel.consolePanelWidth
                                       : root.applicationModel.bookmarksPanelWidth))
            Layout.minimumWidth: 220
            Layout.maximumWidth: Math.max(
                                     220,
                                     root.width
                                     - (root.denseLayout ? 360 : 440)
                                     - 300)
            radius: 8
            color: root.panelColor
            border.color: root.panelBorderColor
            clip: true

            ColumnLayout {
                visible: root.applicationModel.sidebarMode
                         === root.sidebarModeBookmarks
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                spacing: 7

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Bookmarks")
                        color: root.primaryTextColor
                        font.bold: true
                        font.pixelSize: 16
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        implicitHeight: 28
                        text: qsTr("Close")
                        onClicked: root.applicationModel.setSidebarMode(
                                       root.sidebarModeNone)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Button {
                        implicitHeight: 30
                        text: qsTr("Add Group")
                        enabled: !root.applicationModel.bookmarkModel.loading
                        onClicked: bookmarkGroupDialog.openForAdd()
                    }

                    Button {
                        implicitHeight: 30
                        objectName: "addBookmarkButton"
                        text: qsTr("Add Bookmark")
                        enabled: !root.applicationModel.bookmarkModel.loading
                        onClicked: bookmarkNameDialog.openForCurrentBookmark(
                                       bookmarkList.currentIndex)
                    }

                    Button {
                        implicitHeight: 30
                        objectName: "updateBookmarkButton"
                        text: qsTr("Update Bookmark")
                        enabled: !root.applicationModel.bookmarkModel.loading &&
                                 ((bookmarkList.currentIndex >= 0 &&
                                   bookmarkList.currentItem &&
                                   !bookmarkList.currentItem.isGroup) ||
                                  root.applicationModel.bookmarkUpdateAvailable)
                        onClicked: root.applicationModel.updateCurrentBookmark(
                                       bookmarkList.currentIndex)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr(
                            "Update the selected or last tuned bookmark")
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Button {
                        implicitHeight: 30
                        text: qsTr("Edit")
                        enabled: bookmarkList.currentIndex >= 0
                                 && !root.applicationModel.bookmarkModel.loading
                        onClicked: {
                            const details = root.applicationModel.bookmarkModel.itemDetails(
                                              bookmarkList.currentIndex)
                            if (details.isGroup) {
                                bookmarkGroupDialog.openForEdit(
                                    bookmarkList.currentIndex, details)
                            } else {
                                bookmarkEditDialog.openFor(
                                    bookmarkList.currentIndex, details)
                            }
                        }
                    }

                    Button {
                        implicitHeight: 30
                        text: qsTr("Remove")
                        enabled: bookmarkList.currentIndex >= 0
                                 && !root.applicationModel.bookmarkModel.loading
                        onClicked: {
                            const details = root.applicationModel.bookmarkModel.itemDetails(
                                              bookmarkList.currentIndex)
                            if (details.isGroup && details.hasChildren) {
                                removeBookmarkDialog.removingRow = bookmarkList.currentIndex
                                removeBookmarkDialog.open()
                            } else {
                                root.applicationModel.bookmarkModel.removeItem(
                                    bookmarkList.currentIndex)
                                bookmarkList.currentIndex = -1
                            }
                        }
                    }

                    Button {
                        implicitHeight: 30
                        text: qsTr("Tune")
                        enabled: bookmarkList.currentIndex >= 0
                                 && !root.applicationModel.bookmarkModel.loading
                                 && bookmarkList.currentItem
                                 && !bookmarkList.currentItem.isGroup
                                 && bookmarkList.currentItem.demodulatorAvailable
                                 && !root.applicationModel.scannerOwnsTuning
                        onClicked: root.applicationModel.tuneBookmark(
                                       bookmarkList.currentIndex)
                    }

                    Item { Layout.fillWidth: true }
                }

                Rectangle {
                    id: bookmarkScanningSection
                    objectName: "bookmarkScanningSection"
                    Layout.fillWidth: true
                    Layout.minimumHeight: bookmarkScanningContent.implicitHeight + 16
                    radius: 4
                    color: "#111a2b"
                    border.color: root.panelBorderColor

                    ColumnLayout {
                        id: bookmarkScanningContent
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 5

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Bookmark scanning")
                            color: root.primaryTextColor
                            font.bold: true
                            font.pixelSize: 12
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 6
                            rowSpacing: 4

                            Label { text: qsTr("Dwell (ms)"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            TextField {
                                objectName: "bookmarkScanDwellField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.bookmarkScanCanStop
                                text: root.applicationModel.bookmarkScanDwellMilliseconds.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 1 }
                                onEditingFinished: root.applicationModel.setBookmarkScanDwellMilliseconds(Number(text))
                            }
                            Label { text: qsTr("Resume delay (ms)"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            TextField {
                                objectName: "bookmarkScanResumeDelayField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.bookmarkScanCanStop
                                text: root.applicationModel.bookmarkScanResumeDelayMilliseconds.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 0 }
                                onEditingFinished: root.applicationModel.setBookmarkScanResumeDelayMilliseconds(Number(text))
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Button {
                                objectName: "bookmarkScanStartButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.bookmarkScanCanStart
                                text: qsTr("Start")
                                onClicked: root.applicationModel.startBookmarkScan()
                            }
                            Button {
                                objectName: "bookmarkScanPauseResumeButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.bookmarkScanCanPauseResume
                                text: root.applicationModel.bookmarkScanPaused
                                      ? qsTr("Resume") : qsTr("Pause")
                                onClicked: root.applicationModel.pauseOrResumeBookmarkScan()
                            }
                            Button {
                                objectName: "bookmarkScanSkipButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.bookmarkScanCanSkip
                                text: qsTr("Skip")
                                onClicked: root.applicationModel.skipBookmarkScan()
                            }
                            Button {
                                objectName: "bookmarkScanStopButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.bookmarkScanCanStop
                                text: qsTr("Stop")
                                onClicked: root.applicationModel.stopBookmarkScan()
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 6
                            rowSpacing: 2
                            Label { text: qsTr("Bookmark"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "bookmarkScanCurrentName"
                                Layout.fillWidth: true
                                text: root.applicationModel.bookmarkScanCurrentName.length > 0
                                      ? root.applicationModel.bookmarkScanCurrentName : "—"
                                color: root.primaryTextColor
                                elide: Text.ElideRight
                            }
                            Label { text: qsTr("Position"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "bookmarkScanPosition"
                                Layout.fillWidth: true
                                text: root.applicationModel.bookmarkScanPosition
                                color: root.primaryTextColor
                            }
                            Label { text: qsTr("State"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "bookmarkScanState"
                                Layout.fillWidth: true
                                text: root.applicationModel.bookmarkScanState
                                color: root.primaryTextColor
                            }
                        }

                        Label {
                            objectName: "bookmarkScanStatus"
                            Layout.fillWidth: true
                            text: root.applicationModel.bookmarkScanStatusMessage
                            color: root.secondaryTextColor
                            wrapMode: Text.WordWrap
                            font.pixelSize: 9
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Scanner inclusion")
                    color: root.secondaryTextColor
                    font.pixelSize: 10
                }

                ListView {
                    id: bookmarkList

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    model: root.applicationModel.bookmarkModel
                    boundsBehavior: Flickable.StopAtBounds

                    header: DropArea {
                        width: bookmarkList.width
                        height: 28
                        keys: ["bookmark-row"]
                        Rectangle {
                            anchors.fill: parent
                            color: parent.containsDrag ? "#315a78" : "transparent"
                            border.color: parent.containsDrag
                                          ? root.centerColor
                                          : root.panelBorderColor
                            border.width: 1
                            Label {
                                anchors.centerIn: parent
                                text: qsTr("Unfiled Bookmarks")
                                color: parent.parent.containsDrag
                                       ? root.primaryTextColor
                                       : root.secondaryTextColor
                                font.pixelSize: 10
                            }
                        }
                        onDropped: function(drop) {
                            const source = drop.source
                            const movedUuid = source ? source.uuid : ""
                            if (source && root.applicationModel.bookmarkModel.moveBookmark(
                                    movedUuid, "", "into")) {
                                Qt.callLater(function() {
                                    bookmarkList.currentIndex =
                                        root.applicationModel.bookmarkModel.visibleRowForUuid(
                                            movedUuid)
                                })
                                drop.acceptProposedAction()
                            }
                        }
                    }

                    delegate: Item {
                        id: bookmarkDelegate

                        required property int index
                        required property string uuid
                        required property string name
                        required property int depth
                        required property bool isGroup
                        required property bool expanded
                        required property bool hasChildren
                        required property int scannerCheckState
                        required property double listeningFrequency
                        required property string demodulatorName
                        required property bool demodulatorAvailable
                        property string dropPlacement: ""
                        property bool dragOccurred: false
                        property bool dragSessionActive: false

                        width: bookmarkList.width
                        height: isGroup ? 34 : 42

                        HoverHandler {
                            id: bookmarkRowHover
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: 4
                            color: bookmarkList.currentIndex === bookmarkDelegate.index
                                   ? "#29425f"
                                   : (bookmarkDrop.containsDrag
                                      && bookmarkDelegate.dropPlacement === "into"
                                      ? "#315a78"
                                   : "transparent"
                                   )
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 4 + bookmarkDelegate.depth * 16
                            anchors.rightMargin: bookmarkDelegate.isGroup ? 5 : 32
                            spacing: 3

                            ToolButton {
                                implicitWidth: 24
                                implicitHeight: 28
                                visible: bookmarkDelegate.isGroup
                                text: bookmarkDelegate.expanded ? "▾" : "▸"
                                enabled: bookmarkDelegate.hasChildren
                                Accessible.name: bookmarkDelegate.expanded
                                                 ? qsTr("Collapse group")
                                                 : qsTr("Expand group")
                                onClicked: root.applicationModel.bookmarkModel.toggleExpanded(
                                               bookmarkDelegate.index)
                            }

                            Item {
                                visible: !bookmarkDelegate.isGroup
                                implicitWidth: 24
                            }

                            CheckBox {
                                implicitWidth: 30
                                implicitHeight: 30
                                tristate: bookmarkDelegate.isGroup
                                checkState: bookmarkDelegate.scannerCheckState
                                Accessible.name: qsTr("Include %1 in scanner")
                                                 .arg(bookmarkDelegate.name)
                                onClicked: root.applicationModel.bookmarkModel.toggleScannerInclusion(
                                               bookmarkDelegate.index)
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Label {
                                    Layout.fillWidth: true
                                    text: bookmarkDelegate.name
                                    color: root.primaryTextColor
                                    font.bold: bookmarkDelegate.isGroup
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: !bookmarkDelegate.isGroup
                                    text: Number(bookmarkDelegate.listeningFrequency).toLocaleString(
                                              Qt.locale(), "f", 0)
                                          + qsTr(" Hz · ")
                                          + bookmarkDelegate.demodulatorName
                                    color: bookmarkDelegate.demodulatorAvailable
                                           ? root.secondaryTextColor
                                           : "#f6ad55"
                                    font.pixelSize: 9
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        TapHandler {
                            enabled: !bookmarkDrag.active
                            onTapped: bookmarkList.currentIndex = bookmarkDelegate.index
                            onDoubleTapped: {
                                bookmarkList.currentIndex = bookmarkDelegate.index
                                if (!bookmarkDelegate.dragOccurred
                                        && !bookmarkDelegate.isGroup
                                        && bookmarkDelegate.demodulatorAvailable
                                        && !root.applicationModel.scannerOwnsTuning) {
                                    root.applicationModel.tuneBookmark(
                                        bookmarkDelegate.index)
                                }
                            }
                        }

                        Item {
                            id: bookmarkDragHandle
                            objectName: "bookmarkDragHandle"
                            visible: !bookmarkDelegate.isGroup
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            width: 30
                            z: 8

                            Column {
                                anchors.centerIn: parent
                                spacing: 2
                                visible: bookmarkRowHover.hovered
                                         || bookmarkList.currentIndex
                                            === bookmarkDelegate.index
                                         || bookmarkDrag.drag.active
                                Repeater {
                                    model: 3
                                    Rectangle {
                                        width: 12
                                        height: 2
                                        radius: 1
                                        color: bookmarkDrag.drag.active
                                               ? root.centerColor
                                               : root.secondaryTextColor
                                    }
                                }
                            }

                            MouseArea {
                                id: bookmarkDrag
                                objectName: "bookmarkDragHandler"
                                anchors.fill: parent
                                enabled: !root.applicationModel.bookmarkModel.loading
                                         && !root.applicationModel.bookmarkModel.persistencePending
                                acceptedButtons: Qt.LeftButton
                                hoverEnabled: true
                                preventStealing: true
                                cursorShape: drag.active
                                             ? Qt.ClosedHandCursor
                                             : Qt.OpenHandCursor
                                drag.target: bookmarkDragPreview
                                drag.threshold: Qt.styleHints.startDragDistance

                                onPressed: {
                                    const position = bookmarkDelegate.mapToItem(
                                                       bookmarkList, 4, 0)
                                    bookmarkDragPreview.x = position.x
                                    bookmarkDragPreview.y = position.y
                                    bookmarkList.currentIndex = bookmarkDelegate.index
                                }
                                onPositionChanged: function(mouse) {
                                    if (drag.active) {
                                        if (!bookmarkDelegate.dragSessionActive) {
                                            bookmarkDelegate.dragSessionActive = true
                                            bookmarkDelegate.dragOccurred = true
                                            root.bookmarkDragActive = true
                                        }
                                        const point = bookmarkDragHandle.mapToItem(
                                                          bookmarkList,
                                                          mouse.x,
                                                          mouse.y)
                                        root.bookmarkDragListY = point.y
                                    }
                                }
                                onReleased: {
                                    if (bookmarkDelegate.dragSessionActive) {
                                        bookmarkDragPreview.Drag.drop()
                                        bookmarkDelegate.dragSessionActive = false
                                    }
                                    root.bookmarkDragActive = false
                                    Qt.callLater(function() {
                                        bookmarkDelegate.dragOccurred = false
                                    })
                                }
                                onCanceled: {
                                    if (bookmarkDelegate.dragSessionActive) {
                                        bookmarkDragPreview.Drag.cancel()
                                        bookmarkDelegate.dragSessionActive = false
                                    }
                                    root.bookmarkDragActive = false
                                    Qt.callLater(function() {
                                        bookmarkDelegate.dragOccurred = false
                                    })
                                }
                            }
                        }

                        Rectangle {
                            id: bookmarkDragPreview
                            parent: bookmarkList
                            visible: bookmarkDelegate.dragSessionActive
                            width: Math.max(80, bookmarkList.width - 12)
                            height: bookmarkDelegate.height
                            radius: 4
                            z: 100
                            opacity: 0.9
                            color: "#29425f"
                            border.color: root.centerColor
                            border.width: 1

                            Drag.active: bookmarkDelegate.dragSessionActive
                            Drag.source: bookmarkDelegate
                            Drag.keys: ["bookmark-row"]
                            Drag.hotSpot.x: width - 15
                            Drag.hotSpot.y: height / 2

                            Label {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                text: bookmarkDelegate.name
                                color: root.primaryTextColor
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                font.pixelSize: 11
                            }
                        }

                        Shortcut {
                            enabled: bookmarkDelegate.dragSessionActive
                            sequence: "Escape"
                            onActivated: {
                                bookmarkDragPreview.Drag.cancel()
                                bookmarkDelegate.dragSessionActive = false
                                root.bookmarkDragActive = false
                            }
                        }

                        DropArea {
                            id: bookmarkDrop
                            anchors.fill: parent
                            keys: ["bookmark-row"]
                            onEntered: function(drag) {
                                bookmarkDelegate.dropPlacement =
                                    bookmarkDelegate.isGroup ? "into"
                                    : (drag.y < height / 2 ? "before" : "after")
                                if (bookmarkDelegate.isGroup
                                        && !bookmarkDelegate.expanded)
                                    expandDropGroupTimer.restart()
                            }
                            onPositionChanged: function(drag) {
                                bookmarkDelegate.dropPlacement =
                                    bookmarkDelegate.isGroup ? "into"
                                    : (drag.y < height / 2 ? "before" : "after")
                            }
                            onExited: {
                                bookmarkDelegate.dropPlacement = ""
                                expandDropGroupTimer.stop()
                            }
                            onDropped: function(drop) {
                                expandDropGroupTimer.stop()
                                const source = drop.source
                                const movedUuid = source ? source.uuid : ""
                                const placement = bookmarkDelegate.dropPlacement
                                bookmarkDelegate.dropPlacement = ""
                                if (source && movedUuid !== bookmarkDelegate.uuid
                                        && root.applicationModel.bookmarkModel.moveBookmark(
                                            movedUuid,
                                            bookmarkDelegate.uuid,
                                            placement)) {
                                    Qt.callLater(function() {
                                        bookmarkList.currentIndex =
                                            root.applicationModel.bookmarkModel.visibleRowForUuid(
                                                movedUuid)
                                    })
                                    drop.acceptProposedAction()
                                }
                            }
                        }

                        Timer {
                            id: expandDropGroupTimer
                            interval: 500
                            repeat: false
                            onTriggered: root.applicationModel.bookmarkModel.expandGroupForDrop(
                                             bookmarkDelegate.index)
                        }

                        Rectangle {
                            visible: bookmarkDrop.containsDrag
                                     && !bookmarkDelegate.isGroup
                                     && bookmarkDelegate.dropPlacement.length > 0
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: bookmarkDelegate.dropPlacement === "before"
                                         ? parent.top : undefined
                            anchors.bottom: bookmarkDelegate.dropPlacement === "after"
                                            ? parent.bottom : undefined
                            height: 2
                            color: root.centerColor
                            z: 5
                        }
                    }

                    Timer {
                        interval: 16
                        repeat: true
                        running: root.bookmarkDragActive
                        onTriggered: {
                            const edge = 34
                            const maximum = Math.max(
                                                0,
                                                bookmarkList.contentHeight
                                                - bookmarkList.height)
                            if (root.bookmarkDragListY >= 0
                                    && root.bookmarkDragListY < edge) {
                                bookmarkList.contentY = Math.max(
                                    0, bookmarkList.contentY - 8)
                            } else if (root.bookmarkDragListY
                                       > bookmarkList.height - edge) {
                                bookmarkList.contentY = Math.min(
                                    maximum, bookmarkList.contentY + 8)
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: bookmarkList.count === 0
                        text: root.applicationModel.bookmarkModel.loading
                              ? qsTr("Loading bookmarks…")
                              : qsTr("No bookmarks yet")
                        color: root.secondaryTextColor
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.applicationModel.bookmarkModel.lastError.length > 0
                    text: root.applicationModel.bookmarkModel.lastError
                    color: "#f6ad55"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                }

            }

            ColumnLayout {
                id: scanSidebarContent
                objectName: "scanSidebarContent"
                visible: root.applicationModel.sidebarMode
                         === root.sidebarModeScan
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        objectName: "scanPaneHeading"
                        text: qsTr("Scan")
                        color: root.primaryTextColor
                        font.bold: true
                        font.pixelSize: 16
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        objectName: "closeScanPaneButton"
                        implicitHeight: 28
                        text: qsTr("Close")
                        onClicked: root.applicationModel.setSidebarMode(
                                       root.sidebarModeNone)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: scanShellContent.implicitHeight + 20
                    radius: 5
                    color: "#111a2b"
                    border.color: root.panelBorderColor

                    ColumnLayout {
                        id: scanShellContent
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 7

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Frequency scanner")
                            color: root.primaryTextColor
                            font.bold: true
                            font.pixelSize: 12
                        }

                        Label {
                            objectName: "scanShellNotice"
                            Layout.fillWidth: true
                            text: root.applicationModel.scanTypeIndex === 0
                                  ? qsTr("Centers the SDR once on Start, then scans within that fixed usable capture passband.")
                                  : qsTr("Plans safe capture blocks and retunes the SDR only between blocks.")
                            color: root.secondaryTextColor
                            wrapMode: Text.WordWrap
                            font.pixelSize: 10
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 8
                            rowSpacing: 5

                            Label { text: qsTr("Scan type"); color: root.secondaryTextColor }
                            ComboBox {
                                objectName: "scanTypeControl"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.scannerOwnsTuning
                                model: [qsTr("Current passband"), qsTr("Wide range")]
                                currentIndex: root.applicationModel.scanTypeIndex
                                onActivated: root.applicationModel.setScanTypeIndex(currentIndex)
                            }

                            Label { text: qsTr("Lower frequency"); color: root.secondaryTextColor }
                            TextField {
                                objectName: "scanLowerFrequencyField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.scannerOwnsTuning
                                text: root.applicationModel.scanLowerFrequency.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 0 }
                                onEditingFinished: root.applicationModel.setScanLowerFrequency(Number(text))
                            }

                            Label { text: qsTr("Upper frequency"); color: root.secondaryTextColor }
                            TextField {
                                objectName: "scanUpperFrequencyField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.scannerOwnsTuning
                                text: root.applicationModel.scanUpperFrequency.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 0 }
                                onEditingFinished: root.applicationModel.setScanUpperFrequency(Number(text))
                            }

                            Label { text: qsTr("Step size"); color: root.secondaryTextColor }
                            TextField {
                                objectName: "scanStepSizeField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.scannerOwnsTuning
                                text: root.applicationModel.scanStepSize.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 0 }
                                onEditingFinished: root.applicationModel.setScanStepSize(Number(text))
                            }

                            Label { text: qsTr("Dwell time (ms)"); color: root.secondaryTextColor }
                            TextField {
                                objectName: "scanDwellTimeField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.scannerOwnsTuning
                                text: root.applicationModel.scanDwellMilliseconds.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 1 }
                                onEditingFinished: root.applicationModel.setScanDwellMilliseconds(Number(text))
                            }

                            Label { text: qsTr("Resume delay (ms)"); color: root.secondaryTextColor }
                            TextField {
                                objectName: "scanResumeDelayField"
                                Layout.fillWidth: true
                                enabled: !root.applicationModel.scannerOwnsTuning
                                text: root.applicationModel.scanResumeDelayMilliseconds.toString()
                                inputMethodHints: Qt.ImhDigitsOnly
                                validator: IntValidator { bottom: 0 }
                                onEditingFinished: root.applicationModel.setScanResumeDelayMilliseconds(Number(text))
                            }

                            Label { text: qsTr("Squelch source"); color: root.secondaryTextColor }
                            ComboBox {
                                objectName: "scanSquelchSourceControl"
                                Layout.fillWidth: true
                                enabled: false
                                model: [qsTr("Live receiver squelch")]
                                currentIndex: 0
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            Button {
                                objectName: "scanStartButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.scanCanStart
                                text: qsTr("Start")
                                onClicked: root.applicationModel.startScan()
                            }
                            Button {
                                objectName: "scanPauseResumeButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.scanCanPauseResume
                                text: root.applicationModel.scanPaused
                                      ? qsTr("Resume") : qsTr("Pause")
                                onClicked: root.applicationModel.pauseOrResumeScan()
                            }
                            Button {
                                objectName: "scanSkipButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.scanCanSkip
                                text: qsTr("Skip")
                                onClicked: root.applicationModel.skipScanFrequency()
                            }
                            Button {
                                objectName: "scanStopButton"
                                Layout.fillWidth: true
                                enabled: root.applicationModel.scanCanStop
                                text: qsTr("Stop")
                                onClicked: root.applicationModel.stopScan()
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 8
                            rowSpacing: 3

                            Label { text: qsTr("Scan frequency"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "scanCurrentFrequencyDisplay"
                                Layout.fillWidth: true
                                text: root.applicationModel.scanCurrentFrequency === 0
                                      ? "—" : root.applicationModel.scanCurrentFrequency + qsTr(" Hz")
                                color: root.primaryTextColor
                            }
                            Label { text: qsTr("Listening"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "scanListeningFrequencyDisplay"
                                Layout.fillWidth: true
                                text: root.applicationModel.listeningFrequency + qsTr(" Hz")
                                color: root.primaryTextColor
                            }
                            Label { text: qsTr("Hardware center"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "scanHardwareCenterFrequencyDisplay"
                                Layout.fillWidth: true
                                text: root.applicationModel.centerFrequency + qsTr(" Hz")
                                color: root.primaryTextColor
                            }
                            Label { text: qsTr("Capture block"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "scanCaptureBlockProgressDisplay"
                                Layout.fillWidth: true
                                text: root.applicationModel.scanCaptureBlockProgress
                                color: root.primaryTextColor
                            }
                            Label { text: qsTr("State"); color: root.secondaryTextColor; font.pixelSize: 10 }
                            Label {
                                objectName: "scanStateDisplay"
                                Layout.fillWidth: true
                                text: root.applicationModel.scanState
                                color: root.primaryTextColor
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Status")
                            color: root.secondaryTextColor
                            font.pixelSize: 10
                        }
                        Label {
                            objectName: "scanStatusMessage"
                            Layout.fillWidth: true
                            text: root.applicationModel.scanStatusMessage
                            color: root.secondaryTextColor
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            objectName: "scanValidationError"
                            Layout.fillWidth: true
                            visible: root.applicationModel.scanValidationError.length > 0
                            text: root.applicationModel.scanValidationError
                            color: "#ff9b9b"
                            wrapMode: Text.WordWrap
                        }

                        Rectangle {
                            objectName: "scanPresetsSection"
                            Layout.fillWidth: true
                            Layout.minimumHeight: scanPresetsContent.implicitHeight + 20
                            radius: 4
                            color: "#0b111e"
                            border.color: root.panelBorderColor

                            ColumnLayout {
                                id: scanPresetsContent
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Presets")
                                    color: root.primaryTextColor
                                    font.bold: true
                                    font.pixelSize: 12
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 5

                                    TextField {
                                        id: scanPresetNameField
                                        objectName: "scanPresetNameField"
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("Preset name")
                                        Accessible.name: qsTr("Scanner preset name")
                                        Accessible.description: qsTr(
                                            "Name for saving or updating a scanner preset")
                                    }

                                    Button {
                                        objectName: "saveNewScanPresetButton"
                                        text: qsTr("Save New")
                                        Accessible.name: text
                                        onClicked: root.applicationModel.saveNewScanPreset(
                                                       scanPresetNameField.text)
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 5

                                    Button {
                                        objectName: "loadScanPresetButton"
                                        Layout.fillWidth: true
                                        enabled: root.applicationModel.selectedScanPresetId.length > 0
                                                 && !root.applicationModel.scannerOwnsTuning
                                        text: qsTr("Load")
                                        Accessible.name: text
                                        onClicked: root.applicationModel.loadSelectedScanPreset()
                                    }
                                    Button {
                                        objectName: "updateScanPresetButton"
                                        Layout.fillWidth: true
                                        enabled: root.applicationModel.selectedScanPresetId.length > 0
                                        text: qsTr("Update")
                                        Accessible.name: text
                                        onClicked: root.applicationModel.updateSelectedScanPreset(
                                                       scanPresetNameField.text)
                                    }
                                    Button {
                                        objectName: "deleteScanPresetButton"
                                        Layout.fillWidth: true
                                        enabled: root.applicationModel.selectedScanPresetId.length > 0
                                        text: qsTr("Delete")
                                        Accessible.name: text
                                        onClicked: deleteScanPresetDialog.open()
                                    }
                                }

                                Label {
                                    objectName: "scanPresetStatusMessage"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: root.applicationModel.scanPresetStatusMessage
                                    color: text.startsWith("Unable")
                                           ? "#ff9b9b" : root.secondaryTextColor
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 10
                                }

                                ListView {
                                    id: scanPresetList
                                    objectName: "scanPresetList"
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.min(
                                                                220,
                                                                Math.max(42, contentHeight))
                                    clip: true
                                    spacing: 2
                                    model: root.applicationModel.scanPresets
                                    boundsBehavior: Flickable.StopAtBounds
                                    ScrollBar.vertical: ScrollBar {}

                                    delegate: Item {
                                        id: scanPresetDelegate
                                        required property int index
                                        required property var modelData
                                        readonly property var preset: modelData
                                        property bool dragOccurred: false
                                        property bool dragSessionActive: false
                                        property string dropPlacement: ""

                                        width: scanPresetList.width
                                        height: 42

                                        HoverHandler {
                                            id: scanPresetRowHover
                                        }

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 4
                                            color: scanPresetDelegate.preset.presetId
                                                   === root.applicationModel.selectedScanPresetId
                                                   ? "#29425f"
                                                   : (scanPresetDrop.containsDrag
                                                      ? "#315a78" : "transparent")
                                        }

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 4
                                            anchors.rightMargin: 32
                                            spacing: 3

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0

                                                Label {
                                                    Layout.fillWidth: true
                                                    text: scanPresetDelegate.preset.name
                                                    color: root.primaryTextColor
                                                    font.pixelSize: 11
                                                    elide: Text.ElideRight
                                                }

                                                Label {
                                                    Layout.fillWidth: true
                                                    text: scanPresetDelegate.preset.scanType === "wideRange"
                                                          ? qsTr("Wide range")
                                                          : qsTr("Current passband")
                                                    color: root.secondaryTextColor
                                                    font.pixelSize: 9
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        TapHandler {
                                            enabled: !root.scanPresetDragActive
                                            onTapped: {
                                                root.applicationModel.selectScanPreset(
                                                            scanPresetDelegate.preset.presetId)
                                                scanPresetNameField.text =
                                                        scanPresetDelegate.preset.name
                                            }
                                            onDoubleTapped: {
                                                root.applicationModel.selectScanPreset(
                                                            scanPresetDelegate.preset.presetId)
                                                scanPresetNameField.text =
                                                        scanPresetDelegate.preset.name
                                                if (!scanPresetDelegate.dragOccurred)
                                                    root.applicationModel.loadSelectedScanPreset()
                                            }
                                        }

                                        Item {
                                            id: scanPresetDragHandle
                                            objectName: "scanPresetDragHandle"
                                            anchors.top: parent.top
                                            anchors.right: parent.right
                                            anchors.bottom: parent.bottom
                                            width: 30
                                            z: 8

                                            Column {
                                                anchors.centerIn: parent
                                                spacing: 2
                                                visible: scanPresetRowHover.hovered
                                                         || scanPresetDelegate.preset.presetId
                                                            === root.applicationModel.selectedScanPresetId
                                                         || scanPresetDrag.active
                                                Repeater {
                                                    model: 3
                                                    Rectangle {
                                                        width: 12
                                                        height: 2
                                                        radius: 1
                                                        color: scanPresetDrag.active
                                                               ? root.centerColor
                                                               : root.secondaryTextColor
                                                    }
                                                }
                                            }

                                            MouseArea {
                                                id: scanPresetDrag
                                                objectName: "scanPresetDragHandler"
                                                anchors.fill: parent
                                                acceptedButtons: Qt.LeftButton
                                                hoverEnabled: true
                                                preventStealing: true
                                                cursorShape: drag.active
                                                             ? Qt.ClosedHandCursor
                                                             : Qt.OpenHandCursor
                                                drag.target: scanPresetDragPreview
                                                drag.threshold: Qt.styleHints.startDragDistance

                                                onPressed: {
                                                    const position = scanPresetDelegate.mapToItem(
                                                                       scanPresetList, 4, 0)
                                                    scanPresetDragPreview.x = position.x
                                                    scanPresetDragPreview.y = position.y
                                                    root.applicationModel.selectScanPreset(
                                                                scanPresetDelegate.preset.presetId)
                                                    scanPresetNameField.text =
                                                            scanPresetDelegate.preset.name
                                                }
                                                onPositionChanged: function(mouse) {
                                                    if (drag.active) {
                                                        if (!scanPresetDelegate.dragSessionActive) {
                                                            scanPresetDelegate.dragSessionActive = true
                                                            scanPresetDelegate.dragOccurred = true
                                                            root.scanPresetDragActive = true
                                                        }
                                                        const point = scanPresetDragHandle.mapToItem(
                                                                          scanPresetList,
                                                                          mouse.x,
                                                                          mouse.y)
                                                        root.scanPresetDragListY = point.y
                                                    }
                                                }
                                                onReleased: {
                                                    if (scanPresetDelegate.dragSessionActive) {
                                                        scanPresetDragPreview.Drag.drop()
                                                        scanPresetDelegate.dragSessionActive = false
                                                    }
                                                    root.scanPresetDragActive = false
                                                    Qt.callLater(function() {
                                                        scanPresetDelegate.dragOccurred = false
                                                    })
                                                }
                                                onCanceled: {
                                                    if (scanPresetDelegate.dragSessionActive) {
                                                        scanPresetDragPreview.Drag.cancel()
                                                        scanPresetDelegate.dragSessionActive = false
                                                    }
                                                    root.scanPresetDragActive = false
                                                    Qt.callLater(function() {
                                                        scanPresetDelegate.dragOccurred = false
                                                    })
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: scanPresetDragPreview
                                            parent: scanPresetList
                                            visible: scanPresetDelegate.dragSessionActive
                                            width: Math.max(80, scanPresetList.width - 12)
                                            height: scanPresetDelegate.height
                                            radius: 4
                                            z: 100
                                            opacity: 0.9
                                            color: "#29425f"
                                            border.color: root.centerColor
                                            border.width: 1

                                            Drag.active: scanPresetDelegate.dragSessionActive
                                            Drag.source: scanPresetDelegate
                                            Drag.keys: ["scan-preset-row"]
                                            Drag.hotSpot.x: width - 15
                                            Drag.hotSpot.y: height / 2

                                            Label {
                                                anchors.fill: parent
                                                anchors.leftMargin: 10
                                                anchors.rightMargin: 10
                                                text: scanPresetDelegate.preset.name
                                                color: root.primaryTextColor
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                                font.pixelSize: 11
                                            }
                                        }

                                        Shortcut {
                                            enabled: scanPresetDelegate.dragSessionActive
                                            sequence: "Escape"
                                            onActivated: {
                                                scanPresetDragPreview.Drag.cancel()
                                                scanPresetDelegate.dragSessionActive = false
                                                root.scanPresetDragActive = false
                                            }
                                        }

                                        DropArea {
                                            id: scanPresetDrop
                                            anchors.fill: parent
                                            keys: ["scan-preset-row"]
                                            onEntered: function(drag) {
                                                scanPresetDelegate.dropPlacement =
                                                        drag.y < height / 2 ? "before" : "after"
                                            }
                                            onPositionChanged: function(drag) {
                                                scanPresetDelegate.dropPlacement =
                                                        drag.y < height / 2 ? "before" : "after"
                                            }
                                            onExited: scanPresetDelegate.dropPlacement = ""
                                            onDropped: function(drop) {
                                                const source = drop.source
                                                const movedId = source ? source.preset.presetId : ""
                                                const placement = scanPresetDelegate.dropPlacement
                                                scanPresetDelegate.dropPlacement = ""
                                                if (source && movedId !== scanPresetDelegate.preset.presetId
                                                        && root.applicationModel.moveScanPreset(
                                                            movedId,
                                                            scanPresetDelegate.preset.presetId,
                                                            placement)) {
                                                    root.applicationModel.selectScanPreset(movedId)
                                                    drop.acceptProposedAction()
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: scanPresetDrop.containsDrag
                                                     && scanPresetDelegate.dropPlacement.length > 0
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.top: scanPresetDelegate.dropPlacement === "before"
                                                         ? parent.top : undefined
                                            anchors.bottom: scanPresetDelegate.dropPlacement === "after"
                                                            ? parent.bottom : undefined
                                            height: 2
                                            color: root.centerColor
                                            z: 5
                                        }
                                    }

                                    Timer {
                                        interval: 16
                                        repeat: true
                                        running: root.scanPresetDragActive
                                        onTriggered: {
                                            const edge = 34
                                            const maximum = Math.max(
                                                                0,
                                                                scanPresetList.contentHeight
                                                                - scanPresetList.height)
                                            if (root.scanPresetDragListY >= 0
                                                    && root.scanPresetDragListY < edge) {
                                                scanPresetList.contentY = Math.max(
                                                    0, scanPresetList.contentY - 8)
                                            } else if (root.scanPresetDragListY
                                                       > scanPresetList.height - edge) {
                                                scanPresetList.contentY = Math.min(
                                                    maximum, scanPresetList.contentY + 8)
                                            }
                                        }
                                    }

                                    Label {
                                        anchors.centerIn: parent
                                        visible: scanPresetList.count === 0
                                        text: qsTr("No saved presets")
                                        color: root.secondaryTextColor
                                    }
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }

            ColumnLayout {
                id: settingsSidebarContent
                visible: root.applicationModel.sidebarMode
                         === root.sidebarModeSettings
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                spacing: 10

                onVisibleChanged: {
                    if (visible)
                        root.applicationModel.revalidateDsdFmeBinaryPath()
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Settings")
                        color: root.primaryTextColor
                        font.bold: true
                        font.pixelSize: 16
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        implicitHeight: 28
                        text: qsTr("Close")
                        onClicked: root.applicationModel.setSidebarMode(
                                       root.sidebarModeNone)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: recordingSettingsContent.implicitHeight + 20
                    radius: 5
                    color: "#111a2b"
                    border.color: root.panelBorderColor

                    ColumnLayout {
                        id: recordingSettingsContent
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 7

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Recording")
                            color: root.primaryTextColor
                            font.bold: true
                            font.pixelSize: 12
                        }

                        ThemedCheckBox {
                            objectName: "skipQuietRecordingPartsCheckBox"
                            Layout.fillWidth: true
                            implicitHeight: root.controlHeight
                            text: qsTr("Skip quiet parts")
                            checked: root.applicationModel.skipQuietRecordingParts
                            onToggled: root.applicationModel.skipQuietRecordingParts = checked
                        }

                        ThemedCheckBox {
                            objectName: "recordScannerActivityCheckBox"
                            Layout.fillWidth: true
                            implicitHeight: root.controlHeight
                            text: qsTr("Record scanner activity")
                            checked: root.applicationModel.recordScannerActivity
                            onToggled: root.applicationModel.recordScannerActivity = checked
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Label {
                                objectName: "recordingPreRollLabel"
                                text: qsTr("Pre-roll (s)")
                                color: root.primaryTextColor
                            }
                            SpinBox {
                                objectName: "recordingPreRollSpinBox"
                                from: 0
                                to: 10
                                value: root.applicationModel.recordingPreRollSeconds
                                onValueModified: root.applicationModel.recordingPreRollSeconds = value
                            }
                            Label {
                                objectName: "recordingTailLabel"
                                text: qsTr("Tail (s)")
                                color: root.primaryTextColor
                            }
                            SpinBox {
                                objectName: "recordingTailSpinBox"
                                from: 0
                                to: 30
                                value: root.applicationModel.recordingTailSeconds
                                onValueModified: root.applicationModel.recordingTailSeconds = value
                            }
                        }

                        TextField {
                            objectName: "recordingsFolderField"
                            Layout.fillWidth: true
                            text: root.applicationModel.recordingsFolder
                            Accessible.name: qsTr("WAV recordings folder")
                            onEditingFinished: root.applicationModel.setRecordingsFolder(text)
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            Button {
                                objectName: "browseRecordingsFolderButton"
                                text: qsTr("Browse")
                                onClicked: root.applicationFileDialogs.selectRecordingDirectory()
                            }

                            Button {
                                objectName: "openRecordingsFolderButton"
                                text: qsTr("Open folder")
                                enabled: root.applicationModel.recordingsFolderValid
                                onClicked: root.applicationModel.openRecordingsFolder()
                            }
                        }

                        Label {
                            objectName: "recordingsFolderStatus"
                            Layout.fillWidth: true
                            text: root.applicationModel.recordingsFolderStatus
                            color: root.applicationModel.recordingsFolderValid
                                   ? "#6ee7b7" : root.listeningColor
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: ppmCalibrationContent.implicitHeight + 20
                    radius: 5
                    color: "#111a2b"
                    border.color: root.panelBorderColor

                    ColumnLayout {
                        id: ppmCalibrationContent
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 7

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("SDR calibration")
                            color: root.primaryTextColor
                            font.bold: true
                            font.pixelSize: 12
                        }

                        Label {
                            objectName: "ppmCorrectionDisplay"
                            Layout.fillWidth: true
                            text: qsTr("PPM correction: %1%2")
                                      .arg(root.applicationModel.ppmCorrection >= 0
                                           ? "+" : "")
                                      .arg(Math.round(
                                               root.applicationModel.ppmCorrection))
                            color: root.secondaryTextColor
                            font.pixelSize: 11
                        }

                        Button {
                            objectName: "automaticPpmButton"
                            Layout.fillWidth: true
                            text: root.applicationModel.ppmCalibrationRunning
                                  ? qsTr("Cancel") : qsTr("Auto PPM")
                            enabled: root.applicationModel.ppmCalibrationRunning
                                     || (root.applicationModel.automaticPpmCalibrationSupported
                                         && !root.applicationModel.runtimeBusy)
                            Accessible.name: text
                            Accessible.description: qsTr(
                                "Measure RTL-SDR oscillator correction using its internal test counter")
                            onClicked: {
                                if (root.applicationModel.ppmCalibrationRunning)
                                    root.applicationModel.cancelAutomaticPpmCalibration()
                                else
                                    root.applicationModel.startAutomaticPpmCalibration()
                            }
                        }

                        ProgressBar {
                            objectName: "automaticPpmProgress"
                            Layout.fillWidth: true
                            from: 0
                            to: 100
                            value: root.applicationModel.ppmCalibrationProgressPercent
                            visible: root.applicationModel.ppmCalibrationStatus
                                     !== "idle"
                        }

                        Label {
                            objectName: "automaticPpmStatus"
                            Layout.fillWidth: true
                            text: root.applicationModel.ppmCalibrationStatus
                            visible: text !== "idle"
                            color: root.applicationModel.ppmCalibrationStatus
                                   === "failed"
                                   ? "#f6ad55"
                                   : (root.applicationModel.ppmCalibrationStatus
                                      === "completed"
                                      ? "#6ee7b7"
                                      : root.secondaryTextColor)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !root.applicationModel.automaticPpmCalibrationSupported
                                     && !root.applicationModel.ppmCalibrationRunning
                            text: qsTr("Requires an RTL-SDR exposing test mode and frequency correction.")
                            color: root.secondaryTextColor
                            font.pixelSize: 9
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumHeight: dsdFmeSettingsContent.implicitHeight + 20
                    radius: 5
                    color: "#111a2b"
                    border.color: root.panelBorderColor

                    ColumnLayout {
                        id: dsdFmeSettingsContent
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 7

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("External decoder")
                            color: root.primaryTextColor
                            font.bold: true
                            font.pixelSize: 12
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("DSD-FME binary")
                            color: root.secondaryTextColor
                            font.pixelSize: 10
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            TextField {
                                id: dsdFmeBinaryPathField
                                objectName: "dsdFmeBinaryPathField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("No explicit binary configured")
                                text: root.applicationModel.dsdFmeBinaryPath
                                Accessible.name: qsTr("DSD-FME executable path")
                                Accessible.description: qsTr(
                                    "Path to the DSD-FME executable")
                                onTextEdited: root.applicationModel.setDsdFmeBinaryPath(text)
                                onEditingFinished: root.applicationModel.setDsdFmeBinaryPath(text)
                            }

                            Button {
                                text: qsTr("Browse")
                                Accessible.name: qsTr("Browse for DSD-FME executable")
                                onClicked: root.applicationFileDialogs.selectDsdFmeExecutable()
                            }

                            Button {
                                text: qsTr("Clear")
                                enabled: root.applicationModel.dsdFmeBinaryPath.length > 0
                                Accessible.name: qsTr("Clear DSD-FME executable path")
                                onClicked: root.applicationModel.setDsdFmeBinaryPath("")
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.applicationModel.dsdFmeBinaryStatus
                            color: root.applicationModel.dsdFmeBinaryValid
                                   ? "#6ee7b7"
                                   : (root.applicationModel.dsdFmeBinaryPath.length === 0
                                      ? root.secondaryTextColor : root.listeningColor)
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            Accessible.name: qsTr("DSD-FME binary status")
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }

            ColumnLayout {
                id: consoleSidebarContent
                objectName: "consoleSidebarContent"
                visible: root.applicationModel.sidebarMode
                         === root.sidebarModeConsole
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                spacing: 7

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Console")
                        color: root.primaryTextColor
                        font.bold: true
                        font.pixelSize: 16
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        implicitHeight: 28
                        text: qsTr("Close")
                        onClicked: root.applicationModel.setSidebarMode(
                                       root.sidebarModeNone)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Button {
                        objectName: "clearConsoleButton"
                        implicitHeight: 28
                        text: qsTr("Clear")
                        enabled: root.applicationModel.applicationLog.entryCount > 0
                        onClicked: root.applicationModel.applicationLog.clear()
                    }

                    Button {
                        objectName: "copySelectedConsoleButton"
                        implicitHeight: 28
                        text: qsTr("Copy Selected")
                        enabled: consoleText.selectedText.length > 0
                        onClicked: consoleText.copy()
                    }

                    Button {
                        objectName: "copyAllConsoleButton"
                        implicitHeight: 28
                        text: qsTr("Copy All")
                        enabled: consoleText.length > 0
                        onClicked: {
                            const oldStart = consoleText.selectionStart
                            const oldEnd = consoleText.selectionEnd
                            consoleText.selectAll()
                            consoleText.copy()
                            consoleText.select(oldStart, oldEnd)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    ComboBox {
                        id: consoleSeverityFilter
                        objectName: "consoleSeverityFilter"
                        Layout.fillWidth: true
                        implicitHeight: 30
                        model: [qsTr("All"), qsTr("Info+"),
                                qsTr("Warnings+"), qsTr("Errors")]
                        currentIndex: root.applicationModel.applicationLog.minimumSeverity
                        onActivated: root.applicationModel.applicationLog.minimumSeverity =
                                         currentIndex
                        Accessible.name: qsTr("Console severity filter")
                    }

                    ThemedCheckBox {
                        id: consoleAutoScroll
                        objectName: "consoleAutoScroll"
                        text: qsTr("Auto-scroll")
                        checked: true
                        onToggled: {
                            if (checked && consoleFlick.atBottom)
                                Qt.callLater(consoleFlick.positionAtBottom)
                        }
                    }
                }

                Flickable {
                    id: consoleFlick
                    objectName: "consoleFlick"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: Math.max(width, consoleText.implicitWidth)
                    contentHeight: Math.max(height, consoleText.implicitHeight)
                    ScrollBar.horizontal: ScrollBar {}
                    ScrollBar.vertical: ScrollBar {}

                    readonly property bool atBottom:
                        contentHeight <= height
                        || contentY >= contentHeight - height - 2
                    property bool followBottom: true

                    function positionAtBottom() {
                        contentY = Math.max(0, contentHeight - height)
                        followBottom = true
                    }

                    onMovementEnded: followBottom = atBottom
                    onFlickEnded: followBottom = atBottom
                    onContentYChanged: {
                        if (moving || flicking)
                            followBottom = atBottom
                    }

                    TextArea {
                        id: consoleText
                        objectName: "consoleText"
                        width: Math.max(consoleFlick.width, implicitWidth)
                        height: Math.max(consoleFlick.height, implicitHeight)
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.NoWrap
                        textFormat: TextEdit.PlainText
                        text: root.applicationModel.applicationLog.formattedText
                        color: root.primaryTextColor
                        selectionColor: "#365a85"
                        selectedTextColor: "#ffffff"
                        font.family: "monospace"
                        font.pixelSize: 10
                        background: Rectangle {
                            color: "#0b111e"
                            border.color: root.panelBorderColor
                            radius: 4
                        }
                        onTextChanged: {
                            if (consoleAutoScroll.checked
                                    && consoleFlick.followBottom)
                                Qt.callLater(consoleFlick.positionAtBottom)
                        }
                    }
                }
            }

            Item {
                id: sidebarResizeHandle
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 10
                z: 4

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 3
                    width: 2
                    height: Math.min(72, parent.height - 24)
                    radius: 1
                    color: sidebarResizeMouse.pressed
                           ? root.centerColor
                           : (sidebarResizeMouse.containsMouse
                              ? "#61728e" : "#33435f")
                }

                MouseArea {
                    id: sidebarResizeMouse
                    property real dragStartX: 0
                    property real dragStartWidth: 0

                    anchors.fill: parent
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.SizeHorCursor
                    Accessible.name: qsTr("Resize sidebar panel")

                    onPressed: function(mouse) {
                        const pointer = sidebarResizeHandle.mapToItem(
                            root.contentItem, mouse.x, mouse.y)
                        dragStartX = pointer.x
                        dragStartWidth = sidebarPanel.width
                    }

                    onPositionChanged: function(mouse) {
                        if (!pressed)
                            return
                        const pointer = sidebarResizeHandle.mapToItem(
                            root.contentItem, mouse.x, mouse.y)
                        if (root.applicationModel.sidebarMode
                                === root.sidebarModeBookmarks) {
                            root.applicationModel.setBookmarksPanelWidth(
                                dragStartWidth + pointer.x - dragStartX)
                        } else if (root.applicationModel.sidebarMode
                                   === root.sidebarModeScan) {
                            root.applicationModel.setScanPanelWidth(
                                dragStartWidth + pointer.x - dragStartX)
                        } else if (root.applicationModel.sidebarMode
                                   === root.sidebarModeSettings) {
                            root.applicationModel.setSettingsPanelWidth(
                                dragStartWidth + pointer.x - dragStartX)
                        } else if (root.applicationModel.sidebarMode
                                   === root.sidebarModeConsole) {
                            root.applicationModel.setConsolePanelWidth(
                                dragStartWidth + pointer.x - dragStartX)
                        }
                    }

                    onReleased: {
                        if (root.applicationModel.sidebarMode
                                === root.sidebarModeBookmarks) {
                            root.applicationModel.commitBookmarksPanelWidth()
                        } else if (root.applicationModel.sidebarMode
                                   === root.sidebarModeScan) {
                            root.applicationModel.commitScanPanelWidth()
                        } else if (root.applicationModel.sidebarMode
                                   === root.sidebarModeSettings) {
                            root.applicationModel.commitSettingsPanelWidth()
                        } else if (root.applicationModel.sidebarMode
                                   === root.sidebarModeConsole) {
                            root.applicationModel.commitConsolePanelWidth()
                        }
                    }
                }
            }
        }

        Item {
            id: displayWorkspace

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.minimumHeight: spectrumMinimumHeight
                                  + waterfallMinimumHeight
                                  + splitterHeight
                                  + viewportScrollbarHeight

            readonly property real splitterHeight: 12
            readonly property real viewportScrollbarHeight: 16
            readonly property real spectrumMinimumHeight: 135
            readonly property real waterfallMinimumHeight: 150
            readonly property real panelHeight: Math.max(
                0,
                height - splitterHeight - viewportScrollbarHeight)
            readonly property real spectrumPaneHeight: Math.max(
                spectrumMinimumHeight,
                Math.min(
                    panelHeight - waterfallMinimumHeight,
                    panelHeight * root.applicationModel.spectrumWaterfallSplitRatio))

            FrequencyPane {
                id: spectrumPane
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: displayWorkspace.spectrumPaneHeight
                Layout.fillWidth: true
                applicationModel: root.applicationModel
                heading: root.applicationModel.recordedAudioSource ? qsTr("Audio spectrum") : qsTr("Spectrum")
                detail: root.applicationModel.recordedAudioSource ? qsTr("0 Hz to Nyquist") : qsTr("Wheel: center · Ctrl: filter · Shift: listen")
            }

            Item {
                id: splitterHandle
                anchors.left: parent.left
                anchors.right: parent.right
                y: spectrumPane.height
                height: displayWorkspace.splitterHeight
                z: 4

                Rectangle {
                    anchors.centerIn: parent
                    width: Math.min(72, parent.width - 32)
                    height: 2
                    radius: 1
                    color: splitterMouseArea.pressed
                           ? "#61dafb"
                           : (splitterMouseArea.containsMouse ? "#61728e" : "#33435f")
                    opacity: splitterMouseArea.pressed
                             ? 0.95
                             : (splitterMouseArea.containsMouse ? 0.9 : 0.72)
                }

                MouseArea {
                    id: splitterMouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.SizeVerCursor
                    Accessible.name: qsTr("Spectrum and waterfall splitter")
                    Accessible.description: qsTr(
                        "Drag vertically to resize the spectrum and waterfall panels")

                    onPositionChanged: function(mouse) {
                        if (!pressed)
                            return
                        const pointer = splitterHandle.mapToItem(
                            displayWorkspace, mouse.x, mouse.y)
                        const maximumSpectrumHeight = displayWorkspace.panelHeight
                                                     - displayWorkspace.waterfallMinimumHeight
                        const requestedSpectrumHeight = Math.max(
                            displayWorkspace.spectrumMinimumHeight,
                            Math.min(
                                maximumSpectrumHeight,
                                pointer.y - displayWorkspace.splitterHeight / 2))
                        root.applicationModel.setSpectrumWaterfallSplitRatio(
                            requestedSpectrumHeight / displayWorkspace.panelHeight)
                    }

                    onReleased: root.applicationModel.commitSpectrumWaterfallSplitRatio()
                }
            }

            FrequencyPane {
                id: waterfallPane
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: splitterHandle.bottom
                anchors.bottom: viewportPanScrollbar.top
                applicationModel: root.applicationModel
                heading: root.applicationModel.recordedAudioSource ? qsTr("Audio waterfall") : qsTr("Waterfall")
                detail: root.applicationModel.recordedAudioSource ? qsTr("Audio spectrum history") : qsTr("Wheel: zoom · Ctrl: filter · Shift: listen")
                waterfallInteraction: true
            }

            ScrollBar {
                id: viewportPanScrollbar
                objectName: "sharedViewportPanScrollbar"
                anchors.left: waterfallPane.left
                anchors.right: waterfallPane.right
                anchors.bottom: parent.bottom
                height: displayWorkspace.viewportScrollbarHeight
                orientation: Qt.Horizontal
                policy: ScrollBar.AlwaysOn
                size: root.applicationModel.displayPanPageSize
                position: root.applicationModel.displayPanPosition
                enabled: root.applicationModel.displayPanEnabled
                opacity: enabled ? 1.0 : 0.0
                Accessible.name: qsTr("Spectrum and waterfall frequency pan")
                Accessible.description: qsTr(
                    "Pan the shared visible spectrum and waterfall frequency range")
                onPositionChanged: root.applicationModel.setDisplayPanPosition(position)
            }
        }

        ReceiverControlsPane {
            id: receiverControlsPane
            Layout.fillHeight: true
            Layout.preferredWidth: requestedLayoutWidth
            Layout.minimumWidth: requestedLayoutWidth
            Layout.maximumWidth: requestedLayoutWidth
            applicationModel: root.applicationModel
            workspaceAvailableWidth: root.width
                                     - mainLayout.anchors.margins * 2
                                     - (sidebarPanel.visible
                                        ? sidebarPanel.width + mainLayout.spacing
                                        : 0)
            panelColor: root.panelColor
            panelBorderColor: root.panelBorderColor
            primaryTextColor: root.primaryTextColor
            secondaryTextColor: root.secondaryTextColor
            accentColor: root.centerColor
            denseLayout: root.denseLayout

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Receiver controls")
                        color: root.primaryTextColor
                        font.bold: true
                        font.pixelSize: root.denseLayout ? 15 : 18
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Label {
                        text: root.applicationModel.recordedIqSource
                              ? qsTr("RECORDED IQ")
                              : (root.applicationModel.recordedAudioSource
                                 ? qsTr("RECORDED AUDIO")
                              : (root.applicationModel.mockMode
                                 ? qsTr("MOCK")
                                 : (root.applicationModel.backendReady
                                    ? qsTr("HARDWARE READY") : qsTr("NO DEVICE"))))
                        color: root.applicationModel.backendReady ? "#68d391" : "#f6ad55"
                        font.bold: true
                        font.pixelSize: 9
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Device state:")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    Label {
                        Layout.maximumWidth: 170
                        text: root.applicationModel.deviceState
                        color: root.primaryTextColor
                        font.pixelSize: 11
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: qsTr("Reception:")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    Label {
                        text: root.applicationModel.receiverRunning ? qsTr("Running")
                                                                     : qsTr("Stopped")
                        color: root.applicationModel.receiverRunning ? "#68d391" : "#cbd5e0"
                        font.pixelSize: 11
                        font.bold: true
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Button {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        text: qsTr("Start reception")
                        enabled: root.applicationModel.backendReady &&
                                 !root.applicationModel.receiverRunning &&
                                 !root.applicationModel.runtimeBusy
                        onClicked: root.applicationModel.startReception()
                    }
                    Button {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        text: qsTr("Stop reception")
                        enabled: root.applicationModel.receiverRunning &&
                                 !root.applicationModel.runtimeBusy
                        onClicked: root.applicationModel.stopReception()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("FFT resolution")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    ComboBox {
                        id: spectrumFftSizeControl
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        model: root.applicationModel.spectrumFftSizeOptions
                        currentIndex: find(String(root.applicationModel.spectrumFftSize))
                        displayText: String(root.applicationModel.spectrumFftSize)
                        enabled: !root.applicationModel.runtimeBusy
                        Accessible.name: qsTr("Spectrum and waterfall FFT resolution")
                        Accessible.description: qsTr(
                                                    "Changes horizontal frequency resolution while retaining the full capture bandwidth.")
                        onActivated: function(index) {
                            root.applicationModel.setSpectrumFftSize(Number(textAt(index)))
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.applicationModel.spectrumFftSize !==
                             root.applicationModel.effectiveSpectrumFftSize
                    text: qsTr("Requested: %1 · Effective: %2")
                              .arg(root.applicationModel.spectrumFftSize)
                              .arg(root.applicationModel.effectiveSpectrumFftSize)
                    color: "#f6ad55"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                    Accessible.name: qsTr("Requested and effective FFT resolution")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Approx. resolution")
                        color: root.secondaryTextColor
                        font.pixelSize: 10
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: Number(root.applicationModel.spectrumHertzPerBin).toFixed(1)
                              + qsTr(" Hz/bin")
                        color: root.primaryTextColor
                        font.pixelSize: 10
                        Accessible.name: qsTr("Approximate frequency resolution")
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.applicationModel.spectrumFftSize >= 32768
                    text: qsTr("High FFT resolutions increase CPU use. Waterfall history remains reduced and bounded; backend allocation limits may require a reported effective fallback.")
                    color: "#f6ad55"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Waterfall aggregation")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    ComboBox {
                        objectName: "waterfallAggregationSelector"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        model: [
                            { text: qsTr("Original"), value: "original" },
                            { text: qsTr("Average"), value: "average" }
                        ]
                        textRole: "text"
                        currentIndex: waterfallPane.waterfallAggregation === "average" ? 1 : 0
                        Accessible.name: qsTr("Waterfall aggregation")
                        Accessible.description: qsTr("Original preserves brief peaks; Average smooths each live timestamp interval in linear power without delaying the next row")
                        onActivated: function(index) {
                            waterfallPane.waterfallAggregation = model[index].value
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Visible history")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    ComboBox {
                        objectName: "visibleWaterfallHistorySelector"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        model: root.applicationModel.visibleWaterfallHistoryOptions
                        currentIndex: find(String(root.applicationModel.visibleWaterfallHistorySeconds)
                                           + qsTr(" s"))
                        displayText: String(root.applicationModel.visibleWaterfallHistorySeconds)
                                     + qsTr(" s")
                        enabled: !root.applicationModel.runtimeBusy
                        Accessible.name: qsTr("Visible waterfall history")
                        Accessible.description: qsTr("Maps this many seconds of timestamped history to the full waterfall height.")
                        onActivated: function(index) {
                            if (textAt(index) === qsTr("Custom…"))
                                customWaterfallHistoryDialog.open()
                            else
                                root.applicationModel.setVisibleWaterfallHistorySeconds(
                                    Number(textAt(index).split(" ")[0]))
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !waterfallPane.historyFitsMemoryBudget
                    text: qsTr("Requested %1 s; the %2 MiB history budget retains %3 s at %4 stored bins.")
                          .arg(root.applicationModel.visibleWaterfallHistorySeconds)
                          .arg(Number(root.applicationModel.waterfallHistoryMemoryBudgetBytes
                                      / (1024 * 1024)).toFixed(0))
                          .arg(Number(waterfallPane.retainedHistoryCapacitySeconds).toFixed(1))
                          .arg(waterfallPane.storedHistoryBins)
                    color: "#f6ad55"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Label {
                        text: qsTr("Device selection")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        text: qsTr("Refresh")
                        enabled: root.applicationModel.deviceDiscoveryAvailable &&
                                 !root.applicationModel.runtimeBusy
                        onClicked: root.applicationModel.refreshDevices()
                    }
                    Button {
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        text: qsTr("Clear")
                        enabled: !root.applicationModel.mockMode &&
                                 root.applicationModel.backendReady &&
                                 !root.applicationModel.receiverRunning &&
                                 !root.applicationModel.runtimeBusy
                        onClicked: root.applicationModel.clearDeviceSelection()
                    }
                }

                ComboBox {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: root.controlHeight
                    model: root.applicationModel.deviceDisplayNames
                    currentIndex: root.applicationModel.selectedDeviceIndex
                    displayText: root.applicationModel.mockMode
                                 ? qsTr("Mock receiver — no hardware")
                                 : (currentIndex >= 0
                                    ? currentText
                                    : qsTr("No device selected"))
                    enabled: root.applicationModel.deviceDiscoveryAvailable &&
                             count > 0 &&
                             !root.applicationModel.receiverRunning &&
                             !root.applicationModel.runtimeBusy
                    onActivated: function(index) {
                        root.applicationModel.selectDeviceIndex(index)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: root.applicationModel.deviceCapabilitySummary
                    color: root.secondaryTextColor
                    font.pixelSize: 9
                    elide: Text.ElideRight
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Capture bandwidth")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    ComboBox {
                        id: captureBandwidthControl
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        model: root.applicationModel.captureBandwidthOptions
                        editable: false
                        currentIndex: find(
                                          (Number(root.applicationModel.requestedCaptureBandwidth)
                                           / 1000000).toFixed(3) + qsTr(" MS/s"))
                        displayText: (Number(root.applicationModel.requestedCaptureBandwidth)
                                      / 1000000).toFixed(3) + qsTr(" MS/s")
                        enabled: root.applicationModel.backendReady &&
                                 !root.applicationModel.runtimeBusy &&
                                 !root.applicationModel.scannerOwnsTuning &&
                                 count > 0
                        Accessible.name: qsTr("SDR capture bandwidth")
                        Accessible.description: qsTr(
                                                    "Width of spectrum received from the SDR at once. Changes while receiving restart the receiver.")
                        onActivated: function(index) {
                            if (textAt(index) === qsTr("Custom…"))
                                customCaptureBandwidthDialog.open()
                            else
                                root.applicationModel.setCaptureBandwidthText(textAt(index))
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Requested / effective")
                        color: root.secondaryTextColor
                        font.pixelSize: 10
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: (Number(root.applicationModel.requestedCaptureBandwidth)
                               / 1000000).toFixed(3) + qsTr(" / ")
                              + (Number(root.applicationModel.effectiveSampleRate)
                                 / 1000000).toFixed(3) + qsTr(" MS/s")
                        color: root.primaryTextColor
                        font.pixelSize: 10
                        Accessible.name: qsTr("Requested and effective SDR sample rates")
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.applicationModel.requestedCaptureBandwidth >= 4000000
                    text: qsTr("Higher capture bandwidth can increase SDR USB and CPU load; verify stability on this hardware.")
                    color: "#f6ad55"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Audio device")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        model: root.applicationModel.audioDeviceDisplayNames
                        currentIndex: root.applicationModel.selectedAudioDeviceIndex
                        displayText: currentIndex >= 0
                                     ? currentText
                                     : qsTr("No audio output")
                        enabled: count > 0 && !root.applicationModel.runtimeBusy
                        Accessible.name: qsTr("Audio output device")
                        onActivated: function(index) {
                            root.applicationModel.selectAudioDeviceIndex(index)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Volume")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    Slider {
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        stepSize: 1
                        value: root.applicationModel.audioVolume
                        enabled: root.applicationModel.audioReady
                        Accessible.name: qsTr("Receiver audio volume")
                        onMoved: root.applicationModel.setAudioVolume(
                                     Math.round(value))
                    }
                    Label {
                        text: root.applicationModel.audioVolume + qsTr("%")
                        color: root.secondaryTextColor
                        font.pixelSize: 10
                    }
                    ThemedCheckBox {
                        implicitHeight: root.controlHeight
                        text: qsTr("Mute")
                        checked: root.applicationModel.audioMuted
                        enabled: root.applicationModel.audioReady
                        onClicked: root.applicationModel.setAudioMuted(checked)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("Demodulation mode")
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }
                    ComboBox {
                        Layout.preferredWidth: 150
                        Layout.minimumWidth: 0
                        implicitHeight: root.controlHeight
                        model: root.applicationModel.demodulationModes
                        currentIndex: root.applicationModel.demodulationModeIndex
                        enabled: root.applicationModel.backendReady &&
                                 !root.applicationModel.runtimeBusy
                        onActivated: function(index) {
                            root.applicationModel.setDemodulationModeIndex(index)
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Modes: AM · NFM · WFM · USB · LSB · DMR/P25")
                    color: root.secondaryTextColor
                    font.pixelSize: 9
                    elide: Text.ElideRight
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: receiverControlsPane.controlsAvailableWidth < 380
                             ? 1 : 3
                    columnSpacing: 8
                    rowSpacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: qsTr("Filter width")
                            color: root.secondaryTextColor
                            font.pixelSize: 11
                        }
                        ComboBox {
                            id: filterWidthSelector
                            objectName: "filterWidthSelector"
                            readonly property string formattedFilterWidth:
                                (Number(root.applicationModel.filterWidth) / 1000).toFixed(
                                    Number(root.applicationModel.filterWidth) % 1000 === 0 ? 0 : 2)
                                + qsTr(" kHz")
                            readonly property int presetIndex:
                                find(formattedFilterWidth)

                            Layout.fillWidth: true
                            Layout.minimumWidth: implicitWidth
                            Layout.preferredWidth: implicitWidth
                            implicitHeight: root.controlHeight
                            implicitContentWidthPolicy: ComboBox.WidestText
                            model: root.applicationModel.filterWidthOptions
                            currentIndex: presetIndex >= 0 ? presetIndex : count - 1
                            displayText: presetIndex >= 0
                                         ? formattedFilterWidth
                                         : qsTr("Custom · %1").arg(formattedFilterWidth)
                            enabled: root.applicationModel.backendReady &&
                                     !root.applicationModel.runtimeBusy
                            onActivated: function(index) {
                                if (textAt(index) === qsTr("Custom…"))
                                    customFilterWidthDialog.open()
                                else
                                    root.applicationModel.setFilterWidthText(textAt(index))
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: qsTr("Gain (dB)")
                            color: root.secondaryTextColor
                            font.pixelSize: 11
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Slider {
                                id: gainSlider
                                Layout.fillWidth: true
                                Layout.minimumWidth:
                                    receiverControlsPane.controlsAvailableWidth < 380
                                    ? 100 : 120
                                from: root.applicationModel.minimumGain
                                to: root.applicationModel.maximumGain
                                stepSize: root.applicationModel.gainStep
                                // Discovery initially exposes a 0–0 range. Keep this
                                // binding dependent on the later capability limits so
                                // the restored request is reapplied once they arrive.
                                value: Math.max(
                                           gainSlider.from,
                                           Math.min(
                                               gainSlider.to,
                                               root.applicationModel.requestedGain))
                                enabled: root.applicationModel.backendReady &&
                                         root.applicationModel.gainSupported &&
                                         !root.applicationModel.runtimeBusy
                                onMoved: root.applicationModel.previewGain(value)
                                onPressedChanged: {
                                    if (!pressed)
                                        root.applicationModel.commitGain(value)
                                }
                            }
                        }
                        Label {
                            id: gainStatusText
                            objectName: "gainStatusText"
                            Layout.fillWidth: true
                            Layout.minimumHeight: gainStatusMetrics.height * 2
                            Layout.preferredHeight: gainStatusMetrics.height * 2
                            text: root.applicationModel.gainSupported
                                  ? qsTr("Requested: %1 dB · Effective: %2 dB").arg(
                                        Number(root.applicationModel.requestedGain).toFixed(1)).arg(
                                        Number(root.applicationModel.gain).toFixed(1))
                                  : qsTr("Gain control unsupported")
                            color: root.secondaryTextColor
                            font.pixelSize: 9
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        TextMetrics {
                            id: gainStatusMetrics
                            font: gainStatusText.font
                            text: qsTr("M")
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: root.applicationModel.ppmCorrectionSupported
                                  ? qsTr("PPM corr.")
                                  : qsTr("PPM unsupported")
                            color: root.secondaryTextColor
                            font.pixelSize: 11
                        }
                        SpinBox {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: root.controlHeight
                            from: -200
                            to: 200
                            value: Math.round(root.applicationModel.ppmCorrection)
                            enabled: root.applicationModel.backendReady &&
                                     root.applicationModel.ppmCorrectionSupported &&
                                     !root.applicationModel.runtimeBusy
                            onValueModified: root.applicationModel.setPpmCorrection(value)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Squelch: ") + root.applicationModel.squelchStateText
                        color: root.secondaryTextColor
                        font.pixelSize: 11
                    }
                    Slider {
                        Layout.fillWidth: true
                        from: -160
                        to: 0
                        value: root.applicationModel.squelchLevel
                        enabled: !root.applicationModel.squelchDisabled &&
                                 root.applicationModel.backendReady &&
                                 !root.applicationModel.runtimeBusy
                        onMoved: root.applicationModel.setSquelchLevel(value)
                    }
                    Label {
                        text: Math.round(root.applicationModel.squelchLevel) + qsTr(" dB")
                        color: root.secondaryTextColor
                        font.pixelSize: 10
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Button {
                        Layout.preferredWidth: 52
                        implicitHeight: root.controlHeight
                        text: qsTr("Auto")
                        enabled: root.applicationModel.autoSquelchAvailable &&
                                 !root.applicationModel.scannerOwnsTuning &&
                                 !root.applicationModel.runtimeBusy
                        onClicked: root.applicationModel.autoSquelch()
                    }
                    ThemedCheckBox {
                        objectName: "disableSquelchCheckBox"
                        Layout.fillWidth: true
                        implicitHeight: root.controlHeight
                        text: qsTr("Disable squelch")
                        checked: root.applicationModel.squelchDisabled
                        enabled: root.applicationModel.backendReady &&
                                 !root.applicationModel.runtimeBusy
                        onClicked: root.applicationModel.setSquelchDisabled(checked)
                    }
                }

        }
    }

    footer: ToolBar {
        id: footerBar
        readonly property int edgeMargin: 10
        readonly property int controlHeight: 30
        readonly property int transportButtonSize: 30
        readonly property int regionSpacing: 8
        readonly property bool playbackError:
            root.applicationModel.recordingLoaded &&
            (root.applicationModel.statusText.toLowerCase().indexOf("failed") >= 0 ||
             root.applicationModel.statusText.toLowerCase().indexOf("error") >= 0)
        readonly property string playbackStateText:
            !root.applicationModel.recordingLoaded ? ""
            : playbackError ? qsTr("Error")
            : root.applicationModel.recordingPlaying ? qsTr("Playing")
            : root.applicationModel.recordingPaused ? qsTr("Paused")
            : root.applicationModel.recordingDurationFrames > 0 &&
              root.applicationModel.recordingPositionFrames >=
                  root.applicationModel.recordingDurationFrames ? qsTr("EOF")
            : qsTr("Stopped")

        implicitHeight: 76
        background: Rectangle {
            color: "#101827"
            border.color: root.panelBorderColor
        }

        Item {
            anchors.fill: parent

            RowLayout {
                id: footerTopRow
                objectName: "footerTopRow"
                anchors.left: parent.left
                anchors.leftMargin: footerBar.edgeMargin
                anchors.right: parent.right
                anchors.rightMargin: footerBar.edgeMargin
                anchors.top: parent.top
                anchors.topMargin: 4
                height: footerBar.controlHeight
                spacing: footerBar.regionSpacing

                Rectangle {
                    id: playbackTransportGroup
                    objectName: "playbackTransportGroup"
                    Layout.preferredWidth: footerBar.transportButtonSize * 4 + 14
                    Layout.minimumWidth: Layout.preferredWidth
                    Layout.preferredHeight: footerBar.controlHeight
                    radius: 5
                    color: "#121d2e"
                    border.color: "#2f4059"

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 2

                        FooterDarkButton {
                            Layout.preferredWidth: footerBar.transportButtonSize
                            Layout.minimumWidth: footerBar.transportButtonSize
                            text: "⏮"
                            enabled: root.applicationModel.recordingLoaded
                            Accessible.name: qsTr("Restart recording playback")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onClicked: root.applicationModel.restartRecordingPlayback()
                        }
                        FooterDarkButton {
                            Layout.preferredWidth: footerBar.transportButtonSize
                            Layout.minimumWidth: footerBar.transportButtonSize
                            text: root.applicationModel.recordingPlaying ? "⏸" : "▶"
                            stateActive: root.applicationModel.recordingPlaying
                            enabled: root.applicationModel.recordingLoaded
                            Accessible.name: root.applicationModel.recordingPlaying
                                             ? qsTr("Pause recording playback")
                                             : qsTr("Play recording playback")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onClicked: root.applicationModel.toggleRecordingPlayback()
                        }
                        FooterDarkButton {
                            Layout.preferredWidth: footerBar.transportButtonSize
                            Layout.minimumWidth: footerBar.transportButtonSize
                            text: "■"
                            enabled: root.applicationModel.recordingLoaded
                            Accessible.name: qsTr("Stop and rewind recording playback")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onClicked: root.applicationModel.stopRecordingPlayback()
                        }
                        FooterDarkButton {
                            Layout.preferredWidth: footerBar.transportButtonSize
                            Layout.minimumWidth: footerBar.transportButtonSize
                            text: "⏏"
                            enabled: !root.applicationModel.receiverRunning &&
                                     !root.applicationModel.runtimeBusy
                            Accessible.name: root.applicationModel.recordingLoaded
                                             ? qsTr("Eject recording")
                                             : qsTr("Load recording")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onClicked: {
                                if (root.applicationModel.recordingLoaded)
                                    root.applicationModel.ejectRecording()
                                else
                                    root.openRecordingFileDialog()
                            }
                        }
                    }
                }

                Rectangle {
                    id: recordingInformationGroup
                    objectName: "recordingInformationGroup"
                    Layout.fillWidth: true
                    Layout.minimumWidth: root.denseLayout ? 104 : 150
                    Layout.preferredHeight: footerBar.controlHeight
                    radius: 5
                    color: "#121d2e"
                    border.color: "#2a3a52"
                    clip: true

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 9
                        anchors.rightMargin: 7
                        spacing: 7

                        Label {
                            id: recordingFileName
                            objectName: "recordingFileName"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 28
                            text: root.applicationModel.recordingLoaded
                                  ? root.applicationModel.recordingDisplayName
                                  : qsTr("No recording loaded")
                            color: root.applicationModel.recordingLoaded
                                   ? root.primaryTextColor : "#78869b"
                            font.pixelSize: 10
                            elide: Text.ElideMiddle
                            maximumLineCount: 1
                            ToolTip.visible: recordingFileNameHover.containsMouse
                            ToolTip.text: root.applicationModel.recordingLoaded
                                          ? root.applicationModel.recordingDisplayName
                                          : qsTr("No recording loaded")
                            MouseArea {
                                id: recordingFileNameHover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                        }

                        Label {
                            id: recordingPlaybackState
                            objectName: "recordingPlaybackState"
                            visible: root.applicationModel.recordingLoaded
                            Layout.maximumWidth: root.denseLayout ? 58 : 92
                            text: footerBar.playbackStateText
                            color: footerBar.playbackError ? "#fecaca"
                                   : root.applicationModel.recordingPlaying ? "#8ee7ad"
                                   : root.secondaryTextColor
                            font.bold: true
                            font.pixelSize: 9
                            elide: Text.ElideRight
                            ToolTip.visible: recordingPlaybackStateHover.containsMouse &&
                                             footerBar.playbackError
                            ToolTip.text: root.applicationModel.statusText
                            MouseArea {
                                id: recordingPlaybackStateHover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                        }
                    }
                }

                Rectangle {
                    id: recordingControlsGroup
                    objectName: "recordingControlsGroup"
                    Layout.preferredWidth: recordingControls.implicitWidth + 10
                    Layout.minimumWidth: Layout.preferredWidth
                    Layout.preferredHeight: footerBar.controlHeight
                    radius: 5
                    color: "#121d2e"
                    border.color: "#2a3a52"

                    RowLayout {
                        id: recordingControls
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 5

                        FooterDarkButton {
                            id: audioRecordingButton
                            objectName: "audioRecordingButton"
                            text: root.applicationModel.recordingActive
                                  ? qsTr("Stop audio") : qsTr("Record audio")
                            stateActive: root.applicationModel.recordingActive
                            enabled: root.applicationModel.recordingActive ||
                                     root.applicationModel.recordingCanStart
                            Accessible.name: text
                            onClicked: {
                                if (root.applicationModel.recordingActive)
                                    root.applicationModel.stopAudioRecording()
                                else
                                    root.applicationModel.startAudioRecording()
                            }
                        }

                        Label {
                            objectName: "audioRecordingStatus"
                            visible: root.applicationModel.recordingActive
                            text: (root.applicationModel.recordingWriting
                                   ? qsTr("REC ") : qsTr("ARMED ")) +
                                  root.applicationModel.recordingElapsedText
                            color: root.applicationModel.recordingWriting
                                   ? "#fca5a5" : "#f6c66b"
                            font.bold: true
                            font.pixelSize: 9
                        }

                        FooterDarkButton {
                            id: iqRecordingButton
                            objectName: "iqRecordingButton"
                            text: root.applicationModel.iqRecordingActive
                                  ? qsTr("Stop IQ") : qsTr("Record IQ")
                            stateActive: root.applicationModel.iqRecordingActive
                            enabled: root.applicationModel.iqRecordingActive ||
                                     root.applicationModel.iqRecordingCanStart
                            Accessible.name: text
                            onClicked: {
                                if (root.applicationModel.iqRecordingActive)
                                    root.applicationModel.stopIqRecording()
                                else
                                    root.applicationModel.startIqRecording()
                            }
                        }

                        Label {
                            objectName: "iqRecordingStatusLabel"
                            visible: root.applicationModel.iqRecordingActive
                            text: root.applicationModel.iqRecordingDroppedSamples > 0
                                  ? qsTr("IQ %1 · drop %2")
                                      .arg(root.applicationModel.iqRecordingElapsedText)
                                      .arg(root.applicationModel.iqRecordingDroppedSamples)
                                  : qsTr("IQ %1").arg(
                                        root.applicationModel.iqRecordingElapsedText)
                            color: root.applicationModel.iqRecordingDroppedSamples > 0
                                   ? "#fecaca" : root.secondaryTextColor
                            font.bold: true
                            font.pixelSize: 9
                        }

                        Label {
                            objectName: "scannerRecordingStatusLabel"
                            visible: root.applicationModel.scannerRecordingArmed ||
                                     root.applicationModel.scannerRecordingWriting
                            text: root.applicationModel.scannerRecordingWriting
                                  ? qsTr("SCAN REC") : qsTr("SCAN ARMED")
                            color: root.applicationModel.scannerRecordingWriting
                                   ? "#fecaca" : "#f6c66b"
                            font.bold: true
                            font.pixelSize: 9
                        }

                        Rectangle {
                            id: runtimeServiceIndicator
                            objectName: "runtimeServiceIndicator"
                            implicitWidth: 8
                            implicitHeight: 8
                            radius: 4
                            color: root.applicationModel.decoderRunning ||
                                   root.applicationModel.audioReady
                                   ? "#68d391" : "#66758c"
                            ToolTip.visible: runtimeServiceHover.containsMouse
                            ToolTip.text: root.applicationModel.demodulationModeIndex === 5
                                          ? (root.applicationModel.decoderRunning
                                             ? qsTr("Decoder running")
                                             : qsTr("Decoder stopped"))
                                          : (root.applicationModel.audioReady
                                             ? qsTr("Audio ready")
                                             : root.applicationModel.audioStatusText)
                            MouseArea {
                                id: runtimeServiceHover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                        }

                        Label {
                            id: runtimeServiceStatus
                            objectName: "runtimeServiceStatus"
                            visible: !root.denseLayout
                            text: root.applicationModel.demodulationModeIndex === 5
                                  ? (root.applicationModel.decoderRunning
                                     ? qsTr("Decoder") : qsTr("Decoder off"))
                                  : (root.applicationModel.audioReady
                                     ? qsTr("Audio") : qsTr("Audio off"))
                            color: root.applicationModel.decoderRunning ||
                                   root.applicationModel.audioReady
                                   ? "#8ee7ad" : root.secondaryTextColor
                            font.pixelSize: 9
                        }
                    }
                }
            }

            RowLayout {
                id: recordingSeekRow
                objectName: "recordingSeekRow"
                anchors.left: parent.left
                anchors.leftMargin: footerBar.edgeMargin
                anchors.right: parent.right
                anchors.rightMargin: footerBar.edgeMargin
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 5
                height: 30
                spacing: 8

                Label {
                    id: recordingElapsedTime
                    objectName: "recordingElapsedTime"
                    Layout.minimumWidth: root.applicationModel.recordingDurationFrames >=
                                         root.applicationModel.recordingSampleRate * 3600
                                         ? 54 : 34
                    horizontalAlignment: Text.AlignLeft
                    text: recordingSeekSlider.pressed
                          ? root.formatRecordingFrames(recordingSeekSlider.previewFrame)
                          : root.applicationModel.recordingPositionText
                    color: recordingSeekSlider.enabled ? "#c5d1e2" : "#667287"
                    font.family: "monospace"
                    font.pixelSize: 10
                }

                Slider {
                    id: recordingSeekSlider
                    objectName: "recordingSeekSlider"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 160
                    implicitHeight: 26
                    leftPadding: 8
                    rightPadding: 8
                    topPadding: 4
                    bottomPadding: 4
                    activeFocusOnTab: true
                    from: 0
                    to: Math.max(1, root.applicationModel.recordingDurationFrames)
                    enabled: root.applicationModel.recordingLoaded &&
                             root.applicationModel.recordingCanSeek &&
                             root.applicationModel.recordedAudioSource &&
                             root.applicationModel.recordingDurationFrames > 0
                    property double previewFrame: root.applicationModel.recordingPositionFrames
                    value: pressed ? previewFrame : root.applicationModel.recordingPositionFrames
                    Accessible.name: qsTr("Recording playback position")
                    Accessible.description: qsTr("Seek recorded playback")
                    ToolTip.visible: pressed || hovered
                    ToolTip.text: root.applicationModel.recordingCanSeek
                                  ? root.formatRecordingFrames(pressed ? previewFrame : value)
                                  : qsTr("This recording cannot be seeked")

                    background: Rectangle {
                        x: recordingSeekSlider.leftPadding
                        y: recordingSeekSlider.topPadding +
                           (recordingSeekSlider.availableHeight - height) / 2
                        width: recordingSeekSlider.availableWidth
                        height: 7
                        radius: 3.5
                        color: recordingSeekSlider.enabled ? "#27364c" : "#1a2332"
                        border.color: recordingSeekSlider.activeFocus
                                      ? root.centerColor : "#3c4d65"
                        border.width: recordingSeekSlider.activeFocus ? 2 : 1

                        Rectangle {
                            width: parent.width * recordingSeekSlider.visualPosition
                            height: parent.height
                            radius: parent.radius
                            color: recordingSeekSlider.enabled ? "#3ca8d0" : "#354154"
                        }
                    }

                    handle: Rectangle {
                        x: recordingSeekSlider.leftPadding +
                           recordingSeekSlider.visualPosition *
                           (recordingSeekSlider.availableWidth - width)
                        y: recordingSeekSlider.topPadding +
                           (recordingSeekSlider.availableHeight - height) / 2
                        implicitWidth: recordingSeekSlider.pressed ? 17 : 15
                        implicitHeight: recordingSeekSlider.pressed ? 17 : 15
                        radius: width / 2
                        color: !recordingSeekSlider.enabled ? "#536074"
                               : recordingSeekSlider.pressed ? "#eefbff"
                               : recordingSeekSlider.hovered ? "#9ee9ff"
                               : "#73d4f2"
                        border.color: recordingSeekSlider.activeFocus
                                      ? "#ffffff" : "#17364b"
                        border.width: recordingSeekSlider.activeFocus ? 2 : 1
                    }

                    onMoved: {
                        previewFrame = value
                        if (!pressed)
                            root.applicationModel.seekRecordingPlayback(
                                Math.round(previewFrame))
                    }
                    onPressedChanged: {
                        if (pressed) {
                            previewFrame = root.applicationModel.recordingPositionFrames
                        } else if (enabled) {
                            root.applicationModel.seekRecordingPlayback(
                                Math.round(previewFrame))
                        }
                    }
                }

                Label {
                    id: recordingDurationTime
                    objectName: "recordingDurationTime"
                    Layout.minimumWidth: root.applicationModel.recordingDurationFrames >=
                                         root.applicationModel.recordingSampleRate * 3600
                                         ? 54 : 34
                    horizontalAlignment: Text.AlignRight
                    text: root.applicationModel.recordingDurationText
                    color: recordingSeekSlider.enabled ? "#c5d1e2" : "#667287"
                    font.family: "monospace"
                    font.pixelSize: 10
                }
            }
        }
    }
}
