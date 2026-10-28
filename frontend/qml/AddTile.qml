// The last stop in the row: add a game
import QtQuick
import QtQuick.Controls
import "."

Item {
    id: t
    property bool selected: false
    signal clicked()
    width: 226; height: 300
    Rectangle {
        anchors.centerIn: box; width: box.width * 1.08 + 22; height: box.height * 1.08 + 22; radius: Theme.radius + 12
        color: "transparent"; border.width: 4; border.color: Theme.accent; opacity: t.selected ? 0.9 : 0
        Behavior on opacity { NumberAnimation { duration: 140 } }
    }
    Rectangle {
        id: box
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 30 }
        width: 226; height: 226; radius: Theme.radius
        color: ma.containsMouse ? Theme.panelHi : Theme.panel
        border.color: Theme.line; border.width: 2
        scale: t.selected ? 1.08 : 1.0
        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Text { anchors.centerIn: parent; anchors.verticalCenterOffset: -12; text: "+"; color: Theme.accent; font.pixelSize: 96; font.weight: Font.Light }
        Text { anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 26 } text: "Add game"; color: Theme.dim; font.pixelSize: 17 }
    }
    MouseArea { id: ma; anchors.fill: box; hoverEnabled: true; onClicked: t.clicked() }
}
