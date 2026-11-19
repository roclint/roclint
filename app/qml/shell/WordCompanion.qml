pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A phrase-level action, deliberately opened beside the source. Only explicit
// "Write my own answer" gives this window keyboard focus. No review dashboard.
Window {
    id: companion
    title: qsTr("Roclint word companion")
    property bool detailsExpanded: false
    property bool answerEditing: false
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Segoe UI"
    readonly property bool reducedMotion: appearance.reducedMotion
    readonly property bool showReplacement: !contract.wordChecking && !contract.wordFailed && contract.wordReplacement.length > 0 && !contract.wordNeedsClarification
    readonly property var alternativeReplacements: Array.from(contract.wordCandidates).filter(function(candidate) {
        return candidate.id !== contract.wordSelectedCandidateId && !candidate.requiresClarification;
    })
    readonly property bool contractInterviewAvailable: contract.interviewInvokable
    readonly property bool contractReviewAvailable: contract.reviewSurfaceAvailable
    readonly property string stateLabel: contract.wordApplied ? qsTr("APPLIED") : contract.wordFailed ? qsTr("CHECK DRAFT")
        : contract.wordChecking ? qsTr("CHECKING") : contract.wordApplying ? qsTr("APPLYING") : contract.wordNeedsClarification ? qsTr("ONE DETAIL") : qsTr("READY")
    readonly property int preferredWidth: Math.max(240, Math.min(showReplacement ? 280 : 340, (screen ? screen.width : 400) - 24))
    readonly property int maxPopupHeight: Math.max(240, Math.min(580, (screen ? screen.height : 900) * 0.66))
    CompanionContract { id: contract }
    RocTheme { id: theme }
    FontLoader { id: interFont; source: "assets/InterVariable.ttf" }
    Settings { id: appearance; category: "appearance"; property bool reducedMotion: false }

    function applySelected() { contract.applyWord(contract.wordSelectedCandidateId); }
    function explore() { contract.openInterview(); }
    function rewriteDraft() { contract.rewriteWordDraft(); }
    function dismiss() { contract.dismissWord(); }
    function answer(text) { if (contract.answerWord(text)) answerEditing = false; }
    function editAnswer() {
        answerEditing = true;
        Qt.callLater(function() { companion.requestActivate(); answerInput.forceActiveFocus(); });
    }
    width: preferredWidth
    height: Math.min(content.implicitHeight + 20, maxPopupHeight)
    minimumWidth: 240
    visible: false
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | (answerEditing ? 0 : Qt.WindowDoesNotAcceptFocus)
    color: "transparent"
    onVisibleChanged: {
        if (visible) { detailsExpanded = false; bodyScroll.contentY = 0; if (!reducedMotion) reveal.start(); }
        else if (!contract.wordVisible) { answerEditing = false; answerInput.text = ""; }
    }
    onAnswerEditingChanged: Qt.callLater(function() { if (contract.wordVisible) companion.show(); })
    Connections {
        target: contract.host
        ignoreUnknownSignals: true
        function onWordRefinementChanged() { if (!contract.wordNeedsClarification) companion.answerEditing = false; }
    }
    NumberAnimation { id: reveal; target: panel; property: "opacity"; from: 0.6; to: 1; duration: 120 }
    Rectangle {
        id: panel
        anchors.fill: parent
        radius: 14
        color: theme.background
        border.width: 1
        border.color: theme.border
        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 10
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Image { source: "assets/roc-mark.png"; Layout.preferredWidth: 16; Layout.preferredHeight: 14; fillMode: Image.PreserveAspectFit; smooth: false; Accessible.ignored: true }
                Text {
                    text: contract.wordApplied || companion.showReplacement ? qsTr("Wording") : qsTr("Roclint")
                    font.family: companion.monoFont; font.pixelSize: 11; color: theme.muted
                }
                Text {
                    objectName: "wordCompanionStatusLabel"
                    text: companion.stateLabel
                    visible: false // Kept as a semantic state, not another user-facing badge.
                }
                Item { Layout.fillWidth: true }
                RocButton {
                    objectName: "wordCompanionDismissControl"
                    label: ""; iconName: "x"; tip: qsTr("Dismiss this suggestion")
                    quiet: true; implicitWidth: 28; implicitHeight: 28
                    compact: true; currentTheme: "dark"; reduceMotion: companion.reducedMotion
                    onClicked: companion.dismiss()
                }
            }
            Flickable {
                id: bodyScroll
                objectName: "wordCompanionBody"
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: Math.min(body.implicitHeight, companion.maxPopupHeight - footer.implicitHeight - 100)
                contentWidth: width
                contentHeight: body.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: HudScrollBar { policy: ScrollBar.AsNeeded; background: Item {} }
                ColumnLayout {
                    id: body
                    width: bodyScroll.width - (bodyScroll.contentHeight > bodyScroll.height ? 10 : 0)
                    spacing: 6
                    Text {
                        objectName: "wordCompanionOriginalLabel"
                        Layout.fillWidth: true
                        text: contract.wordOriginal
                        visible: !companion.showReplacement && !contract.wordApplied
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: companion.sansFont; font.pixelSize: 13; font.weight: Font.Normal
                        color: theme.muted
                    }
                    Text {
                        objectName: "wordCompanionClarificationLabel"
                        Layout.fillWidth: true
                        visible: contract.wordNeedsClarification && !contract.wordChecking
                        text: contract.wordClarificationQuestion
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: companion.sansFont; font.pixelSize: 17; font.weight: Font.Medium
                        color: theme.foreground
                    }
                    Repeater {
                        model: contract.wordNeedsClarification && !contract.wordChecking ? contract.wordChoices : []
                        delegate: Action {
                            required property string modelData
                            required property int index
                            objectName: "wordCompanionChoice_" + index
                            Layout.fillWidth: true
                            label: modelData
                            active: !contract.wordWorking
                            onClicked: companion.answer(modelData)
                        }
                    }
                    Action {
                        objectName: "wordCompanionCustomAnswerControl"
                        visible: contract.wordNeedsClarification && !contract.wordChecking && !companion.answerEditing && contract.wordAnswerInvokable
                        label: qsTr("Write my own answer…")
                        quiet: true
                        onClicked: companion.editAnswer()
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: companion.answerEditing && contract.wordNeedsClarification
                        TextArea {
                            id: answerInput
                            objectName: "wordCompanionAnswerInput"
                            Layout.fillWidth: true
                            implicitHeight: 80
                            placeholderText: qsTr("What should this phrase mean here?")
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            font.family: companion.sansFont; font.pixelSize: 14; font.weight: Font.Normal
                            color: theme.foreground
                            placeholderTextColor: theme.muted
                            background: Rectangle { radius: 8; color: theme.inset; border.color: answerInput.activeFocus ? theme.focus : theme.border }
                            Accessible.name: qsTr("Your clarification for this phrase")
                        }
                        RowLayout {
                            Action {
                                objectName: "wordCompanionAnswerControl"
                                label: qsTr("Use this answer"); primary: true
                                active: answerInput.text.trim().length > 0 && !contract.wordChecking
                                onClicked: companion.answer(answerInput.text)
                            }
                            Action { label: qsTr("Cancel"); quiet: true; onClicked: companion.answerEditing = false }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: companion.showReplacement && !contract.wordApplied
                        spacing: 6
                        Text {
                            Layout.fillWidth: true
                            text: contract.wordOriginal
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.family: companion.sansFont; font.pixelSize: 13; font.weight: Font.Normal; font.strikeout: true
                            color: theme.muted
                        }
                        Action {
                            objectName: "wordCompanionApplyControl"
                            Layout.fillWidth: true
                            label: contract.wordReplacement
                            tip: qsTr("Replace the underlined text")
                            primary: true
                            active: contract.wordCanApply
                            onClicked: companion.applySelected()
                        }
                        Repeater {
                            model: companion.alternativeReplacements
                            delegate: Action {
                                required property var modelData
                                required property int index
                                objectName: "wordCompanionCandidate_" + index
                                Layout.fillWidth: true
                                label: modelData.assumption
                                    ? qsTr("%1\nAssumes: %2").arg(modelData.replacement).arg(modelData.assumption)
                                    : modelData.replacement
                                active: contract.wordCanApply
                                onClicked: contract.applyWord(modelData.id)
                            }
                        }
                        Text {
                            objectName: "wordCompanionExplanationLabel"
                            Layout.fillWidth: true
                            visible: companion.detailsExpanded && contract.wordExplanation.length > 0
                            text: contract.wordExplanation
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.family: companion.sansFont; font.pixelSize: 13; font.weight: Font.Normal
                            color: theme.muted
                        }
                    }
                    Text {
                        objectName: "wordCompanionAssumptionLabel"
                        Layout.fillWidth: true
                        visible: companion.showReplacement && !contract.wordApplied && contract.wordAssumption.length > 0
                        text: qsTr("Assumes: %1").arg(contract.wordAssumption)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: companion.sansFont; font.pixelSize: 12; font.weight: Font.Normal
                        color: theme.warning
                    }
                    Text {
                        objectName: "wordCompanionBusyLabel"
                        Layout.fillWidth: true
                        visible: contract.wordChecking || contract.wordApplying
                        text: contract.wordMessage.length ? contract.wordMessage : qsTr("Checking this phrase…")
                        wrapMode: Text.Wrap
                        font.family: companion.sansFont; font.pixelSize: 14; font.weight: Font.Normal
                        color: theme.muted
                    }
                    Text {
                        objectName: "wordCompanionFeedbackLabel"
                        Layout.fillWidth: true
                        visible: contract.wordMessage.length > 0 && !contract.wordChecking && !contract.wordApplying
                        text: contract.wordMessage
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: companion.sansFont; font.pixelSize: 14; font.weight: Font.Normal
                        color: contract.wordApplied ? theme.success : theme.warning
                    }
                    ColumnLayout {
                        objectName: "wordCompanionCandidates"
                        Layout.fillWidth: true
                        visible: companion.detailsExpanded && !contract.wordChecking && !contract.wordNeedsClarification
                        spacing: 6
                        Action { objectName: "wordCompanionExploreControl"; label: qsTr("Explore further…"); quiet: true; onClicked: companion.explore() }
                    }
                }
            }
            ColumnLayout {
                id: footer
                Layout.fillWidth: true
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    visible: !contract.wordApplied
                    Action {
                        objectName: "wordCompanionRetryControl"
                        label: qsTr("Try again")
                        visible: contract.wordFailed && contract.wordRetryInvokable
                        onClicked: contract.retryWord()
                    }
                    Action {
                        objectName: "wordCompanionKeepOriginalControl"
                        label: contract.wordNeedsClarification ? qsTr("Skip") : qsTr("Dismiss")
                        quiet: true
                        onClicked: companion.dismiss()
                    }
                    Item { Layout.fillWidth: true }
                    Action {
                        objectName: "wordCompanionDetailsControl"
                        label: companion.detailsExpanded ? qsTr("Less") : qsTr("Why?")
                        quiet: true
                        visible: !contract.wordChecking && !contract.wordFailed && !contract.wordNeedsClarification
                        onClicked: companion.detailsExpanded = !companion.detailsExpanded
                    }
                }
                RowLayout {
                    visible: contract.wordApplied
                    Action { objectName: "wordCompanionUndoControl"; label: qsTr("Undo"); visible: contract.wordUndoAvailable; onClicked: contract.undoWord() }
                    Action { objectName: "wordCompanionDoneControl"; label: qsTr("Close"); quiet: true; onClicked: companion.dismiss() }
                }
                Action {
                    objectName: "wordCompanionRewriteControl"
                    label: qsTr("Full rewrite…"); quiet: !contract.wordFailed; primary: contract.wordFailed
                    visible: contract.wordRewriteInvokable && !contract.wordApplied &&
                             (companion.detailsExpanded || !companion.showReplacement)
                    active: (contract.hasWordRefinement || contract.wordFailed || contract.wordChecking) && !contract.wordApplying
                    onClicked: companion.rewriteDraft()
                }
            }
        }
    }
    component Action: RocButton {
        id: actionControl
        compact: true
        currentTheme: "dark"
        reduceMotion: companion.reducedMotion
        font.family: companion.sansFont
        font.pixelSize: 13
        font.weight: Font.Normal
        focusPolicy: companion.answerEditing ? Qt.StrongFocus : Qt.NoFocus
        implicitHeight: Math.max(32, contentItem.implicitHeight + 14)
        contentItem: Text {
            text: actionControl.label
            textFormat: Text.PlainText
            font: actionControl.font
            color: actionControl.primary ? "#FFFFFF" : theme.foreground
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
        }
    }
}
