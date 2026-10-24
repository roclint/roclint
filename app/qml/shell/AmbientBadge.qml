import QtQuick
import QtQuick.Window

// Passive indicator shown next to the caret in another app when a background check found
// something worth clarifying. It never takes focus; only an explicit click acts on it.
Window {
    id: badge
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"

    flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    width: row.implicitWidth + 16
    height: 26
    x: hc ? hostController.ambientBadgeX : 0
    y: hc ? hostController.ambientBadgeY : 0
    visible: hc && hostController.ambientBadgeVisible

    Rectangle {
        anchors.fill: parent
        radius: 13
        color: "#111418"
        border.color: "#F59E0B"
        border.width: 1

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 8

            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                text: badge.hc ? qsTr("Roclint: %1  (%2)").arg(hostController.ambientBadgeLabel).arg(hostController.shortcutLabel) : ""
                font.family: badge.isMac ? "Helvetica Neue" : "Segoe UI"
                font.pixelSize: 11
                color: "#F59E0B"
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: hostController.openFromAmbient()
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "x"
                font.pixelSize: 11
                color: "#8B949E"
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: hostController.dismissAmbientBadge()
                }
            }
        }
    }
}
