// A progress bar: value 0..1, or below 0 for an indeterminate sweep
import QtQuick
import "."

Rectangle {
    id: b
    property real value: 0
    height: 8; radius: 4; color: "#2a3148"; clip: true
    Rectangle {
        height: parent.height; radius: 4; color: Theme.accent
        width: b.value < 0 ? parent.width * 0.3 : parent.width * Math.max(0, Math.min(1, b.value))
        x: b.value < 0 ? sweep.pos * (b.width * 1.3) - b.width * 0.3 : 0
        Behavior on width { enabled: b.value >= 0; NumberAnimation { duration: 200 } }
    }
    QtObject { id: sweep; property real pos: 0; NumberAnimation on pos { from: 0; to: 1; duration: 1300; loops: Animation.Infinite; running: b.value < 0 && b.visible } }
}
