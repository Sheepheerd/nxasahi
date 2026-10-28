// A dialog button that a controller can reach: outlined when it has the focus, A / Return presses it
import QtQuick
import QtQuick.Controls
import "."

Button {
    id: b
    property bool danger: false
    padding: 12; leftPadding: 26; rightPadding: 26
    contentItem: Text { text: b.text; color: b.danger ? "white" : Theme.text; font.pixelSize: 16; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
    background: Rectangle {
        radius: 12; color: b.danger ? Theme.bad : b.hovered ? Theme.panelHi : "#262c3e"
        border.width: b.activeFocus ? 3 : 1; border.color: b.activeFocus ? Theme.accent : Theme.line
    }
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()
}
