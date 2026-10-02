import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Window {
    id: rootWindow
    title: qsTr("Cadroc")
    width: 620
    height: 340
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

    // Window Root Container with outer margin for soft shadow
    Item {
        id: cardContainer
        anchors.fill: parent
        anchors.margins: 6

        // Mask shape for rounded corners
        Rectangle {
            id: cardMask
            anchors.fill: parent
            radius: 18
            visible: false
        }

        // ASCII Sky Background Image
        Image {
            id: bgImage
            anchors.fill: parent
            source: "qrc:/shell/assets/ascii_sky_clean.jpg"
            fillMode: Image.PreserveAspectCrop
            smooth: true
            visible: false
        }

        // Rounded masked background with shadow
        MultiEffect {
            id: maskedBg
            anchors.fill: parent
            source: bgImage
            maskEnabled: true
            maskSource: cardMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
            shadowEnabled: true
            shadowColor: "#60000000"
            shadowBlur: 0.8
            shadowVerticalOffset: 4
        }

        // Subtle glass tint and border over the background
        Rectangle {
            id: glassBorder
            anchors.fill: parent
            radius: 18
            color: "transparent"
            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.28)
            border.width: 1

            // Subtle dark vignette at bottom for contrast
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.05) }
                GradientStop { position: 0.7; color: Qt.rgba(0, 0, 0, 0.12) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.35) }
            }
        }

        // Native window drag handler across the entire card
        MouseArea {
            id: windowDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: rootWindow.startSystemMove()
        }

        // ====================================================================
        // Top-Left Floating Frosted Glass Pill Toolbar (Arc / macOS aesthetic)
        // ====================================================================
        Rectangle {
            id: pillToolbar
            x: 18
            y: 16
            width: 196
            height: 34
            radius: 17
            color: Qt.rgba(1.0, 1.0, 1.0, 0.26)
            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.38)
            border.width: 1

            // MouseArea inside pill so clicking empty space drags the window
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: rootWindow.startSystemMove()
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 9

                // 🔴 Red Traffic Light (Close / Hide to Tray)
                Rectangle {
                    id: redBtn
                    width: 12
                    height: 12
                    radius: 6
                    color: redMouse.containsMouse ? "#FF453A" : "#FF5F56"
                    border.color: redMouse.containsMouse ? "#B22222" : Qt.rgba(0, 0, 0, 0.2)
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        font.pixelSize: 8
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

                // 🟡 Yellow Traffic Light (Minimize)
                Rectangle {
                    id: yellowBtn
                    width: 12
                    height: 12
                    radius: 6
                    color: yellowMouse.containsMouse ? "#FFB300" : "#FFBD2E"
                    border.color: yellowMouse.containsMouse ? "#B37D00" : Qt.rgba(0, 0, 0, 0.2)
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "—"
                        font.pixelSize: 8
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
                    id: greenBtn
                    width: 12
                    height: 12
                    radius: 6
                    color: greenMouse.containsMouse ? "#30D158" : "#27C93F"
                    border.color: greenMouse.containsMouse ? "#1E7E34" : Qt.rgba(0, 0, 0, 0.2)
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "+"
                        font.pixelSize: 9
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
                            if (rootWindow.width === 620) {
                                rootWindow.width = 780
                                rootWindow.height = 420
                            } else {
                                rootWindow.width = 620
                                rootWindow.height = 340
                            }
                        }
                    }
                }

                // Divider
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 14
                    color: Qt.rgba(1.0, 1.0, 1.0, 0.35)
                }

                // Sidebar / Split Panel Icon
                Rectangle {
                    width: 16
                    height: 14
                    radius: 3
                    color: "transparent"
                    border.color: sideMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.7)
                    border.width: 1

                    Rectangle {
                        x: 5
                        width: 1
                        height: parent.height
                        color: sideMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.7)
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
                    text: "←"
                    font.pixelSize: 13
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
                    text: "→"
                    font.pixelSize: 13
                    font.bold: true
                    color: navFwdMouse.containsMouse ? "#FFFFFF" : Qt.rgba(1, 1, 1, 0.4)

                    MouseArea {
                        id: navFwdMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }
        }

        // ====================================================================
        // Center Content: Clean "Cadroc" Monospace / Glass Branding
        // ====================================================================
        Rectangle {
            id: centerGlassBadge
            anchors.centerIn: parent
            width: 290
            height: 104
            radius: 16
            color: Qt.rgba(0.06, 0.08, 0.14, 0.42)
            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.22)
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
                    styleColor: Qt.rgba(0, 0, 0, 0.4)
                }

                // Subtitle / Status row
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8

                    Rectangle {
                        width: 7
                        height: 7
                        radius: 3.5
                        color: "#A6E3A1" // mint green dot
                    }

                    Text {
                        text: "RESIDENT HOST · v0.1.0"
                        font.pixelSize: 11
                        font.bold: true
                        font.family: "Consolas, 'Courier New', monospace"
                        color: Qt.rgba(1.0, 1.0, 1.0, 0.82)
                    }
                }
            }
        }
    }
}
