// A round top-bar button with a glyph and an optional count badge
import QtQuick
import QtQuick.Controls
import "."

AbstractButton {
    id: b
    property string glyph
    property int badge: 0
    property string tip
    implicitWidth: 46; implicitHeight: 46
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    ToolTip.visible: hovered && tip !== ""; ToolTip.text: tip; ToolTip.delay: 500
    background: Rectangle { radius: width / 2; color: b.hovered ? Theme.panelHi : Theme.panel; border.color: Theme.line }
    contentItem: Text { text: b.glyph; color: Theme.text; font.pixelSize: 21; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
    Rectangle {
        visible: b.badge > 0
        anchors { right: parent.right; top: parent.top; rightMargin: -2; topMargin: -2 }
        width: 20; height: 20; radius: 10; color: Theme.good
        Text { anchors.centerIn: parent; text: b.badge; color: "#06210f"; font.pixelSize: 12; font.bold: true }
    }
}
