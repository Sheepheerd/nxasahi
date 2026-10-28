// The chosen game's state as a coloured pill
import QtQuick
import QtQuick.Controls
import "."

Rectangle {
    property string status
    readonly property var look: ({ ready: ["READY", Theme.good], building: ["BUILDING", Theme.accent], failed: ["BUILD FAILED", Theme.bad], new: ["NOT BUILT", Theme.warn],
                                   playing: ["PLAYING", Theme.good], add: ["NEW", Theme.accent2] })[status] || ["", Theme.dim]
    visible: look[0] !== ""
    radius: 11; height: 26; width: pl.implicitWidth + 22
    color: "transparent"; border.color: look[1]; border.width: 2
    Label { id: pl; anchors.centerIn: parent; text: parent.look[0]; color: parent.look[1]; font.pixelSize: 13; font.bold: true; font.letterSpacing: 1 }
}
