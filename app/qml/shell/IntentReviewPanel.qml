import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Added to the original HUD; authoritative state and transitions stay in HostController.
FocusScope {
    id: rootWindow
    objectName: "intentReviewPanel"
    property real viewportHeight: 520
    property bool showDiagnostics: false
    signal reviewRequested()
    signal revealReviewItem(var item)
    signal editDraftRequested()
    property string diagnosticMetrics: ""
    readonly property bool previewActive: hc && (hostController.proposalStatus === "READY_FOR_REVIEW" || hostController.proposalStatus === "GENERATING")
    readonly property color questionTone: hc && hostController.isOptionalHelp ? borderColor : warnColor
    implicitHeight: workflowColumn.implicitHeight
    readonly property bool ambient: hc && hostController.isAmbientCapture
    readonly property bool manualResolution: hc && hostController.isManual && hostController.interventionReason === "CLARIFICATION_BUDGET_EXCEEDED"
    readonly property bool customAnswerHasFocus: customAnswer.activeFocus
    readonly property bool choiceHasFocus: {
        let item = rootWindow.Window.window ? rootWindow.Window.window.activeFocusItem : null;
        while (item) { if (item === clarifyPanel) return true; item = item.parent; }
        return false;
    }
    function focusClarification() {
        if (manualResolution) customAnswer.forceActiveFocus();
        else clarifyPanel.forceActiveFocus();
    }
    function focusReview() {
        for (let i = 0; i < constraintRepeater.count; ++i) {
            const row = constraintRepeater.itemAt(i);
            if (row && row.pending) {
                row.reviewCheck.forceActiveFocus(Qt.TabFocusReason);
                revealReviewItem(row);
                return;
            }
        }
        diffView.forceActiveFocus(Qt.TabFocusReason);
        revealReviewItem(diffView);
    }
    property bool reviewPresented: false
    Component.onCompleted: reviewPresented = hc && hostController.proposalStatus === "READY_FOR_REVIEW"
    property int reviewRefresh: 0
    property var pendingDecisions: ({})
    Connections {
        target: rootWindow.hc ? hostController : null
        function onStorageCommandFinished(requestId, name, succeeded, errorCode, message) {
            if (name === "acknowledgeConstraint" && !succeeded) rootWindow.reviewRefresh++;
            if (!rootWindow.pendingDecisions[requestId]) return;
            delete rootWindow.pendingDecisions[requestId];
            if (succeeded) customAnswer.text = "";
            rootWindow.maybeSuggestRewrite(succeeded);
        }
        function onInterventionChanged() {
            if (rootWindow.manualResolution)
                Qt.callLater(function() { if (customAnswer.visible) customAnswer.forceActiveFocus(); });
            if (hostController.isClarifying && !hostController.isDraftDirty)
                Qt.callLater(function() { if (clarifyPanel.visible) clarifyPanel.forceActiveFocus(); });
        }
        function onRewriteProposalChanged() {
            // Acknowledgements must not scroll the user back to the heading.
            if (hostController.proposalStatus !== "READY_FOR_REVIEW") { rootWindow.reviewPresented = false; return; }
            if (rootWindow.reviewPresented) return;
            rootWindow.reviewPresented = true;
            // Arrival never opens, resizes or activates the HUD. Review is an explicit gesture.
            if (rootWindow.Window.window && rootWindow.Window.window.active)
                Qt.callLater(function() { if (reviewPanel.visible) rootWindow.revealReviewItem(reviewHeading); });
        }
    }
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Segoe UI"
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null

    // Theme palette
    property string currentTheme: "dark"
    readonly property var themes: ["dark", "light", "acid", "amber"]
    readonly property color bgColor: currentTheme === "light" ? "#F4F4F4" : currentTheme === "acid" ? "#070D08" : currentTheme === "amber" ? "#0F0902" : "#050505"
    readonly property color fgColor: currentTheme === "light" ? "#050505" : currentTheme === "acid" ? "#4ADE80" : currentTheme === "amber" ? "#FBBF24" : "#C9D1D9"
    readonly property color borderColor: currentTheme === "light" ? "#050505" : currentTheme === "acid" ? "#22C55E" : currentTheme === "amber" ? "#F59E0B" : "#6E7681"
    readonly property color dimColor: currentTheme === "light" ? "#D1D5DB" : currentTheme === "acid" ? "#14331B" : currentTheme === "amber" ? "#3B2207" : "#262626"
    readonly property color mutedTextColor: currentTheme === "light" ? "#4B5563" : currentTheme === "acid" ? "#4ADE80" : currentTheme === "amber" ? "#D97706" : "#8B949E"
    readonly property color inputBgColor: currentTheme === "light" ? "#E5E7EB" : currentTheme === "acid" ? "#0A140B" : currentTheme === "amber" ? "#140D04" : "#09090B"
    readonly property color addedColor: currentTheme === "light" ? "#166534" : "#7EE787"
    readonly property color removedColor: currentTheme === "light" ? "#991B1B" : "#FF7B72"
    readonly property color changedColor: currentTheme === "light" ? "#92400E" : "#F59E0B"
    readonly property color warnColor: "#F59E0B"
    readonly property color errorColor: "#FF7B72"

    function chooseAlternative(index) {
        if (!hc || !hostController.isClarifying || hostController.activeItemKind !== "CHOICE_GROUP") {
            return;
        }
        const alts = hostController.activeAlternatives;
        if (index >= 0 && index < alts.length) {
            confirmAlternative(alts[index].id);
        }
    }

    function afterDecision(requestId) {
        if (requestId) pendingDecisions[requestId] = true;
        return requestId;
    }
    function maybeSuggestRewrite(succeeded) {
        if (succeeded && hc && hostController.hasConfirmedIntent && hostController.isRewriteEnabled && hostController.proposalStatus === "NONE") {
            const version = hostController.stateVersion;
            Qt.callLater(function() {
                if (hostController.stateVersion === version && hostController.hasConfirmedIntent && hostController.proposalStatus === "NONE")
                    hostController.synthesizeRewrite(version);
            });
        }
        return succeeded;
    }
    function confirmAlternative(id) {
        return afterDecision(hostController.confirmChoice(id, hostController.stateVersion));
    }

    function isDeferral(text) {
        const t = text.trim().toLowerCase();
        const exact = [
            "", "idk", "you pick", "u pick", "you decide",
            "doesn't matter", "doesnt matter", "either", "both",
            "?", "??", "...", "no preference", "any"
        ];
        return exact.indexOf(t) !== -1 || t.startsWith("idk ");
    }

    function handleAnswerSubmit(text) {
        if (!rootWindow.hc) return;
        rootWindow.afterDecision(hostController.submitClarification(text, isDeferral(text), hostController.stateVersion));
    }

    function kindTitle(kind) {
        if (hc && hostController.isOptionalHelp) return qsTr("Optional ideas");
        if (kind === "CHOICE_GROUP") return qsTr("One detail to clarify");
        if (kind === "ASSUMPTION") return qsTr("Is this assumption right?");
        if (kind === "MISSING_DECISION") return qsTr("A detail is missing");
        if (kind === "CONFLICT") return qsTr("These requirements conflict");
        return "";
    }

    component Pill: Rectangle {
        id: pill
        property string label: ""
        property color tone: rootWindow.fgColor
        property bool clickable: false
        signal clicked()
        activeFocusOnTab: clickable
        Accessible.role: clickable ? Accessible.Button : Accessible.StaticText
        Accessible.name: label
        Accessible.onPressAction: if (clickable) clicked()
        Keys.onPressed: function(event) {
            if (clickable && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
                clicked();
                event.accepted = true;
            }
        }
        height: 22
        implicitWidth: pillText.implicitWidth + 16
        color: pillMouse.containsMouse && clickable ? rootWindow.dimColor : "transparent"
        border.color: tone
        border.width: activeFocus ? 3 : 1
        radius: 3
        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label
            font.family: rootWindow.monoFont
            font.pixelSize: 10
            font.bold: true
            color: pill.tone
        }
        MouseArea {
            id: pillMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: pill.clickable
            cursorShape: pill.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: { pill.forceActiveFocus(); pill.clicked(); }
        }
    }

    component ActionButton: Rectangle {
        id: btn
        property string label: ""
        property bool primary: false
        property bool active: true
        signal clicked()
        enabled: active
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.onPressAction: if (active) clicked()
        Keys.onPressed: function(event) {
            if (active && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
                clicked();
                event.accepted = true;
            }
        }
        implicitHeight: 28
        implicitWidth: btnText.implicitWidth + 22
        radius: 4
        opacity: active ? 1.0 : 0.4
        color: primary ? (btnMouse.containsMouse && active ? Qt.darker(rootWindow.fgColor, 1.15) : rootWindow.fgColor)
                       : (btnMouse.containsMouse && active ? rootWindow.dimColor : "transparent")
        border.color: rootWindow.fgColor
        border.width: activeFocus ? 3 : 1
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label
            font.family: rootWindow.sansFont
            font.pixelSize: 12
            font.bold: btn.primary
            color: btn.primary ? rootWindow.bgColor : rootWindow.fgColor
        }
        MouseArea {
            id: btnMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.active
            cursorShape: btn.active ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: { btn.forceActiveFocus(); btn.clicked(); }
        }
    }

    component Banner: Rectangle {
        id: banner
        property string message: ""
        property color tone: rootWindow.warnColor
        property bool dismissable: false
        signal dismissed()
        visible: message.length > 0
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: bannerRow.implicitHeight + 12
        color: "transparent"
        border.color: tone
        border.width: 1
        radius: 4
        RowLayout {
            id: bannerRow
            anchors.fill: parent
            anchors.margins: 6
            spacing: 8
            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: banner.message
                wrapMode: Text.WordWrap
                font.family: rootWindow.sansFont
                font.pixelSize: 12
                color: banner.tone
            }
            Pill {
                visible: banner.dismissable
                label: qsTr("Dismiss")
                tone: banner.tone
                clickable: true
                onClicked: banner.dismissed()
            }
        }
    }


    ColumnLayout {
        id: workflowColumn
        width: parent.width
        spacing: 8
        Flow {
            Layout.fillWidth: true
            spacing: 8
            ActionButton {
                objectName: "reviewPromptButton"
                label: qsTr("Clarify")
                active: rootWindow.hc && hostController.isStorageAvailable && !hostController.storageBusy && !hostController.isAnalyzing && hostController.proposalStatus !== "GENERATING"
                onClicked: rootWindow.reviewRequested()
            }
            ActionButton {
                objectName: "rewriteWithoutQuestionsButton"
                label: qsTr("Rewrite without questions")
                visible: rootWindow.hc && !rootWindow.previewActive && (hostController.hasConfirmedIntent || hostController.isClarifying)
                active: rootWindow.hc && hostController.isStorageAvailable && !hostController.storageBusy && !hostController.isAnalyzing && !hostController.isDraftDirty
                onClicked: hostController.rewriteWithoutQuestions(hostController.stateVersion)
            }
            ActionButton {
                objectName: "editDraftInHudButton"
                visible: rootWindow.hc && (hostController.isAmbientAttached || rootWindow.previewActive)
                label: qsTr("Edit prompt in HUD")
                onClicked: {
                    rootWindow.editDraftRequested();
                    if (rootWindow.hc) hostController.pauseAmbientFollowing();
                }
            }
            Text {
                objectName: "reviewDiagnosticsButton"
                text: rootWindow.showDiagnostics ? qsTr("Hide details") : qsTr("Technical details")
                color: rootWindow.mutedTextColor
                font.pixelSize: 11
                font.underline: true
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: text
                function activate() { rootWindow.showDiagnostics = !rootWindow.showDiagnostics; }
                Accessible.onPressAction: activate()
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: parent.activate() }
            }
        }
        Text {
            visible: rootWindow.showDiagnostics && rootWindow.hc
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: rootWindow.hc ? hostController.reviewDiagnostics + "\n" + hostController.interventionReason + "\n" + hostController.rankingStatus + "\n" + rootWindow.diagnosticMetrics : ""
            color: rootWindow.mutedTextColor
            font.pixelSize: 11
        }
        ColumnLayout {
            objectName: "groundedEvidenceList"
            visible: rootWindow.showDiagnostics && rootWindow.hc && hostController.groundedEvidence.length > 0
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 2
            Text {
                objectName: "groundingStatusLabel"
                Layout.fillWidth: true
                text: qsTr("Code evidence: %1").arg(rootWindow.hc ? hostController.groundingStatus : "")
                color: rootWindow.mutedTextColor
                font.pixelSize: 11
            }
            Repeater {
                model: rootWindow.hc ? hostController.groundedEvidence : []
                delegate: Text {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    color: modelData.status === "VERIFIED" ? rootWindow.fgColor : rootWindow.mutedTextColor
                    font.family: rootWindow.monoFont
                    font.pixelSize: 11
                    text: "[" + modelData.status + "] " + modelData.qualifiedTarget
                          + (modelData.signature.length ? "  " + modelData.signature : "")
                          + (modelData.filePath.length ? "\n" + modelData.filePath + ":" + modelData.line : "")
                          + (modelData.matchReason.length ? "  · " + modelData.matchReason : "")
                          + (modelData.complete ? "" : "  · incomplete")
                          + (modelData.hashValidated ? "" : "  · stale")
                          + (modelData.candidates && modelData.candidates.length
                             ? "\nCandidates: " + modelData.candidates.join(" · ") : "")
                }
            }
        }
            // Clarification panel
            Rectangle {
                id: clarifyPanel
                objectName: "clarifyPanel"
                activeFocusOnTab: visible
                Accessible.role: Accessible.Grouping
                Accessible.name: rootWindow.hc ? hostController.activeQuestion : ""
                visible: rootWindow.hc && (hostController.isClarifying || rootWindow.manualResolution)
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: clarifyColumn.implicitHeight + 20
                color: "transparent"
                border.color: rootWindow.questionTone
                border.width: activeFocus ? 3 : 1
                radius: 6

                ColumnLayout {
                    id: clarifyColumn
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: rootWindow.hc ? rootWindow.kindTitle(hostController.activeItemKind) : ""
                            font.family: rootWindow.sansFont
                            font.pixelSize: 13
                            font.bold: true
                            color: rootWindow.hc && hostController.isOptionalHelp ? rootWindow.fgColor : rootWindow.warnColor
                        }
                        Text {
                            text: rootWindow.hc && hostController.isOptionalHelp ? qsTr("Optional") : rootWindow.manualResolution ? qsTr("Manual decision") : rootWindow.hc ? qsTr("%1 guided answer(s) left").arg(hostController.remainingBudget) : ""
                            font.family: rootWindow.monoFont
                            font.pixelSize: 10
                            color: rootWindow.mutedTextColor
                        }
                        Pill {
                            visible: rootWindow.showDiagnostics && rootWindow.hc && (hostController.activeItemKind === "CHOICE_GROUP" || hostController.activeItemKind === "ASSUMPTION")
                            label: rootWindow.hc && hostController.isAmbiguityScored
                                   ? (hostController.activeItemKind === "ASSUMPTION" ? qsTr("UNCERTAINTY %1") : qsTr("RANKED %1")).arg(hostController.ambiguityScore.toFixed(2))
                                   : qsTr("UNRANKED")
                            tone: rootWindow.mutedTextColor
                            ToolTip.visible: unrankedHover.containsMouse
                            ToolTip.text: rootWindow.hc && hostController.isAmbiguityScored
                                          ? (hostController.scorerEngine === "JULIA_LOCAL" ? qsTr("Scored by Julia locally. This is a model estimate, not a confirmed interpretation.") : qsTr("Ranked by the Jev cloud scorer."))
                                          : qsTr("This item is unscored. Confirm or edit it yourself; no interpretation is selected automatically.")
                            MouseArea { id: unrankedHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: rootWindow.hc ? hostController.activeQuestion : ""
                        wrapMode: Text.WordWrap
                        font.family: rootWindow.sansFont
                        font.pixelSize: 13
                        color: rootWindow.fgColor
                    }

                    Repeater {
                        model: rootWindow.hc && !rootWindow.manualResolution && hostController.activeItemKind === "CHOICE_GROUP" ? hostController.activeAlternatives : []
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            objectName: "alternative" + index
                            enabled: modelData.permitted !== false
                            opacity: enabled ? 1.0 : 0.55
                            activeFocusOnTab: enabled
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.label
                            Accessible.onPressAction: rootWindow.confirmAlternative(modelData.id)
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                    rootWindow.confirmAlternative(modelData.id);
                                    event.accepted = true;
                                }
                            }
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: altColumn.implicitHeight + 12
                            radius: 4
                            color: altMouse.containsMouse ? rootWindow.dimColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: activeFocus ? 3 : 1
                            ColumnLayout {
                                id: altColumn
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: qsTr("[%1]  %2").arg(index + 1).arg(modelData.label)
                                    wrapMode: Text.WordWrap
                                    font.family: rootWindow.sansFont
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: rootWindow.fgColor
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    visible: modelData.whyMaterial.length > 0
                                    text: modelData.permitted === false ? modelData.constraintViolation : modelData.whyMaterial
                                    wrapMode: Text.WordWrap
                                    font.family: rootWindow.sansFont
                                    font.pixelSize: 11
                                    color: rootWindow.mutedTextColor
                                }
                            }
                            MouseArea {
                                id: altMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    parent.forceActiveFocus();
                                    if (rootWindow.hc) hostController.pauseAmbientFollowing();
                                    rootWindow.confirmAlternative(modelData.id);
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: rootWindow.hc && !rootWindow.manualResolution && hostController.activeItemKind === "ASSUMPTION"
                        spacing: 8
                        ActionButton { label: qsTr("Yes, that's right"); primary: true; onClicked: rootWindow.afterDecision(hostController.resolveActiveAssumption(true, hostController.stateVersion)) }
                        ActionButton { label: qsTr("No, that's wrong"); onClicked: rootWindow.afterDecision(hostController.resolveActiveAssumption(false, hostController.stateVersion)) }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8
                        visible: rootWindow.hc && (rootWindow.manualResolution || hostController.activeItemKind !== "ASSUMPTION")
                        TextField {
                            id: customAnswer
                            objectName: "customAnswer"
                            Accessible.name: qsTr("Your clarification")
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            placeholderText: rootWindow.hc && hostController.activeItemKind === "CHOICE_GROUP"
                                             ? qsTr("Or say what you meant in your own words")
                                             : qsTr("Type your answer")
                            font.family: rootWindow.sansFont
                            font.pixelSize: 12
                            color: rootWindow.fgColor
                            placeholderTextColor: rootWindow.mutedTextColor
                            background: Rectangle { color: rootWindow.inputBgColor; border.color: rootWindow.borderColor; radius: 4 }
                            onActiveFocusChanged: {
                                if (activeFocus) {
                                    if (rootWindow.Window.window) rootWindow.Window.window.requestActivate();
                                    if (rootWindow.hc) hostController.pauseAmbientFollowing();
                                }
                            }
                            onTextEdited: {
                                if (rootWindow.hc) hostController.pauseAmbientFollowing();
                            }
                            TapHandler {
                                onTapped: {
                                    if (rootWindow.Window.window) rootWindow.Window.window.requestActivate();
                                    if (rootWindow.hc) hostController.pauseAmbientFollowing();
                                }
                            }
                            onAccepted: {
                                handleAnswerSubmit(text);
                            }
                        }
                        ActionButton {
                            label: qsTr("Use this answer")
                            active: customAnswer.text.trim().length > 0
                            onClicked: {
                                handleAnswerSubmit(customAnswer.text);
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8
                        visible: !rootWindow.manualResolution
                        ActionButton {
                            visible: rootWindow.hc && hostController.activeItemKind === "CHOICE_GROUP"
                            label: qsTr("Keep my original wording")
                            onClicked: hostController.keepOriginalPrompt(hostController.stateVersion)
                        }
                        ActionButton { label: qsTr("Skip"); onClicked: hostController.skipCurrentItem(hostController.stateVersion) }
                        Item { Layout.fillWidth: true }
                        ActionButton { label: qsTr("I'll edit it myself"); onClicked: hostController.switchToManual(hostController.stateVersion) }
                    }
                }
            }

            // Manual mode notice
            Banner {
                message: rootWindow.manualResolution
                         ? qsTr("The three guided questions are complete. A remaining decision is shown above; answer it in your own words to finish the rewrite.")
                         : rootWindow.hc && hostController.isManual
                         ? qsTr("Roclint couldn't offer a reliable suggestion. Edit your prompt, review again, or keep your original wording.")
                         : ""
                tone: rootWindow.mutedTextColor
            }

            // Confirmed intent and rewrite request
            ColumnLayout {
                Layout.fillWidth: true
                objectName: "materialAssumptionsList"
                visible: rootWindow.hc && hostController.isManual && hostController.assumptionReviews.length > 0
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Material assumptions — confirm them in your draft")
                    color: rootWindow.warnColor
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                }
                Repeater {
                    model: rootWindow.hc ? hostController.assumptionReviews : []
                    Text {
                        required property var modelData
                        objectName: "assumptionReview_" + modelData.id
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: modelData.statement + "\n" + modelData.state + (rootWindow.showDiagnostics ? " · " + modelData.scoring : "")
                        color: rootWindow.mutedTextColor
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        Accessible.name: text
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8
                visible: rootWindow.hc && hostController.hasConfirmedIntent && !rootWindow.previewActive

                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: rootWindow.hc ? hostController.confirmedIntentSummary : ""
                    wrapMode: Text.WordWrap
                    font.family: rootWindow.sansFont
                    font.pixelSize: 12
                    color: rootWindow.hc && hostController.isIntentStale ? rootWindow.warnColor : rootWindow.addedColor
                }
                RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8
                ComboBox {
                    id: profileBox
                    Accessible.name: qsTr("Rewrite profile")
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 200
                    model: rootWindow.hc ? hostController.availableRewriteProfiles : []
                    textRole: "displayName"
                    valueRole: "id"
                    font.pixelSize: 11
                    indicator: Text {
                        x: profileBox.width - width - 8
                        y: (profileBox.height - height) / 2
                        text: "▾"
                        color: rootWindow.fgColor
                    }
                    contentItem: Text {
                        text: profileBox.displayText
                        color: rootWindow.fgColor
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        leftPadding: 8
                        rightPadding: 24
                        font.pixelSize: 11
                    }
                    background: Rectangle {
                        color: rootWindow.inputBgColor
                        border.color: profileBox.activeFocus ? rootWindow.fgColor : rootWindow.borderColor
                        border.width: profileBox.activeFocus ? 2 : 1
                        radius: 4
                    }
                    Component.onCompleted: if (rootWindow.hc) currentIndex = indexOfValue(hostController.rewriteProfile)
                    onActivated: hostController.setRewriteProfile(currentValue, hostController.stateVersion)
                    Connections {
                        target: rootWindow.hc ? hostController : null
                        function onRewriteProfileChanged(profile) { profileBox.currentIndex = profileBox.indexOfValue(profile) }
                    }
                }
                ActionButton {
                    label: qsTr("Improve wording")
                    primary: true
                    active: rootWindow.hc && !hostController.storageBusy && hostController.isStorageAvailable && hostController.isRewriteEnabled
                    onClicked: hostController.synthesizeRewrite(hostController.stateVersion)
                }
                }
            }

            // Natural-height review content: checks cannot be pushed outside a fixed rectangle.
            Rectangle {
                id: reviewPanel
                objectName: "reviewPanel"
                readonly property string status: rootWindow.hc ? hostController.proposalStatus : "NONE"
                readonly property string preview: rootWindow.hc ? hostController.streamingPreview : ""
                visible: status !== "NONE"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: reviewBody.implicitHeight + 20
                color: "transparent"
                border.color: status === "READY_FOR_REVIEW" ? rootWindow.borderColor
                            : status === "GENERATING" ? rootWindow.mutedTextColor : rootWindow.warnColor
                border.width: 1
                radius: 6

                ColumnLayout {
                    id: reviewBody
                    x: 10
                    y: 10
                    width: Math.max(0, parent.width - 20)
                    spacing: 8
                    Text {
                        id: reviewHeading
                        objectName: "reviewHeading"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        textFormat: Text.PlainText
                        text: reviewPanel.status === "READY_FOR_REVIEW"
                            ? (rootWindow.hc && hostController.proposalText === hostController.currentRevisionText
                               ? qsTr("No wording changes suggested. This is your original prompt.")
                               : qsTr("Review the changes and confirm your choice."))
                            : rootWindow.hc ? hostController.proposalMessage : ""
                        wrapMode: Text.Wrap
                        font.family: rootWindow.sansFont
                        font.pixelSize: 12
                        font.bold: reviewPanel.status === "READY_FOR_REVIEW"
                        color: rootWindow.fgColor
                    }
                    Text {
                        visible: reviewPanel.status === "GENERATING" && text.length > 0
                        Layout.fillWidth: true
                        text: reviewPanel.preview
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        font.family: rootWindow.monoFont
                        font.pixelSize: 12
                        color: rootWindow.mutedTextColor
                    }
                    Repeater {
                        id: constraintRepeater
                        model: {
                            rootWindow.reviewRefresh;
                            return rootWindow.hc && reviewPanel.status === "READY_FOR_REVIEW"
                                ? hostController.proposalConstraints.filter(function(item) { return rootWindow.showDiagnostics || item.needsReview || item.status === "FAILED"; }) : [];
                        }
                        delegate: RowLayout {
                            required property var modelData
                            property bool pending: modelData.needsReview && !modelData.acknowledged
                            property alias reviewCheck: constraintCheck
                            objectName: "reviewRow_" + modelData.id
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            CheckBox {
                                id: constraintCheck
                                Layout.alignment: Qt.AlignTop
                                objectName: "constraintReview_" + modelData.id
                                Accessible.name: constraintLabel.text
                                indicator: Rectangle {
                                    implicitWidth: 18; implicitHeight: 18
                                    y: (constraintCheck.height - height) / 2
                                    color: rootWindow.inputBgColor
                                    border.color: rootWindow.fgColor
                                    border.width: constraintCheck.activeFocus ? 3 : 1
                                    radius: 2
                                    Text { anchors.centerIn: parent; text: constraintCheck.checked ? "✓" : ""; color: rootWindow.fgColor }
                                }
                                visible: modelData.needsReview
                                checked: modelData.acknowledged
                                enabled: rootWindow.hc && hostController.isStorageAvailable && !hostController.storageBusy
                                onClicked: {
                                    if (!hostController.acknowledgeConstraint(modelData.id, checked, hostController.stateVersion)) rootWindow.reviewRefresh++;
                                }
                            }
                            Text {
                                id: constraintLabel
                                objectName: "reviewLabel_" + modelData.id
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                textFormat: Text.PlainText
                                text: (rootWindow.showDiagnostics ? "[" + modelData.status + "] " : "") + modelData.description
                                    + (modelData.needsReview && modelData.evidence.length ? "\nYour answer: “" + modelData.evidence + "”" : "")
                                wrapMode: Text.Wrap
                                font.family: rootWindow.sansFont
                                font.pixelSize: 12
                                color: modelData.status === "FAILED" ? rootWindow.errorColor : rootWindow.fgColor
                            }
                        }
                    }
                    RowLayout {
                        visible: reviewPanel.status === "READY_FOR_REVIEW"
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: rootWindow.hc ? hostController.deliveryLabel : ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: rootWindow.mutedTextColor
                        }
                        ActionButton {
                            objectName: "connectRewriteEditorControl"
                            visible: rootWindow.hc && hostController.isAmbientActive
                            label: rootWindow.ambient ? qsTr("Change editor…") : qsTr("Connect editor…")
                            onClicked: hostController.beginInsertionTargetSelection(hostController.stateVersion)
                        }
                    }
                    ListView {
                        id: diffView
                        objectName: "diffView"
                        visible: reviewPanel.status === "READY_FOR_REVIEW"
                        activeFocusOnTab: true
                        Accessible.name: qsTr("Changes in the suggested rewrite")
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: Math.min(contentHeight, Math.max(80, Math.min(240, rootWindow.viewportHeight * 0.4)))
                        implicitHeight: Layout.preferredHeight
                        clip: true
                        Keys.onPressed: function(event) {
                            const maximum = Math.max(0, contentHeight - height);
                            let next = contentY;
                            if (event.key === Qt.Key_End) next = maximum;
                            else if (event.key === Qt.Key_Home) next = 0;
                            else if (event.key === Qt.Key_PageDown) next += height * 0.8;
                            else if (event.key === Qt.Key_PageUp) next -= height * 0.8;
                            else if (event.key === Qt.Key_Down) next += 24;
                            else if (event.key === Qt.Key_Up) next -= 24;
                            else return;
                            contentY = Math.max(0, Math.min(maximum, next));
                            event.accepted = true;
                        }
                        model: rootWindow.hc && hostController.lastDiffText.length > 0 ? hostController.lastDiffText.split("\n") : []
                        ScrollBar.vertical: HudScrollBar {}
                        delegate: Text {
                            required property string modelData
                            width: diffView.width
                            text: modelData
                            textFormat: Text.PlainText
                            wrapMode: Text.WrapAnywhere
                            font.family: rootWindow.monoFont
                            font.pixelSize: 12
                            color: modelData.startsWith("+ ") ? rootWindow.addedColor
                                 : modelData.startsWith("- ") ? rootWindow.removedColor
                                 : modelData.startsWith("~ ") ? rootWindow.changedColor : rootWindow.mutedTextColor
                        }
                    }
                }
            }

    }
}
