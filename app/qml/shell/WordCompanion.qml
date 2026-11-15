import QtQuick
import QtQuick.Layouts

// Lightweight, editor-anchored companion. It is a separate non-activating window so the
// destination editor keeps focus; all suggestions stay bound to the host's frozen span.
Window {
    id: companion
    title: qsTr("Roclint word companion")
    width: 360
    height: contentColumn.implicitHeight + 24
    visible: false
    // Never take focus from the destination editor. Mouse clicks still work, and the captured
    // field token remains the stable binding for one-click replacement.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"

    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Segoe UI"

    RocTheme { id: palette }

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: palette.background
        border.color: palette.focus
        border.width: 1

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 2
            anchors.topMargin: 2
            radius: 10
            color: "#66000000"
            z: -1
        }

        ColumnLayout {
            id: contentColumn
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Image {
                    source: "assets/roc-mark.png"
                    sourceSize.width: 40; sourceSize.height: 34
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 15
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                }
                Text {
                    text: qsTr("WORD COMPANION")
                    font.family: companion.monoFont
                    font.pixelSize: 10
                    font.bold: true
                    color: palette.success
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    objectName: "wordCompanionDismissControl"
                    implicitWidth: 22; implicitHeight: 22
                    radius: 4
                    color: dismissMouse.containsMouse ? palette.hover : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        font.pixelSize: 15
                        color: palette.muted
                    }
                    MouseArea {
                        id: dismissMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (companion.hc) hostController.dismissWordRefinement()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: companion.hc && hostController.wordRefinementReplacement.length > 0
                implicitHeight: previewRow.implicitHeight + 12
                radius: 6
                color: palette.inset
                border.color: palette.border
                border.width: 1

                RowLayout {
                    id: previewRow
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 6
                    Text {
                        text: companion.hc ? hostController.wordRefinementOriginal : ""
                        font.family: companion.monoFont
                        font.pixelSize: 12
                        font.strikeout: true
                        color: palette.warning
                    }
                    Text {
                        text: "→"
                        font.family: companion.monoFont
                        font.pixelSize: 12
                        color: palette.foreground
                    }
                    Text {
                        Layout.fillWidth: true
                        text: companion.hc ? hostController.wordRefinementReplacement : ""
                        font.family: companion.monoFont
                        font.pixelSize: 12
                        font.bold: true
                        color: palette.success
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: companion.hc ? hostController.wordRefinementExplanation : ""
                font.family: companion.sansFont
                font.pixelSize: 11
                color: palette.muted
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: companion.hc && hostController.wordRefinementAssumption.length > 0
                      ? qsTr("Assumes: ") + hostController.wordRefinementAssumption : ""
                font.family: companion.sansFont
                font.pixelSize: 10
                color: palette.warning
                wrapMode: Text.WordWrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: companion.hc && hostController.wordRefinementCandidates.length > 1
                spacing: 3
                Text {
                    text: qsTr("Other readings:")
                    font.family: companion.monoFont
                    font.pixelSize: 9
                    color: palette.muted
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 5
                    Repeater {
                        model: companion.hc ? hostController.wordRefinementCandidates : []
                        delegate: Rectangle {
                            id: candidateChip
                            required property var modelData
                            required property int index
                            objectName: "wordCompanionCandidate_" + index
                            readonly property bool selected: companion.hc &&
                                hostController.wordRefinementSelectedCandidateId === modelData.id
                            implicitWidth: chipText.implicitWidth + 12
                            implicitHeight: 22
                            radius: 4
                            color: selected ? palette.hover : palette.inset
                            border.color: selected ? palette.focus : palette.border
                            border.width: 1
                            opacity: modelData.requiresClarification ? 0.55 : 1.0
                            Text {
                                id: chipText
                                anchors.centerIn: parent
                                text: modelData.label
                                font.family: companion.sansFont
                                font.pixelSize: 10
                                color: palette.foreground
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                enabled: !modelData.requiresClarification
                                onClicked: if (companion.hc)
                                    hostController.selectWordRefinementCandidate(modelData.id)
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: companion.hc && hostController.wordRefinementNeedsClarification
                text: companion.hc ? hostController.wordRefinementClarificationQuestion : ""
                font.family: companion.sansFont
                font.pixelSize: 11
                color: palette.warning
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    objectName: "wordCompanionApplyControl"
                    visible: companion.hc && !hostController.wordRefinementNeedsClarification &&
                             !hostController.wordRefinementApplying
                    implicitWidth: applyText.implicitWidth + 16
                    implicitHeight: 24
                    radius: 4
                    color: palette.accent
                    Text {
                        id: applyText
                        anchors.centerIn: parent
                        text: qsTr("Apply")
                        font.family: companion.monoFont
                        font.pixelSize: 10
                        font.bold: true
                        color: "#FFFFFF"
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (companion.hc)
                            hostController.applyWordRefinement(hostController.stateVersion,
                                hostController.wordRefinementSelectedCandidateId)
                    }
                }
                Rectangle {
                    objectName: "wordCompanionExploreControl"
                    implicitWidth: exploreText.implicitWidth + 16
                    implicitHeight: 24
                    radius: 4
                    color: exploreMouse.containsMouse ? palette.hover : "transparent"
                    border.color: palette.border
                    border.width: 1
                    Text {
                        id: exploreText
                        anchors.centerIn: parent
                        text: qsTr("Explore")
                        font.family: companion.monoFont
                        font.pixelSize: 10
                        color: palette.foreground
                    }
                    MouseArea {
                        id: exploreMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (companion.hc) hostController.exploreInCompanion()
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }
    }
}
