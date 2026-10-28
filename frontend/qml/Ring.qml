// A progress ring: value 0..1, or below 0 for a spinner
import QtQuick
import "."

Item {
    id: r
    property real value: -1
    property real shown: value < 0 ? 0.25 : value
    Behavior on shown { NumberAnimation { duration: 200 } }
    Canvas {
        id: c
        anchors.fill: parent
        rotation: r.value < 0 ? spin.angle : -90
        onPaint: {
            const ctx = getContext("2d"); ctx.reset()
            const w = width / 2, rr = w - 7
            ctx.lineWidth = 8; ctx.lineCap = "round"
            ctx.strokeStyle = "#33ffffff"; ctx.beginPath(); ctx.arc(w, w, rr, 0, 2 * Math.PI); ctx.stroke()
            ctx.strokeStyle = Theme.accent; ctx.beginPath(); ctx.arc(w, w, rr, 0, 2 * Math.PI * Math.max(0.02, r.shown)); ctx.stroke()
        }
        Connections { target: r; function onShownChanged() { c.requestPaint() } }
    }
    QtObject { id: spin; property real angle: 0; NumberAnimation on angle { from: 0; to: 360; duration: 1100; loops: Animation.Infinite; running: r.value < 0 && r.visible } }
    Text { anchors.centerIn: parent; visible: r.value >= 0; text: Math.round(r.value * 100) + "%"; color: "white"; font.pixelSize: 18; font.bold: true }
}
