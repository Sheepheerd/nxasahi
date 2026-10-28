// nxasahi's game library: a console-style home screen. The games in a row (the newest played first, an Add tile last), the chosen one's name and state under it,
// the button hints at the bottom; a controller, the keyboard and the mouse all drive it (frontend/backend.py turns controller buttons into these keys):
//   ← → choose · A / Return play · X options · Y rebuild · + add a game · − settings · F11 fullscreen
// Pages (options, settings, controllers) open over the home screen and close with B / Escape. Files dropped on the window are added.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "."

ApplicationWindow {
    id: win
    width: 1280; height: 760
    minimumWidth: 860; minimumHeight: 540
    visible: true
    title: "nxasahi"
    color: Theme.bg0
    font.family: Theme.font

    property string page: ""          // "", "options", "settings", "controllers"
    property string pageTid: ""
    property string playingTid: ""
    readonly property int count: backend.gameCount
    readonly property bool onAdd: carousel.currentIndex === count

    Component.onCompleted: if (backend.config.library_fullscreen) win.showFullScreen()

    function openPage(p, tid) { pageTid = tid || ""; page = p }
    function closePage() { page = ""; carousel.forceActiveFocus() }
    function toggleFullscreen() { if (win.visibility === Window.FullScreen) win.showNormal(); else win.showFullScreen() }
    function current() { return carousel.currentItem && !onAdd ? carousel.currentItem.game : null }

    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.bg1 }
            GradientStop { position: 1.0; color: Theme.bg0 }
        }
        // the chosen game's art, very soft, behind everything
        Image {
            anchors.fill: parent; fillMode: Image.PreserveAspectCrop; opacity: 0.10
            source: win.current() ? win.current().icon : ""
            smooth: true; asynchronous: true
            Behavior on source { }
        }
        Rectangle { anchors.fill: parent; color: Theme.bg0; opacity: 0.55 }
    }

    // ---- top bar ----
    RowLayout {
        id: top
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 28 }
        spacing: 18
        Label { text: "nxasahi"; color: Theme.text; font.pixelSize: 26; font.weight: Font.Bold; font.letterSpacing: 1 }
        Label { text: count ? count + (count === 1 ? " game" : " games") : ""; color: Theme.dim; font.pixelSize: 15; Layout.alignment: Qt.AlignBottom; bottomPadding: 3 }
        Item { Layout.fillWidth: true }
        IconButton { glyph: "🎮"; badge: backend.padCount > 0 ? backend.padCount : 0; tip: "Controllers"; onClicked: win.openPage("controllers") }
        IconButton { glyph: "⚙"; tip: "Settings (−)"; onClicked: win.openPage("settings") }
        Label {
            id: clock; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold
            text: Qt.formatTime(new Date(), "hh:mm")
            Timer { interval: 5000; running: true; repeat: true; onTriggered: clock.text = Qt.formatTime(new Date(), "hh:mm") }
        }
    }

    // ---- the games ----
    ListView {
        id: carousel
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; verticalCenterOffset: -50 }
        height: 300
        orientation: ListView.Horizontal
        spacing: 26
        leftMargin: 70; rightMargin: 70
        model: backend.gameModel
        focus: win.page === ""
        keyNavigationEnabled: true
        highlightMoveDuration: 160
        preferredHighlightBegin: 70; preferredHighlightEnd: width - 330
        highlightRangeMode: ListView.ApplyRange
        boundsBehavior: Flickable.StopAtBounds
        clip: false
        delegate: GameTile {
            required property int index
            required property var model
            game: model
            selected: ListView.isCurrentItem && carousel.activeFocus
            onClicked: { carousel.currentIndex = index; carousel.forceActiveFocus() }
            onDoubleClicked: { win.playingTid = model.tid; backend.play(model.tid) }
            onMenu: { carousel.currentIndex = index; win.openPage("options", model.tid) }
        }
        footer: Item {
            width: 226 + 26; height: 300
            AddTile {
                x: 26
                selected: win.onAdd && carousel.activeFocus
                onClicked: addDialog.open()
            }
        }
        // the Add tile sits in the footer: one more stop to the right of the last game
        Keys.onRightPressed: (e) => { if (currentIndex === count - 1 || count === 0) { currentIndex = count; positionViewAtEnd() } else incrementCurrentIndex() }
        Keys.onLeftPressed: (e) => { if (currentIndex >= count) currentIndex = Math.max(0, count - 1); else decrementCurrentIndex() }
        Keys.onPressed: (e) => {
            const g = win.current()
            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Space) { if (win.onAdd || !g) addDialog.open(); else { win.playingTid = g.tid; backend.play(g.tid) } e.accepted = true }
            else if (e.key === Qt.Key_X && g) { win.openPage("options", g.tid); e.accepted = true }
            else if (e.key === Qt.Key_Y && g) { backend.rebuild(g.tid); e.accepted = true }
            else if (e.key === Qt.Key_Plus || (e.key === Qt.Key_O && (e.modifiers & Qt.ControlModifier))) { addDialog.open(); e.accepted = true }
            else if (e.key === Qt.Key_Minus || (e.key === Qt.Key_Comma && (e.modifiers & Qt.ControlModifier))) { win.openPage("settings"); e.accepted = true }
            else if (e.key === Qt.Key_K && (e.modifiers & Qt.ControlModifier)) { win.openPage("controllers"); e.accepted = true }
            else if (e.key === Qt.Key_F11) { win.toggleFullscreen(); e.accepted = true }
            else if (e.key === Qt.Key_Home) { currentIndex = 0; e.accepted = true }
            else if (e.key === Qt.Key_End) { currentIndex = count; positionViewAtEnd(); e.accepted = true }
        }
        Connections {   // a game moved to the front when it was played: keep it chosen
            target: backend
            function onReordered() { if (win.playingTid !== "") carousel.currentIndex = backend.indexOf(win.playingTid) }
        }
    }

    // the empty library
    Column {
        visible: count === 0
        anchors { horizontalCenter: parent.horizontalCenter; top: carousel.bottom; topMargin: 6 }
        spacing: 6
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "Drop a game here"; color: Theme.text; font.pixelSize: 26; font.weight: Font.DemiBold }
        Label { anchors.horizontalCenter: parent.horizontalCenter; text: "NSP, XCI, NSZ or XCZ files — or press + to choose them. Updates and DLC join their game."; color: Theme.dim; font.pixelSize: 16 }
    }

    // the chosen game: its name, its state and, while it builds, its progress
    Column {
        id: info
        visible: count > 0
        anchors { left: parent.left; right: parent.right; top: carousel.bottom; leftMargin: 76; rightMargin: 76; topMargin: 8 }
        spacing: 8
        Label {
            width: parent.width; elide: Text.ElideRight
            text: win.onAdd ? "Add a game" : (win.current() ? win.current().name : "")
            color: Theme.text; font.pixelSize: 34; font.weight: Font.Bold
        }
        Row {
            spacing: 12
            StatusPill { status: win.onAdd ? "add" : (win.current() ? win.current().status : "") }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: win.onAdd ? "Drag game files onto the window, or press A to choose them" : (win.current() ? win.current().subtitle : "")
                color: Theme.dim; font.pixelSize: 17
            }
        }
        Bar {
            visible: !win.onAdd && win.current() !== null && win.current().building
            width: Math.min(parent.width, 640); value: win.current() ? win.current().progress : 0
        }
        Label {
            visible: !win.onAdd && win.current() !== null && win.current().error !== "" && !win.current().building
            width: Math.min(parent.width, 900); wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight
            text: win.current() ? win.current().error : ""; color: Theme.bad; font.pixelSize: 15
        }
    }

    // ---- the button hints ----
    Rectangle {
        id: hints
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 64; color: "#0a0d13"; z: 15
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: Theme.line }
        RowLayout {
            anchors { fill: parent; leftMargin: 28; rightMargin: 28 }
            spacing: 26
            Label { text: backend.anyRunning ? "A game is running" : ""; color: Theme.good; font.pixelSize: 15 }
            Item { Layout.fillWidth: true }
            Hint { button: "A"; label: win.onAdd ? "Add" : (win.current() && win.current().running ? "Running" : "Play"); visible: win.page === "" }
            Hint { button: "X"; label: "Options"; visible: win.page === "" && !win.onAdd && count > 0 }
            Hint { button: "Y"; label: "Rebuild"; visible: win.page === "" && !win.onAdd && count > 0 }
            Hint { button: "+"; label: "Add game"; visible: win.page === "" }
            Hint { button: "−"; label: "Settings"; visible: win.page === "" }
            Hint { button: "↕"; label: "Move"; visible: win.page !== "" }
            Hint { button: "↔"; label: "Change"; visible: win.page !== "" }
            Hint { button: "A"; label: "Select"; visible: win.page !== "" }
            Hint { button: "B"; label: "Back"; visible: win.page !== "" }
        }
    }

    // ---- builds ----
    ListView {
        id: builds
        anchors { right: parent.right; bottom: hints.top; margins: 20 }
        width: 460; height: Math.min(contentHeight, parent.height * 0.5)
        spacing: 10; interactive: contentHeight > height
        verticalLayoutDirection: ListView.BottomToTop
        model: backend.buildModel
        delegate: BuildCard { width: builds.width }
        z: 5
    }

    // ---- notices ----
    Column {
        id: toasts
        anchors { horizontalCenter: parent.horizontalCenter; bottom: hints.top; bottomMargin: 22 }
        spacing: 8; z: 20
        move: Transition { NumberAnimation { properties: "y"; duration: 160 } }
    }
    Component {
        id: toastC
        Rectangle {
            id: t
            property alias text: tl.text
            property int seconds: 4
            width: Math.min(tl.implicitWidth + 40, win.width * 0.7); height: tl.implicitHeight + 22
            radius: 14; color: Theme.panelHi; border.color: Theme.line
            opacity: 0
            Label { id: tl; anchors.centerIn: parent; width: parent.width - 40; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; color: Theme.text; font.pixelSize: 16 }
            Component.onCompleted: { opacity = 1; life.start() }
            Behavior on opacity { NumberAnimation { duration: 180 } }
            Timer { id: life; interval: t.seconds * 1000; onTriggered: { t.opacity = 0; gone.start() } }
            Timer { id: gone; interval: 200; onTriggered: t.destroy() }
            MouseArea { anchors.fill: parent; onClicked: { t.opacity = 0; gone.start() } }
        }
    }
    Connections {
        target: backend
        function onToast(text, seconds) { toastC.createObject(toasts, { text: text, seconds: seconds }) }
    }

    // ---- adding games ----
    FileDialog {
        id: addDialog
        title: "Add games"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Switch games (*.nsp *.xci *.nsz *.xcz)", "All files (*)"]
        onAccepted: backend.addFiles(selectedFiles)
    }
    DropArea {
        id: drop
        anchors.fill: parent; z: 30
        keys: ["text/uri-list"]
        onDropped: (d) => { backend.addFiles(d.urls); d.accept() }
        Rectangle {
            anchors { fill: parent; margins: 22 }
            visible: drop.containsDrag; radius: 26; color: "#334fa3ff"
            border.color: Theme.accent; border.width: 3
            Label { anchors.centerIn: parent; text: "Drop to add"; color: Theme.text; font.pixelSize: 36; font.weight: Font.Bold }
        }
    }

    // ---- pages ----
    Loader {
        id: pages
        anchors.fill: parent; z: 10
        active: win.page !== ""
        source: win.page === "options" ? "OptionsPage.qml" : win.page === "settings" ? "SettingsPage.qml" : win.page === "controllers" ? "ControllersPage.qml" : ""
        onLoaded: { if (item.tid !== undefined) item.tid = win.pageTid; item.forceActiveFocus() }
        Connections { target: pages.item; ignoreUnknownSignals: true; function onClosed() { win.closePage() } }
    }
    Shortcut { sequence: "F11"; onActivated: win.toggleFullscreen() }
}
