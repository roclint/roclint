import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("Cadroc")
    width: 460
    height: 280
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

    Rectangle {
        id: cardBackground
        anchors.fill: parent
        anchors.margins: 8
        radius: 14
        color: "#181825"
        border.color: "#313244"
        border.width: 1

        // Draggable area across the header and card background
        MouseArea {
            id: dragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: rootWindow.startSystemMove()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 0

            // Header Bar
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 32

                Text {
                    text: qsTr("Cadroc")
                    font.bold: true
                    font.pixelSize: 15
                    color: "#CDD6F4"
                }

                // Spacer allows dragging by clicking the top bar
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                // Minimize Button
                Rectangle {
                    width: 28
                    height: 28
                    radius: 6
                    color: minMouse.containsMouse ? "#313244" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "—"
                        color: "#BAC2DE"
                        font.pixelSize: 14
                        font.bold: true
                    }

                    MouseArea {
                        id: minMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: rootWindow.showMinimized()
                    }
                }

                // Close Button
                Rectangle {
                    width: 28
                    height: 28
                    radius: 6
                    color: closeMouse.containsMouse ? "#F38BA8" : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: closeMouse.containsMouse ? "#11111B" : "#BAC2DE"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    MouseArea {
                        id: closeMouse
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
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: "#313244"
                Layout.topMargin: 8
                Layout.bottomMargin: 16
            }

            // Body Area: Just cleanly displays "Cadroc"
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 8

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Cadroc")
                        font.bold: true
                        font.pixelSize: 34
                        color: "#89B4FA"
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Desktop Resident Card")
                        font.pixelSize: 13
                        color: "#6C7086"
                    }
                }
            }
        }
    }
}
