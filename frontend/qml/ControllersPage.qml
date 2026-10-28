// The controller settings (the 🎮 button, Ctrl+K): each of the eight players' keyboard and controller, and every binding. A binding: A / Return, then press the key or the
// controller button / stick to give it (Escape cancels, Backspace unbinds). Saved for the host as it changes (frontend/controllers.py, dm/host/linux/front_input.c)
import QtQuick
import QtQuick.Controls
import "."

PageFrame {
    id: p
    property int player: 0
    property var pl: backend.player(player)
    property var listen: null          // {kind, target} while a binding waits for its input
    property var choices: backend.controllerChoices(player)
    title: "Controllers"
    subtitle: backend.padCount ? backend.padCount + " controller" + (backend.padCount === 1 ? "" : "s") + " connected" : "No controller connected (the keyboard works)"
    capturing: listen !== null
    function reload() { pl = backend.player(player); choices = backend.controllerChoices(player) }
    Connections { target: backend; function onPlayersChanged() { p.reload() } function onPadsChanged() { p.reload() }
                  function onPadCaptured(source) { if (p.listen && p.listen.kind === "buttons") { backend.setBind(p.player, "buttons", p.listen.target, source); p.stopListen() } } }
    onEscaped: stopListen()

    function bindRows(kind) {
        let out = []
        for (const col of backend.bindGroups) for (const grp of col) {
            out.push({ type: "header", title: (kind === "keys" ? "Keyboard · " : "Controller · ") + grp.title })
            for (const t of grp.targets) out.push({ type: "bind", kind: kind, target: t.id, title: t.label })
        }
        return out
    }
    rows: [
        { type: "header", title: "Player" },
        { type: "choice", key: "player", title: "Player" },
        { type: "toggle", key: "keyboard", title: "Keyboard", subtitle: "This player uses the keyboard" },
        { type: "choice", key: "controller", title: "Controller", subtitle: "Any: the next controller that connects. A named model stays with this player" },
        { type: "slider", key: "deadzone", title: "Stick deadzone", subtitle: "Stick travel ignored around the centre", from: 0, to: 0.5, step: 0.01 },
        { type: "action", key: "reset", title: "Reset this player", subtitle: "The default bindings" }
    ].concat(bindRows("keys")).concat(bindRows("buttons"))
    readonly property var playerChoices: Array.from({ length: backend.playerCount }, (_, i) => ({ id: i, name: "Player " + (i + 1) }))
    function valueOf(m, pl) {
        if (m.key === "player") return p.player
        if (m.key === "keyboard") return pl.keyboard
        if (m.key === "controller") return pl.controller
        if (m.key === "deadzone") return pl.deadzone
        if (m.type === "bind") return m.kind === "keys" ? pl.keys[m.target] : (pl.buttons[m.target] === "Unbound" ? "" : pl.buttons[m.target])
        return undefined
    }
    function subOf(m, pl) {
        if (m.key === "keyboard" && pl.keyboard && pl.others && pl.others.length) return "Also on for player " + pl.others.join(", ") + ": a key bound for both presses both"
        return m.subtitle || ""
    }
    rowDelegate: SettingRow {
        required property var modelData
        required property int index
        current: ListView.isCurrentItem
        type: modelData.type; title: modelData.title; subtitle: p.subOf(modelData, p.pl); value: p.valueOf(modelData, p.pl)
        choices: modelData.key === "player" ? p.playerChoices : modelData.key === "controller" ? p.choices : []
        from: modelData.from || 0; to: modelData.to || 1; step: modelData.step || 0.05; unit: modelData.key === "deadzone" ? "%" : ""
        listening: p.listen !== null && modelData.type === "bind" && p.listen.kind === modelData.kind && p.listen.target === modelData.target
        live: modelData.type === "bind" && modelData.kind === "buttons" && p.pl.raw[modelData.target] !== "" && backend.liveSources.indexOf(p.pl.raw[modelData.target]) >= 0
        enabled: modelData.type !== "bind" || (modelData.kind === "keys" ? p.pl.keyboard : p.pl.controller !== "none")
        opacity: enabled ? 1 : 0.4
        onChanged: (v) => {
            if (modelData.key === "player") { p.player = v; p.reload() }
            else backend.setPlayer(p.player, modelData.key, v)
        }
        onTriggered: {
            if (modelData.key === "reset") { backend.resetPlayer(p.player); return }
            if (modelData.type !== "bind" || !enabled) return
            p.startListen(modelData.kind, modelData.target)
        }
        onUnbind: backend.setBind(p.player, modelData.kind, modelData.target, "")
    }
    // while a binding waits, this item has the focus: every key comes here first, the arrows and Return too
    // (a keyboard binding is the next key, by position: its evdev code, as the host matches keys)
    Item {
        id: catcher
        Keys.onPressed: (e) => {
            e.accepted = true
            if (!p.listen) return
            if (e.key === Qt.Key_Escape) { p.stopListen(); return }
            if (p.listen.kind !== "keys") return
            if (e.key === Qt.Key_Backspace) { backend.setBind(p.player, "keys", p.listen.target, ""); p.stopListen(); return }
            const name = backend.keyName(e.nativeScanCode)
            if (name) { backend.setBind(p.player, "keys", p.listen.target, name); p.stopListen() }
        }
    }
    function startListen(kind, target) {
        p.listen = { kind: kind, target: target }
        if (kind === "buttons") backend.startPadCapture(p.player)
        catcher.forceActiveFocus()
    }
    function stopListen() {
        if (p.listen && p.listen.kind === "buttons") backend.stopPadCapture()
        p.listen = null
        if (p.list.currentItem) p.list.currentItem.forceActiveFocus()
    }
    Timer { running: p.listen !== null && p.listen.kind === "buttons"; interval: 8000; onTriggered: p.stopListen() }
    Component.onDestruction: backend.stopPadCapture()
}
