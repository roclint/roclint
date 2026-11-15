pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Compact, editor-anchored word companion. It is a separate non-activating
// window so the destination editor keeps focus; the host freezes the target
// span and positions this window from wordRefinementAnchorRect.
//
// The popup never invents alternatives: every row comes from
// hostController.wordRefinementCandidates. Apply is only reachable for a real
// local replacement that does not require a clarification answer. Keyboard
// access to the interactive interview is the explicit Explore route (the HUD
// is an activating window); this popup intentionally cannot steal keys from
// the editor.
Window {
    id: companion
    title: qsTr("Roclint word companion")
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: "Consolas"

    CompanionContract { id: contract }
    RocTheme { id: rocTheme }
    FontLoader { id: interFont; source: "assets/InterVariable.ttf" }
    Settings { id: appearance; category: "appearance"; property bool reducedMotion: false }
    readonly property alias reducedMotion: appearance.reducedMotion

    readonly property string sansFont: interFont.name.length
                                        ? interFont.name
                                        : (isMac ? "Helvetica Neue" : "Segoe UI")
    readonly property color errorColor: "#EEA4AF"
    readonly property int preferredWidth: {
        const available = companion.screen ? companion.screen.width : 376;
        return Math.max(232, Math.min(376, available - 16));
    }
    readonly property int maxPopupHeight: {
        const available = companion.screen ? companion.screen.height : 640;
        return Math.max(220, Math.min(560, Math.round(available * 0.62)));
    }
    readonly property string stateLabel: {
        if (!contract.hasWordRefinement) return qsTr("STANDBY");
        if (contract.wordApplying) return qsTr("APPLYING");
        if (contract.wordNeedsClarification) return qsTr("ONE DETAIL");
        if (contract.wordWorking) return qsTr("WORKING");
        return qsTr("READY");
    }
    readonly property color stateTone: {
        if (contract.wordNeedsClarification) return rocTheme.warning;
        if (contract.wordApplying || contract.wordWorking) return rocTheme.focus;
        return rocTheme.success;
    }
    readonly property bool showAlternatives: contract.wordCandidates.length > 1
    readonly property bool showReplacement: contract.wordReplacement.length > 0
    readonly property int selectedCandidateIndex: {
        for (let i = 0; i < contract.wordCandidates.length; ++i) {
            if (contract.wordCandidates[i] && contract.wordCandidates[i].id === contract.wordSelectedCandidateId)
                return i;
        }
        return -1;
    }

    // Explicit entry points keep the mouse and test paths identical.
    function applySelected() {
        if (contract.wordCanApply) contract.applyWord(contract.wordSelectedCandidateId);
    }
    function explore() { contract.openInterview(); }
    function dismiss() { contract.dismissWord(); }
    function selectCandidate(candidateId) { contract.selectWordCandidate(candidateId); }
    // Kept for hosts/tests that only know the pre-contract word surface.
    readonly property bool contractInterviewAvailable: contract.interviewInvokable
    readonly property bool contractReviewAvailable: contract.reviewSurfaceAvailable

    width: preferredWidth
    height: Math.min(panelColumn.implicitHeight + 26, maxPopupHeight)
    minimumWidth: 232
    minimumHeight: 132
    visible: false
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"

    Rectangle {
        id: panel
        anchors.fill: parent
        radius: 12
        color: rocTheme.surface
        border.color: rocTheme.focus
        border.width: 1

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 2
            anchors.topMargin: 3
            radius: 12
            color: "#66000000"
            z: -1
        }

        ColumnLayout {
            id: panelColumn
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Image {
                    source: "assets/roc-mark.png"
                    sourceSize.width: 40; sourceSize.height: 34
                    Layout.preferredWidth: 18; Layout.preferredHeight: 15
                    fillMode: Image.PreserveAspectFit
                    smooth: false
                    Accessible.ignored: true
                }
                Text {
                    text: qsTr("WORD COMPANION")
                    font.family: companion.monoFont
                    font.pixelSize: 9
                    font.bold: true
                    color: rocTheme.muted
                }
                Rectangle {
                    implicitWidth: statusText.implicitWidth + 10
                    implicitHeight: 16
                    radius: 3
                    color: "transparent"
                    border.color: companion.stateTone
                    border.width: 1
                    Text {
                        id: statusText
                        objectName: "wordCompanionStatusLabel"
                        anchors.centerIn: parent
                        text: companion.stateLabel
                        font.family: companion.monoFont
                        font.pixelSize: 8
                        font.bold: true
                        color: companion.stateTone
                    }
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    objectName: "wordCompanionDismissControl"
                    implicitWidth: 22; implicitHeight: 22
                    radius: 4
                    color: dismissMouse.containsMouse ? rocTheme.hover : "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Keep original wording and close")
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        font.pixelSize: 15
                        color: rocTheme.muted
                    }
                    MouseArea {
                        id: dismissMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: companion.dismiss()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: rocTheme.border
            }

            Flickable {
                id: bodyScroll
                objectName: "wordCompanionBody"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Math.min(bodyColumn.implicitHeight, 288)
                implicitHeight: Math.min(bodyColumn.implicitHeight, 288)
                Layout.minimumHeight: 0
                contentWidth: width
                contentHeight: bodyColumn.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: HudScrollBar {}

                ColumnLayout {
                    id: bodyColumn
                    width: Math.max(0, bodyScroll.width - 12)
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: qsTr("IN YOUR DRAFT")
                            font.family: companion.monoFont
                            font.pixelSize: 8
                            color: rocTheme.muted
                        }
                        Text {
                            id: originalLabel
                            objectName: "wordCompanionOriginalLabel"
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: contract.wordSurfaceAvailable ? contract.wordOriginal : ""
                            wrapMode: Text.WordWrap
                            maximumLineCount: 4
                            elide: Text.ElideRight
                            font.family: companion.sansFont
                            font.pixelSize: 14
                            font.italic: true
                            color: rocTheme.foreground
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: companion.showReplacement
                        implicitHeight: proposalColumn.implicitHeight + 16
                        radius: 8
                        color: rocTheme.inset
                        border.color: rocTheme.border
                        border.width: 1
                        ColumnLayout {
                            id: proposalColumn
                            x: 8; y: 8
                            width: Math.max(0, parent.width - 16)
                            spacing: 4
                            Text {
                                text: qsTr("PROPOSED IN PLACE")
                                font.family: companion.monoFont
                                font.pixelSize: 8
                                color: rocTheme.muted
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 6
                                Text {
                                    Layout.maximumWidth: Math.max(64, proposalColumn.width * 0.42)
                                    textFormat: Text.PlainText
                                    text: contract.wordOriginal
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 3
                                    elide: Text.ElideRight
                                    font.family: companion.sansFont
                                    font.pixelSize: 11
                                    font.strikeout: true
                                    color: rocTheme.warning
                                }
                                Text {
                                    text: "→"
                                    font.family: companion.monoFont
                                    font.pixelSize: 11
                                    color: rocTheme.muted
                                }
                                Text {
                                    objectName: "wordCompanionReplacementLabel"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    textFormat: Text.PlainText
                                    text: contract.wordReplacement
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                    font.family: companion.sansFont
                                    font.pixelSize: 15
                                    font.bold: true
                                    color: rocTheme.success
                                    Accessible.name: qsTr("Replacement: %1").arg(contract.wordReplacement)
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: contract.wordSurfaceAvailable && !companion.showReplacement &&
                                 !contract.wordNeedsClarification
                        spacing: 5
                        Row {
                            spacing: 6
                            PulseDots {
                                running: contract.wordApplying || contract.wordWorking
                                reduceMotion: appearance.reducedMotion
                            }
                            Text {
                                objectName: "wordCompanionBusyLabel"
                                text: contract.wordApplying
                                      ? qsTr("Applying to your draft…")
                                      : qsTr("Preparing a local wording suggestion…")
                                font.family: companion.sansFont
                                font.pixelSize: 12
                                color: rocTheme.muted
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("No suggestion is shown until there is a real replacement.")
                            wrapMode: Text.WordWrap
                            font.family: companion.sansFont
                            font.pixelSize: 10
                            color: rocTheme.muted
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: contract.wordExplanation.length > 0
                        spacing: 3
                        Text {
                            text: qsTr("WHAT CHANGES")
                            font.family: companion.monoFont
                            font.pixelSize: 8
                            color: rocTheme.muted
                        }
                        Text {
                            objectName: "wordCompanionExplanationLabel"
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: contract.wordExplanation
                            wrapMode: Text.WordWrap
                            font.family: companion.sansFont
                            font.pixelSize: 13
                            color: rocTheme.foreground
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: contract.wordAssumption.length > 0
                        implicitHeight: assumptionColumn.implicitHeight + 12
                        radius: 6
                        color: "transparent"
                        border.color: rocTheme.warning
                        border.width: 1
                        ColumnLayout {
                            id: assumptionColumn
                            x: 7; y: 6
                            width: Math.max(0, parent.width - 14)
                            spacing: 2
                            Text {
                                text: qsTr("PROPOSED ASSUMPTION · NOT CONFIRMED")
                                font.family: companion.monoFont
                                font.pixelSize: 8
                                color: rocTheme.warning
                            }
                            Text {
                                objectName: "wordCompanionAssumptionLabel"
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: contract.wordAssumption
                                wrapMode: Text.WordWrap
                                font.family: companion.sansFont
                                font.pixelSize: 11
                                color: rocTheme.warning
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: contract.wordNeedsClarification
                        implicitHeight: clarifyColumn.implicitHeight + 14
                        radius: 8
                        color: rocTheme.inset
                        border.color: rocTheme.warning
                        border.width: 1
                        ColumnLayout {
                            id: clarifyColumn
                            x: 8; y: 7
                            width: Math.max(0, parent.width - 16)
                            spacing: 4
                            Text {
                                text: qsTr("ONE DETAIL NEEDED")
                                font.family: companion.monoFont
                                font.pixelSize: 8
                                font.bold: true
                                color: rocTheme.warning
                            }
                            Text {
                                objectName: "wordCompanionClarificationLabel"
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                text: contract.wordClarificationQuestion
                                wrapMode: Text.WordWrap
                                font.family: companion.sansFont
                                font.pixelSize: 12
                                color: rocTheme.foreground
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Explore to answer it without losing this wording.")
                                wrapMode: Text.WordWrap
                                font.family: companion.sansFont
                                font.pixelSize: 10
                                color: rocTheme.muted
                            }
                        }
                    }

                    ColumnLayout {
                        objectName: "wordCompanionCandidates"
                        Layout.fillWidth: true
                        visible: companion.showAlternatives
                        spacing: 4
                        Text {
                            text: qsTr("OTHER READINGS · CHOOSE ONE")
                            font.family: companion.monoFont
                            font.pixelSize: 8
                            color: rocTheme.muted
                        }
                        Repeater {
                            id: candidateRepeater
                            model: companion.showAlternatives ? contract.wordCandidates : []
                            delegate: Rectangle {
                                id: candidateRow
                                required property var modelData
                                required property int index
                                objectName: "wordCompanionCandidate_" + index
                                readonly property bool selected: modelData.id === contract.wordSelectedCandidateId
                                readonly property string explanation: modelData.explanation !== undefined
                                                                      ? String(modelData.explanation) : ""
                                readonly property string assumption: modelData.assumption !== undefined
                                                                     ? String(modelData.assumption) : ""
                                readonly property string replacement: modelData.replacement !== undefined
                                                                      ? String(modelData.replacement) : ""
                                Layout.fillWidth: true
                                implicitHeight: candidateColumn.implicitHeight + 14
                                radius: 6
                                color: selected ? rocTheme.hover : "transparent"
                                border.color: selected ? rocTheme.focus : rocTheme.border
                                border.width: 1
                                opacity: modelData.requiresClarification === true ? 0.62 : 1.0
                                activeFocusOnTab: false
                                Accessible.role: Accessible.Button
                                Accessible.name: replacement + (explanation.length ? ". " + explanation : "") +
                                                 (modelData.requiresClarification === true
                                                  ? ". " + qsTr("Needs a clarifying question before it can be applied.")
                                                  : "")
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: modelData.requiresClarification === true
                                                 ? Qt.ArrowCursor : Qt.PointingHandCursor
                                    enabled: !contract.wordApplying && modelData.requiresClarification !== true
                                    onClicked: companion.selectCandidate(modelData.id)
                                }
                                ColumnLayout {
                                    id: candidateColumn
                                    x: 7; y: 6
                                    width: Math.max(0, parent.width - 14)
                                    spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 5
                                        Text {
                                            objectName: "wordCompanionCandidateLabel_" + index
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            textFormat: Text.PlainText
                                            text: replacement
                                            wrapMode: Text.WordWrap
                                            font.family: companion.sansFont
                                            font.pixelSize: 13
                                            font.bold: true
                                            color: candidateRow.selected ? rocTheme.foreground : rocTheme.muted
                                        }
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        visible: candidateRow.selected || modelData.requiresClarification === true
                                        Text {
                                            visible: candidateRow.selected
                                            text: qsTr("SELECTED")
                                            font.family: companion.monoFont
                                            font.pixelSize: 7
                                            color: rocTheme.focus
                                        }
                                        Text {
                                            visible: modelData.requiresClarification === true
                                            text: qsTr("NEEDS A QUESTION")
                                            font.family: companion.monoFont
                                            font.pixelSize: 7
                                            color: rocTheme.warning
                                        }
                                    }
                                    Text {
                                        objectName: "wordCompanionCandidateConsequence_" + index
                                        Layout.fillWidth: true
                                        visible: candidateRow.explanation.length > 0
                                        textFormat: Text.PlainText
                                        text: candidateRow.explanation
                                        wrapMode: Text.WordWrap
                                        font.family: companion.sansFont
                                        font.pixelSize: 11
                                        color: rocTheme.muted
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: candidateRow.assumption.length > 0
                                        textFormat: Text.PlainText
                                        text: qsTr("Assumes: ") + candidateRow.assumption
                                        wrapMode: Text.WordWrap
                                        font.family: companion.sansFont
                                        font.pixelSize: 10
                                        color: rocTheme.warning
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: rocTheme.border
            }

            Flow {
                Layout.fillWidth: true
                spacing: 6
                RocButton {
                    objectName: "wordCompanionApplyControl"
                    visible: contract.wordSurfaceAvailable && companion.showReplacement &&
                             !contract.wordNeedsClarification
                    label: contract.wordApplying ? qsTr("Applying…") : qsTr("Apply")
                    primary: true
                    compact: true
                    currentTheme: "dark"
                    reduceMotion: appearance.reducedMotion
                    active: contract.wordCanApply
                    Accessible.name: qsTr("Apply this wording")
                    onClicked: companion.applySelected()
                }
                RocButton {
                    objectName: "wordCompanionExploreControl"
                    label: contract.wordNeedsClarification ? qsTr("Explore & answer") : qsTr("Explore")
                    primary: contract.wordNeedsClarification
                    compact: true
                    currentTheme: "dark"
                    reduceMotion: appearance.reducedMotion
                    active: contract.wordSurfaceAvailable &&
                            (!contract.intentSurfaceAvailable || contract.canExploreInterview)
                    Accessible.name: qsTr("Open the companion interview with this draft")
                    onClicked: companion.explore()
                }
                RocButton {
                    objectName: "wordCompanionKeepOriginalControl"
                    label: qsTr("Keep original")
                    compact: true
                    quiet: true
                    currentTheme: "dark"
                    reduceMotion: appearance.reducedMotion
                    active: contract.wordSurfaceAvailable
                    Accessible.name: qsTr("Keep original wording")
                    onClicked: companion.dismiss()
                }
            }

            Text {
                objectName: "wordCompanionHint"
                Layout.fillWidth: true
                text: qsTr("Your editor keeps focus here. Explore opens keyboard controls.")
                wrapMode: Text.WordWrap
                font.family: companion.monoFont
                font.pixelSize: 8
                color: rocTheme.muted
            }
        }
    }

    component PulseDots: Row {
        id: dots
        property bool running: false
        property bool reduceMotion: false
        spacing: 3
        Repeater {
            model: 3
            Rectangle {
                required property int index
                id: pixel
                width: 5; height: 5
                radius: 1
                color: rocTheme.focus
                property real pulseOpacity: 0.35
                opacity: pulseOpacity
                SequentialAnimation {
                    running: dots.running && !dots.reduceMotion
                    loops: Animation.Infinite
                    PauseAnimation { duration: 140 * pixel.index }
                    NumberAnimation { target: pixel; property: "pulseOpacity"; to: 1.0; duration: 260 }
                    NumberAnimation { target: pixel; property: "pulseOpacity"; to: 0.35; duration: 280 }
                }
            }
        }
    }
}
