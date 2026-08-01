// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

import QtQuick
import QtQuick.Controls

CheckBox {
    id: control

    property color enabledTextColor: palette.windowText
    property color disabledTextColor: palette.disabled.windowText

    spacing: 8
    activeFocusOnTab: true

    contentItem: Text {
        id: label
        objectName: "themedCheckBoxLabel"
        leftPadding: control.indicator
                     ? control.indicator.width + control.spacing : 0
        rightPadding: control.rightPadding
        text: control.text
        color: control.enabled
               ? control.enabledTextColor : control.disabledTextColor
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
