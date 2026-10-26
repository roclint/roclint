import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("ROCLINT // RETROFUTURIST POSTER HUD")
    property bool isDetailedExpanded: false
    readonly property bool isAmbientMode: hc && (hostController.isAmbientAttached || hostController.isAmbientActive)

    width: !isAmbientMode ? 820 : (isDetailedExpanded ? 820 : 620)
    height: !isAmbientMode ? 520 : (isDetailedExpanded ? 520 : 250)
    minimumWidth: !isAmbientMode ? 520 : (isDetailedExpanded ? 520 : 420)
    minimumHeight: !isAmbientMode ? 360 : (isDetailedExpanded ? 360 : 200)
    visible: false
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    function toggleDetailedExpanded() {
        isDetailedExpanded = !isDetailedExpanded;
        if (isAmbientMode) {
            const targetW = isDetailedExpanded ? 820 : 620;
            const targetH = isDetailedExpanded ? 520 : 250;
            width = targetW;
            height = targetH;
            const pt = hostController.calculateAmbientAnchor(targetW, targetH);
            x = pt.x;
            y = pt.y;
        }
    }

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Arial"
    readonly property string serifFont: isMac ? "Times New Roman" : "Georgia"

    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    property var projectList: hc ? hostController.recentProjects() : []
    property int currentProjectIndex: 0
    readonly property var activeProject: projectList.length > currentProjectIndex ? projectList[currentProjectIndex] : ({name: qsTr("Choose workspace"), path: ""})
    property string activeScope: "global"
    property string targetLocalFile: ""
    property var projectFiles: []
    property string scopePreamble: ""
    property bool suppressEditSignal: false
    readonly property bool isCompiledPreview: hc && hostController.hasProposal
    readonly property bool hasAmbiguity: hc && hostController.isClarifying
    readonly property bool needsManualAnswer: hc && hostController.isManual && hostController.interventionReason === "CLARIFICATION_BUDGET_EXCEEDED"
    readonly property real ambiguityScore: hc ? hostController.ambiguityScore : 0.0

    function refreshProjectFiles() {
        projectFiles = hc && activeProject.path ? hostController.getProjectFiles(activeProject.path) : [];
        if (projectFiles.indexOf(targetLocalFile) < 0) targetLocalFile = "";
    }
    onCurrentProjectIndexChanged: refreshProjectFiles()

    onActiveFocusItemChanged: focusRevealTimer.restart()
    onWidthChanged: focusRevealTimer.restart()
    onHeightChanged: focusRevealTimer.restart()
    Timer {
        id: focusRevealTimer
        interval: 50
        onTriggered: rootWindow.revealFocusedControl()
    }
    function revealFocusedControl() {
        const item = rootWindow.activeFocusItem;
        const view = contentScroll.contentItem;
        if (!item || !view || typeof view.contentY !== "number") return;
        let ancestor = item;
        while (ancestor && ancestor !== mainContent) ancestor = ancestor.parent;
        if (!ancestor) return;
        const y = item.mapToItem(mainContent, 0, 0).y;
        if (y < view.contentY) view.contentY = y;
        else if (y + item.height > view.contentY + view.height) view.contentY = Math.max(0, y + item.height - view.height);
    }

    // Multi-Palette Theme System (Direct from user prototype)
    property string currentTheme: "dark" // "dark", "light", "acid", "amber"

    readonly property color bgColor: {
        if (currentTheme === "light") return "#F4F4F4";
        if (currentTheme === "acid") return "#070D08";
        if (currentTheme === "amber") return "#0F0902";
        return "#050505";
    }

    readonly property color fgColor: {
        if (currentTheme === "light") return "#050505";
        if (currentTheme === "acid") return "#4ADE80";
        if (currentTheme === "amber") return "#FBBF24";
        return "#C9D1D9"; // Soft greyish white (calmer contrast, reduced eye strain)
    }

    readonly property color borderColor: {
        if (currentTheme === "light") return "#050505";
        if (currentTheme === "acid") return "#22C55E";
        if (currentTheme === "amber") return "#F59E0B";
        return "#6E7681"; // Muted slate gray border instead of harsh stark white
    }

    readonly property color dimColor: {
        if (currentTheme === "light") return "#D1D5DB";
        if (currentTheme === "acid") return "#14331B";
        if (currentTheme === "amber") return "#3B2207";
        return "#262626";
    }

    readonly property color mutedTextColor: {
        if (currentTheme === "light") return "#4B5563";
        if (currentTheme === "acid") return "#4ADE80";
        if (currentTheme === "amber") return "#D97706";
        return "#8B949E"; // Crisp, high-contrast readable zinc/slate gray
    }

    readonly property color commentColor: {
        if (currentTheme === "light") return "#6B7280";
        if (currentTheme === "acid") return "#22C55E";
        if (currentTheme === "amber") return "#B45309";
        return "#8B949E"; // Legible IDE comment gray
    }

    readonly property color inputBgColor: {
        if (currentTheme === "light") return "#E5E7EB";
        if (currentTheme === "acid") return "#0A140B";
        if (currentTheme === "amber") return "#140D04";
        return "#09090B";
    }

    readonly property color shadowColor: {
        if (currentTheme === "light") return "#050505";
        if (currentTheme === "acid") return "#22C55E";
        if (currentTheme === "amber") return "#F59E0B";
        return "#3B4252"; // Subtle tactile shadow instead of stark white block
    }

    property bool isClarificationActive: false
    property string toastMessage: ""
    property bool pinToTop: true
    property bool hasBeenActive: false

    onActiveChanged: {
        if (active) {
            hasBeenActive = true;
        } else if (hasBeenActive && visible) {
            hasBeenActive = false;
            if (hc && (hostController.isAmbientAttached || hostController.isAmbientCapture)) {
                dismissCard();
            } else if (!pinToTop) {
                dismissCard();
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        onActivated: rootWindow.dismissCard()
    }

    Connections {
        target: rootWindow.hc ? hostController : null
        function onProjectFilesChanged() { rootWindow.refreshProjectFiles(); }
        function onDraftCaptured(text) {
            if (!promptInput.text.length || promptInput.text === hostController.currentRevisionText) {
                rootWindow.setEditorText(text);
                if (hostController.currentRevisionId.length && text !== hostController.currentRevisionText)
                    hostController.noteDraftEdited(hostController.stateVersion);
                if (text.length) rootWindow.showToast(qsTr("REVIEW CLIPBOARD DRAFT, THEN ANALYZE"));
            }
        }
        function onRevisionRestored(text) { rootWindow.setEditorText(text); rootWindow.scopePreamble = ""; }
        function onAmbientDraftUpdated(text) {
            rootWindow.setEditorText(text);
            rootWindow.scopePreamble = "";
        }
        // Delivery controls visibility. A clipboard fallback must leave the failed draft open.
        function onPromptInjected(text, appName) { rootWindow.dismissCard(); }
        function onClarificationDeferred(message, autoSelected) { rootWindow.showToast(message); }
    }

    function setEditorText(text) {
        suppressEditSignal = true;
        promptInput.text = text;
        suppressEditSignal = false;
    }
    function dismissCard() {
        toastMessage = "";
        if (hc) hostController.hideCard(); else rootWindow.hide();
    }
    function executeHardBypass() {
        if (!hc) return;
        const text = promptInput.text.trim().length ? promptInput.text : hostController.currentRevisionText;
        if (text.trim().length) {
            hostController.hardBypass(text, hostController.stateVersion);
        } else {
            dismissCard();
        }
    }
    function sendAsTyped() { executeHardBypass(); }
    function revertToDraft() { if (hc) hostController.discardProposal(hostController.stateVersion); promptInput.forceActiveFocus(); }
    function analyzeNow() {
        if (hc && promptInput.text.trim().length) hostController.analyzePrompt(promptInput.text, hostController.stateVersion);
    }
    function approveForCurrentDestination() {
        if (!hc) return;
        if (!hostController.isAmbientCapture && hostController.isAmbientActive)
            hostController.beginInsertionTargetSelection(hostController.stateVersion);
        else hostController.approveAndCopyRewrite(hostController.stateVersion);
    }
    function handleEnter() {
        if (!hc) return;
        if (hostController.isAnalyzing) { hostController.cancelAnalysis(hostController.stateVersion); return; }
        if (needsManualAnswer && !hostController.isDraftDirty) { reviewFlow.focusClarification(); return; }
        if (hostController.isDraftDirty || (!hostController.hasConfirmedIntent && !hostController.isClarifying)) analyzeNow();
        else if (hostController.isOriginalRoute) sendAsTyped();
        else if (hostController.isClarifying) reviewFlow.focusClarification();
        else if (hostController.canApprove) rootWindow.approveForCurrentDestination();
        else if (hostController.isRewriteEnabled && hostController.proposalStatus === "NONE") hostController.synthesizeRewrite(hostController.stateVersion);
        else if (hostController.hasProposal) reviewFlow.focusReview();
    }
    function enterAction() { handleEnter(); }
    function commitChoice(choiceNumber) { reviewFlow.chooseAlternative(choiceNumber - 1); }
    function applyScope() {
        if (!hc || !activeProject.path) { showToast(qsTr("CHOOSE A WORKSPACE FIRST")); return; }
        if ((!targetLocalFile || targetLocalFile.length === 0) && hostController.recommendedOption.length > 0 && hostController.recommendedConfidence >= 0.70) {
            targetLocalFile = hostController.recommendedOption;
        }
        const preamble = hostController.scopePreamble(activeProject.path, activeScope, targetLocalFile);
        if (!preamble.length) return;
        let draft = promptInput.text;
        if (scopePreamble.length && draft.startsWith(scopePreamble)) draft = draft.substring(scopePreamble.length);
        scopePreamble = preamble;
        promptInput.text = preamble + draft;
        showToast(qsTr("SCOPE ADDED TO VISIBLE DRAFT"));
    }

    function openFileDropdown() {
        activeScope = "local";
        projectDropdown.visible = false;
        if (typeof fileDropdown !== "undefined") {
            fileDropdown.filterText = "";
            if (typeof fileSearchInput !== "undefined") fileSearchInput.text = "";
            var pt = fileSelectorButton.mapToItem(subBorderInset, 0, fileSelectorButton.height + 4);
            fileDropdown.x = Math.max(10, Math.min(pt.x, subBorderInset.width - fileDropdown.width - 10));
            fileDropdown.y = pt.y;
            fileDropdown.visible = true;
        }
    }

    function showToast(msg) {
        toastMessage = msg;
        toastTimer.restart();
    }

    Timer {
        id: toastTimer
        interval: 2500
        repeat: false
        onTriggered: {
            rootWindow.toastMessage = "";
        }
    }





    function classifyIntent(text) {
        const lower = text.toLowerCase();
        if (text.trim().length === 0) return "[INTENT: EMPTY]";
        if (lower.includes("refactor") || lower.includes("clean") || lower.includes("rewrite")) return "[INTENT: REFACTOR]";
        if (lower.includes("fix") || lower.includes("bug") || lower.includes("patch") || lower.includes("error")) return "[INTENT: BUGFIX]";
        if (lower.includes("cuda") || lower.includes("kernel") || lower.includes("gpu") || lower.includes("simd")) return "[INTENT: GPU_OPTIM]";
        if (lower.includes("auth") || lower.includes("token") || lower.includes("security") || lower.includes("jwt")) return "[INTENT: SECURITY]";
        if (lower.includes("test") || lower.includes("spec") || lower.includes("bench")) return "[INTENT: VERIFY]";
        if (lower.includes("create") || lower.includes("add") || lower.includes("implement")) return "[INTENT: FEATURE]";
        return "[INTENT: GENERAL_PROMPT]";
    }

    function getWordCount(text) {
        const trimmed = text.trim();
        if (trimmed.length === 0) return 0;
        return trimmed.split(/\s+/).length;
    }

    Component.onCompleted: {
        x = (Screen.width - width) / 2;
        y = (Screen.height - height) / 2;
        refreshProjectFiles();
    }

    onVisibleChanged: {
        if (visible) {
            if (isAmbientMode) {
                isDetailedExpanded = false;
                if (hostController.currentRevisionText.length) {
                    rootWindow.setEditorText(hostController.currentRevisionText);
                }
            } else {
                promptInput.forceActiveFocus();
            }
        } else {
            hasBeenActive = false;
        }
    }

    Shortcut {
        sequence: "Esc"
        onActivated: rootWindow.dismissCard()
    }
    Shortcut {
        sequences: rootWindow.isMac ? ["Meta+Return", "Meta+Enter", "Ctrl+Return", "Ctrl+Enter"] : ["Ctrl+Return", "Ctrl+Enter"]
        onActivated: rootWindow.enterAction()
    }
    Shortcut {
        sequences: ["Shift+Return", "Shift+Enter"]
        enabled: rootWindow.hc && !promptInput.activeFocus && !reviewFlow.customAnswerHasFocus
        onActivated: rootWindow.sendAsTyped()
    }
    Shortcut {
        sequence: "1"
        enabled: rootWindow.hc && reviewFlow.choiceHasFocus && hostController.isClarifying && !reviewFlow.customAnswerHasFocus
        onActivated: reviewFlow.chooseAlternative(0)
    }
    Shortcut {
        sequence: "2"
        enabled: rootWindow.hc && reviewFlow.choiceHasFocus && hostController.isClarifying && !reviewFlow.customAnswerHasFocus
        onActivated: reviewFlow.chooseAlternative(1)
    }
    Shortcut {
        sequence: "3"
        enabled: rootWindow.hc && reviewFlow.choiceHasFocus && hostController.isClarifying && !reviewFlow.customAnswerHasFocus
        onActivated: reviewFlow.chooseAlternative(2)
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Return", "Ctrl+Shift+Enter"]
        enabled: rootWindow.hc && hostController.canApprove
        onActivated: rootWindow.approveForCurrentDestination()
    }
    Shortcut {
        sequence: "Alt+1"
        enabled: rootWindow.hc && reviewFlow.choiceHasFocus && hostController.isClarifying && !reviewFlow.customAnswerHasFocus
        onActivated: reviewFlow.chooseAlternative(0)
    }
    Shortcut {
        sequence: "Alt+2"
        enabled: rootWindow.hc && reviewFlow.choiceHasFocus && hostController.isClarifying && !reviewFlow.customAnswerHasFocus
        onActivated: reviewFlow.chooseAlternative(1)
    }
    Shortcut {
        sequence: "Alt+3"
        enabled: rootWindow.hc && reviewFlow.choiceHasFocus && hostController.isClarifying && !reviewFlow.customAnswerHasFocus
        onActivated: reviewFlow.chooseAlternative(2)
    }
    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: if (rootWindow.hc) hostController.showDashboard()
    }

    // =========================================================================
    // POSTER BOARD CONTAINER (Double Bounding Box Structure)
    // =========================================================================
    Rectangle {
        id: posterBoard
        anchors.fill: parent
        color: rootWindow.bgColor
        border.color: rootWindow.borderColor
        border.width: 2
        radius: 16

        MouseArea {
            id: windowDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: rootWindow.startSystemMove()
        }

        // =====================================================================
        // FRAMELESS WINDOW RESIZE HANDLES (Edge & Corner Dragging)
        // =====================================================================
        MouseArea {
            id: resizeBottomRight
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: 14; height: 14
            z: 100
            cursorShape: Qt.SizeFDiagCursor
            onPressed: rootWindow.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        }
        MouseArea {
            id: resizeRight
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: 14; anchors.bottomMargin: 14
            width: 7
            z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: rootWindow.startSystemResize(Qt.RightEdge)
        }
        MouseArea {
            id: resizeBottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 7
            z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: rootWindow.startSystemResize(Qt.BottomEdge)
        }
        MouseArea {
            id: resizeBottomLeft
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            width: 14; height: 14
            z: 100
            cursorShape: Qt.SizeBDiagCursor
            onPressed: rootWindow.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
        }
        MouseArea {
            id: resizeLeft
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: 14; anchors.bottomMargin: 14
            width: 7
            z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: rootWindow.startSystemResize(Qt.LeftEdge)
        }
        MouseArea {
            id: resizeTop
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 7
            z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: rootWindow.startSystemResize(Qt.TopEdge)
        }
        MouseArea {
            id: resizeTopRight
            anchors.right: parent.right
            anchors.top: parent.top
            width: 14; height: 14
            z: 100
            cursorShape: Qt.SizeBDiagCursor
            onPressed: rootWindow.startSystemResize(Qt.RightEdge | Qt.TopEdge)
        }
        MouseArea {
            id: resizeTopLeft
            anchors.left: parent.left
            anchors.top: parent.top
            width: 14; height: 14
            z: 100
            cursorShape: Qt.SizeFDiagCursor
            onPressed: rootWindow.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
        }

        // SUB-BORDER INSET (Double Bounding Frame)
        Rectangle {
            id: subBorderInset
            anchors.fill: parent
            anchors.margins: 10
            color: rootWindow.bgColor
            border.color: rootWindow.borderColor
            border.width: 1
            radius: 10
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                // =============================================================
                // 1. TOP HEADER: Status, Model Hot-Swap, Palette, Window Controls
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 24
                    spacing: 8

                    // Pulsing status dot
                    Rectangle {
                        width: 8
                        height: 8
                        color: rootWindow.fgColor
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            NumberAnimation { from: 1.0; to: 0.2; duration: 600; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: 0.2; to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
                        }
                    }

                    Text {
                        text: rootWindow.width >= 620 ? "ROCLINT" : "ROC"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 10
                        font.letterSpacing: 1
                        color: rootWindow.fgColor
                    }

                    // Project Dropdown Selector Pill
                    Rectangle {
                        id: projectSelectorButton
                        visible: rootWindow.width >= 530
                        height: 20
                        width: Math.min(140, projectRow.implicitWidth + 12)
                        clip: true
                        color: (projectDropdown.visible || projectMouse.containsMouse) ? rootWindow.dimColor : "transparent"
                        border.color: projectDropdown.visible ? rootWindow.fgColor : rootWindow.borderColor
                        border.width: 1

                        RowLayout {
                            id: projectRow
                            anchors.centerIn: parent
                            spacing: 4

                            Text {
                                text: "PROJECT:"
                                visible: rootWindow.width >= 720
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.mutedTextColor
                            }
                            Text {
                                text: rootWindow.activeProject.name + " " + (projectDropdown.visible ? "▴" : "▾")
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                                elide: Text.ElideRight
                                Layout.maximumWidth: 90
                            }
                        }

                        HudMouseArea {
                            id: projectMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (!projectDropdown.visible) {
                                    var pt = projectSelectorButton.mapToItem(subBorderInset, 0, projectSelectorButton.height + 4);
                                    projectDropdown.x = pt.x;
                                    projectDropdown.y = pt.y;
                                    projectDropdown.visible = true;
                                } else {
                                    projectDropdown.visible = false;
                                }
                            }
                        }
                    }

                    // Host Context Advisory Chip (B02H / C09)
                    Rectangle {
                        id: hostContextChip
                        visible: (typeof hostController !== "undefined" && hostController.hasHostContext) && rootWindow.width >= 690
                        height: 20
                        width: Math.min(140, ctxRow.implicitWidth + 16)
                        clip: true
                        color: "transparent"
                        border.color: rootWindow.borderColor
                        border.width: 1

                        Row {
                            id: ctxRow
                            anchors.centerIn: parent
                            spacing: 6

                            Text {
                                text: (rootWindow.width >= 740 ? "FROM: " : "") + (typeof hostController !== "undefined" ? hostController.hostContextDisplayName.toUpperCase() : "")
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                font.bold: true
                                color: rootWindow.fgColor
                                elide: Text.ElideMiddle
                                width: Math.min(85, implicitWidth)
                            }

                            Rectangle {
                                width: 14
                                height: 14
                                color: clearCtxMouse.containsMouse ? rootWindow.dimColor : "transparent"
                                border.color: rootWindow.borderColor
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: "×"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: rootWindow.mutedTextColor
                                }

                                HudMouseArea {
                                    id: clearCtxMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (typeof hostController !== "undefined") {
                                            hostController.clearHostContext();
                                            rootWindow.showToast("CONTEXT CLEARED");
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                    // Window Controls & Status Group (Anchored to Right)
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: 6

                        // Inference / Engine Badge
                        Rectangle {
                            id: engineBadge
                            visible: rootWindow.width >= 600
                            implicitWidth: engineBadgeTxt.implicitWidth + 10
                            implicitHeight: 20
                            height: 20
                            color: "transparent"
                            border.color: (typeof hostController !== "undefined" && hostController.isInferring)
                                          ? "#FFB000"
                                          : (typeof hostController !== "undefined" && hostController.modelRuntimeState === "FAILED")
                                            ? "#F85149"
                                          : (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                            ? "#7EE787"
                                          : (typeof hostController !== "undefined" && hostController.isNativeGraniteInstalled)
                                            ? "#38BDF8"
                                          : rootWindow.borderColor
                            border.width: 1

                            Text {
                                id: engineBadgeTxt
                                objectName: "modelRuntimeLabel"
                                anchors.centerIn: parent
                                text: {
                                    if (!rootWindow.hc) return "[ MODEL UNAVAILABLE ]";
                                    if (hostController.isInferring) return "[ 🧠 INFERRING (" + (hostController.acceleratorName ? hostController.acceleratorName : "GPU") + ") ]";
                                    if (hostController.modelRuntimeState === "LOADING") return "[ LOADING MODEL ]";
                                    if (hostController.useLiveWeights) return "[ LOCAL MODEL ]";
                                    if (hostController.modelRuntimeState === "FAILED") return "[ MODEL ERROR ]";
                                    if (hostController.isNativeGraniteInstalled) return "[ READY · STANDBY ]";
                                    return "[ MODEL NOT LOADED ]";
                                }
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: (typeof hostController !== "undefined" && hostController.isInferring)
                                       ? "#FFB000"
                                       : (typeof hostController !== "undefined" && hostController.modelRuntimeState === "FAILED")
                                         ? "#F85149"
                                       : ((typeof hostController !== "undefined" && hostController.useLiveWeights)
                                         ? "#7EE787"
                                         : ((typeof hostController !== "undefined" && hostController.isNativeGraniteInstalled)
                                           ? "#38BDF8"
                                           : rootWindow.mutedTextColor))
                            }

                            HudMouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (typeof hostController !== "undefined") {
                                        hostController.showDashboard();
                                    }
                                }
                            }
                        }

                        // Status Badge
                        Rectangle {
                            id: statusBadge
                            implicitWidth: statusTextLabel.implicitWidth + 12
                            implicitHeight: 20
                            Layout.preferredWidth: implicitWidth
                            Layout.preferredHeight: 20
                            height: 20
                            width: implicitWidth
                            color: "transparent"
                            border.color: rootWindow.hasAmbiguity ? "#FFB000" : "#7EE787"
                            border.width: 1

                            Text {
                                id: statusTextLabel
                                anchors.centerIn: parent
                                text: rootWindow.hasAmbiguity ? "[ CLARIFY ]"
                                      : (rootWindow.hc ? "[ " + hostController.analysisStatus + " ]" : "[ IDLE ]")
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 9
                                color: rootWindow.hasAmbiguity ? "#FFB000" : "#7EE787"
                            }
                        }

                        // Progressive Expansion Toggle [ ⤢ SHOW MORE ] / [ ⤡ SHOW LESS ]
                        Rectangle {
                            id: expandToggleButton
                            visible: rootWindow.isAmbientMode
                            implicitWidth: expandToggleText.implicitWidth + 12
                            implicitHeight: 20
                            Layout.preferredWidth: implicitWidth
                            Layout.preferredHeight: 20
                            height: 20
                            width: implicitWidth
                            color: expandToggleMouse.containsMouse ? rootWindow.dimColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1
                            radius: 3

                            Text {
                                id: expandToggleText
                                anchors.centerIn: parent
                                text: rootWindow.isDetailedExpanded ? "[ ⤡ SHOW LESS ]" : "[ ⤢ SHOW MORE ]"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                            }

                            HudMouseArea {
                                id: expandToggleMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: rootWindow.toggleDetailedExpanded()
                            }
                        }

                        // Pin Toggle [PIN] / [PINNED] - Fixed width to prevent shifting/overlapping adjacent items
                        Rectangle {
                            id: pinButton
                            implicitWidth: 50
                            implicitHeight: 20
                            Layout.preferredWidth: 50
                            Layout.preferredHeight: 20
                            width: 50
                            height: 20
                            color: rootWindow.pinToTop ? rootWindow.fgColor : (pinMouse.containsMouse ? rootWindow.dimColor : "transparent")
                            border.color: rootWindow.borderColor
                            border.width: 1

                            Text {
                                id: pinText
                                anchors.centerIn: parent
                                text: rootWindow.pinToTop ? "PINNED" : "PIN"
                                font.family: rootWindow.monoFont
                                font.bold: rootWindow.pinToTop
                                font.pixelSize: 8
                                color: rootWindow.pinToTop ? rootWindow.bgColor : rootWindow.fgColor
                            }
                            HudMouseArea {
                                id: pinMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    rootWindow.pinToTop = !rootWindow.pinToTop;
                                    rootWindow.showToast(rootWindow.pinToTop ? "WINDOW PINNED (ALWAYS ON TOP)" : "WINDOW UNPINNED");
                                }
                            }
                        }

                        // Settings Dashboard Button [SET]
                        Rectangle {
                            id: setButton
                            implicitWidth: 32
                            implicitHeight: 20
                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 20
                            width: 32
                            height: 20
                            color: setMouse.containsMouse ? rootWindow.dimColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            Text {
                                id: setText
                                anchors.centerIn: parent
                                text: "SET"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                            }
                            HudMouseArea {
                                id: setMouse
                                objectName: "openSettingsControl"
                                controlName: qsTr("Open settings")
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (typeof hostController !== "undefined" && hostController) {
                                        hostController.showDashboard();
                                    }
                                }
                            }
                        }

                        // Window Minimize [-]
                        Rectangle {
                            id: minButton
                            implicitWidth: 22
                            implicitHeight: 20
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 20
                            width: 22
                            height: 20
                            color: minMouse.containsMouse ? rootWindow.dimColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "[-]"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 9
                                color: rootWindow.fgColor
                            }
                            HudMouseArea {
                                id: minMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: rootWindow.isMac ? rootWindow.dismissCard() : rootWindow.showMinimized()
                            }
                        }

                        // Window Close [x]
                        Rectangle {
                            id: closeButton
                            implicitWidth: 22
                            implicitHeight: 20
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 20
                            width: 22
                            height: 20
                            color: closeMouse.containsMouse ? "#8B1E1E" : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                text: "[x]"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 9
                                color: rootWindow.fgColor
                            }
                            HudMouseArea {
                                id: closeMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: rootWindow.dismissCard()
                            }
                        }
                    }
                }

                // Divider below top bar
                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    height: 1
                    color: rootWindow.borderColor
                    opacity: 0.3
                }

                // =============================================================
                // 1b. SCOPE GOVERNANCE STRIP (Global Codebase vs Local File)
                // =============================================================
                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    height: 24
                    color: rootWindow.dimColor
                    border.color: rootWindow.borderColor
                    border.width: 1
                    radius: 4

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Text {
                            text: "SCOPE:"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 8
                            color: rootWindow.fgColor
                        }

                        // GLOBAL CODEBASE Pill (Default)
                        Rectangle {
                            height: 18
                            width: globalRow.width + 10
                            color: rootWindow.activeScope === "global" ? rootWindow.fgColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1
                            radius: 3

                            RowLayout {
                                id: globalRow
                                anchors.centerIn: parent
                                spacing: 4
                                Text {
                                    text: rootWindow.activeScope === "global" ? "●" : "○"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 8
                                    color: rootWindow.activeScope === "global" ? rootWindow.bgColor : rootWindow.fgColor
                                }
                                Text {
                                    text: rootWindow.width >= 680 ? "ENTIRE PROJECT" : "ALL FILES"
                                    font.family: rootWindow.monoFont
                                    font.bold: rootWindow.activeScope === "global"
                                    font.pixelSize: 8
                                    color: rootWindow.activeScope === "global" ? rootWindow.bgColor : rootWindow.fgColor
                                }
                            }
                            HudMouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    rootWindow.activeScope = "global";
                                    rootWindow.showToast("SCOPE: ENTIRE PROJECT (" + rootWindow.activeProject.name + ")");
                                }
                            }
                        }

                        // SINGLE FILE Pill
                        Rectangle {
                            height: 18
                            width: localRow.width + 10
                            color: rootWindow.activeScope === "local" ? rootWindow.fgColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1
                            radius: 3

                            RowLayout {
                                id: localRow
                                anchors.centerIn: parent
                                spacing: 4
                                Text {
                                    text: rootWindow.activeScope === "local" ? "●" : "○"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 8
                                    color: rootWindow.activeScope === "local" ? rootWindow.bgColor : rootWindow.fgColor
                                }
                                Text {
                                    text: "SINGLE FILE"
                                    font.family: rootWindow.monoFont
                                    font.bold: rootWindow.activeScope === "local"
                                    font.pixelSize: 8
                                    color: rootWindow.activeScope === "local" ? rootWindow.bgColor : rootWindow.fgColor
                                }
                            }
                            HudMouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    rootWindow.activeScope = "local";
                                    rootWindow.showToast("SCOPE: SINGLE FILE ACTIVE");
                                }
                            }
                        }

                        // File Selector Dropdown Button (ONLY visible when activeScope === "local")
                        Rectangle {
                            id: fileSelectorButton
                            visible: rootWindow.activeScope === "local"
                            height: 18
                            width: fileSelectorRow.width + 12
                            color: (typeof fileDropdown !== "undefined" && fileDropdown.visible || fileSelectorMouse.containsMouse) ? rootWindow.dimColor : rootWindow.inputBgColor
                            border.color: (typeof fileDropdown !== "undefined" && fileDropdown.visible) ? "#FFB000" : rootWindow.borderColor
                            border.width: 1
                            radius: 2

                            RowLayout {
                                id: fileSelectorRow
                                anchors.centerIn: parent
                                spacing: 4

                                Text {
                                    text: "FILE:"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: "#FFB000"
                                }

                                Text {
                                    readonly property string displayFileName: {
                                        if (rootWindow.targetLocalFile && rootWindow.targetLocalFile.length > 0) {
                                            var parts = rootWindow.targetLocalFile.split("/");
                                            return parts[parts.length - 1];
                                        }
                                        if (hostController.recommendedOption.length > 0 && hostController.recommendedConfidence >= 0.70) {
                                            var rparts = hostController.recommendedOption.split("/");
                                            return "Auto: " + rparts[rparts.length - 1];
                                        }
                                        return "Select file...";
                                    }
                                    text: displayFileName + " " + (typeof fileDropdown !== "undefined" && fileDropdown.visible ? "▴" : "▾")
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: (rootWindow.targetLocalFile && rootWindow.targetLocalFile.length > 0) ? rootWindow.fgColor :
                                           ((hostController.recommendedOption.length > 0 && hostController.recommendedConfidence >= 0.70) ? "#F59E0B" : rootWindow.mutedTextColor)
                                    elide: Text.ElideMiddle
                                    Layout.maximumWidth: Math.min(220, Math.max(90, rootWindow.width - 450))
                                }
                            }

                            HudMouseArea {
                                id: fileSelectorMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    projectDropdown.visible = false;
                                    if (typeof fileDropdown !== "undefined") {
                                        if (!fileDropdown.visible) {
                                            fileDropdown.filterText = "";
                                            if (typeof fileSearchInput !== "undefined") fileSearchInput.text = "";
                                            var pt = fileSelectorButton.mapToItem(subBorderInset, 0, fileSelectorButton.height + 4);
                                            fileDropdown.x = Math.max(10, Math.min(pt.x, subBorderInset.width - fileDropdown.width - 10));
                                            fileDropdown.y = pt.y;
                                            fileDropdown.visible = true;
                                            if (typeof fileSearchInput !== "undefined") fileSearchInput.forceActiveFocus();
                                        } else {
                                            fileDropdown.visible = false;
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        Rectangle {
                            implicitWidth: applyScopeText.implicitWidth + 10; height: 18
                            color: "transparent"; border.color: rootWindow.borderColor; radius: 3
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Apply selected scope to draft")
                            Accessible.onPressAction: rootWindow.applyScope()
                            Keys.onReturnPressed: rootWindow.applyScope()
                            Keys.onSpacePressed: rootWindow.applyScope()
                            Text { id: applyScopeText; anchors.centerIn: parent; text: "[ APPLY SCOPE ]"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor }
                            HudMouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: rootWindow.applyScope() }
                        }

                        // Active Scope Target Tag
                        RowLayout {
                            visible: rootWindow.width >= 640
                            spacing: 4

                            Text {
                                text: rootWindow.activeScope === "global" ?
                                      "TARGET: ALL FILES" :
                                      "TARGET: @" + (rootWindow.targetLocalFile.length > 0 ? rootWindow.targetLocalFile : "...")
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.activeScope === "global" ? "#7EE787" : "#FFB000"
                            }

                            // If single file, show a small [×] to quickly clear back to global
                            Rectangle {
                                visible: rootWindow.activeScope === "local"
                                height: 12
                                width: 12
                                color: "transparent"
                                border.color: "#FFB000"
                                border.width: 1
                                radius: 2
                                Text {
                                    anchors.centerIn: parent
                                    text: "×"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: "#FFB000"
                                }
                                HudMouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        rootWindow.activeScope = "global";
                                        rootWindow.showToast("SCOPE: ALL FILES");
                                    }
                                }
                            }
                        }
                    }
                }

                // =============================================================
                // 2. CENTRAL PROMPT INGESTION & INTERACTION MATRIX
                // =============================================================
                ScrollView {
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical: HudScrollBar {}
                    id: contentScroll
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                ColumnLayout {
                    id: mainContent
                    width: contentScroll.availableWidth
                    height: Math.max(implicitHeight, contentScroll.availableHeight)
                    spacing: 6



                        // Prompt Terminal Canvas (Double Bounded Inset)
                        Rectangle {
                            visible: !rootWindow.hc || !hostController.isAmbientAttached
                            Layout.minimumHeight: visible ? 150 : 0
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.fillHeight: visible
                            color: rootWindow.inputBgColor
                            border.color: promptInput.activeFocus ? rootWindow.fgColor : rootWindow.borderColor
                            border.width: 1
                            radius: 6

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 6

                                ScrollView {
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical: HudScrollBar {}
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.fillHeight: true
                                    clip: true

                                    TextArea {
                                        id: promptInput
                                        background: null
                                        readOnly: false
                                        objectName: "promptInput"
                                        Accessible.name: qsTr("Prompt draft")
                                        activeFocusOnTab: true
                                        placeholderText: qsTr("// Type your prompt here...\n// Example: 'Update SessionManager timeout handling and run tests'")
                                        placeholderTextColor: rootWindow.commentColor
                                        color: rootWindow.fgColor
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 12
                                        wrapMode: TextEdit.Wrap
                                        selectByMouse: true
                                        focus: true
                                        leftPadding: 6
                                        rightPadding: 6
                                        topPadding: 6
                                        bottomPadding: 6
                                        onTextChanged: {
                                            if (rootWindow.hc && !rootWindow.suppressEditSignal)
                                                hostController.noteDraftEdited(hostController.stateVersion);
                                        }
                                        Keys.onPressed: function(event) {
                                            if ((event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                                                rootWindow.handleEnter(); event.accepted = true;
                                            }
                                        }
                                    }
                                }

                                // Canvas Sub-bar
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    height: 1
                                    color: rootWindow.borderColor
                                    opacity: 0.3
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    spacing: 8

                                    RowLayout {
                                        spacing: 4
                                        Text {
                                            text: "STATUS:"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: rootWindow.fgColor
                                        }
                                        Text {
                                            text: {
                                                if (rootWindow.isCompiledPreview) return "REWRITE AWAITING REVIEW";
                                                if (rootWindow.needsManualAnswer) return "ANSWER REMAINING DECISION";
                                                if (rootWindow.hasAmbiguity) return "NEEDS CLARITY";
                                                return "READY";
                                            }
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: {
                                                if (rootWindow.isCompiledPreview) return "#7EE787";
                                                if (rootWindow.hasAmbiguity) return "#FFB000";
                                                return rootWindow.fgColor;
                                            }
                                        }
                                        Text {
                                            text: "█"
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 8
                                            color: rootWindow.isCompiledPreview ? "#7EE787" : rootWindow.fgColor
                                            SequentialAnimation on opacity {
                                                loops: Animation.Infinite
                                                NumberAnimation { from: 1; to: 0; duration: 420 }
                                                NumberAnimation { from: 0; to: 1; duration: 420 }
                                            }
                                        }

                                        // Revert to draft pill when previewing compiled prompt
                                        Rectangle {
                                            visible: rootWindow.isCompiledPreview
                                            height: 16
                                            implicitWidth: revertDraftText.contentWidth + 10
                                            color: "transparent"
                                            border.color: "#FFB000"
                                            border.width: 1
                                            radius: 3
                                            Text {
                                                id: revertDraftText
                                                anchors.centerIn: parent
                                                text: "[ ↺ EDIT DRAFT ]"
                                                font.family: rootWindow.monoFont
                                                font.bold: true
                                                font.pixelSize: 7
                                                color: "#FFB000"
                                            }
                                            HudMouseArea {
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: rootWindow.revertToDraft()
                                            }
                                        }
                                    }

                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                    Text {
                                        visible: rootWindow.width >= 620
                                        text: rootWindow.isCompiledPreview ? "MODE: PREVIEW & VERIFY" : "INPUT: VALID"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.isCompiledPreview ? "#7EE787" : rootWindow.fgColor
                                    }
                                    Text {
                                        text: rootWindow.isCompiledPreview ? "REVIEW" : (rootWindow.hasAmbiguity ? "RESOLVE INTENT" : (rootWindow.hc ? hostController.analysisStatus : "IDLE"))
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.isCompiledPreview ? "#7EE787" : (rootWindow.hasAmbiguity ? "#FFB000" : rootWindow.fgColor)
                                    }
                                    Text {
                                        text: rootWindow.hc && hostController.isAmbiguityScored ? "AMBIGUITY: " + (rootWindow.ambiguityScore * 100).toFixed(0) + "%" : "UNRANKED"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.isCompiledPreview ? "#7EE787" : rootWindow.fgColor
                                    }
                                }
                            }
                        }

                        // =============================================================
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: text.length > 0
                    text: rootWindow.hc ? [hostController.shortcutWarning, hostController.lastError === hostController.analysisMessage ? "" : hostController.lastError, hostController.analysisMessage].filter(function(x) { return x.length > 0; }).join("\n") : ""
                    wrapMode: Text.WordWrap
                    color: rootWindow.mutedTextColor
                    font.family: rootWindow.monoFont
                    font.pixelSize: 10
                }
                IntentReviewPanel {
                    id: reviewFlow
                    visible: true
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    currentTheme: rootWindow.currentTheme
                    viewportHeight: rootWindow.height
                }
            }
                }

                // =============================================================
                // 3. TELEMETRY STRIP (Real System & Compiler Metrics)
                // =============================================================
                Rectangle {
                    visible: !rootWindow.isAmbientMode || rootWindow.isDetailedExpanded
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 22
                    color: "transparent"
                    border.color: rootWindow.borderColor
                    border.width: 1
                    radius: 3

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Text {
                            text: "EST. " + Math.ceil(promptInput.text.length / 4) + " TOKENS"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 8
                            color: rootWindow.fgColor
                        }

                        Text { text: "│"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }

                        Text {
                            text: "REVISION: v" + (typeof hostController !== "undefined" ? hostController.stateVersion : "0")
                            font.family: rootWindow.monoFont
                            font.pixelSize: 8
                            color: rootWindow.fgColor
                        }

                        Text { text: "│"; visible: rootWindow.width >= 620 && (!rootWindow.hc || !hostController.isAmbientAttached); font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }

                        Text {
                            visible: rootWindow.width >= 620 && (!rootWindow.hc || !hostController.isAmbientAttached)
                            text: rootWindow.hc ? hostController.deliveryLabel : "CLIPBOARD"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 8
                            color: rootWindow.fgColor
                        }

                        Text { text: "│"; visible: rootWindow.width >= 540 && rootWindow.hc && hostController.inferenceSpeed.length > 0; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }

                        Text {
                            visible: rootWindow.width >= 540 && rootWindow.hc && hostController.inferenceSpeed.length > 0
                            text: "SPEED: " + (rootWindow.hc ? hostController.inferenceSpeed : "")
                            font.family: rootWindow.monoFont
                            font.pixelSize: 8
                            font.bold: true
                            color: "#38BDF8"
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        Text {
                            text: "SESSIONS: " + (typeof hostController !== "undefined" ? hostController.sessionCount : "0")
                            font.family: rootWindow.monoFont
                            font.pixelSize: 8
                            color: rootWindow.mutedTextColor
                        }
                    }
                }

                // =============================================================
                // 4. FOOTER BAR: Metrics, Hard-Shadow Tactile Action Buttons
                // =============================================================
                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 36
                    color: "transparent"
                    border.color: rootWindow.borderColor
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        // Live Character & Word Counts (Visible on wider layouts when toast isn't taking space)
                        Rectangle {
                            visible: rootWindow.width >= 680 && rootWindow.toastMessage.length === 0
                            height: 16
                            implicitWidth: cTag.contentWidth + 6
                            color: rootWindow.fgColor
                            Text {
                                id: cTag
                                anchors.centerIn: parent
                                text: "STATS"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.bgColor
                            }
                        }

                        Text {
                            visible: rootWindow.width >= 660 && rootWindow.toastMessage.length === 0
                            text: "CHARS: " + promptInput.text.length
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        Text {
                            visible: rootWindow.width >= 680 && rootWindow.toastMessage.length === 0
                            text: "|"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 9
                            color: rootWindow.mutedTextColor
                        }

                        Text {
                            visible: rootWindow.width >= 680 && rootWindow.toastMessage.length === 0
                            text: "WORDS: " + rootWindow.getWordCount(promptInput.text)
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        Text {
                            visible: rootWindow.width >= 680 && rootWindow.toastMessage.length === 0
                            text: "|"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 9
                            color: rootWindow.mutedTextColor
                        }

                        // Progressive Expansion Link in Footer
                        Text {
                            visible: rootWindow.isAmbientMode && !rootWindow.isDetailedExpanded && rootWindow.toastMessage.length === 0
                            text: "[ ⤢ SHOW MORE ]"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 8
                            color: expandFooterMouse.containsMouse ? rootWindow.fgColor : rootWindow.mutedTextColor
                            HudMouseArea {
                                id: expandFooterMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: rootWindow.toggleDetailedExpanded()
                            }
                        }

                        Text {
                            visible: (!rootWindow.isAmbientMode || rootWindow.isDetailedExpanded) && rootWindow.width >= 580 && rootWindow.hc && hostController.inferenceSpeed.length > 0 && rootWindow.toastMessage.length === 0
                            text: "SPEED: " + (rootWindow.hc ? hostController.inferenceSpeed : "")
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: "#38BDF8"
                        }

                        Text {
                            visible: rootWindow.width >= 580 && rootWindow.hc && hostController.inferenceSpeed.length > 0 && rootWindow.toastMessage.length === 0
                            text: "|"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 9
                            color: rootWindow.mutedTextColor
                        }

                        // System Toast Banner
                        Text {
                            visible: rootWindow.toastMessage.length > 0
                            text: "✦ " + rootWindow.toastMessage
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                            elide: Text.ElideRight
                            Layout.maximumWidth: Math.max(100, rootWindow.width - 340)
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        // Clear Action Buttons (Right-aligned container)
                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: 6

                            // Button 1: AS-IS or EDIT DRAFT
                            Rectangle {
                                id: bypassBtn
                                height: 22
                                implicitWidth: b1Text.contentWidth + 14
                                color: bypassMouse.pressed ? rootWindow.dimColor : (bypassMouse.containsMouse ? rootWindow.dimColor : "transparent")
                                border.color: rootWindow.isCompiledPreview ? "#FFB000" : rootWindow.borderColor
                                border.width: 1
                                radius: 3

                                Text {
                                    id: b1Text
                                    anchors.centerIn: parent
                                    text: {
                                        if (rootWindow.hc && hostController.isAmbientAttached) return "[ SEND AS-IS ]";
                                        if (rootWindow.isCompiledPreview) return "[ ↺ EDIT DRAFT ]";
                                        const insert = rootWindow.hc && hostController.isAmbientCapture;
                                        return rootWindow.width >= 720 ? (insert ? "[ SEND AS-IS ]" : "[ COPY AS-IS ]")
                                                                      : (insert ? "[ SEND AS-IS ]" : "[ COPY ]");
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.isCompiledPreview ? "#FFB000" : rootWindow.fgColor
                                }
                                HudMouseArea {
                                    id: bypassMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (rootWindow.hc && hostController.isAmbientAttached) {
                                            rootWindow.executeHardBypass();
                                        } else if (rootWindow.isCompiledPreview) {
                                            rootWindow.revertToDraft();
                                        } else {
                                            rootWindow.executeHardBypass();
                                        }
                                    }
                                }
                            }

                            // Button 2: ENHANCE or COPY AGAIN (Primary Action)
                            Rectangle {
                                id: triageBtn
                                height: 22
                                implicitWidth: b2Text.contentWidth + 16
                                color: {
                                    if (triageMouse.pressed) return rootWindow.dimColor;
                                    if (triageMouse.containsMouse) return "#E2E8F0";
                                    if (rootWindow.isCompiledPreview) return "#7EE787";
                                    return rootWindow.fgColor;
                                }
                                border.color: rootWindow.borderColor
                                border.width: 1
                                radius: 3

                                Text {
                                    id: b2Text
                                    objectName: "analysisActionLabel"
                                    anchors.centerIn: parent
                                    text: {
                                        if (rootWindow.hc && hostController.isAnalyzing) return "[ CANCEL ANALYSIS ]";
                                        if (rootWindow.isCompiledPreview) return (hostController.canApprove ? ((hostController.isAmbientCapture || hostController.isAmbientAttached) ? "[ ENHANCE & REPLACE ]" : (hostController.isAmbientActive ? "[ CONNECT EDITOR ]" : "[ APPROVE & COPY ]")) : "[ REVIEW REWRITE ]");
                                        if (rootWindow.hc && hostController.isOriginalRoute) return "[ SEND ORIGINAL ]";
                                        if (rootWindow.hasAmbiguity) return rootWindow.width >= 720 ? "[ 1-2: RESOLVE ]" : "[ RESOLVE ]";
                                        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent && !hostController.isIntentStale) {
                                            return rootWindow.width >= 720 ? ((hostController.isAmbientCapture || hostController.isAmbientAttached) ? "[ ⏎ ENHANCE & REPLACE ]" : "[ ⏎ SYNTHESIZE REWRITE ]") : "[ ⏎ REWRITE ]";
                                        }
                                        if (rootWindow.needsManualAnswer) return "[ ANSWER REMAINING ]";
                                        return rootWindow.width >= 720 ? "[ ANALYZE PROMPT ]" : "[ ANALYZE ]";
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: triageMouse.pressed ? rootWindow.fgColor : (rootWindow.isCompiledPreview ? "#050505" : rootWindow.bgColor)
                                }
                                HudMouseArea {
                                    id: triageMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: rootWindow.handleEnter()
                                }
                            }

                            // Button 3: HIDE
                            Rectangle {
                                id: hideBtn
                                height: 22
                                implicitWidth: hideTextLabel.contentWidth + 14
                                color: hideMouse.pressed ? rootWindow.dimColor : (hideMouse.containsMouse ? rootWindow.dimColor : "transparent")
                                border.color: rootWindow.borderColor
                                border.width: 1
                                radius: 3

                                Text {
                                    id: hideTextLabel
                                    anchors.centerIn: parent
                                    text: "[ HIDE ]"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                }
                                HudMouseArea {
                                    id: hideMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: rootWindow.dismissCard()
                                }
                            }
                        }
                    }
                }
            }

            // Dropdown Backdrop: Click outside dropdowns to close
            MouseArea {
                id: dropdownBackdrop
                anchors.fill: parent
                visible: projectDropdown.visible || (typeof fileDropdown !== "undefined" && fileDropdown.visible)
                z: 990
                onClicked: {
                    projectDropdown.visible = false;
                    if (typeof fileDropdown !== "undefined") fileDropdown.visible = false;
                }
            }

            // Neo-Brutalist Project Dropdown Menu
            Item {
                id: projectDropdown
                visible: false
                z: 999
                width: 280
                height: projectDropdownContent.implicitHeight

                // Hard shadow
                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.topMargin: 4
                    color: rootWindow.shadowColor
                    radius: 4
                }

                // Dropdown Card
                Rectangle {
                    anchors.fill: parent
                    color: rootWindow.bgColor
                    border.color: rootWindow.borderColor
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        id: projectDropdownContent
                        width: parent.width
                        spacing: 0

                        // Dropdown Title Strip
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            height: 22
                            color: rootWindow.dimColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                Text {
                                    text: "SELECT WORKSPACE FOLDER"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text {
                                    text: "PROJECTS"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 7
                                    color: rootWindow.fgColor
                                    opacity: 0.7
                                }
                            }
                        }

                        // Project List
                        Repeater {
                            model: rootWindow.projectList
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                height: 32
                                color: pItemMouse.containsMouse ? rootWindow.dimColor : (rootWindow.currentProjectIndex === index ? rootWindow.inputBgColor : "transparent")

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    Text {
                                        text: rootWindow.currentProjectIndex === index ? "●" : "○"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 9
                                        color: rootWindow.currentProjectIndex === index ? rootWindow.fgColor : rootWindow.dimColor
                                    }

                                    ColumnLayout {
                                        spacing: 1
                                        Text {
                                            text: modelData.name
                                            font.family: rootWindow.monoFont
                                            font.bold: rootWindow.currentProjectIndex === index
                                            font.pixelSize: 9
                                            color: rootWindow.fgColor
                                        }
                                        Text {
                                            text: modelData.path
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 7
                                            color: rootWindow.mutedTextColor
                                            elide: Text.ElideMiddle
                                            Layout.maximumWidth: 220
                                        }
                                    }
                                }

                                HudMouseArea {
                                    id: pItemMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        rootWindow.currentProjectIndex = index;
                                        rootWindow.refreshProjectFiles();
                                        rootWindow.showToast("PROJECT: " + modelData.name + " (" + rootWindow.projectFiles.length + " files)");
                                        projectDropdown.visible = false;
                                    }
                                }
                            }
                        }

                        // Add Folder Action
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            height: 26
                            color: addProjMouse.containsMouse ? rootWindow.dimColor : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 6

                                Text { text: "[ + ]"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 8; color: rootWindow.fgColor }
                                Text { text: "OPEN PROJECT FOLDER..."; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.8 }
                            }

                            HudMouseArea {
                                id: addProjMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    projectDropdown.visible = false;
                                    if (typeof hostController !== "undefined") {
                                        var chosen = hostController.openFolderDialog();
                                        if (chosen && chosen.length > 0) {
                                            var segs = chosen.split("/");
                                            var folderName = segs[segs.length - 1];
                                            if (!folderName || folderName.length === 0) folderName = chosen;

                                            var foundIdx = -1;
                                            for (var i = 0; i < rootWindow.projectList.length; ++i) {
                                                if (rootWindow.projectList[i].path === chosen) {
                                                    foundIdx = i;
                                                    break;
                                                }
                                            }
                                            if (foundIdx !== -1) {
                                                rootWindow.currentProjectIndex = foundIdx;
                                            } else {
                                                var updated = rootWindow.projectList.slice(0);
                                                updated.push({ name: folderName, path: chosen });
                                                rootWindow.projectList = updated;
                                                rootWindow.currentProjectIndex = updated.length - 1;
                                            }
                                            rootWindow.refreshProjectFiles();
                                            rootWindow.showToast("OPENED: " + folderName + " (" + rootWindow.projectFiles.length + " files)");
                                            return;
                                        }
                                    }
                                    rootWindow.showToast("PROJECT FOLDER UNCHANGED");
                                }
                            }
                        }
                    }
                }
            }

            // Neo-Brutalist Single-File Target Dropdown Menu
            Item {
                id: fileDropdown
                visible: false
                z: 999
                width: 340
                property string filterText: ""
                readonly property var filteredFiles: {
                    if (!filterText || filterText.trim() === "") return rootWindow.projectFiles;
                    var ft = filterText.toLowerCase();
                    return rootWindow.projectFiles.filter(function(f) {
                        return f.toLowerCase().indexOf(ft) !== -1;
                    });
                }
                // Clamp height to available window space so it never clips or cuts through
                height: Math.min(280, Math.max(140, subBorderInset.height - y - 12))

                // Hard shadow
                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.topMargin: 4
                    color: rootWindow.shadowColor
                    radius: 4
                }

                // Dropdown Card
                Rectangle {
                    anchors.fill: parent
                    color: rootWindow.bgColor
                    border.color: rootWindow.borderColor
                    border.width: 1
                    radius: 4
                    clip: true

                    ColumnLayout {
                        id: fileDropdownContent
                        anchors.fill: parent
                        spacing: 0

                        // Title Strip
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            height: 22
                            color: rootWindow.dimColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                Text {
                                    text: "SELECT TARGET FILE (" + fileDropdown.filteredFiles.length + ")"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: "#FFB000"
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text {
                                    text: "SINGLE-FILE SCOPE"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 7
                                    color: rootWindow.fgColor
                                    opacity: 0.7
                                }
                            }
                        }

                        // Search Filter Input Strip
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            height: 26
                            color: rootWindow.inputBgColor
                            border.color: fileSearchInput.activeFocus ? "#FFB000" : rootWindow.borderColor
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: 4

                                Text {
                                    text: "FILTER:"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.mutedTextColor
                                }

                                TextInput {
                                    id: fileSearchInput
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                    selectByMouse: true
                                    text: fileDropdown.filterText
                                    onTextChanged: fileDropdown.filterText = text
                                    Keys.onEscapePressed: fileDropdown.visible = false
                                }

                                Text {
                                    visible: fileSearchInput.text.length > 0
                                    text: "×"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 9
                                    color: rootWindow.mutedTextColor
                                    HudMouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: fileSearchInput.text = ""
                                    }
                                }
                            }
                        }

                        // Scrollable File List (Smooth mouse & wheel scroll)
                        ListView {
                            id: fileListView
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.fillHeight: true
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: fileDropdown.filteredFiles

                            ScrollBar.vertical: HudScrollBar {
                                id: fileScrollBar
                                active: true
                                policy: ScrollBar.AsNeeded
                                width: 8
                                contentItem: Rectangle {
                                    implicitWidth: 6
                                    radius: 3
                                    color: fileScrollBar.pressed ? "#FFB000" : (fileScrollBar.hovered ? rootWindow.fgColor : rootWindow.borderColor)
                                }
                            }

                            delegate: Rectangle {
                                width: fileListView.width
                                height: 32
                                readonly property string fileName: {
                                    var parts = modelData.split("/");
                                    return parts[parts.length - 1];
                                }
                                readonly property bool isSelected: rootWindow.targetLocalFile === fileName || rootWindow.targetLocalFile === modelData
                                readonly property bool isRecommended: (hostController.recommendedOption.length > 0) &&
                                    (hostController.recommendedOption === fileName || hostController.recommendedOption === modelData) &&
                                    (hostController.recommendedConfidence >= 0.70)
                                color: fItemMouse.containsMouse ? rootWindow.dimColor : (isSelected ? rootWindow.inputBgColor : (isRecommended ? "#261400" : "transparent"))

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 12
                                    spacing: 8

                                    Text {
                                        text: isSelected ? "●" : (isRecommended ? "★" : "○")
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: isSelected ? "#FFB000" : (isRecommended ? "#F59E0B" : rootWindow.dimColor)
                                    }

                                    ColumnLayout {
                                        spacing: 1
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        RowLayout {
                                            spacing: 6
                                            Layout.fillWidth: true
                                            Text {
                                                text: fileName
                                                font.family: rootWindow.monoFont
                                                font.bold: isSelected || isRecommended
                                                font.pixelSize: 9
                                                color: isSelected ? "#FFB000" : (isRecommended ? "#F59E0B" : rootWindow.fgColor)
                                                elide: Text.ElideRight
                                            }
                                            Rectangle {
                                                visible: isRecommended
                                                implicitWidth: recPillText.implicitWidth + 8
                                                implicitHeight: 14
                                                radius: 2
                                                color: "#261400"
                                                border.color: "#F59E0B"
                                                Text {
                                                    id: recPillText
                                                    anchors.centerIn: parent
                                                    text: "[ RECOMMENDED " + Math.round(hostController.recommendedConfidence * 100) + "% ]"
                                                    font.family: rootWindow.monoFont
                                                    font.pixelSize: 7
                                                    font.bold: true
                                                    color: "#F59E0B"
                                                }
                                            }
                                        }
                                        Text {
                                            text: modelData
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 7
                                            color: rootWindow.mutedTextColor
                                            elide: Text.ElideMiddle
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                        }
                                    }
                                }

                                HudMouseArea {
                                    id: fItemMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        rootWindow.targetLocalFile = modelData;
                                        rootWindow.showToast("TARGET FILE: " + fileName);
                                        fileDropdown.visible = false;
                                    }
                                }
                            }
                        }

                        // No results message
                        Rectangle {
                            visible: fileDropdown.filteredFiles.length === 0
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.fillHeight: true
                            color: "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: "No files match '" + fileDropdown.filterText + "'"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                color: rootWindow.mutedTextColor
                            }
                        }
                    }
                }
            }
        }
    }
}
