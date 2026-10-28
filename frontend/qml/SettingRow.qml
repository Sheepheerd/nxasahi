// One row of a page, driven the same way by a controller, the keyboard and the mouse:
//   toggle  ← → / A switch it · choice  ← → / A step through choices · slider  ← → step · action  A run it · text  A edit, Return keep, Escape drop
//   bind    A then press the input to give, Backspace unbinds · info  shown only · header  a section title (skipped by the page)
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

FocusScope {
    id: r
    property string type: "info"
    property string title
    property string subtitle
    property var value                 // toggle: bool; choice: the chosen id; slider: number; text: string; bind / info: the text shown
    property var choices: []           // choice: [{id, name}]
    property real from: 0
    property real to: 1
    property real step: 0.05
    property string unit: ""
    property bool danger: false
    property bool live: false          // bind: this input is pressed now
    property bool listening: false     // bind: waiting for the input to give
    property bool current: false
    signal changed(var v)
    signal triggered()
    signal unbind()
    width: ListView.view ? ListView.view.width : 600
    height: type === "header" ? 52 : Math.max(64, col.implicitHeight + 22)
    activeFocusOnTab: type !== "header" && type !== "info"

    function choiceIndex() { for (let i = 0; i < choices.length; i++) if (choices[i].id === value) return i; return 0 }
    function choiceName() { return choices.length ? choices[choiceIndex()].name : "" }
    function stepChoice(d) { if (!choices.length) return; const i = (choiceIndex() + d + choices.length) % choices.length; changed(choices[i].id) }
    function act(d) {
        if (type === "toggle") changed(!value)
        else if (type === "choice") stepChoice(d)
        else if (type === "slider") changed(Math.max(from, Math.min(to, Math.round((value + d * step) / step) * step)))
        else if (type === "action" || type === "bind") triggered()
        else if (type === "text") { edit.text = value || ""; edit.forceActiveFocus(); edit.selectAll() }
    }

    Keys.onLeftPressed: (e) => { if (type === "toggle" || type === "choice" || type === "slider") { act(-1); e.accepted = true } else e.accepted = false }
    Keys.onRightPressed: (e) => { if (type === "toggle" || type === "choice" || type === "slider") { act(1); e.accepted = true } else e.accepted = false }
    Keys.onReturnPressed: (e) => { if (type !== "slider" && type !== "info" && type !== "header") { act(1); e.accepted = true } else e.accepted = false }
    Keys.onEnterPressed: (e) => Keys.onReturnPressed(e)
    Keys.onSpacePressed: (e) => Keys.onReturnPressed(e)
    Keys.onPressed: (e) => { if (type === "bind" && (e.key === Qt.Key_Backspace || e.key === Qt.Key_Delete)) { unbind(); e.accepted = true } }

    Label {
        visible: r.type === "header"
        anchors { left: parent.left; bottom: parent.bottom; leftMargin: 6; bottomMargin: 8 }
        text: r.title.toUpperCase(); color: Theme.accent; font.pixelSize: 14; font.bold: true; font.letterSpacing: 2
    }
    Rectangle {
        visible: r.type !== "header"
        anchors.fill: parent; anchors.bottomMargin: 4
        radius: 14
        color: r.activeFocus || r.current && r.ListView.view && r.ListView.view.activeFocus ? Theme.panelHi : Theme.panel
        border.width: r.activeFocus || edit.activeFocus ? 3 : 1
        border.color: r.activeFocus || edit.activeFocus ? Theme.accent : Theme.line
        RowLayout {
            anchors { fill: parent; leftMargin: 18; rightMargin: 16 }
            spacing: 14
            ColumnLayout {
                id: col
                Layout.fillWidth: true; spacing: 2
                Label { text: r.title; color: r.danger ? Theme.bad : Theme.text; font.pixelSize: 17; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                Label { visible: r.subtitle !== "" && r.type !== "text"; text: r.subtitle; color: Theme.dim; font.pixelSize: 13; wrapMode: Text.Wrap; Layout.fillWidth: true }
                TextField {
                    id: edit
                    visible: r.type === "text"; Layout.fillWidth: true
                    text: r.value || ""; placeholderText: r.subtitle
                    color: Theme.text; placeholderTextColor: Theme.dim; font.pixelSize: 14
                    background: Rectangle { radius: 8; color: "#10141d"; border.color: edit.activeFocus ? Theme.accent : Theme.line }
                    onAccepted: { r.changed(text); r.forceActiveFocus() }
                    Keys.onEscapePressed: (e) => { text = r.value || ""; r.forceActiveFocus(); e.accepted = true }
                }
            }
            // the value, by kind
            Rectangle {   // toggle
                visible: r.type === "toggle"
                width: 58; height: 32; radius: 16; color: r.value ? Theme.accent : "#3a4259"
                Rectangle { width: 26; height: 26; radius: 13; y: 3; x: r.value ? 29 : 3; color: "white"; Behavior on x { NumberAnimation { duration: 120 } } }
            }
            Row {   // choice
                visible: r.type === "choice"; spacing: 10
                Label { text: "‹"; color: Theme.dim; font.pixelSize: 22; anchors.verticalCenter: parent.verticalCenter }
                Label { text: r.choiceName(); color: Theme.text; font.pixelSize: 16; anchors.verticalCenter: parent.verticalCenter; width: Math.min(implicitWidth, 320); elide: Text.ElideRight }
                Label { text: "›"; color: Theme.dim; font.pixelSize: 22; anchors.verticalCenter: parent.verticalCenter }
            }
            Row {   // slider
                visible: r.type === "slider"; spacing: 12
                Bar { width: 200; anchors.verticalCenter: parent.verticalCenter; value: r.type === "slider" ? (r.value - r.from) / (r.to - r.from) : 0 }
                Label { text: r.type === "slider" ? (r.unit === "%" ? Math.round(r.value * 100) + " %" : r.value + r.unit) : ""; color: Theme.text; font.pixelSize: 16; width: 56 }
            }
            Rectangle {   // bind
                visible: r.type === "bind"
                width: 190; height: 36; radius: 10
                color: r.listening ? Theme.accent : r.live ? "#2b5a3d" : "#10141d"
                border.color: r.live ? Theme.good : Theme.line
                Label { anchors.centerIn: parent; text: r.listening ? "Press…" : (r.value || "Unbound"); color: r.listening ? "#0b0e14" : r.value ? Theme.text : Theme.dim; font.pixelSize: 15; font.bold: r.listening }
            }
            Label {   // info, action
                visible: r.type === "info" || r.type === "action"
                text: r.type === "action" ? "›" : (r.value === undefined ? "" : String(r.value))
                color: r.type === "action" ? Theme.dim : Theme.text; font.pixelSize: r.type === "action" ? 26 : 15
                elide: Text.ElideMiddle; Layout.maximumWidth: 520
            }
        }
        MouseArea {
            anchors.fill: parent; enabled: r.type !== "info" && r.type !== "text"
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (m) => { r.forceActiveFocus(); if (m.button === Qt.RightButton && r.type === "bind") r.unbind(); else r.act(1) }
        }
    }
}
