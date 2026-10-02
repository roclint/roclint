import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("Cadroc")
    width: 720
    height: 332
    visible: false
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    Component.onCompleted: {
        x = (Screen.width - width) / 2
        y = (Screen.height - height) / 2
    }

    // Dismiss with Escape key
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (typeof hostController !== "undefined") {
                hostController.hideCard()
            } else {
                rootWindow.hide()
            }
        }
    }

    Item {
        id: cardContainer
        anchors.fill: parent

        // ASCII Sky Background Image (contains antialiased rounded corners & frosted pill background)
        Image {
            id: bgImage
            anchors.fill: parent
            source: "qrc:/shell/assets/ascii_sky_clean.png"
            fillMode: Image.Stretch
            smooth: true
        }

        // Translucent card border for crisp modern outline
        Rectangle {
            anchors.fill: parent
            radius: 16
            color: "transparent"
            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.25)
            border.width: 1
        }

        // Native window drag handler across the entire card
        MouseArea {
            id: windowDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: rootWindow.startSystemMove()
        }

        // ====================================================================
        // Top-Left Interactive Pill Toolbar (Overlaying the Frosted Glass Pill)
        // ====================================================================
        Item {
            id: pillToolbar
            x: 6
            y: 0
            width: 156
            height: 32

            // Draggable empty space inside pill
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: rootWindow.startSystemMove()
            }

            // 🔴 Red Traffic Light (Close / Hide to Tray)
            Rectangle {
                x: 12
                y: 10
                width: 11
                height: 11
                radius: 5.5
                color: redMouse.containsMouse ? "#FF453A" : "#FF5F56"
                border.color: redMouse.containsMouse ? "#B22222" : Qt.rgba(0, 0, 0, 0.25)
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "✕"
                    font.pixelSize: 7
                    font.bold: true
                    color: "#4A0000"
                    visible: redMouse.containsMouse
                }

                MouseArea {
                    id: redMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof hostController !== "undefined") {
                            hostController.hideCard()
                        } else {
                            rootWindow.hide()
                        }
                    }
                }
            }

            // 🟡 Yellow Traffic Light (Minimize to Taskbar)
            Rectangle {
                x: 33
                y: 10
                width: 11
                height: 11
                radius: 5.5
                color: yellowMouse.containsMouse ? "#FFB300" : "#FFBD2E"
                border.color: yellowMouse.containsMouse ? "#B37D00" : Qt.rgba(0, 0, 0, 0.25)
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "—"
                    font.pixelSize: 7
                    font.bold: true
                    color: "#5C4300"
                    visible: yellowMouse.containsMouse
                }

                MouseArea {
                    id: yellowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: rootWindow.showMinimized()
                }
            }

            // 🟢 Green Traffic Light (Resize / Expand toggle)
            Rectangle {
                x: 54
                y: 10
                width: 11
                height: 11
                radius: 5.5
                color: greenMouse.containsMouse ? "#30D158" : "#27C93F"
                border.color: greenMouse.containsMouse ? "#1E7E34" : Qt.rgba(0, 0, 0, 0.25)
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    font.pixelSize: 8
                    font.bold: true
                    color: "#0B4619"
                    visible: greenMouse.containsMouse
                }

                MouseArea {
                    id: greenMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (rootWindow.width === 720) {
                            rootWindow.width = 860
                            rootWindow.height = 396
                        } else {
                            rootWindow.width = 720
                            rootWindow.height = 332
                        }
                    }
                }
            }

            // Divider
            Rectangle {
                x: 74
                y: 9
                width: 1
                height: 13
                color: Qt.rgba(1.0, 1.0, 1.0, 0.35)
            }

            // Sidebar / Split Panel Icon
            Rectangle {
                x: 82
                y: 9
                width: 13
                height: 12
                radius: 2.5
                color: "transparent"
                border.color: sideMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.70)
                border.width: 1

                Rectangle {
                    x: 4
                    width: 1
                    height: parent.height
                    color: sideMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.70)
                }

                MouseArea {
                    id: sideMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                }
            }

            // Navigation: Back Arrow ←
            Text {
                x: 106
                y: 7
                text: "←"
                font.pixelSize: 11
                font.bold: true
                color: navBackMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.65)

                MouseArea {
                    id: navBackMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                }
            }

            // Navigation: Forward Arrow →
            Text {
                x: 124
                y: 7
                text: "→"
                font.pixelSize: 11
                font.bold: true
                color: navFwdMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.40)

                MouseArea {
                    id: navFwdMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                }
            }
        }

        // ====================================================================
        // Center Content: Clean "Cadroc" Monospace / Frosted Glass Badge
        // ====================================================================
        Rectangle {
            id: centerGlassBadge
            anchors.centerIn: parent
            width: 310
            height: 106
            radius: 18
            color: Qt.rgba(0.08, 0.10, 0.16, 0.48)
            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.28)
            border.width: 1

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 6

                // Cadroc Title
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "C A D R O C"
                    font.pixelSize: 28
                    font.bold: true
                    font.family: "Segoe UI, -apple-system, sans-serif"
                    color: "#FFFFFF"
                    style: Text.Raised
                    styleColor: Qt.rgba(0, 0, 0, 0.45)
                }

                // Subtitle / Status row
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8

                    Rectangle {
                        width: 7
                        height: 7
                        radius: 3.5
                        color: "#A6E3A1" // mint green status dot
                    }

                    Text {
                        text: "RESIDENT HOST · v0.1.0"
                        font.pixelSize: 11
                        font.bold: true
                        font.family: "Consolas, 'Courier New', monospace"
                        color: Qt.rgba(1.0, 1.0, 1.0, 0.88)
                    }
                }
            }
        }
    }
}
