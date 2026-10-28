// A button hint in the bottom bar: the button as a round glyph, then what it does
import QtQuick
import QtQuick.Controls
import "."

Row {
    property string button
    property string label
    spacing: 9
    Rectangle {
        width: 30; height: 30; radius: 15; color: Theme.text
        anchors.verticalCenter: parent.verticalCenter
        Text { anchors.centerIn: parent; text: parent.parent.button; color: "#0b0e14"; font.pixelSize: 16; font.bold: true }
    }
    Label { text: parent.label; color: Theme.text; font.pixelSize: 17; anchors.verticalCenter: parent.verticalCenter }
}
