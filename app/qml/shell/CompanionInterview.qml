pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Substantial adaptive interview that expands the word/draft surface without
// restarting it. Every displayed question, choice, decision and open topic is
// read from HostController; the component only groups and presents them.
//
// Correcting an answer uses changeIntentDecision(itemId, newText, expectedVersion)
// so the host keeps provenance and invalidates dependent proposals. Skipping
// and "I don't know" are sent as dispositions; the UI never renders them as a
// confirmed value.
FocusScope {
    id: root
    objectName: "companionInterview"
    property string currentTheme: "dark"
    property bool reducedMotion: false
    property bool showingWordView: false
    property string changingItemId: ""
    property string changeDraftText: ""
    property bool showEvidence: false
    property bool showSuperseded: false
    property int lastSeenTurns: 0
    property bool justAnswered: false

    signal rewriteRequested()
    signal keepOriginalRequested()
    signal editDraftRequested()

    CompanionContract { id: contract }
    RocTheme { id: rocTheme; mode: root.currentTheme }
    readonly property string monoFont: Qt.platform.os === "osx" ? "Menlo" : "Consolas"
    readonly property string sansFont: "Inter Variable"
    readonly property color errorColor: "#EEA4AF"

    implicitWidth: contentColumn.implicitWidth
    implicitHeight: contentColumn.implicitHeight
    activeFocusOnTab: false

    readonly property var sourceItems: contract.intentItems.filter(function(item) {
        return item && item.kind === "source_requirement" &&
               ((item.sourceExcerpt || "").length > 0 || (item.text || "").length > 0);
    })
    readonly property var confirmedItems: contract.intentItems.filter(function(item) {
        return item && item.status === "confirmed" &&
               (item.kind === "confirmed_decision" || item.kind === "preference" || item.kind === "prohibition");
    })
    readonly property var proposedItems: contract.intentItems.filter(function(item) {
        return item && item.status === "proposed";
    })
    readonly property var openItems: contract.intentItems.filter(function(item) {
        return item && item.status === "open";
    })
    readonly property var evidenceItems: contract.intentItems.filter(function(item) {
        return item && item.kind === "evidence";
    })
    readonly property var supersededItems: contract.intentItems.filter(function(item) {
        return item && item.status === "superseded";
    })
    readonly property string draftExcerpt: {
        if (contract.hasWordRefinement && contract.wordOriginal.length > 0) return contract.wordOriginal;
        if (sourceItems.length > 0) {
            const first = sourceItems[0];
            return (first.sourceExcerpt || "").length > 0 ? first.sourceExcerpt : (first.text || "");
        }
        return "";
    }
    readonly property bool interviewAvailable: contract.intentSurfaceAvailable
    readonly property bool hasQuestion: contract.isInterviewActive && contract.interviewQuestion.length > 0
    readonly property bool canChange: contract.canChangeDecisions && !contract.isInterviewBusy

    function kindLabel(kind) {
        switch (kind) {
        case "source_requirement": return qsTr("FROM YOUR DRAFT");
        case "confirmed_decision": return qsTr("DECISION");
        case "preference": return qsTr("PREFERENCE");
        case "prohibition": return qsTr("MUST NOT");
        case "evidence": return qsTr("CODE EVIDENCE");
        case "proposed_assumption": return qsTr("PROPOSED");
        case "open_choice": return qsTr("OPEN");
        case "superseded_decision": return qsTr("REPLACED");
        default: return qsTr("NOTE");
        }
    }
    function displayStatus(item) {
        if (!item) return "";
        if (item.status === "confirmed") return qsTr("CONFIRMED");
        if (item.status === "proposed") return qsTr("PROPOSED · NOT CONFIRMED");
        if (item.status === "open") return qsTr("OPEN");
        if (item.status === "superseded") return qsTr("NOT CURRENT");
        return "";
    }
    function submit(text, disposition) {
        if (!interviewAvailable || contract.isInterviewBusy) return;
        contract.submitInterview(text, disposition);
        if (disposition === "custom" || disposition === "answer") customAnswer.text = "";
    }
    function submitCustom() {
        const text = customAnswer.text.trim();
        if (!text.length) return;
        submit(text, "custom");
    }
    function startChange(item) {
        if (!item || !canChange) return;
        if (root.showingWordView) root.showingWordView = false;
        root.changingItemId = item.id;
        root.changeDraftText = (item.rawAnswer || "").length > 0 ? item.rawAnswer : item.text;
    }
    function saveChange(item) {
        if (!item || !root.canChange) return;
        const text = root.changeDraftText.trim();
        if (!text.length) return;
        contract.changeDecision(item.id, text);
        root.changingItemId = "";
    }
    function cancelChange() {
        root.changingItemId = "";
    }
    function focusQuestion() {
        if (!root.visible || contract.isInterviewBusy) return;
        if (root.showingWordView) {
            wordView.focusFirst();
            return;
        }
        if (customAnswer.visible && customAnswer.enabled) {
            customAnswer.forceActiveFocus(Qt.TabFocusReason);
        } else if (choiceRepeater.count > 0) {
            const first = choiceRepeater.itemAt(0);
            if (first) first.forceActiveFocus(Qt.TabFocusReason);
        }
    }
    function openWordView() {
        if (!contract.hasWordRefinement) return;
        root.showingWordView = true;
        Qt.callLater(function() { wordView.focusFirst(); });
    }

    Component.onCompleted: lastSeenTurns = contract.interviewTurnsCompleted
    Connections {
        target: contract
        function onInterviewTurnsCompletedChanged() {
            if (contract.interviewTurnsCompleted > root.lastSeenTurns && root.lastSeenTurns >= 0) {
                root.justAnswered = true;
                answeredTimer.restart();
            }
            root.lastSeenTurns = contract.interviewTurnsCompleted;
        }
    }
    Timer {
        id: answeredTimer
        interval: 2600
        onTriggered: root.justAnswered = false
    }

    component TinyButton: RocButton {
        currentTheme: root.currentTheme
        reduceMotion: root.reducedMotion
        compact: true
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

    ColumnLayout {
        id: contentColumn
        width: parent.width
        spacing: 10

        // ------------------------------------------------------------ header
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 6
            Text {
                text: qsTr("COMPANION / INTERVIEW")
                font.family: root.monoFont
                font.pixelSize: 9
                font.bold: true
                color: rocTheme.muted
            }
            Rectangle {
                visible: contract.interviewTurnsCompleted > 0
                implicitWidth: turnsLabel.implicitWidth + 10
                implicitHeight: 16
                radius: 3
                color: "transparent"
                border.color: rocTheme.border
                border.width: 1
                Text {
                    id: turnsLabel
                    objectName: "interviewTurnsLabel"
                    anchors.centerIn: parent
                    text: qsTr("%1 DECIDED").arg(contract.interviewTurnsCompleted)
                    font.family: root.monoFont
                    font.pixelSize: 8
                    color: rocTheme.muted
                }
            }
            Rectangle {
                visible: root.justAnswered
                implicitWidth: savedLabel.implicitWidth + 10
                implicitHeight: 16
                radius: 3
                color: "transparent"
                border.color: rocTheme.success
                border.width: 1
                Text {
                    id: savedLabel
                    objectName: "interviewSavedLabel"
                    anchors.centerIn: parent
                    text: qsTr("SAVED")
                    font.family: root.monoFont
                    font.pixelSize: 8
                    font.bold: true
                    color: rocTheme.success
                }
            }
            Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
            TinyButton {
                objectName: "interviewWordViewControl"
                visible: contract.hasWordRefinement
                label: qsTr("Word view")
                quiet: true
                active: contract.hasWordRefinement
                Accessible.name: qsTr("Return to the word-level suggestion")
                onClicked: root.openWordView()
            }
        }

        // ---------------------------------------------------------- word view
        Rectangle {
            id: wordView
            objectName: "interviewWordView"
            visible: root.showingWordView
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: wordViewColumn.implicitHeight + 20
            radius: 10
            color: rocTheme.inset
            border.color: rocTheme.border
            border.width: 1
            function focusFirst() { wordApply.forceActiveFocus(Qt.TabFocusReason); }
            ColumnLayout {
                id: wordViewColumn
                x: 10; y: 10
                width: Math.max(0, parent.width - 20)
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        text: qsTr("WORD IN YOUR DRAFT")
                        font.family: root.monoFont
                        font.pixelSize: 8
                        color: rocTheme.muted
                    }
                    Item { Layout.fillWidth: true }
                    TinyButton {
                        objectName: "interviewBackToInterviewControl"
                        label: qsTr("Back to interview")
                        quiet: true
                        onClicked: {
                            root.showingWordView = false;
                            Qt.callLater(root.focusQuestion);
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: contract.wordOriginal.length > 0
                    textFormat: Text.PlainText
                    text: "“" + contract.wordOriginal + "”"
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 14
                    font.italic: true
                    color: rocTheme.foreground
                }
                Text {
                    Layout.fillWidth: true
                    visible: contract.wordReplacement.length > 0
                    textFormat: Text.PlainText
                    text: contract.wordReplacement
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 15
                    font.bold: true
                    color: rocTheme.success
                }
                Text {
                    Layout.fillWidth: true
                    visible: contract.wordExplanation.length > 0
                    textFormat: Text.PlainText
                    text: contract.wordExplanation
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 12
                    color: rocTheme.muted
                }
                Text {
                    Layout.fillWidth: true
                    visible: contract.wordAssumption.length > 0
                    textFormat: Text.PlainText
                    text: qsTr("Assumes: ") + contract.wordAssumption
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 10
                    color: rocTheme.warning
                }
                Text {
                    Layout.fillWidth: true
                    visible: !contract.hasWordRefinement
                    text: qsTr("The word-level suggestion is no longer available. Your draft and answers are still here.")
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 12
                    color: rocTheme.muted
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    TinyButton {
                        id: wordApply
                        objectName: "interviewWordApplyControl"
                        visible: contract.hasWordRefinement
                        label: qsTr("Apply wording")
                        primary: true
                        active: contract.wordCanApply
                        onClicked: contract.applyWord(contract.wordSelectedCandidateId)
                    }
                    TinyButton {
                        objectName: "interviewWordClarifyControl"
                        visible: contract.hasWordRefinement
                        label: contract.wordNeedsClarification ? qsTr("Answer below") : qsTr("Explore in interview")
                        quiet: !contract.wordNeedsClarification
                        primary: contract.wordNeedsClarification
                        active: contract.hasWordRefinement
                        onClicked: {
                            root.showingWordView = false;
                            Qt.callLater(root.focusQuestion);
                        }
                    }
                    TinyButton {
                        label: qsTr("Keep original wording")
                        quiet: true
                        active: contract.wordSurfaceAvailable
                        onClicked: contract.dismissWord()
                    }
                }
            }
        }

        // ----------------------------------------------------- source excerpt
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: !root.showingWordView && root.draftExcerpt.length > 0
            spacing: 2
            Text {
                text: qsTr("THE PHRASE IN QUESTION")
                font.family: root.monoFont
                font.pixelSize: 8
                color: rocTheme.muted
            }
            Text {
                objectName: "interviewDraftExcerpt"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                textFormat: Text.PlainText
                text: "“" + root.draftExcerpt + "”"
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.family: root.sansFont
                font.pixelSize: 13
                font.italic: true
                color: rocTheme.foreground
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Your full draft stays in the editor. Roclint carries it forward; you don't need to repeat it.")
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 10
                color: rocTheme.muted
            }
        }

        // ------------------------------------------------------ current question
        Rectangle {
            id: questionCard
            objectName: "interviewQuestionCard"
            visible: !root.showingWordView && root.hasQuestion
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: questionColumn.implicitHeight + 24
            radius: 12
            color: "transparent"
            border.color: contract.isInterviewBusy ? rocTheme.focus : rocTheme.warning
            border.width: 1

            ColumnLayout {
                id: questionColumn
                x: 12; y: 12
                width: Math.max(0, parent.width - 24)
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        text: qsTr("ONE OPEN DETAIL")
                        font.family: root.monoFont
                        font.pixelSize: 8
                        font.bold: true
                        color: rocTheme.warning
                    }
                    Item { Layout.fillWidth: true }
                    Row {
                        visible: contract.isInterviewBusy
                        spacing: 5
                        PulseDots {
                            running: contract.isInterviewBusy
                            reduceMotion: root.reducedMotion
                        }
                        Text {
                            objectName: "interviewBusyLabel"
                            text: qsTr("Thinking about your answer…")
                            font.family: root.monoFont
                            font.pixelSize: 8
                            color: rocTheme.focus
                        }
                    }
                }
                Text {
                    objectName: "interviewQuestionLabel"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    textFormat: Text.PlainText
                    text: contract.interviewQuestion
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 21
                    font.weight: Font.DemiBold
                    color: rocTheme.foreground
                }
                Text {
                    objectName: "interviewConsequenceLabel"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: contract.interviewConsequence.length > 0
                    textFormat: Text.PlainText
                    text: contract.interviewConsequence
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 12
                    color: rocTheme.muted
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: !contract.isInterviewBusy && contract.interviewChoices.length > 0
                    spacing: 5
                    Repeater {
                        id: choiceRepeater
                        model: contract.interviewChoices
                        delegate: Rectangle {
                            id: choiceRow
                            required property var modelData
                            required property int index
                            objectName: "interviewChoice_" + index
                            readonly property string choiceLabel: modelData.label !== undefined ? String(modelData.label) : ""
                            readonly property string choiceConsequence: modelData.consequence !== undefined
                                                                         ? String(modelData.consequence) : ""
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: choiceColumn.implicitHeight + 16
                            radius: 7
                            color: choiceRow.activeFocus ? rocTheme.hover : rocTheme.inset
                            border.color: choiceRow.activeFocus ? rocTheme.focus : rocTheme.border
                            border.width: choiceRow.activeFocus ? 2 : 1
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: choiceLabel + (choiceConsequence.length ? ". " + choiceConsequence : "")
                            Accessible.onPressAction: root.submit(choiceLabel, "answer")
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                    root.submit(choiceRow.choiceLabel, "answer");
                                    event.accepted = true;
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    choiceRow.forceActiveFocus(Qt.TabFocusReason);
                                    root.submit(choiceRow.choiceLabel, "answer");
                                }
                            }
                            ColumnLayout {
                                id: choiceColumn
                                x: 9; y: 7
                                width: Math.max(0, parent.width - 18)
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    textFormat: Text.PlainText
                                    text: choiceRow.choiceLabel
                                    wrapMode: Text.WordWrap
                                    font.family: root.sansFont
                                    font.pixelSize: 13
                                    font.bold: true
                                    color: rocTheme.foreground
                                }
                                Text {
                                    objectName: "interviewChoiceConsequence_" + index
                                    Layout.fillWidth: true
                                    visible: choiceRow.choiceConsequence.length > 0
                                    textFormat: Text.PlainText
                                    text: choiceRow.choiceConsequence
                                    wrapMode: Text.WordWrap
                                    font.family: root.sansFont
                                    font.pixelSize: 11
                                    color: rocTheme.muted
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 6
                    TextField {
                        id: customAnswer
                        objectName: "interviewCustomAnswer"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        enabled: !contract.isInterviewBusy
                        readOnly: contract.isInterviewBusy
                        implicitHeight: 38
                        placeholderText: qsTr("Or say it in your own words")
                        Accessible.name: qsTr("Your answer in your own words")
                        font.family: root.sansFont
                        font.pixelSize: 13
                        color: rocTheme.foreground
                        placeholderTextColor: rocTheme.muted
                        background: Rectangle {
                            color: rocTheme.inset
                            border.color: customAnswer.activeFocus ? rocTheme.focus : rocTheme.border
                            border.width: customAnswer.activeFocus ? 2 : 1
                            radius: 7
                        }
                        Keys.onEscapePressed: customAnswer.focus = false
                        onAccepted: root.submitCustom()
                    }
                    TinyButton {
                        objectName: "interviewUseAnswerControl"
                        label: qsTr("Use my answer")
                        primary: true
                        active: !contract.isInterviewBusy && customAnswer.text.trim().length > 0
                        onClicked: root.submitCustom()
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    TinyButton {
                        objectName: "interviewUnknownControl"
                        label: qsTr("I don't know")
                        quiet: true
                        active: !contract.isInterviewBusy
                        Accessible.name: qsTr("I don't know — record this as uncertain, not as a choice")
                        onClicked: root.submit("", "unknown")
                    }
                    TinyButton {
                        objectName: "interviewSkipControl"
                        label: qsTr("Skip this question")
                        quiet: true
                        active: !contract.isInterviewBusy
                        Accessible.name: qsTr("Skip this question and leave it open")
                        onClicked: root.submit("", "skip")
                    }
                    TinyButton {
                        objectName: "interviewRewriteControl"
                        label: qsTr("Full rewrite now")
                        quiet: !root.hasQuestion
                        onClicked: root.rewriteRequested()
                    }
                }

                Text {
                    objectName: "interviewSkipNote"
                    Layout.fillWidth: true
                    text: qsTr("Skipping leaves this topic open; it does not confirm an answer. “I don't know” records an explicit unknown.")
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 10
                    color: rocTheme.muted
                }

                Rectangle {
                    Layout.fillWidth: true
                    visible: contract.interviewFeedback.length > 0
                    implicitHeight: feedbackLabel.implicitHeight + 12
                    radius: 6
                    color: "transparent"
                    border.color: rocTheme.warning
                    border.width: 1
                    Text {
                        id: feedbackLabel
                        objectName: "interviewFeedbackLabel"
                        x: 7; y: 6
                        width: Math.max(0, parent.width - 14)
                        textFormat: Text.PlainText
                        text: contract.interviewFeedback
                        wrapMode: Text.WordWrap
                        font.family: root.sansFont
                        font.pixelSize: 11
                        color: rocTheme.warning
                    }
                }
            }
        }

        // --------------------------------------------------- no open questions
        Rectangle {
            visible: !root.showingWordView && contract.isInterviewActive && !root.hasQuestion
            Layout.fillWidth: true
            implicitHeight: noQuestionColumn.implicitHeight + 20
            radius: 10
            color: rocTheme.inset
            border.color: rocTheme.border
            border.width: 1
            ColumnLayout {
                id: noQuestionColumn
                x: 10; y: 10
                width: Math.max(0, parent.width - 20)
                spacing: 6
                Text {
                    text: qsTr("NO OPEN QUESTIONS RIGHT NOW")
                    font.family: root.monoFont
                    font.pixelSize: 8
                    font.bold: true
                    color: rocTheme.success
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Your decisions are saved. Continue with a full rewrite, go back to the word-level view, or edit the draft yourself.")
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 12
                    color: rocTheme.muted
                }
            }
        }

        // ------------------------------------------------------ understanding
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: !root.showingWordView
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 6
                Text {
                    text: qsTr("CURRENT UNDERSTANDING")
                    font.family: root.monoFont
                    font.pixelSize: 9
                    font.bold: true
                    color: rocTheme.foreground
                }
                Text {
                    objectName: "interviewCountsLabel"
                    Layout.fillWidth: true
                    text: qsTr("%1 confirmed · %2 proposed · %3 open")
                          .arg(root.confirmedItems.length)
                          .arg(root.proposedItems.length)
                          .arg(root.openItems.length)
                    wrapMode: Text.WordWrap
                    font.family: root.monoFont
                    font.pixelSize: 8
                    color: rocTheme.muted
                }
            }
            Text {
                Layout.fillWidth: true
                visible: contract.briefSummary.length > 0
                textFormat: Text.PlainText
                text: contract.briefSummary
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 11
                color: rocTheme.muted
            }
            Text {
                Layout.fillWidth: true
                visible: contract.briefSummary.length > 0
                text: qsTr("Generated summary — display only. Your answers and quoted source lines are authoritative.")
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 9
                color: rocTheme.muted
            }

            // Confirmed decisions, with correction.
            Repeater {
                model: root.confirmedItems
                delegate: Rectangle {
                    id: confirmedRow
                    required property var modelData
                    readonly property bool editing: root.changingItemId === modelData.id
                    objectName: "interviewItem_" + modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: confirmedColumn.implicitHeight + 18
                    radius: 7
                    color: rocTheme.inset
                    border.color: confirmedRow.editing ? rocTheme.focus : rocTheme.border
                    border.width: 1
                    onEditingChanged: if (editing)
                        Qt.callLater(function() { changeField.forceActiveFocus(Qt.TabFocusReason); })
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 3
                        radius: 1
                        color: rocTheme.success
                    }
                    ColumnLayout {
                        id: confirmedColumn
                        x: 10; y: 8
                        width: Math.max(0, parent.width - 20)
                        spacing: 4
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                objectName: "interviewItemKind_" + modelData.id
                                text: root.kindLabel(modelData.kind)
                                font.family: root.monoFont
                                font.pixelSize: 8
                                font.bold: true
                                color: rocTheme.success
                            }
                            Text {
                                objectName: "interviewItemStatus_" + modelData.id
                                text: root.displayStatus(modelData)
                                font.family: root.monoFont
                                font.pixelSize: 8
                                color: rocTheme.success
                            }
                            Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
                            TinyButton {
                                objectName: "interviewChangeControl_" + modelData.id
                                visible: !confirmedRow.editing
                                label: qsTr("Change")
                                quiet: true
                                active: root.canChange
                                Accessible.name: qsTr("Change this answer")
                                onClicked: root.startChange(modelData)
                            }
                        }
                        Text {
                            objectName: "interviewItemText_" + modelData.id
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            textFormat: Text.PlainText
                            text: modelData.text
                            wrapMode: Text.WordWrap
                            font.family: root.sansFont
                            font.pixelSize: 13
                            color: rocTheme.foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: (modelData.rawAnswer || "").length > 0 && modelData.rawAnswer !== modelData.text
                            textFormat: Text.PlainText
                            text: qsTr("Your words: “%1”").arg(modelData.rawAnswer)
                            wrapMode: Text.WordWrap
                            font.family: root.sansFont
                            font.pixelSize: 10
                            color: rocTheme.muted
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: confirmedRow.editing
                            spacing: 4
                            TextField {
                                id: changeField
                                objectName: "interviewChangeField_" + modelData.id
                                Layout.fillWidth: true
                                implicitHeight: 36
                                text: confirmedRow.editing ? root.changeDraftText : ""
                                enabled: root.canChange
                                Accessible.name: qsTr("Your corrected answer")
                                font.family: root.sansFont
                                font.pixelSize: 13
                                color: rocTheme.foreground
                                background: Rectangle {
                                    color: rocTheme.background
                                    border.color: changeField.activeFocus ? rocTheme.focus : rocTheme.border
                                    border.width: changeField.activeFocus ? 2 : 1
                                    radius: 6
                                }
                                onTextEdited: root.changeDraftText = text
                                onAccepted: root.saveChange(modelData)
                                Keys.onEscapePressed: root.cancelChange()
                            }
                            RowLayout {
                                spacing: 6
                                TinyButton {
                                    objectName: "interviewSaveChange_" + modelData.id
                                    label: qsTr("Save change")
                                    primary: true
                                    active: root.canChange && root.changeDraftText.trim().length > 0
                                    onClicked: root.saveChange(modelData)
                                }
                                TinyButton {
                                    label: qsTr("Cancel")
                                    quiet: true
                                    onClicked: root.cancelChange()
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Saving replaces this answer. The earlier wording stays in the interview history.")
                                wrapMode: Text.WordWrap
                                font.family: root.sansFont
                                font.pixelSize: 9
                                color: rocTheme.muted
                            }
                        }
                    }
                }
            }

            // Verbatim source requirements.
            Repeater {
                model: root.sourceItems
                delegate: Rectangle {
                    required property var modelData
                    objectName: "interviewSource_" + modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: sourceRowColumn.implicitHeight + 16
                    radius: 7
                    color: "transparent"
                    border.color: rocTheme.border
                    border.width: 1
                    ColumnLayout {
                        id: sourceRowColumn
                        x: 9; y: 7
                        width: Math.max(0, parent.width - 18)
                        spacing: 3
                        Text {
                            text: qsTr("FROM YOUR DRAFT")
                            font.family: root.monoFont
                            font.pixelSize: 8
                            color: rocTheme.muted
                        }
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: "“" + ((modelData.sourceExcerpt || "").length > 0 ? modelData.sourceExcerpt : modelData.text) + "”"
                            wrapMode: Text.WordWrap
                            font.family: root.sansFont
                            font.pixelSize: 12
                            font.italic: true
                            color: rocTheme.foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: modelData.sourceRef.length > 0
                            text: modelData.sourceRef
                            wrapMode: Text.WrapAnywhere
                            font.family: root.monoFont
                            font.pixelSize: 9
                            color: rocTheme.muted
                        }
                    }
                }
            }

            // Proposed assumptions: never rendered as confirmed.
            Repeater {
                model: root.proposedItems
                delegate: Rectangle {
                    required property var modelData
                    objectName: "interviewProposed_" + modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: proposedColumn.implicitHeight + 16
                    radius: 7
                    color: "transparent"
                    border.color: rocTheme.warning
                    border.width: 1
                    ColumnLayout {
                        id: proposedColumn
                        x: 9; y: 7
                        width: Math.max(0, parent.width - 18)
                        spacing: 3
                        Text {
                            objectName: "interviewItemStatus_" + modelData.id
                            text: qsTr("PROPOSED · NOT CONFIRMED")
                            font.family: root.monoFont
                            font.pixelSize: 8
                            font.bold: true
                            color: rocTheme.warning
                        }
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: modelData.text
                            wrapMode: Text.WordWrap
                            font.family: root.sansFont
                            font.pixelSize: 12
                            color: rocTheme.foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Roclint inferred this. It is not a decision and is not used as one.")
                            wrapMode: Text.WordWrap
                            font.family: root.sansFont
                            font.pixelSize: 9
                            color: rocTheme.muted
                        }
                    }
                }
            }

            // Open choices, including skipped questions.
            Repeater {
                model: root.openItems
                delegate: Rectangle {
                    required property var modelData
                    objectName: "interviewOpen_" + modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: openColumn.implicitHeight + 16
                    radius: 7
                    color: "transparent"
                    border.color: rocTheme.warning
                    border.width: 1
                    ColumnLayout {
                        id: openColumn
                        x: 9; y: 7
                        width: Math.max(0, parent.width - 18)
                        spacing: 3
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                text: qsTr("OPEN")
                                font.family: root.monoFont
                                font.pixelSize: 8
                                font.bold: true
                                color: rocTheme.warning
                            }
                            Text {
                                objectName: "interviewItemStatus_" + modelData.id
                                text: qsTr("NO ANSWER RECORDED")
                                font.family: root.monoFont
                                font.pixelSize: 8
                                color: rocTheme.muted
                            }
                            Item { Layout.fillWidth: true }
                            TinyButton {
                                objectName: "interviewAnswerTopic_" + modelData.id
                                label: qsTr("Answer")
                                quiet: true
                                active: !contract.isInterviewBusy
                                onClicked: root.focusQuestion()
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: modelData.text
                            wrapMode: Text.WordWrap
                            font.family: root.sansFont
                            font.pixelSize: 12
                            color: rocTheme.foreground
                        }
                    }
                }
            }

            // Evidence is inspectable but not treated as a confirmed decision.
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.evidenceItems.length > 0
                spacing: 3
                TinyButton {
                    objectName: "interviewEvidenceToggle"
                    label: root.showEvidence
                           ? qsTr("Hide evidence (%1)").arg(root.evidenceItems.length)
                           : qsTr("Show evidence (%1)").arg(root.evidenceItems.length)
                    quiet: true
                    onClicked: root.showEvidence = !root.showEvidence
                }
                Repeater {
                    model: root.showEvidence ? root.evidenceItems : []
                    delegate: Text {
                        required property var modelData
                        objectName: "interviewEvidence_" + modelData.id
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: "[" + modelData.sourceRef + "] " +
                              ((modelData.sourceExcerpt || "").length > 0 ? modelData.sourceExcerpt : modelData.text)
                        wrapMode: Text.WrapAnywhere
                        font.family: root.monoFont
                        font.pixelSize: 9
                        color: rocTheme.muted
                    }
                }
            }

            // Superseded decisions remain inspectable provenance.
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.supersededItems.length > 0
                spacing: 3
                TinyButton {
                    objectName: "interviewSupersededToggle"
                    label: root.showSuperseded
                           ? qsTr("Hide replaced answers (%1)").arg(root.supersededItems.length)
                           : qsTr("Replaced answers (%1)").arg(root.supersededItems.length)
                    quiet: true
                    onClicked: root.showSuperseded = !root.showSuperseded
                }
                Repeater {
                    model: root.showSuperseded ? root.supersededItems : []
                    delegate: Text {
                        required property var modelData
                        objectName: "interviewSuperseded_" + modelData.id
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        text: qsTr("Not current · %1").arg(modelData.text)
                        wrapMode: Text.WordWrap
                        font.family: root.sansFont
                        font.pixelSize: 11
                        font.strikeout: true
                        color: rocTheme.muted
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.confirmedItems.length === 0 && root.openItems.length === 0 &&
                         root.proposedItems.length === 0 && root.sourceItems.length === 0
                text: qsTr("No decisions recorded yet. Your draft is still here; answer only what is useful.")
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 11
                color: rocTheme.muted
            }
        }

        // ------------------------------------------------------------ footer
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: rocTheme.border
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            visible: !root.showingWordView
            TinyButton {
                objectName: "interviewKeepOriginalControl"
                label: qsTr("Keep original")
                quiet: true
                Accessible.name: qsTr("Keep your original draft")
                onClicked: root.keepOriginalRequested()
            }
            TinyButton {
                objectName: "interviewEditDraftControl"
                label: qsTr("Edit draft")
                quiet: true
                onClicked: root.editDraftRequested()
            }
            TinyButton {
                objectName: "interviewWordViewFooterControl"
                visible: contract.hasWordRefinement
                label: qsTr("Word view")
                quiet: true
                onClicked: root.openWordView()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !root.showingWordView
            text: qsTr("Enter saves an answer in this interview. Roclint never presses Send or Enter in your editor.")
            wrapMode: Text.WordWrap
            font.family: root.monoFont
            font.pixelSize: 8
            color: rocTheme.muted
        }
    }
}
