// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: pane
    objectName: "receiverControlsPane"

    required property var applicationModel
    required property real workspaceAvailableWidth
    required property color panelColor
    required property color panelBorderColor
    required property color primaryTextColor
    required property color secondaryTextColor
    required property color accentColor
    property bool denseLayout: false
    property real minimumWorkspaceWidth: 360
    readonly property real minimumPaneWidth: 330
    readonly property real maximumPaneWidth: 520
    readonly property real toggleWidth: 34
    readonly property real paneSpacing: denseLayout ? 4 : 6
    readonly property real boundedPaneWidth: Math.max(
        minimumPaneWidth,
        Math.min(maximumPaneWidth,
                 applicationModel.receiverControlsPaneWidth))
    readonly property bool responsiveCollapsed:
        workspaceAvailableWidth < minimumWorkspaceWidth
                                  + minimumPaneWidth
                                  + toggleWidth
                                  + paneSpacing
    readonly property bool expanded:
        applicationModel.receiverControlsPaneOpen && !responsiveCollapsed
    readonly property real requestedLayoutWidth:
        expanded ? boundedPaneWidth + paneSpacing + toggleWidth : toggleWidth
    readonly property real controlsAvailableWidth:
        receiverControlsScroll.availableWidth

    default property alias controls: receiverControlsContent.data

    implicitWidth: requestedLayoutWidth
    implicitHeight: 320

    Rectangle {
        id: receiverControlsPanel
        objectName: "receiverControlsPanel"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: pane.boundedPaneWidth
        visible: pane.expanded
        radius: 8
        color: pane.panelColor
        border.color: pane.panelBorderColor
        clip: true

        ScrollView {
            id: receiverControlsScroll
            objectName: "receiverControlsScroll"
            anchors.fill: parent
            anchors.leftMargin: pane.denseLayout ? 9 : 14
            anchors.rightMargin: pane.denseLayout ? 7 : 12
            anchors.topMargin: pane.denseLayout ? 7 : 12
            anchors.bottomMargin: pane.denseLayout ? 7 : 12
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            ColumnLayout {
                id: receiverControlsContent
                objectName: "receiverControlsContent"
                width: receiverControlsScroll.availableWidth
                spacing: pane.denseLayout ? 2 : 6
            }
        }

        Item {
            id: receiverControlsResizeHandle
            objectName: "receiverControlsResizeHandle"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 10
            z: 4

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 3
                width: 2
                height: Math.max(0, Math.min(72, parent.height - 24))
                radius: 1
                color: receiverControlsResizeMouse.pressed
                       ? pane.accentColor
                       : (receiverControlsResizeMouse.containsMouse
                          ? "#61728e" : "#33435f")
            }

            MouseArea {
                id: receiverControlsResizeMouse
                objectName: "receiverControlsResizeMouse"
                property real dragStartX: 0
                property real dragStartWidth: 0

                anchors.fill: parent
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.SizeHorCursor
                Accessible.name: qsTr("Resize receiver controls pane")
                Accessible.description: qsTr(
                    "Drag horizontally to resize the receiver controls pane")

                onPressed: function(mouse) {
                    const pointer = receiverControlsResizeHandle.mapToItem(
                        pane.parent, mouse.x, mouse.y)
                    dragStartX = pointer.x
                    dragStartWidth = receiverControlsPanel.width
                }

                onPositionChanged: function(mouse) {
                    if (!pressed)
                        return
                    const pointer = receiverControlsResizeHandle.mapToItem(
                        pane.parent, mouse.x, mouse.y)
                    pane.applicationModel.setReceiverControlsPaneWidth(
                        dragStartWidth + dragStartX - pointer.x)
                }

                onReleased:
                    pane.applicationModel.commitReceiverControlsPaneWidth()
            }
        }
    }

    Button {
        id: receiverControlsToggleButton
        objectName: "receiverControlsToggleButton"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: pane.toggleWidth
        height: 88
        checkable: true
        checked: pane.expanded
        text: pane.expanded ? "›" : "‹"
        font.pixelSize: 20
        Accessible.name: pane.expanded
                         ? qsTr("Collapse receiver controls")
                         : qsTr("Expand receiver controls")
        Accessible.description: pane.responsiveCollapsed
                                ? qsTr("The receiver controls pane will reopen when the window is wide enough")
                                : qsTr("Toggle the right-side receiver controls pane")
        ToolTip.visible: hovered
        ToolTip.text: pane.responsiveCollapsed
                      ? qsTr("Receiver controls need a wider window")
                      : Accessible.name + qsTr(" (Ctrl+Shift+R)")
        onClicked: pane.applicationModel.toggleReceiverControlsPane()

        background: Rectangle {
            radius: 5
            color: receiverControlsToggleButton.down ? "#314968"
                   : receiverControlsToggleButton.hovered ? "#25364f"
                   : pane.panelColor
            border.color: receiverControlsToggleButton.activeFocus
                          ? pane.accentColor : pane.panelBorderColor
            border.width: receiverControlsToggleButton.activeFocus ? 2 : 1
        }

        contentItem: Text {
            text: receiverControlsToggleButton.text
            color: pane.primaryTextColor
            font: receiverControlsToggleButton.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
