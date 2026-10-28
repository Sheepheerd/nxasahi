// A game's options (X on its tile, or a right-click): what it is, its language and environment, and what can be done with it
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import "."

PageFrame {
    id: p
    property string tid: ""
    property var g: tid !== "" ? backend.gameInfo(tid) : ({})
    property bool asking: false
    title: g.name || ""
    subtitle: tid + (g.version && g.version !== "—" ? "  ·  version " + g.version : "")
    art: g.icon || ""
    capturing: asking
    function reload() { g = backend.gameInfo(tid) }
    Connections { target: backend.gameModel; function onDataChanged() { p.reload() } }

    // the rows are fixed; their values follow g (so the cursor stays where it is when one changes)
    rows: [
        { type: "header", title: "Play" },
        { type: "action", key: "play" },
        { type: "header", title: "Options" },
        { type: "choice", key: "language", title: "Language", subtitle: "Overrides the library setting for this game" },
        { type: "text", key: "extra_env", title: "Extra environment", subtitle: "KEY=value … for this game (Return keeps it)" },
        { type: "header", title: "Updates and builds" },
        { type: "action", key: "content", title: "Add update or DLC…" },
        { type: "action", key: "rebuild", title: "Rebuild", subtitle: "Run the import again (after a tool or profile update)" },
        { type: "action", key: "cache", title: "Build shader cache", subtitle: "Compile every shader for your GPU now, so nothing stutters in play" },
        { type: "header", title: "Folders" },
        { type: "action", key: "saves", title: "Open save folder" },
        { type: "action", key: "logs", title: "Open build logs" },
        { type: "action", key: "folder", title: "Open game folder", subtitle: "Links to your game files and the build" },
        { type: "header", title: "About this build" },
        { type: "info", key: "built", title: "Built" },
        { type: "info", key: "build", title: "Package" },
        { type: "info", key: "profile", title: "Profile" },
        { type: "info", key: "error", title: "Last build error" },
        { type: "header", title: "" },
        { type: "action", key: "remove", title: "Remove from library…", subtitle: "Deletes its build files; your saves and the game files stay", danger: true }
    ]
    readonly property var langChoices: [{ id: "", name: "Use the library setting" }].concat(backend.languages.slice(1).map(l => ({ id: l.code, name: l.name })))
    function titleOf(m, g) { return m.key === "play" ? (g.running ? "Stop" : "Play") : m.title }
    function subOf(m, g) {
        if (m.key === "play") return g.running ? "Quit the game" : g.ready ? "Start the game" : "It builds first"
        if (m.key === "content") return (g.updates || 0) + " update(s), " + (g.dlc || 0) + " DLC"
        if (m.key === "saves") return g.saves || ""
        if (m.key === "logs") return g.logs || ""
        return m.subtitle || ""
    }
    function valueOf(m, g) { return m.key === "language" ? (g.language || "") : m.key === "error" ? (g.error || "none") : g[m.key] }
    rowDelegate: SettingRow {
        required property var modelData
        required property int index
        current: ListView.isCurrentItem
        type: modelData.type; title: p.titleOf(modelData, p.g); subtitle: p.subOf(modelData, p.g); value: p.valueOf(modelData, p.g)
        choices: modelData.key === "language" ? p.langChoices : []; danger: modelData.danger || false
        onChanged: (v) => { backend.setGameOption(p.tid, modelData.key, v); p.reload() }
        onTriggered: {
            const k = modelData.key
            if (k === "play") { if (p.g.running) backend.stop(p.tid); else { backend.play(p.tid); p.closed() } }
            else if (k === "content") content.open()
            else if (k === "rebuild") { backend.rebuild(p.tid); p.closed() }
            else if (k === "cache") backend.buildCache(p.tid)
            else if (k === "saves") backend.openSaves(p.tid)
            else if (k === "logs") backend.openLogs(p.tid)
            else if (k === "folder") backend.openFolder(p.tid)
            else if (k === "remove") { p.asking = true; confirm.open() }
        }
    }

    FileDialog {
        id: content
        title: "Add update or DLC to " + (p.g.name || "")
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Switch updates and DLC (*.nsp *.xci *.nsz *.xcz)", "All files (*)"]
        onAccepted: backend.addContent(p.tid, selectedFiles)
    }
    Popup {
        id: confirm
        anchors.centerIn: parent; modal: true; focus: true; padding: 28
        width: Math.min(620, p.width - 80)
        onClosed: { p.asking = false; p.list.forceActiveFocus() }
        background: Rectangle { radius: 20; color: Theme.panel; border.color: Theme.bad; border.width: 2 }
        contentItem: Column {
            spacing: 16
            Label { text: "Remove " + (p.g.name || "") + "?"; color: Theme.text; font.pixelSize: 24; font.bold: true; width: parent.width; wrapMode: Text.Wrap }
            Label { text: "This deletes its build files. Your saves stay in " + (p.g.saves || "") + ", and the original game files are not touched."; color: Theme.dim; font.pixelSize: 15; width: parent.width; wrapMode: Text.Wrap }
            Row {
                spacing: 12; anchors.right: parent.right
                FocusButton { id: no; text: "Cancel"; focus: true; KeyNavigation.right: yes; onClicked: confirm.close() }
                FocusButton { id: yes; text: "Remove"; danger: true; KeyNavigation.left: no; onClicked: { confirm.close(); backend.remove(p.tid); p.closed() } }
            }
        }
        Keys.onEscapePressed: confirm.close()
        onOpened: no.forceActiveFocus()
    }
}
