import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("Cadroc")
    width: 480
    height: 360
    visible: false
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
    color: "transparent"

    x: (Screen.width - width) / 2
    y: (Screen.height - height) / 2

    DragHandler {
        target: rootWindow
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

    Rectangle {
        id: cardBackground
        anchors.fill: parent
        anchors.margins: 10
        radius: 12
        color: "#1E1E2E"
        border.color: "#45475A"
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12

            // Header bar
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: qsTr("Cadroc / Stardust")
                    font.bold: true
                    font.pixelSize: 15
                    color: "#CDD6F4"
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: qsTr("Stage 00: B00 Shell")
                    font.pixelSize: 11
                    color: "#89DCEB"
                }

                Button {
                    text: "✕"
                    flat: true
                    onClicked: {
                        if (typeof hostController !== "undefined") {
                            hostController.hideCard()
                        } else {
                            rootWindow.hide()
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#313244"
            }

            // Body
            Text {
                text: qsTr("DRAFT (Placeholder Card)")
                font.bold: true
                font.pixelSize: 13
                color: "#F9E2AF"
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#181825"
                radius: 8
                border.color: "#313244"
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Resident desktop host active.\n\nClosing this card leaves the host resident in the system tray.\nRe-open from tray restores this single card instance.\nUse Tray -> Quit to terminate.")
                    color: "#A6ADC8"
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            // Footer
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: qsTr("Host State: ") + (typeof hostController !== "undefined" ? hostController.hostStateString : "RESIDENT")
                    color: "#A6E3A1"
                    font.pixelSize: 11
                }
            }
        }
    }
}
