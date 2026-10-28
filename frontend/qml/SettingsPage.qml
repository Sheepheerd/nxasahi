// The library's settings (− on the home screen): keys, language, sound, performance, the library itself
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import "."

PageFrame {
    id: p
    readonly property var c: backend.config
    title: "Settings"
    subtitle: "Saved as you change them"
    rows: [
        { type: "header", title: "Files" },
        { type: "action", key: "keys", title: "Keys file (prod.keys)" },
        { type: "action", key: "library", title: "Open the library folder" },
        { type: "header", title: "Games" },
        { type: "choice", key: "language", title: "Language", subtitle: "A game that lacks it falls back to English" },
        { type: "toggle", key: "fullscreen", title: "Fullscreen", subtitle: "Start games in fullscreen" },
        { type: "toggle", key: "audio", title: "Sound", subtitle: "Play sound through the default output (SDL)" },
        { type: "header", title: "Performance" },
        { type: "toggle", key: "big_cores", title: "Fast cores only", subtitle: "Keep games on the performance cores (much faster on Apple silicon)" },
        { type: "toggle", key: "full_cache", title: "Build the full shader cache", subtitle: "Compile every shader for your GPU when a game is imported or rebuilt (a few minutes for a big game). Off: the game compiles them while you play, with stutter until they are done" },
        { type: "slider", key: "precompile", title: "Shader precompile threads", subtitle: "Threads on the efficiency cores that compile, while you play, what the import left (0 = off)", from: 0, to: 8, step: 1 },
        { type: "header", title: "Library" },
        { type: "toggle", key: "library_fullscreen", title: "Start the library in fullscreen", subtitle: "F11 switches at any time" },
        { type: "action", key: "controllers", title: "Controllers…", subtitle: "Players, keyboard and controller bindings" },
        { type: "action", key: "desktop", title: "Add to the app launcher", subtitle: "Writes a desktop entry for the library (the installed package has one)" },
        { type: "header", title: "Advanced" },
        { type: "text", key: "extra_env", title: "Extra environment for every game", subtitle: "KEY=value … (Return keeps it)" },
        { type: "header", title: "" },
        { type: "info", key: "about", title: "nxasahi", value: "Switch games running natively on Asahi Linux" }
    ]
    readonly property var langChoices: backend.languages.map(l => ({ id: l.code, name: l.name }))
    function subOf(m, c) {
        if (m.key === "keys") return c.keys + (c.keys_found ? "" : "  —  not found")
        if (m.key === "library") return c.library
        return m.subtitle || ""
    }
    function valueOf(m, c) { return m.key === "about" ? m.value : c[m.key] }
    rowDelegate: SettingRow {
        required property var modelData
        required property int index
        current: ListView.isCurrentItem
        type: modelData.type; title: modelData.title; subtitle: p.subOf(modelData, p.c); value: p.valueOf(modelData, p.c)
        choices: modelData.key === "language" ? p.langChoices : []
        from: modelData.from || 0; to: modelData.to || 1; step: modelData.step || 0.05
        danger: modelData.key === "keys" && !p.c.keys_found
        onChanged: (v) => backend.setSetting(modelData.key, v)
        onTriggered: {
            const k = modelData.key
            if (k === "keys") keys.open()
            else if (k === "library") backend.openLibrary()
            else if (k === "desktop") backend.installDesktop()
            else if (k === "controllers") { p.closed(); Qt.callLater(() => win.openPage("controllers")) }
        }
    }
    FileDialog {
        id: keys
        title: "Choose prod.keys"
        nameFilters: ["Keys (*.keys)", "All files (*)"]
        onAccepted: backend.setSetting("keys", selectedFile)
    }
}
