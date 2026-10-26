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
    visible: false

    Rectangle {
        id: pill
        anchors.fill: parent
        radius: 13
        color: pillMouseArea.containsMouse ? "#1A2028" : "#111418"
        border.color: "#F59E0B"
        border.width: 1

        MouseArea {
            id: pillMouseArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: hostController.openFromAmbient()
        }

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6

            // Notification count circle (Grammarly style)
            Rectangle {
                width: 16
                height: 16
                radius: 8
                color: "#F59E0B"
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    anchors.centerIn: parent
                    text: "1"
                    font.family: badge.isMac ? "Helvetica Neue" : "Segoe UI"
                    font.bold: true
                    font.pixelSize: 10
                    color: "#050505"
                }
            }

            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                text: badge.hc ? qsTr("%1  (%2)").arg(hostController.ambientBadgeLabel).arg(hostController.shortcutLabel) : ""
                font.family: badge.isMac ? "Helvetica Neue" : "Segoe UI"
                font.pixelSize: 11
                color: "#F59E0B"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "×"
                font.pixelSize: 12
                font.bold: true
                color: dismissMouseArea.containsMouse ? "#F87171" : "#8B949E"
                MouseArea {
                    id: dismissMouseArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: hostController.dismissAmbientBadge()
                }
            }
        }
    }
}
