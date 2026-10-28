// A small text button for cards and dialogs
import QtQuick
import QtQuick.Controls
import "."

Button {
    id: b
    focusPolicy: Qt.NoFocus
    padding: 6; leftPadding: 12; rightPadding: 12
    contentItem: Text { text: b.text; color: Theme.text; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
    background: Rectangle { radius: 10; color: b.down ? Theme.line : b.hovered ? Theme.panelHi : "#262c3e"; border.color: Theme.line }
}
