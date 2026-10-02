import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("Cadroc")
    width: 520
    height: 270
    visible: false
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    readonly property string monoFont: (Qt.platform.os === "osx" || Qt.platform.os === "macos") ? "Menlo" : "Consolas"

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

    // Main Card Frame
    Rectangle {
        id: cardRoot
        anchors.fill: parent
        color: "#0D1117" // Deep terminal background
        border.color: "#30363D" // ASCII frame border
        border.width: 1

        // Draggable across the window surface
        MouseArea {
            id: windowDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: rootWindow.startSystemMove()
        }

        // Ambient ASCII character matrix dithering in background
        Item {
            anchors.fill: parent
            anchors.margins: 6
            clip: true
            opacity: 0.08

            Grid {
                anchors.fill: parent
                columns: 34
                rows: 15
                spacing: 8

                Repeater {
                    model: 34 * 15
                    Text {
                        text: (index % 7 === 0) ? "x" : ((index % 3 === 0) ? "+" : "·")
                        font.family: rootWindow.monoFont
                        font.pixelSize: 11
                        color: "#89DCEB"
                    }
                }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 0

            // ================================================================
            // ASCII Top Bar: +-- [ CADROC ] ------------------------ [-] [x] -+
            // ================================================================
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                spacing: 8

                // Header Title in ASCII bracket format
                Text {
                    text: "+--[ CADROC ]"
                    font.family: rootWindow.monoFont
                    font.bold: true
                    font.pixelSize: 13
                    color: "#89DCEB" // Cyan terminal accent
                }

                // ASCII horizontal rule line
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#30363D"
                }

                // ASCII Minimize Button: [ - ]
                Rectangle {
                    width: 32
                    height: 22
                    color: minMouse.containsMouse ? "#21262D" : "transparent"
                    border.color: minMouse.containsMouse ? "#89DCEB" : "#30363D"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "[-]"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 12
                        color: minMouse.containsMouse ? "#89DCEB" : "#8B949E"
                    }

                    MouseArea {
                        id: minMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (Qt.platform.os === "osx" || Qt.platform.os === "macos") {
                                if (typeof hostController !== "undefined") {
                                    hostController.hideCard()
                                } else {
                                    rootWindow.hide()
                                }
                            } else {
                                rootWindow.showMinimized()
                            }
                        }
                    }
                }

                // ASCII Close/Hide to Tray Button: [ x ]
                Rectangle {
                    width: 32
                    height: 22
                    color: closeMouse.containsMouse ? "#F85149" : "transparent"
                    border.color: closeMouse.containsMouse ? "#FF7B72" : "#30363D"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "[x]"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 12
                        color: closeMouse.containsMouse ? "#FFFFFF" : "#8B949E"
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

                Text {
                    text: "+"
                    font.family: rootWindow.monoFont
                    font.pixelSize: 13
                    color: "#30363D"
                }
            }

            // Divider line
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // ============================================================
                // Center ASCII Art: CADROC
                // ============================================================
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 14

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "  ____    _    ____  ____   ___   ____ \n" +
                              " / ___|  / \\  |  _ \\|  _ \\ / _ \\ / ___|\n" +
                              "| |     / _ \\ | | | | |_) | | | | |    \n" +
                              "| |___ / ___ \\| |_| |  _ <| |_| | |___ \n" +
                              " \\____/_/   \\_\\____/|_| \\_\\___/ \\____|"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 15
                        color: "#C9D1D9" // Crisp terminal text
                        lineHeight: 1.15
                    }

                    // ASCII Status row
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 8

                        Text {
                            text: "[ * ]"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 11
                            color: "#7EE787" // Green terminal status light
                        }

                        Text {
                            text: "RESIDENT HOST :: v0.1.0"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 11
                            color: "#8B949E"
                        }
                    }
                }
            }

            // ================================================================
            // ASCII Footer Bar: +-- [ ESC: HIDE TO TRAY ] --- [ DRAGGABLE ] --+
            // ================================================================
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 18
                spacing: 8

                Text {
                    text: "+--"
                    font.family: rootWindow.monoFont
                    font.pixelSize: 12
                    color: "#30363D"
                }

                Text {
                    text: "[ ESC: HIDE TO TRAY ]"
                    font.family: rootWindow.monoFont
                    font.pixelSize: 10
                    color: "#484F58"
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#30363D"
                }

                Text {
                    text: "[ DRAGGABLE ]--+"
                    font.family: rootWindow.monoFont
                    font.pixelSize: 10
                    color: "#484F58"
                }
            }
        }
    }
}
