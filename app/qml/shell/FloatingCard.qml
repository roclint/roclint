import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

Window {
    id: rootWindow
    title: qsTr("Roclint — prompt assistant")
    property bool isDetailedExpanded: false
    readonly property bool isAmbientMode: hc && (hostController.isAmbientAttached || hostController.isAmbientActive)

    readonly property bool compactPresentation: isAmbientMode && !isDetailedExpanded
    readonly property bool compactQuestion: compactPresentation && hc &&
        (!hostController.hasProposal || hostController.comprehensionReviewState === "waiting_for_answer") &&
        (hostController.isClarifying || hostController.isInterviewActive || needsManualAnswer)
    readonly property bool compactDraftActions: compactPresentation && hc && !compactQuestion &&
        !hostController.hasProposal && !generationPending
    readonly property int compactWidth: 430
    // The reading surface grows with the bounded draft preview. Questions and
    // proposals keep a larger viewport for choices, diffs and review controls.
    readonly property int compactHeight: (compactDraftActions && !showPromptEditor)
        ? Math.min(380, Math.max(174, Math.ceil(mainContent.implicitHeight + hudHeader.implicitHeight + hudFooter.implicitHeight
            + 2 * hudLayout.anchors.margins + 3 * hudLayout.spacing + 4)))
        : 380

    width: !isAmbientMode || isDetailedExpanded ? 820 : compactWidth
    height: !isAmbientMode ? 600 : (isDetailedExpanded ? 520 : compactHeight)
    minimumWidth: !isAmbientMode ? 520 : (isDetailedExpanded ? 520 : 420)
    minimumHeight: !isAmbientMode ? 360 : (isDetailedExpanded ? 360 : 174)
    visible: false
    flags: (hc && hostController.isAmbientAttached ? Qt.Tool : Qt.Window) | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    function toggleDetailedExpanded() {
        isDetailedExpanded = !isDetailedExpanded;
        if (isAmbientMode) {
            const targetW = isDetailedExpanded ? 820 : compactWidth;
            const targetH = isDetailedExpanded ? 520 : compactHeight;
            width = Qt.binding(function() { return !isAmbientMode || isDetailedExpanded ? 820 : compactWidth; });
            height = Qt.binding(function() { return !isAmbientMode ? 600 : (isDetailedExpanded ? 520 : compactHeight); });
            const pt = hostController.calculateAmbientAnchor(targetW, targetH);
            x = pt.x;
            y = pt.y;
        }
    }

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Segoe UI"
    readonly property string serifFont: isMac ? "Times New Roman" : "Georgia"

    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    readonly property bool generationPending: hc && (hostController.isAnalyzing || hostController.proposalStatus === "GENERATING")
    property var projectList: hc ? hostController.recentProjects() : []
    property int currentProjectIndex: 0
    readonly property var activeProject: projectList.length > currentProjectIndex ? projectList[currentProjectIndex] : ({name: qsTr("Choose workspace"), path: ""})
    property string activeScope: "global"
    property string targetLocalFile: ""
    property var projectFiles: []
    property string scopePreamble: ""
    property bool suppressEditSignal: false
    property bool editingDraft: false
    property double pendingEditRequest: 0
    readonly property bool showPromptEditor: !rootWindow.isCompiledPreview && (
        !rootWindow.hc ||
        rootWindow.editingDraft ||
        (!hostController.isAmbientAttached && !hostController.isAmbientCapture && !hostController.isClarifying && !hostController.hasProposal)
    )
    readonly property bool isCompiledPreview: hc && hostController.hasProposal
    readonly property bool hasAmbiguity: hc && hostController.isClarifying
    readonly property bool needsManualAnswer: hc && hostController.isManual && hostController.interventionReason === "CLARIFICATION_BUDGET_EXCEEDED"
    readonly property real ambiguityScore: hc ? hostController.ambiguityScore : 0.0
    // Host/UI v1: a substantial proposal is applicable only after a current,
    // completed independent reread. When the contract is absent this stays true
    // so the pre-contract approval path is unchanged.
    readonly property bool reviewContractAvailable: hc && typeof hostController.canApplyReviewedOutput !== "undefined"
    readonly property bool reviewedOutputApplicable: !reviewContractAvailable || hostController.canApplyReviewedOutput

    readonly property var presentation: {
        const neutral = rootWindow.fgColor;
        const warning = "#FFB000";
        if (!hc) return {badge: "DRAFT", tone: neutral, label: qsTr("Review prompt"), action: "REVIEW_PROMPT"};
        if (hostController.isAnalyzing) return {badge: "CHECKING", tone: neutral, label: qsTr("Cancel analysis"), action: "CANCEL"};
        if (hostController.proposalStatus === "GENERATING") return {badge: "WRITING", tone: neutral, label: qsTr("Writing…"), action: "NONE"};
        if (hostController.hasProposal && hostController.proposalStatus === "READY_FOR_REVIEW") {
            if (reviewContractAvailable && hostController.comprehensionReviewState === "running")
                return {badge: "REVIEW", tone: neutral, label: qsTr("Checking rewrite…"), action: "NONE"};
            if (!hostController.canApprove || !rootWindow.reviewedOutputApplicable)
                return {badge: "REVIEW", tone: neutral,
                    label: hostController.pendingReviewCount > 0 ? qsTr("Confirm changes") : qsTr("Resolve checks"),
                    action: "REVIEW_REWRITE"};
            const connected = hostController.isAmbientCapture || hostController.isAmbientAttached;
            return {badge: "REVIEW", tone: neutral,
                label: connected ? qsTr("Approve & Insert") : qsTr("Approve & Copy"),
                action: "APPROVE"};
        }
        if (hostController.proposalStatus === "FAILED" || hostController.proposalStatus === "VALIDATION_FAILED")
            return {badge: "NO REWRITE", tone: rootWindow.mutedTextColor,
                label: hostController.isRewriteEnabled ? qsTr("Retry rewrite") : qsTr("Review prompt"),
                action: hostController.isRewriteEnabled ? "REWRITE" : "REVIEW_PROMPT"};
        if (hostController.proposalStatus === "NEEDS_CLARIFICATION")
            return {badge: "NEEDS DETAIL", tone: warning, label: qsTr("Edit prompt"), action: "EDIT"};
        if (hostController.isDraftDirty) return {badge: "DRAFT", tone: neutral, label: qsTr("Review prompt"), action: "REVIEW_PROMPT"};
        if (hostController.reviewOutcome === "UNAVAILABLE" || hostController.analysisStatus === "FAILED")
            return {badge: "NO SUGGESTION", tone: rootWindow.mutedTextColor, label: qsTr("Review prompt"), action: "REVIEW_PROMPT"};
        if (hostController.isOptionalHelp && hostController.isClarifying)
            return {badge: "OPTIONAL", tone: neutral, label: qsTr("Optional ideas"), action: "CLARIFY"};
        if (hostController.isClarifying || rootWindow.needsManualAnswer)
            return {badge: "CLARIFY", tone: warning, label: qsTr("Answer question"), action: "CLARIFY"};
        if (hostController.isManual) return {badge: "REVIEW NEEDED", tone: warning, label: qsTr("Review prompt"), action: "REVIEW_PROMPT"};
        if (hostController.hasConfirmedIntent && !hostController.isIntentStale && !hostController.isOriginalRoute && !hostController.isOptionalHelp)
            return {badge: "READY", tone: "#7EE787", label: qsTr("Improve wording"), action: "REWRITE"};
        return {badge: hostController.analysisStatus === "READY" ? "READY" : "DRAFT", tone: neutral,
            label: qsTr("Review prompt"), action: "REVIEW_PROMPT"};
    }
    function openReview() {
        if (!visible || visibility === Window.Minimized) return;
        if (isAmbientMode && !isDetailedExpanded) toggleDetailedExpanded();
        Qt.callLater(function() {
            if (rootWindow.visible && rootWindow.visibility !== Window.Minimized && hc && hostController.hasProposal)
                reviewFlow.focusReview();
        });
    }
    function revealReviewItem(item) {
        if (!visible || visibility === Window.Minimized || !item) return;
        const view = contentScroll;
        if (!view || typeof view.contentY !== "number") return;
        const y = item.mapToItem(mainContent, 0, 0).y;
        const maximum = Math.max(0, view.contentHeight - view.height);
        if (item.height > view.height - 16 || y < view.contentY + 8) view.contentY = Math.min(maximum, Math.max(0, y - 8));
        else if (y + item.height > view.contentY + view.height - 8)
            view.contentY = Math.min(maximum, Math.max(0, y + item.height - view.height + 8));
    }

    function refreshProjectFiles() {
        projectFiles = hc && activeProject.path ? hostController.getProjectFiles(activeProject.path) : [];
        if (projectFiles.indexOf(targetLocalFile) < 0) targetLocalFile = "";
    }
    onCurrentProjectIndexChanged: refreshProjectFiles()

    onActiveFocusItemChanged: focusRevealTimer.restart()
    onWidthChanged: focusRevealTimer.restart()
    onHeightChanged: {
        focusRevealTimer.restart();
        if (visible && compactPresentation && hc) {
            const point = hostController.calculateAmbientAnchor(width, height);
            x = point.x; y = point.y;
        }
    }
    Timer {
        id: focusRevealTimer
        interval: 50
        onTriggered: rootWindow.revealFocusedControl()
    }
    function revealFocusedControl() {
        if (!visible || visibility === Window.Minimized) return;
        let item = rootWindow.activeFocusItem;
        const view = contentScroll;
        if (!item || !view || typeof view.contentY !== "number") return;
        let ancestor = item;
        while (ancestor && ancestor !== mainContent) {
            // ListView delegates can own keyboard focus; reveal the bounded
            // diff viewport rather than scrolling only its first text line.
            if (ancestor.objectName === "diffView") item = ancestor;
            if (ancestor.objectName.startsWith("reviewRow_")) item = ancestor;
            ancestor = ancestor.parent;
        }
        if (!ancestor) return;
        revealReviewItem(item);
    }

    property string currentTheme: "dark"
    RocTheme { id: rocPalette; mode: rootWindow.currentTheme }
    FontLoader { id: interFont; source: "assets/InterVariable.ttf" }
    Settings { id: appearance; category: "appearance"; property bool reducedMotion: false }
    property alias reducedMotion: appearance.reducedMotion
    component HudButton: RocButton { currentTheme: rootWindow.currentTheme; reduceMotion: rootWindow.reducedMotion }
    property bool showWorkspaceOptions: false
    property bool inspectPanelExplicitOpen: false
    property bool rewriteAfterReview: false
    property string rewriteDraft: ""
    readonly property color bgColor: rocPalette.background
    readonly property color fgColor: rocPalette.foreground
    readonly property color borderColor: rocPalette.border
    readonly property color dimColor: rocPalette.hover
    readonly property color mutedTextColor: rocPalette.muted
    readonly property color commentColor: rocPalette.muted
    readonly property color inputBgColor: rocPalette.inset
    readonly property color shadowColor: "#030918"

    function requestFullRewrite() {
        if (!hc || !promptInput.text.trim().length || hostController.storageBusy || !hostController.isStorageAvailable) return;
        if (hostController.isAnalyzing && hostController.currentRevisionText === promptInput.text) {
            rewriteAfterReview = true;
            rewriteDraft = promptInput.text;
            return; // Reuse the running check; do not cancel or duplicate it.
        }
        if (!hostController.isDraftDirty && hostController.analysisStatus === "READY" &&
            hostController.currentRevisionText === promptInput.text) {
            hostController.rewriteWithoutQuestions(hostController.stateVersion);
        } else {
            rewriteAfterReview = true;
            rewriteDraft = promptInput.text;
            analyzeNow();
        }
    }
    function continueRequestedRewrite() {
        if (!rewriteAfterReview || !hc || hostController.isAnalyzing || hostController.storageBusy) return;
        if (promptInput.text !== rewriteDraft) { rewriteAfterReview = false; return; }
        if (hostController.analysisStatus === "FAILED") { rewriteAfterReview = false; return; }
        if (hostController.analysisStatus !== "READY" || hostController.isDraftDirty) return;
        // Storage/model-startup notifications can precede the draft commit. Only
        // compare the committed revision after this analysis is ready.
        if (hostController.currentRevisionText !== rewriteDraft) return;
        rewriteAfterReview = false;
        hostController.rewriteWithoutQuestions(hostController.stateVersion);
    }

    property bool isClarificationActive: false
    property string toastMessage: ""
    property bool pinToTop: true
    property int autoAnalysisDelayMs: 800
    property string autoAnalysisText: ""
    property bool autoAnalysisDue: false
    // Focus and draft changes do not dismiss the HUD. Presentation belongs to
    // the user, independently of the captured prompt and its analysis lifetime.

    Connections {
        target: rootWindow.hc ? hostController : null
        function onCompanionRewriteRequested(draft, expectedVersion) {
            if (hostController.stateVersion !== expectedVersion || hostController.currentRevisionText !== draft) return;
            rootWindow.setEditorText(draft);
            rootWindow.rewriteDraft = draft;
            rootWindow.rewriteAfterReview = true;
            Qt.callLater(rootWindow.continueRequestedRewrite);
        }
        function onStorageCommandFinished(requestId, name, succeeded, errorCode, message) {
            if (!succeeded && (name === "rewriteWithoutQuestions" || name === "requestComprehensionReview"))
                rootWindow.showToast(message);
            if (!rootWindow.pendingEditRequest || requestId !== rootWindow.pendingEditRequest) return;
            rootWindow.pendingEditRequest = 0;
            if (!hostController.hasProposal && rootWindow.visible && rootWindow.visibility !== Window.Minimized) {
                if (!promptInput.text.length && !hostController.isDraftDirty) rootWindow.setEditorText(hostController.currentRevisionText);
                Qt.callLater(function() {
                    if (rootWindow.visible && rootWindow.visibility !== Window.Minimized && !hostController.hasProposal)
                        promptInput.forceActiveFocus();
                });
            }
        }
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
            rootWindow.editingDraft = false;
            rootWindow.setEditorText(text);
            if (!hostController.isDraftDirty) rootWindow.editingDraft = false;
            rootWindow.scopePreamble = "";
            rootWindow.queueAutoAnalysis();
        }
        function onCaptureSourceChanged() {
            if (hostController.isAmbientAttached || hostController.isAmbientCapture) {
                rootWindow.editingDraft = false;
            }
        }
        function onPromptInjected(text, appName) {
            rootWindow.cancelQueuedAnalysis();
            rootWindow.showToast(qsTr("Inserted into %1").arg(appName));
            if (!rootWindow.isDetailedExpanded) {
                rootWindow.dismissCard();
            }
        }
        function onPromptCopied(text) {
            rootWindow.cancelQueuedAnalysis();
            rootWindow.showToast(qsTr("Copied to clipboard. Press %1 to paste.").arg(rootWindow.isMac ? "Cmd+V" : "Ctrl+V"));
        }
        function onDeliveryFellBack(message) {
            rootWindow.cancelQueuedAnalysis();
            rootWindow.showToast(message);
        }
        function onClarificationDeferred(message, autoSelected) { rootWindow.showToast(message); }
        function onStorageBusyChanged() { rootWindow.tryAutoAnalysis(); Qt.callLater(rootWindow.continueRequestedRewrite); }
        function onStorageChanged() {
            if (hostController.storageNeedsRecovery) rootWindow.cancelQueuedAnalysis();
            else rootWindow.tryAutoAnalysis();
        }
        function onScorerEngineChanged() { rootWindow.tryAutoAnalysis(); }
        function onAnalysisStatusChanged() { rootWindow.tryAutoAnalysis(); Qt.callLater(rootWindow.continueRequestedRewrite); }
        function onInterventionChanged() {
            if (hostController.isClarifying && !hostController.isDraftDirty) rootWindow.editingDraft = false;
        }
        function onHostStateChanged() {
            if (hostController.hostStateString === "QUITTING") rootWindow.cancelQueuedAnalysis();
        }
    }

    Timer {
        id: autoAnalysisTimer
        interval: rootWindow.autoAnalysisDelayMs
        repeat: false
        onTriggered: { rootWindow.autoAnalysisDue = true; rootWindow.tryAutoAnalysis(); }
    }
    function cancelQueuedAnalysis() {
        autoAnalysisTimer.stop();
        autoAnalysisText = "";
        autoAnalysisDue = false;
    }
    function queueAutoAnalysis() {
        cancelQueuedAnalysis();
        if (!hc || !visible || visibility === Window.Minimized || !promptInput.text.trim().length || hostController.storageNeedsRecovery) return;
        autoAnalysisText = promptInput.text;
        autoAnalysisTimer.restart();
    }
    function tryAutoAnalysis() {
        if (!autoAnalysisDue || !autoAnalysisText.length || !hc) return;
        if (!visible || visibility === Window.Minimized || promptInput.text !== autoAnalysisText || hostController.storageNeedsRecovery || hostController.hostStateString === "QUITTING") {
            cancelQueuedAnalysis(); return;
        }
        // Manual analysis, cancellation or failure consumes this draft's check.
        // Model/state notifications must never retry an unchanged draft.
        if (!hostController.isDraftDirty && hostController.currentRevisionText === autoAnalysisText &&
                (hostController.isAnalyzing || hostController.analysisStatus === "READY" || hostController.analysisStatus === "FAILED")) {
            cancelQueuedAnalysis(); return;
        }
        if (!hostController.isStorageAvailable || hostController.storageBusy || hostController.juliaState === "CANCELLING") return;
        if (hostController.isAnalyzing || hostController.juliaState === "STOPPING") { cancelQueuedAnalysis(); return; }
        const text = autoAnalysisText;
        cancelQueuedAnalysis(); // Consume before admission: synchronous signals cannot enqueue a duplicate.
        hostController.analyzePrompt(text, hostController.stateVersion);
    }
    function setEditorText(text) {
        cancelQueuedAnalysis();
        suppressEditSignal = true;
        promptInput.text = text;
        suppressEditSignal = false;
    }
    function openSettings() {
        if (!hc) return;
        hostController.minimizeCard();
        rootWindow.hide();
        hostController.showDashboard();
    }
    function dismissCard() {
        rewriteAfterReview = false;
        cancelQueuedAnalysis();
        toastMessage = "";
        if (hc) hostController.hideCard();
        // The native window can still be visible after the host has recorded it as hidden.
        rootWindow.hide();
    }
    function executeHardBypass() {
        cancelQueuedAnalysis();
        if (!hc || !hostController.isStorageAvailable) return;
        const text = promptInput.text.trim().length ? promptInput.text : hostController.currentRevisionText;
        if (text.trim().length) {
            hostController.hardBypass(text, hostController.stateVersion);
        } else {
            dismissCard();
        }
    }
    function sendAsTyped() { executeHardBypass(); }
    function revertToDraft() {
        if (!hc) return;
        editingDraft = true;
        pendingEditRequest = hostController.discardProposal(hostController.stateVersion);
    }
    function retryHistory() {
        if (hc) hostController.retryStorage(promptInput.text, hostController.stateVersion);
    }

    function analyzeNow() {
        cancelQueuedAnalysis();
        if (hc && hostController.isStorageAvailable && promptInput.text.trim().length) hostController.reviewPrompt(promptInput.text, hostController.stateVersion);
    }
    function approveForCurrentDestination() {
        if (!hc || hostController.storageBusy || !hostController.isStorageAvailable) return;
        hostController.approveAndCopyRewrite(hostController.stateVersion);
    }
    function handleEnter() {
        if (!hc) return;
        switch (presentation.action) {
        case "CANCEL": rewriteAfterReview = false; cancelQueuedAnalysis(); hostController.cancelAnalysis(hostController.stateVersion); break;
        case "REVIEW_REWRITE":
            if (isAmbientMode && !isDetailedExpanded) toggleDetailedExpanded();
            Qt.callLater(function() {
                if (rootWindow.visible && rootWindow.visibility !== Window.Minimized && hostController.hasProposal)
                    reviewFlow.advanceReview();
            });
            break;
        case "APPROVE": approveForCurrentDestination(); break;
        case "EDIT": revertToDraft(); break;
        case "CLARIFY": reviewFlow.focusClarification(); break;
        case "REWRITE": hostController.synthesizeRewrite(hostController.stateVersion); break;
        case "REVIEW_PROMPT": analyzeNow(); break;
        }
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
        showWorkspaceOptions = true;
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
            contentScroll.contentY = 0;
            if (isAmbientMode) {
                if (!editingDraft && !hostController.isDraftDirty && hostController.currentRevisionText.length) {
                    rootWindow.setEditorText(hostController.currentRevisionText);
                }
            } else {
                promptInput.forceActiveFocus();
            }
        } else cancelQueuedAnalysis();
    }
    onVisibilityChanged: function(visibility) { if (visibility === Window.Minimized) cancelQueuedAnalysis(); }

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
        onActivated: rootWindow.openSettings()
    }

    Rectangle {
        id: posterBoard
        anchors.fill: parent
        color: rootWindow.bgColor
        radius: rootWindow.compactPresentation ? 15 : 18
        gradient: rootWindow.compactPresentation ? companionWash : null
        Gradient {
            id: companionWash
            GradientStop { position: 0; color: rocPalette.light ? "#F1F5FF" : "#172746" }
            GradientStop { position: 0.62; color: rootWindow.bgColor }
        }
        border.color: rootWindow.currentTheme === "light" ? rootWindow.borderColor : "#6279AA"
        border.width: 1

        MouseArea {
            anchors.right: parent.right; anchors.bottom: parent.bottom
            width: 12; height: 12; z: 100
            cursorShape: Qt.SizeFDiagCursor
            onPressed: rootWindow.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        }
        MouseArea {
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.margins: 14; width: 5; z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: rootWindow.startSystemResize(Qt.RightEdge)
        }
        MouseArea {
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.margins: 14; height: 5; z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: rootWindow.startSystemResize(Qt.BottomEdge)
        }

        Item {
            id: subBorderInset
            anchors.fill: parent
            anchors.margins: 1
            clip: true

            ColumnLayout {
                id: hudLayout
                anchors.fill: parent
                anchors.margins: rootWindow.compactPresentation ? 14 : rootWindow.height <= 260 ? 10 : 20
                spacing: rootWindow.compactPresentation ? 9 : rootWindow.height <= 260 ? 5 : 12

                RowLayout {
                    id: hudHeader
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: rootWindow.compactPresentation || rootWindow.height <= 260 ? 32 : 54
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: rootWindow.compactPresentation ? 32 : rootWindow.height <= 260 ? 28 : 48
                        Layout.preferredHeight: rootWindow.compactPresentation ? 32 : rootWindow.height <= 260 ? 28 : 42
                        radius: 9
                        color: rootWindow.compactPresentation ? "#263D79" : "transparent"
                        border.width: rootWindow.compactPresentation ? 1 : 0
                        border.color: "#4965A2"
                        Image {
                            anchors.fill: parent
                            anchors.margins: rootWindow.compactPresentation ? 4 : 0
                            source: "assets/roc-mark.png"
                            sourceSize.width: 144; sourceSize.height: 128
                            fillMode: Image.PreserveAspectFit
                            smooth: false
                            Accessible.ignored: true
                        }
                    }
                    Text {
                        text: "roclint"
                        font.family: interFont.status === FontLoader.Ready ? interFont.name : rootWindow.sansFont
                        font.pixelSize: rootWindow.compactPresentation ? 17 : rootWindow.height <= 260 ? 19 : 26
                        font.weight: Font.DemiBold
                        font.letterSpacing: -0.7
                        color: rootWindow.fgColor
                        MouseArea { anchors.fill: parent; onPressed: rootWindow.startSystemMove() }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredHeight: rootWindow.compactPresentation || rootWindow.height <= 260 ? 30 : 52
                        GenerationMeter {
                            visible: !rootWindow.compactPresentation
                            anchors.right: parent.right
                            width: Math.min(250, parent.width)
                            height: rootWindow.height <= 260 ? 30 : 52
                            tokensPerSecond: rootWindow.hc ? hostController.tokensPerSecond : 0
                            generationTokensPerSecond: rootWindow.hc ? hostController.generationTokensPerSecond : 0
                            decodeTokensPerSecond: rootWindow.hc ? hostController.decodeTokensPerSecond : 0
                            tokenSpeedMode: rootWindow.hc ? hostController.tokenSpeedMode : "BOTH"
                            tokenCount: rootWindow.hc ? hostController.generationTokens : 0
                            reasoningTokenCount: rootWindow.hc ? hostController.reasoningTokens : 0
                            answerTokenCount: rootWindow.hc ? hostController.answerTokens : 0
                            generationPhase: rootWindow.hc ? hostController.generationPhase : "idle"
                            generationElapsedMs: rootWindow.hc ? hostController.generationElapsedMs : 0
                            decodeElapsedMs: rootWindow.hc ? hostController.decodeElapsedMs : 0
                            processing: rootWindow.visible && rootWindow.hc && (rootWindow.generationPending || hostController.isInferring)
                            preparing: rootWindow.generationPending && !hostController.isInferring
                            metricsAvailable: rootWindow.hc && hostController.nativeGraniteSelected
                            currentTheme: rootWindow.currentTheme
                            reducedMotion: rootWindow.reducedMotion
                        }
                        MouseArea { anchors.fill: parent; onPressed: rootWindow.startSystemMove() }
                    }
                    RowLayout {
                        objectName: "hudWindowControls"
                        spacing: 1
                        HudButton {
                            id: expandToggleButton
                            objectName: "expandToggleButton"
                            compact: true; quiet: true
                            iconName: "arrows-out-simple"
                            tip: rootWindow.isDetailedExpanded ? qsTr("Compact view") : qsTr("Expand review")
                            onClicked: rootWindow.toggleDetailedExpanded()
                        }
                        HudButton {
                            objectName: "pinButton"
                            visible: !rootWindow.compactPresentation
                            compact: true; quiet: !rootWindow.pinToTop
                            iconName: "push-pin"
                            tip: rootWindow.pinToTop ? qsTr("Unpin window") : qsTr("Keep on top")
                            onClicked: {
                                rootWindow.pinToTop = !rootWindow.pinToTop;
                                rootWindow.setFlag(Qt.WindowStaysOnTopHint, rootWindow.pinToTop);
                                rootWindow.show();
                            }
                        }
                        HudButton {
                            objectName: "openSettingsControl"
                            visible: !rootWindow.compactPresentation
                            compact: true; quiet: true; iconName: "gear-six"; tip: qsTr("Settings")
                            onClicked: rootWindow.openSettings()
                        }
                        HudButton {
                            objectName: "minButton"
                            compact: true; quiet: true; iconName: "minus"; tip: qsTr("Minimize")
                            onClicked: {
                                if (rootWindow.isMac) {
                                    if (rootWindow.hc) hostController.hideCard(); else rootWindow.hide();
                                } else if (Qt.platform.os === "windows") {
                                    hostController.minimizeCard(); rootWindow.hide();
                                } else {
                                    rootWindow.showMinimized();
                                }
                            }
                        }
                        HudButton {
                            objectName: "closeButton"
                            compact: true; quiet: true; iconName: "x"; tip: qsTr("Close · Esc")
                            onClicked: rootWindow.dismissCard()
                        }
                    }
                }

                RowLayout {
                    objectName: "hudStatusRow"
                    visible: !rootWindow.compactPresentation
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 8
                    HudButton {
                        id: projectSelectorButton
                        objectName: "projectSelectorButton"
                        label: rootWindow.activeProject.name
                        iconName: "caret-down"; compact: true; quiet: true
                        Layout.preferredWidth: Math.min(rootWindow.width < 500 ? 120 : 160, implicitWidth)
                        Layout.maximumWidth: rootWindow.width < 500 ? 120 : 160
                        Layout.minimumWidth: 40
                        clip: true
                        tip: qsTr("Choose workspace and scope")
                        onClicked: {
                            const pt = mapToItem(subBorderInset, 0, height + 4);
                            projectDropdown.x = Math.min(pt.x, subBorderInset.width - projectDropdown.width - 8);
                            projectDropdown.y = pt.y;
                            projectDropdown.visible = !projectDropdown.visible;
                        }
                    }
                    Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
                    Rectangle {
                        objectName: "engineBadge"
                        Layout.preferredWidth: Math.min(rootWindow.width < 500 ? 100 : 160, runtimeLabel.implicitWidth + 16)
                        Layout.maximumWidth: rootWindow.width < 500 ? 100 : 160
                        Layout.minimumWidth: 40
                        implicitHeight: 23
                        radius: 6; color: rocPalette.surface
                        Text {
                            id: runtimeLabel; objectName: "modelRuntimeLabel"
                            anchors.fill: parent; anchors.margins: 6
                            text: {
                                if (!rootWindow.hc) return qsTr("Model unavailable");
                                if (hostController.isInferring) return qsTr("Writing locally");
                                if (hostController.modelRuntimeState === "LOADING") return qsTr("Loading model");
                                if (hostController.modelRuntimeState === "FAILED") return qsTr("Model error");
                                if (hostController.useLiveWeights) return qsTr("Local model");
                                return hostController.isNativeGraniteInstalled ? qsTr("Ready · standby") : qsTr("Model not loaded");
                            }
                            font.family: rootWindow.monoFont; font.pixelSize: 10
                            color: rootWindow.mutedTextColor
                            verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                        }
                    }
                    Rectangle {
                        objectName: "hudStateBadge"
                        implicitWidth: stateLabel.implicitWidth + 16
                        Layout.preferredWidth: Math.min(rootWindow.width < 500 ? 100 : 120, implicitWidth)
                        Layout.maximumWidth: rootWindow.width < 500 ? 100 : 120
                        Layout.minimumWidth: 40
                        implicitHeight: 23; radius: 6
                        color: rocPalette.surface
                        Text {
                            id: stateLabel; objectName: "hudStateLabel"
                            anchors.fill: parent; anchors.margins: 6
                            text: rootWindow.presentation.badge
                            font.family: rootWindow.monoFont; font.pixelSize: 10
                            color: rootWindow.presentation.tone
                            verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                        }
                    }
                    Rectangle {
                        id: hudModeBadge
                        objectName: "hudModeBadge"
                        visible: rootWindow.hc && hostController.assistanceMode !== "auto"
                        implicitWidth: modeLabel.implicitWidth + 14
                        implicitHeight: 23; radius: 6
                        color: rocPalette.surface
                        border.color: rocPalette.focus
                        border.width: 1
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Assistance mode %1").arg(rootWindow.hc ? hostController.assistanceMode : "")
                        Text {
                            id: modeLabel; objectName: "hudModeLabel"
                            anchors.fill: parent; anchors.margins: 4
                            text: "/" + (rootWindow.hc ? hostController.assistanceMode.toUpperCase() : "")
                            font.family: rootWindow.monoFont; font.pixelSize: 10; font.bold: true
                            color: rocPalette.focus
                            verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter
                        }
                        MouseArea {
                            id: modeBadgeMouse
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onClicked: {
                                if (rootWindow.hc) {
                                    if (hostController.isInspectMode) {
                                        rootWindow.inspectPanelExplicitOpen = !rootWindow.inspectPanelExplicitOpen;
                                    } else {
                                        hostController.setAssistanceMode("inspect");
                                        rootWindow.inspectPanelExplicitOpen = true;
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle { visible: !rootWindow.compactPresentation; Layout.fillWidth: true; implicitHeight: 1; color: rootWindow.borderColor }

                Flickable {
                    id: contentScroll
                    objectName: "workflowViewport"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 0
                    Layout.minimumWidth: 0
                    clip: true
                    property real availableWidth: width - (rootWindow.compactDraftActions ? 0 : 10)
                    contentWidth: availableWidth
                    contentHeight: mainContent.implicitHeight
                    onContentHeightChanged: focusRevealTimer.restart()
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: HudScrollBar {
                        policy: ScrollBar.AsNeeded
                        background: Item {}
                    }

                    ColumnLayout {
                        id: mainContent
                        width: contentScroll.availableWidth
                        spacing: 14

                        RowLayout {
                            Layout.fillWidth: true
                            objectName: "promptEditorHeading"
                            visible: rootWindow.showPromptEditor && !rootWindow.hasAmbiguity
                            Text {
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                text: rootWindow.hc && hostController.isAnalyzing ? qsTr("Finding the right words…") : qsTr("What are you working on?")
                                font.family: rootWindow.sansFont; font.pixelSize: 23; font.weight: Font.DemiBold
                                color: rootWindow.fgColor; wrapMode: Text.WordWrap
                            }
                        }
                        RowLayout {
                            id: commandPaletteRow
                            visible: rootWindow.showPromptEditor && promptInput.text.startsWith("/") && !promptInput.text.includes(" ")
                            Layout.fillWidth: true
                            spacing: 6

                            Repeater {
                                model: [
                                    { cmd: "/auto", desc: "Auto" },
                                    { cmd: "/tech", desc: "Tech" },
                                    { cmd: "/create", desc: "Create" },
                                    { cmd: "/strict", desc: "Strict" },
                                    { cmd: "/inspect", desc: "Inspect" },
                                    { cmd: "/quiet", desc: "Quiet" }
                                ]
                                delegate: Rectangle {
                                    readonly property bool matches: modelData.cmd.startsWith(promptInput.text.toLowerCase())
                                    visible: matches
                                    implicitWidth: cmdText.implicitWidth + 14
                                    implicitHeight: 22
                                    radius: 6
                                    color: pillArea.containsMouse ? rocPalette.surface : rocPalette.inset
                                    border.color: promptInput.text.toLowerCase() === modelData.cmd ? rocPalette.focus : rootWindow.borderColor
                                    border.width: 1

                                    Text {
                                        id: cmdText
                                        anchors.centerIn: parent
                                        text: modelData.cmd
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 11
                                        font.bold: true
                                        color: rootWindow.fgColor
                                    }
                                    MouseArea {
                                        id: pillArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            promptInput.text = modelData.cmd + " ";
                                            promptInput.cursorPosition = promptInput.text.length;
                                            promptInput.forceActiveFocus();
                                            if (rootWindow.hc) {
                                                hostController.setAssistanceMode(modelData.cmd.substring(1));
                                                if (modelData.cmd === "/inspect") {
                                                    rootWindow.inspectPanelExplicitOpen = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        Rectangle {
                            visible: rootWindow.showPromptEditor
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: Math.min(240, Math.max(125, promptInput.contentHeight + 30))
                            color: rocPalette.inset; radius: 11
                            border.color: promptInput.activeFocus ? rocPalette.focus : rootWindow.borderColor
                            ScrollView {
                                anchors.fill: parent; anchors.margins: 12
                                clip: true
                                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                ScrollBar.vertical: HudScrollBar {}
                                TextArea {
                                    id: promptInput
                                    objectName: "promptInput"
                                    Accessible.name: qsTr("Prompt draft")
                                    background: null
                                    activeFocusOnTab: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    font.family: rootWindow.sansFont; font.pixelSize: 15
                                    color: rootWindow.fgColor
                                    placeholderText: qsTr("Describe the change you want to make.\nKeep the details that matter to you.")
                                    placeholderTextColor: rootWindow.mutedTextColor
                                    padding: 0
                                    Keys.onTabPressed: (event) => {
                                        if (text.startsWith("/") && !text.includes(" ")) {
                                            const cmds = ["/auto", "/tech", "/create", "/strict", "/inspect", "/quiet"];
                                            for (let i = 0; i < cmds.length; ++i) {
                                                if (cmds[i].startsWith(text.toLowerCase())) {
                                                    text = cmds[i] + " ";
                                                    cursorPosition = text.length;
                                                    event.accepted = true;
                                                    return;
                                                }
                                            }
                                        }
                                        event.accepted = false;
                                    }
                                    onActiveFocusChanged: if (activeFocus) {
                                        rootWindow.requestActivate();
                                        if (rootWindow.hc) hostController.pauseAmbientFollowing();
                                        rootWindow.editingDraft = true;
                                    }
                                    TapHandler { onTapped: { rootWindow.requestActivate(); promptInput.forceActiveFocus(); } }
                                    onTextChanged: if (rootWindow.hc && !rootWindow.suppressEditSignal) {
                                        rootWindow.rewriteAfterReview = false;
                                        hostController.noteDraftEdited(hostController.stateVersion);
                                        rootWindow.queueAutoAnalysis();
                                    }
                                }
                            }
                        }
                        Rectangle {
                            objectName: "draftPreview"
                            visible: !rootWindow.showPromptEditor && !rootWindow.isCompiledPreview && !rootWindow.compactQuestion
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            implicitHeight: sourceColumn.implicitHeight + (rootWindow.compactPresentation ? 12 : 22)
                            radius: 10; color: rootWindow.compactPresentation ? "transparent" : rocPalette.inset
                            Rectangle {
                                visible: rootWindow.compactPresentation
                                anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                                anchors.topMargin: 10; anchors.bottomMargin: 5
                                width: 3; radius: 1
                                color: rocPalette.focus
                            }
                            ColumnLayout {
                                id: sourceColumn
                                x: 13; y: rootWindow.compactPresentation ? 3 : 11; width: parent.width - 25
                                spacing: rootWindow.compactPresentation ? 1 : 7
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.minimumWidth: 0; Layout.fillWidth: true
                                        text: rootWindow.compactPresentation && rootWindow.hc && hostController.captureTargetName.length
                                            ? qsTr("Draft from %1").arg(hostController.captureTargetName) : qsTr("Your draft")
                                        textFormat: Text.PlainText; elide: Text.ElideRight
                                        font.family: rootWindow.monoFont; font.pixelSize: 11
                                        color: rootWindow.mutedTextColor
                                    }
                                    HudButton {
                                        objectName: "editCapturedDraftControl"
                                        label: rootWindow.compactPresentation ? "" : qsTr("Edit draft"); tip: qsTr("Edit draft"); iconName: "pencil-simple"; compact: true; quiet: true
                                        onClicked: {
                                            if (rootWindow.isAmbientMode && !rootWindow.isDetailedExpanded) rootWindow.toggleDetailedExpanded();
                                            rootWindow.editingDraft = true;
                                            if (rootWindow.hc) hostController.pauseAmbientFollowing();
                                            Qt.callLater(function() { promptInput.forceActiveFocus(); });
                                        }
                                    }
                                }
                                Text {
                                    objectName: "draftPreviewText"
                                    Layout.fillWidth: true; Layout.minimumWidth: 0
                                    text: promptInput.text; textFormat: Text.PlainText
                                    font.family: rootWindow.compactPresentation && interFont.status === FontLoader.Ready ? interFont.name : rootWindow.sansFont
                                    font.pixelSize: rootWindow.compactPresentation ? 17 : 13
                                    lineHeight: rootWindow.compactPresentation ? 1.12 : 1.0
                                    color: rootWindow.compactPresentation ? rootWindow.fgColor : rootWindow.mutedTextColor
                                    wrapMode: Text.Wrap; maximumLineCount: rootWindow.compactPresentation ? 4 : 2; elide: Text.ElideRight
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            visible: text.length > 0 && (!rootWindow.hc || !hostController.isClarifying) &&
                                (!rootWindow.compactPresentation || rootWindow.generationPending ||
                                 (rootWindow.hc && (hostController.lastError.length > 0 ||
                                  hostController.shortcutWarning.length > 0 || hostController.analysisStatus === "FAILED")))
                            text: rootWindow.autoAnalysisText.length ? qsTr("Checking after you pause typing…") : rootWindow.hc ? [hostController.shortcutWarning, hostController.lastError === hostController.analysisMessage ? "" : hostController.lastError, hostController.analysisMessage].filter(function(x) { return x.length > 0; }).join("\n") : ""
                            wrapMode: Text.WordWrap
                            font.family: rootWindow.sansFont; font.pixelSize: 12
                            color: rootWindow.mutedTextColor
                        }
                        HudButton {
                            visible: rootWindow.hc && hostController.storageNeedsRecovery
                            label: qsTr("Retry history")
                            onClicked: rootWindow.retryHistory()
                        }
                        IntentReviewPanel {
                            id: reviewFlow
                            visible: !rootWindow.compactDraftActions || implicitHeight > 0
                            compactPresentation: rootWindow.compactPresentation
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            currentTheme: rootWindow.currentTheme
                            viewportHeight: contentScroll.height
                            reducedMotion: rootWindow.reducedMotion
                            inspectPanelExplicitOpen: rootWindow.inspectPanelExplicitOpen
                            draftAvailable: promptInput.text.trim().length > 0
                            rewritePending: rootWindow.rewriteAfterReview
                            onReviewRequested: rootWindow.analyzeNow()
                            onRewriteRequested: rootWindow.requestFullRewrite()
                            onRevealReviewItem: function(item) { rootWindow.revealReviewItem(item); }
                            onEditDraftRequested: {
                                if (rootWindow.isAmbientMode && !rootWindow.isDetailedExpanded) rootWindow.toggleDetailedExpanded();
                                if (rootWindow.isCompiledPreview) rootWindow.revertToDraft();
                                else { rootWindow.editingDraft = true; promptInput.forceActiveFocus(); }
                            }
                        }
                        HudButton {
                            visible: !rootWindow.compactPresentation
                            label: rootWindow.showWorkspaceOptions ? qsTr("Hide workspace options") : qsTr("Workspace options")
                            compact: true; quiet: true
                            onClicked: rootWindow.showWorkspaceOptions = !rootWindow.showWorkspaceOptions
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: !rootWindow.compactPresentation && rootWindow.showWorkspaceOptions
                            spacing: 10
                            RowLayout {
                                Layout.fillWidth: true
                                HudButton { label: qsTr("Entire project"); compact: true; primary: rootWindow.activeScope === "global"; onClicked: rootWindow.activeScope = "global" }
                                HudButton { label: qsTr("Single file"); compact: true; primary: rootWindow.activeScope === "local"; onClicked: rootWindow.openFileDropdown() }
                                Item { Layout.fillWidth: true }
                            }
                            HudButton {
                                id: fileSelectorButton
                                label: rootWindow.targetLocalFile.length ? rootWindow.targetLocalFile : qsTr("Choose a file")
                                visible: rootWindow.activeScope === "local"
                                onClicked: rootWindow.openFileDropdown()
                            }
                            HudButton { label: qsTr("Add scope to draft"); compact: true; onClicked: rootWindow.applyScope() }
                            CheckBox {
                                text: qsTr("Reduce motion")
                                checked: rootWindow.reducedMotion
                                onToggled: rootWindow.reducedMotion = checked
                                font.family: rootWindow.sansFont; font.pixelSize: 13
                                palette.windowText: rootWindow.fgColor
                            }
                        }
                    }
                }

                Rectangle {
                    visible: !rootWindow.compactQuestion
                    Layout.fillWidth: true; implicitHeight: 1; color: rootWindow.borderColor
                }
                GridLayout {
                    id: hudFooter
                    visible: !rootWindow.compactQuestion
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    columns: rootWindow.width < 600 ? 1 : 2
                    rowSpacing: 3; columnSpacing: 12
                    RowLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 6
                        visible: !rootWindow.compactDraftActions
                        Text {
                            objectName: "deliveryDestinationLabel"
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: rootWindow.hc ? hostController.deliveryLabel : qsTr("Copies to clipboard")
                            textFormat: Text.PlainText; elide: Text.ElideRight
                            font.family: rootWindow.sansFont; font.pixelSize: 11
                            color: rootWindow.mutedTextColor
                        }
                        Text {
                            objectName: "pendingReviewLabel"
                            visible: rootWindow.hc && hostController.pendingReviewCount > 0
                            text: rootWindow.hc ? qsTr("%1 checks left").arg(hostController.pendingReviewCount) : ""
                            font.family: rootWindow.sansFont; font.pixelSize: 11
                            color: rocPalette.warning
                        }
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignRight
                        Layout.fillWidth: rootWindow.compactPresentation
                        spacing: 6
                        Item { visible: rootWindow.compactPresentation; Layout.fillWidth: true }
                        HudButton {
                            objectName: "compactClarifyButton"
                            visible: rootWindow.compactDraftActions
                            label: qsTr("Clarify")
                            compact: true; iconName: "sparkle"
                            active: promptInput.text.trim().length > 0 && hostController.isStorageAvailable && !hostController.storageBusy
                            onClicked: rootWindow.analyzeNow()
                        }
                        HudButton {
                            objectName: "bypassBtn"
                            visible: !rootWindow.compactDraftActions
                            label: qsTr("Keep original")
                            compact: rootWindow.height <= 260
                            quiet: true
                            tip: qsTr("Use your original wording")
                            active: promptInput.text.trim().length > 0 && rootWindow.hc && hostController.isStorageAvailable && !hostController.storageBusy
                            onClicked: rootWindow.executeHardBypass()
                        }
                        HudButton {
                            objectName: "approveRewriteControl"
                            label: rootWindow.compactDraftActions ? qsTr("Rewrite prompt") : rootWindow.presentation.label
                            iconName: rootWindow.compactDraftActions ? "pencil-simple" : ""
                            primary: true
                            compact: rootWindow.compactPresentation || rootWindow.height <= 260
                            active: rootWindow.hc && hostController.isStorageAvailable && !hostController.storageBusy &&
                                (rootWindow.compactDraftActions ? promptInput.text.trim().length > 0 :
                                 rootWindow.presentation.action !== "NONE" && (promptInput.text.trim().length > 0 || hostController.hasProposal || hostController.isAnalyzing))
                            onClicked: rootWindow.compactDraftActions ? rootWindow.requestFullRewrite() : rootWindow.handleEnter()
                            Text {
                                objectName: "analysisActionLabel"
                                text: rootWindow.presentation.label
                                visible: false
                            }
                        }
                        HudButton {
                            objectName: "hideBtn"
                            visible: !rootWindow.compactPresentation
                            iconName: "x"; tip: qsTr("Hide Roclint · Esc")
                            compact: rootWindow.height <= 260; quiet: true
                            onClicked: rootWindow.dismissCard()
                        }
                    }
                }
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom; anchors.bottomMargin: 76
                width: Math.min(parent.width - 40, toastLabel.implicitWidth + 28)
                height: toastLabel.implicitHeight + 22
                radius: 10; color: rocPalette.hover; border.color: rocPalette.border
                visible: rootWindow.toastMessage.length > 0
                z: 1001
                Text {
                    id: toastLabel; anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 28)
                    text: rootWindow.toastMessage; wrapMode: Text.WordWrap
                    font.family: rootWindow.sansFont; font.pixelSize: 12; color: rootWindow.fgColor
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
                                    font.pixelSize: 12
                                    color: rootWindow.fgColor
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text {
                                    text: "PROJECTS"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 11
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
                                        font.pixelSize: 12
                                        color: rootWindow.currentProjectIndex === index ? rootWindow.fgColor : rootWindow.dimColor
                                    }

                                    ColumnLayout {
                                        spacing: 1
                                        Text {
                                            text: modelData.name
                                            font.family: rootWindow.monoFont
                                            font.bold: rootWindow.currentProjectIndex === index
                                            font.pixelSize: 12
                                            color: rootWindow.fgColor
                                        }
                                        Text {
                                            text: modelData.path
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 11
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

                                Text { text: "[ + ]"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 12; color: rootWindow.fgColor }
                                Text { text: "OPEN PROJECT FOLDER..."; font.family: rootWindow.monoFont; font.pixelSize: 12; color: rootWindow.fgColor; opacity: 0.8 }
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
                                    font.pixelSize: 12
                                    color: "#FFB000"
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text {
                                    text: "SINGLE-FILE SCOPE"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 11
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
                                    font.pixelSize: 12
                                    color: rootWindow.mutedTextColor
                                }

                                TextInput {
                                    id: fileSearchInput
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 12
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
                                    font.pixelSize: 12
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
                                        font.pixelSize: 12
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
                                                font.pixelSize: 12
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
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: "#F59E0B"
                                                }
                                            }
                                        }
                                        Text {
                                            text: modelData
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 11
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
                                font.pixelSize: 12
                                color: rootWindow.mutedTextColor
                            }
                        }
                    }
                }
            }

        }
    }
}
