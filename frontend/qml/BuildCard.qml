// A build in progress (or just finished): its title, stage, progress, the output on request, stop / dismiss
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

Rectangle {
    id: c
    required property string tid
    required property string title
    required property string detail
    required property real progress
    required property string state
    required property string log
    property bool showLog: false
    height: col.implicitHeight + 24
    radius: 16; color: Theme.panel; border.color: c.state === "failed" ? Theme.bad : Theme.line
    opacity: 0.97
    ColumnLayout {
        id: col
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
        spacing: 6
        RowLayout {
            Label { text: c.title; color: Theme.text; font.pixelSize: 15; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
            SmallButton { text: c.showLog ? "Hide output" : "Output"; onClicked: c.showLog = !c.showLog }
            SmallButton { text: c.state === "running" ? "Stop" : "Close"; onClicked: c.state === "running" ? backend.cancelBuild(c.tid) : backend.dismissBuild(c.tid) }
        }
        Label { text: c.detail; color: c.state === "failed" ? Theme.bad : Theme.dim; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true; maximumLineCount: 2; wrapMode: Text.Wrap }
        Bar { Layout.fillWidth: true; value: c.progress; visible: c.state !== "failed" }
        ScrollView {
            visible: c.showLog; Layout.fillWidth: true; Layout.preferredHeight: 180
            TextArea {
                text: c.log; readOnly: true; wrapMode: Text.WrapAnywhere; color: Theme.dim; font.family: "monospace"; font.pixelSize: 11
                background: Rectangle { color: "#0b0e14"; radius: 8 }
                onTextChanged: cursorPosition = length
            }
        }
    }
}
