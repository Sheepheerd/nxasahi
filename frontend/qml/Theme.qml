// The library's colours and sizes, in one place (a console-style dark theme)
pragma Singleton
import QtQuick

QtObject {
    readonly property color bg0: "#0d1017"
    readonly property color bg1: "#171b27"
    readonly property color panel: "#1e2332"
    readonly property color panelHi: "#282f43"
    readonly property color line: "#323a52"
    readonly property color text: "#eef1f8"
    readonly property color dim: "#8b93a9"
    readonly property color accent: "#4fa3ff"
    readonly property color accent2: "#7c5cff"
    readonly property color good: "#45d483"
    readonly property color warn: "#ffb547"
    readonly property color bad: "#ff5c6c"
    readonly property string font: "Inter, Cantarell, Noto Sans, sans-serif"
    readonly property int radius: 18
}
