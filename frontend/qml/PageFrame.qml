// A page over the home screen: a title, then its rows in a list that ↑ ↓ move through (section headers are skipped); B / Escape closes it
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

FocusScope {
    id: f
    property string title
    property string subtitle
    property string art: ""
    property alias rows: list.model          // a list of row objects (see the pages)
    property alias list: list
    property Component rowDelegate
    property bool capturing: false            // a page that is capturing input handles Escape itself
    signal closed()
    signal escaped()
    focus: true

    Rectangle {
        anchors.fill: parent
        gradient: Gradient { GradientStop { position: 0.0; color: Theme.bg1 } GradientStop { position: 1.0; color: Theme.bg0 } }
    }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = false }   // the home screen under it takes no clicks

    RowLayout {
        id: head
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 40 }
        spacing: 22
        Rectangle {
            visible: f.art !== ""; width: 96; height: 96; radius: 16; clip: true; color: Theme.panel
            Image { anchors.fill: parent; source: f.art; fillMode: Image.PreserveAspectCrop; sourceSize: Qt.size(192, 192) }
        }
        ColumnLayout {
            spacing: 4; Layout.fillWidth: true
            Label { text: f.title; color: Theme.text; font.pixelSize: 34; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
            Label { visible: f.subtitle !== ""; text: f.subtitle; color: Theme.dim; font.pixelSize: 16; elide: Text.ElideRight; Layout.fillWidth: true }
        }
        SmallButton { text: "Close"; onClicked: f.closed() }
    }

    ListView {
        id: list
        anchors { left: parent.left; right: parent.right; top: head.bottom; bottom: parent.bottom; leftMargin: 40; rightMargin: 40; topMargin: 24; bottomMargin: 84 }
        width: Math.min(parent.width - 80, 1100)
        spacing: 6; clip: true
        focus: true
        keyNavigationEnabled: true
        highlightMoveDuration: 120
        preferredHighlightBegin: 80; preferredHighlightEnd: height - 120; highlightRangeMode: ListView.ApplyRange
        delegate: f.rowDelegate
        property int dir: 1
        Keys.onUpPressed: (e) => { dir = -1; e.accepted = false }
        Keys.onDownPressed: (e) => { dir = 1; e.accepted = false }
        onCurrentIndexChanged: Qt.callLater(skip)
        function skip() {   // a section header is not a stop: go on in the same direction
            if (!model || currentIndex < 0) return
            const n = typeof model.length === "number" ? model.length : count
            const at = (i) => (typeof model.length === "number" ? model[i] : null)
            let i = currentIndex
            while (i >= 0 && i < n && at(i) && (at(i).type === "header" || at(i).type === "info")) i += dir
            if (i < 0 || i >= n) { i = currentIndex; while (i >= 0 && i < n && at(i) && (at(i).type === "header" || at(i).type === "info")) i -= dir }
            if (i >= 0 && i < n && i !== currentIndex) currentIndex = i
            if (currentItem) currentItem.forceActiveFocus()
        }
        Component.onCompleted: { currentIndex = 0; Qt.callLater(skip) }
        ScrollBar.vertical: ScrollBar { }
    }
    Keys.onEscapePressed: (e) => { if (f.capturing) f.escaped(); else f.closed(); e.accepted = true }
    Keys.onPressed: (e) => { if (e.key === Qt.Key_Minus || e.key === Qt.Key_X) { if (!f.capturing) { f.closed(); e.accepted = true } } }
}
