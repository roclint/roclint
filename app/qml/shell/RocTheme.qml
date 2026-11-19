import QtQuick

QtObject {
    property string mode: "dark"
    readonly property bool light: mode === "light"
    readonly property color background: light ? "#F4F6FC" : "#09132E"
    readonly property color surface: light ? "#FFFFFF" : "#101E40"
    readonly property color inset: light ? "#EAF0FD" : "#0C1836"
    readonly property color foreground: light ? "#182445" : "#F3F5FF"
    readonly property color muted: light ? "#586780" : "#A9B8D9"
    readonly property color border: light ? "#BBC9E4" : "#344B7B"
    readonly property color hover: light ? "#DFE8FC" : "#1B315E"
    readonly property color accent: mode === "acid" ? "#33B78D" : mode === "amber" ? "#D79124" : "#345CFF"
    readonly property color focus: light ? "#345CFF" : "#A8BCFF"
    readonly property color warning: light ? "#94640B" : "#EBC47C"
    readonly property color success: light ? "#227B62" : "#92D4BB"
    readonly property string sansFont: Qt.platform.os === "osx" ? "Helvetica Neue" : "Segoe UI"
    readonly property string monoFont: Qt.platform.os === "osx" ? "Menlo" : "Consolas"
}
