import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtCore

// Passive, non-focusing companion to the native prompt field.
Window {
    id: badge
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    width: row.implicitWidth + 16
    height: 36
    x: hc ? hostController.ambientBadgeX : 0
    y: hc ? hostController.ambientBadgeY : 0
    visible: false
    Settings { id: appearance; category: "appearance"; property bool reducedMotion: false }
    Rectangle {
        anchors.fill: parent
        radius: 11
        color: pillMouseArea.containsMouse ? "#1B315E" : "#09132E"
        border.color: "#627EB8"
        Behavior on color { ColorAnimation { duration: appearance.reducedMotion ? 0 : 110 } }
        MouseArea {
            id: pillMouseArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: if (badge.hc) hostController.openFromAmbient()
        }
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 8
            Image { source: "assets/roc-mark.png"; width: 25; height: 25; fillMode: Image.PreserveAspectFit; anchors.verticalCenter: parent.verticalCenter }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: badge.hc ? hostController.ambientBadgeLabel : ""
                font.family: "Inter Variable"
                font.pixelSize: 12
                color: "#F3F5FF"
            }
            RocButton {
                compact: true; quiet: true; iconName: "x"; tip: qsTr("Dismiss suggestion")
                focusPolicy: Qt.NoFocus
                anchors.verticalCenter: parent.verticalCenter
                onClicked: if (badge.hc) hostController.dismissAmbientBadge()
            }
        }
    }
}
