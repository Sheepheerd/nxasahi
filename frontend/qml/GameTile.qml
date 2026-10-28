// One game in the row: its icon, rounded, lifted and outlined when chosen; a badge for its state, a progress ring while it builds
import QtQuick
import QtQuick.Controls
import "."

Item {
    id: t
    property var game
    property bool selected: false
    signal clicked()
    signal doubleClicked()
    signal menu()
    width: 226; height: 300

    Rectangle {   // the glow behind the chosen tile
        anchors.centerIn: art; width: art.width * 1.08 + 22; height: art.height * 1.08 + 22; radius: Theme.radius + 12
        color: "transparent"; border.width: 4; border.color: Theme.accent
        opacity: t.selected ? 0.9 : 0
        Behavior on opacity { NumberAnimation { duration: 140 } }
    }
    Rectangle {
        id: art
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: 30 }
        width: 226; height: 226; radius: Theme.radius
        color: Theme.panel; clip: true
        scale: t.selected ? 1.08 : 1.0
        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        Image {
            anchors.fill: parent; source: t.game ? t.game.icon : ""; fillMode: Image.PreserveAspectCrop
            sourceSize: Qt.size(320, 320); asynchronous: true; smooth: true; mipmap: true
            visible: status === Image.Ready
            opacity: t.game && (t.game.ready || t.game.building) ? 1 : 0.45
        }
        Text {   // no icon yet: the name's initials
            anchors.centerIn: parent; visible: !t.game || t.game.icon === ""
            text: t.game ? t.game.name.split(" ").filter(w => w.length).slice(0, 2).map(w => w[0]).join("") : ""
            color: Theme.dim; font.pixelSize: 64; font.bold: true
        }
        Rectangle {   // building: the image dims under a ring
            anchors.fill: parent; color: "#99000000"; visible: t.game && t.game.building
            Ring { anchors.centerIn: parent; width: 96; height: 96; value: t.game ? t.game.progress : -1 }
        }
        Rectangle {
            visible: t.game && (t.game.status === "new" || t.game.status === "failed" || t.game.status === "playing")
            anchors { left: parent.left; top: parent.top; margins: 12 }
            radius: 12; height: 26; width: bl.implicitWidth + 20
            color: t.game && t.game.status === "failed" ? Theme.bad : t.game && t.game.status === "playing" ? Theme.good : Theme.accent
            Label { id: bl; anchors.centerIn: parent; color: "#0b0e14"; font.pixelSize: 13; font.bold: true
                    text: t.game ? (t.game.status === "failed" ? "FAILED" : t.game.status === "playing" ? "▶ PLAYING" : "NEW") : "" }
        }
    }
    MouseArea {
        anchors.fill: art; acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (m) => { if (m.button === Qt.RightButton) t.menu(); else t.clicked() }
        onDoubleClicked: (m) => { if (m.button === Qt.LeftButton) t.doubleClicked() }
    }
}
