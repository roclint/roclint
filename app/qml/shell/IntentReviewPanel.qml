import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Added to the original HUD; authoritative state and transitions stay in HostController.
FocusScope {
    id: rootWindow
    property real viewportHeight: 520
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
    function focusReview() { diffView.forceActiveFocus(); }
    Connections {
        target: rootWindow.hc ? hostController : null
        function onInterventionChanged() {
            if (rootWindow.manualResolution)
                Qt.callLater(function() { if (customAnswer.visible) customAnswer.forceActiveFocus(); });
            if (hostController.isClarifying && !hostController.isDraftDirty)
                Qt.callLater(function() { if (clarifyPanel.visible) clarifyPanel.forceActiveFocus(); });
        }
        function onRewriteProposalChanged() {
            if (hostController.proposalStatus === "READY_FOR_REVIEW")
                Qt.callLater(function() { if (reviewPanel.visible) diffView.forceActiveFocus(); });
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

    function afterDecision(succeeded) {
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

    function kindTitle(kind) {
        if (kind === "CHOICE_GROUP") return qsTr("This could mean more than one thing");
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
                border.color: rootWindow.warnColor
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
                            color: rootWindow.warnColor
                        }
                        Text {
                            text: rootWindow.manualResolution ? qsTr("Manual decision") : rootWindow.hc ? qsTr("%1 guided answer(s) left").arg(hostController.remainingBudget) : ""
                            font.family: rootWindow.monoFont
                            font.pixelSize: 10
                            color: rootWindow.mutedTextColor
                        }
                        Pill {
                            visible: rootWindow.hc && (hostController.activeItemKind === "CHOICE_GROUP" || hostController.activeItemKind === "ASSUMPTION")
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
                                if (text.trim().length > 0 && rootWindow.afterDecision(hostController.submitCustomIntent(text, hostController.stateVersion))) {
                                    text = "";
                                }
                            }
                        }
                        ActionButton {
                            label: qsTr("Use this answer")
                            active: customAnswer.text.trim().length > 0
                            onClicked: {
                                if (rootWindow.afterDecision(hostController.submitCustomIntent(customAnswer.text, hostController.stateVersion))) {
                                    customAnswer.text = "";
                                }
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
                         ? qsTr("Roclint won't suggest options for this prompt (%1). Edit it yourself, or copy it as-is with Shift+Enter.")
                               .arg(hostController.diagnosticMessage.length > 0 ? hostController.diagnosticMessage : hostController.interventionReason)
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
                        text: modelData.statement + "\n" + modelData.state + " · " + modelData.scoring
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
                visible: rootWindow.hc && hostController.hasConfirmedIntent

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
                    label: qsTr("Suggest a rewrite")
                    primary: true
                    active: rootWindow.hc && hostController.isRewriteEnabled
                    onClicked: hostController.synthesizeRewrite(hostController.stateVersion)
                }
                }
            }

            // Proposal review
            Rectangle {
                id: reviewPanel
                objectName: "reviewPanel"
                readonly property string status: rootWindow.hc ? hostController.proposalStatus : "NONE"
                readonly property string preview: rootWindow.hc ? hostController.streamingPreview : ""
                readonly property bool hasPreview: status === "GENERATING" && preview.length > 0
                visible: status !== "NONE"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredHeight: status === "READY_FOR_REVIEW" || hasPreview
                                        ? Math.min(300, rootWindow.viewportHeight * 0.55)
                                        : reviewMessage.implicitHeight + 20
                color: "transparent"
                border.color: status === "READY_FOR_REVIEW" ? rootWindow.fgColor
                            : status === "GENERATING" ? rootWindow.mutedTextColor : rootWindow.warnColor
                border.width: 1
                radius: 6

                Text {
                    id: reviewMessage
                    visible: reviewPanel.status !== "READY_FOR_REVIEW" && !reviewPanel.hasPreview
                    anchors.fill: parent
                    anchors.margins: 10
                    text: rootWindow.hc ? (reviewPanel.status === "NEEDS_CLARIFICATION"
                                           ? qsTr("The model needs more detail before it can rewrite this: %1 Add it to your prompt and analyze again.").arg(hostController.proposalMessage)
                                           : hostController.proposalMessage) : ""
                    wrapMode: Text.WordWrap
                    font.family: rootWindow.sansFont
                    font.pixelSize: 12
                    color: reviewPanel.status === "GENERATING" ? rootWindow.mutedTextColor : rootWindow.warnColor
                }

                // Live streaming preview — visible once chunks start arriving during generation.
                ScrollView {
                    visible: reviewPanel.hasPreview
                    anchors.fill: parent
                    anchors.margins: 10
                    clip: true
                    ScrollBar.vertical: HudScrollBar {}
                    Text {
                        width: parent.width
                        text: reviewPanel.preview
                        wrapMode: Text.WrapAnywhere
                        font.family: rootWindow.monoFont
                        font.pixelSize: 12
                        color: rootWindow.mutedTextColor
                    }
                }

                ColumnLayout {
                    visible: reviewPanel.status === "READY_FOR_REVIEW"
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: rootWindow.hc ? hostController.proposalMessage : ""
                            wrapMode: Text.WordWrap
                            font.family: rootWindow.sansFont
                            font.pixelSize: 12
                            font.bold: true
                            color: rootWindow.fgColor
                        }
                        Pill {
                            visible: rootWindow.hc && hostController.approvalLabel.length > 0
                            label: rootWindow.hc ? hostController.approvalLabel : ""
                            tone: rootWindow.addedColor
                        }
                    }

                    ListView {
                        id: diffView
                        objectName: "diffView"
                        activeFocusOnTab: true
                        Accessible.name: qsTr("Changes in the suggested rewrite")
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.fillHeight: true
                        clip: true
                        model: rootWindow.hc && hostController.lastDiffText.length > 0 ? hostController.lastDiffText.split("\n") : []
                        ScrollBar.vertical: HudScrollBar {}
                        delegate: Text {
                            required property string modelData
                            width: diffView.width
                            text: modelData
                            wrapMode: Text.WrapAnywhere
                            font.family: rootWindow.monoFont
                            font.pixelSize: 12
                            color: modelData.startsWith("+ ") ? rootWindow.addedColor
                                 : modelData.startsWith("- ") ? rootWindow.removedColor
                                 : modelData.startsWith("~ ") ? rootWindow.changedColor : rootWindow.mutedTextColor
                        }
                    }

                    Repeater {
                        model: rootWindow.hc ? hostController.proposalConstraints : []
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            CheckBox {
                                id: constraintCheck
                                objectName: "constraintReview_" + modelData.id
                                Accessible.name: modelData.description
                                indicator: Rectangle {
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    y: (constraintCheck.height - height) / 2
                                    color: rootWindow.inputBgColor
                                    border.color: rootWindow.fgColor
                                    border.width: constraintCheck.activeFocus ? 3 : 1
                                    radius: 2
                                    Text { anchors.centerIn: parent; text: constraintCheck.checked ? "✓" : ""; color: rootWindow.fgColor }
                                }
                                visible: modelData.needsReview
                                checked: modelData.acknowledged
                                onClicked: hostController.acknowledgeConstraint(modelData.id, checked, hostController.stateVersion)
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: qsTr("[%1] %2").arg(modelData.status).arg(modelData.description)
                                wrapMode: Text.WordWrap
                                font.family: rootWindow.sansFont
                                font.pixelSize: 11
                                color: modelData.status === "FAILED" ? rootWindow.errorColor : rootWindow.fgColor
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: rootWindow.hc && !rootWindow.ambient && hostController.isAmbientActive
                        text: qsTr("To insert this rewrite, connect the destination prompt box. Your wording and answers will be kept.")
                        wrapMode: Text.WordWrap
                        font.family: rootWindow.sansFont
                        font.pixelSize: 11
                        color: rootWindow.mutedTextColor
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            visible: rootWindow.hc && hostController.pendingReviewCount > 0
                            text: rootWindow.hc ? qsTr("%1 item(s) left to check").arg(hostController.pendingReviewCount) : ""
                            font.family: rootWindow.sansFont
                            font.pixelSize: 11
                            color: rootWindow.warnColor
                        }
                        Item { Layout.fillWidth: true }
                        ActionButton { label: qsTr("Discard"); onClicked: hostController.discardProposal(hostController.stateVersion) }
                        ActionButton {
                            objectName: "connectRewriteEditorControl"
                            visible: rootWindow.hc && hostController.isAmbientActive
                            label: rootWindow.ambient ? qsTr("Change editor…") : qsTr("Connect editor…")
                            primary: !rootWindow.ambient
                            onClicked: hostController.beginInsertionTargetSelection(hostController.stateVersion)
                        }
                        ActionButton {
                            objectName: "approveRewriteControl"
                            label: rootWindow.ambient ? qsTr("Approve & Insert") : qsTr("Approve & Copy")
                            primary: rootWindow.ambient || !rootWindow.hc || !hostController.isAmbientActive
                            active: rootWindow.hc && hostController.canApprove
                            onClicked: hostController.approveAndCopyRewrite(hostController.stateVersion)
                        }
                    }
                }
            }

    }
}
