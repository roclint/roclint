import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("ROCLINT // RETROFUTURIST POSTER HUD")
    width: 820
    height: 520
    minimumWidth: 520
    minimumHeight: 360
    visible: false
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Arial"
    readonly property string serifFont: isMac ? "Times New Roman" : "Georgia"

    // Model Hot-Swapping System (Ollama / Unsloth style)
    property var modelList: [
        { name: "Llama-3.2-3B-Instruct", quant: "Q8_0" },
        { name: "DeepSeek-Coder-6.7B", quant: "Q8_0" },
        { name: "Qwen-2.5-Coder-7B", quant: "Q8_0" },
        { name: "Mistral-7B-v0.3", quant: "Q8_0" }
    ]
    property int currentModelIndex: 0
    readonly property var activeModel: modelList[currentModelIndex]

    // Active Project / Workspace System (Antigravity-aligned)
    property var projectList: [
        { name: "roclint", path: "/Users/jesse/roclint" },
        { name: "Stardust_v4_2", path: "/Users/jesse/Stardust_v4_2 2" },
        { name: "cadroc-core", path: "~/code/cadroc-core" }
    ]
    property int currentProjectIndex: 0
    readonly property var activeProject: projectList[currentProjectIndex]

    // Scope Governance System (Default: Global Codebase; Local File selectable)
    property string activeScope: "global" // "global" or "local"
    property string targetLocalFile: "FloatingCard.qml"
    property var projectFiles: [
        "app/qml/shell/FloatingCard.qml",
        "app/host/HostController.cpp",
        "app/host/ScreenPlacement.cpp",
        "app/host/HostContext.cpp",
        "src/core/prompt_state/RevisionState.cpp",
        "src/core/prompt_state/PromptRevision.cpp",
        "CMakeLists.txt"
    ]

    function refreshProjectFiles() {
        if (typeof hostController !== "undefined" && activeProject && activeProject.path) {
            var scanned = hostController.getProjectFiles(activeProject.path);
            if (scanned && scanned.length > 0) {
                projectFiles = scanned;
                targetLocalFile = scanned[0];
                return;
            }
        }
        if (activeProject && activeProject.name.indexOf("Stardust") !== -1) {
            projectFiles = [
                "ROCLINT_GUIDE.md",
                "master_architecture.md",
                "stardust_architecture_spec.md",
                "evaluation_plan.md",
                "contracts/C03_analysis_semantics.md",
                "contracts/C05_desktop_view_model.md",
                "contracts/C09_invocation_host_context.md",
                "curriculum/COURSE_MAP.md",
                "curriculum/PROGRESS_TRACKER.md"
            ];
        } else {
            projectFiles = [
                "app/qml/shell/FloatingCard.qml",
                "app/host/HostController.cpp",
                "app/host/ScreenPlacement.cpp",
                "app/host/HostContext.cpp",
                "src/core/prompt_state/RevisionState.cpp",
                "src/core/prompt_state/PromptRevision.cpp",
                "CMakeLists.txt"
            ];
        }
        targetLocalFile = projectFiles[0];
    }

    onCurrentProjectIndexChanged: {
        refreshProjectFiles();
    }
    property bool telemetryMaximized: false
    property bool isCompiledPreview: false
    property string originalDraft: ""

    // Real-Time Linter & Ambiguity State (Contract C03 / B06)
    readonly property bool hasAmbiguity: {
        if (isCompiledPreview) return false;
        if (typeof hostController !== "undefined") {
            return hostController.isClarifying;
        }
        return false;
    }
    readonly property real ambiguityScore: {
        if (typeof hostController !== "undefined") {
            return hostController.ambiguityScore;
        }
        return 0.0;
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
    property real liveLatency: 4.8
    property real liveCpu: 0.02
    property real liveRam: 40.2
    property int maxTokens: 350
    property bool pinToTop: true
    property bool hasBeenActive: false

    onActiveChanged: {
        if (active) {
            hasBeenActive = true;
        } else if (hasBeenActive && visible && !pinToTop) {
            hasBeenActive = false;
            dismissCard();
        }
    }

    Connections {
        target: typeof hostController !== "undefined" ? hostController : null
        function onDraftCaptured(text) {
            if (text && text.trim().length > 0) {
                if (promptInput.text.trim().length === 0) {
                    promptInput.text = text.trim();
                    rootWindow.showToast("PRE-FILLED FROM CLIPBOARD");
                }
            }
        }
        function onRevisionRestored(text) {
            if (text && text.trim().length > 0) {
                promptInput.text = text;
                rootWindow.showToast("REVISION RESTORED");
            }
        }
    }

    function dismissCard() {
        isClarificationActive = false;
        toastMessage = "";
        if (typeof hostController !== "undefined") {
            hostController.hideCard();
        } else {
            rootWindow.hide();
        }
    }

    function executeHardBypass() {
        if (promptInput.text.trim().length === 0) return;
        showToast("PROMPT SENT AS-IS");
        if (typeof hostController !== "undefined") {
            hostController.hardBypass(promptInput.text);
        }
    }

    function revertToDraft() {
        if (originalDraft && originalDraft.trim().length > 0) {
            promptInput.text = originalDraft;
        }
        isCompiledPreview = false;
        showToast("REVERTED TO EDITABLE DRAFT");
    }

    function handleEnter() {
        if (promptInput.text.trim().length === 0) {
            showToast("PLEASE TYPE A PROMPT FIRST");
            return;
        }

        if (isCompiledPreview) {
            if (typeof hostController !== "undefined") {
                hostController.commitPrompt(promptInput.text);
                hostController.copyToClipboard(promptInput.text);
            }
            showToast("PROMPT COPIED TO CLIPBOARD");
            return;
        }

        if (hasAmbiguity) {
            commitChoice(1);
            return;
        }

        let compiled = "";
        const fileTarget = (targetLocalFile.trim().length > 0) ? targetLocalFile.trim() : "FloatingCard.qml";
        const hasCtx = (typeof hostController !== "undefined" && hostController.hasHostContext);
        const ctxLine = hasCtx ? ("/* HOST CONTEXT: " + hostController.hostContextCategory + " (" + hostController.hostContextDisplayName + ") */\n") : "";
        const profile = (typeof hostController !== "undefined") ? hostController.rewriteProfile : "GENERIC@1";
        const hasIntent = (typeof hostController !== "undefined" && hostController.hasConfirmedIntent && !hostController.isIntentStale);
        const intentHeader = hasIntent ? ("/* TARGET PROFILE: " + profile + " */\n/* " + hostController.confirmedIntentSummary + " */\n") : "";

        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent && !hostController.isIntentStale) {
            const synthesized = hostController.synthesizeRewrite(promptInput.text.trim());
            if (synthesized && synthesized.length > 0) {
                compiled = synthesized;
            }
        }

        if (compiled.length === 0) {
            if (activeScope === "local") {
                compiled = "/* ROCLINT [SCOPE: SINGLE FILE] */\n" +
                           "/* TARGET: " + fileTarget + " */\n" +
                           intentHeader +
                           ctxLine +
                           "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n" +
                           "/* RULE: STRICT SINGLE-FILE LOCK — DO NOT MODIFY FILES OUTSIDE " + fileTarget + " */\n\n" +
                           promptInput.text.trim() + "\n\n" +
                           "/* CONSTRAINTS: KEEP EXISTING PUBLIC APIS UNCHANGED */";
            } else {
                compiled = "/* ROCLINT [SCOPE: ENTIRE PROJECT] */\n" +
                           intentHeader +
                           ctxLine +
                           "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n\n" +
                           promptInput.text.trim() + "\n\n" +
                           "/* CONSTRAINTS: VERIFY CHANGES ACROSS ALL REFERENCED MODULES */";
            }
        }

        originalDraft = promptInput.text;
        promptInput.text = compiled;
        isCompiledPreview = true;

        if (typeof hostController !== "undefined") {
            hostController.commitPrompt(compiled);
            if (hostController.directInjectionEnabled) {
                const injected = hostController.performDirectInjection(compiled);
                if (injected) {
                    showToast("ENHANCED & INJECTED DIRECTLY IN-PLACE");
                } else {
                    showToast("FALLBACK: COPIED TO CLIPBOARD");
                }
            } else {
                hostController.copyToClipboard(compiled);
                showToast("ENHANCED & COPIED TO CLIPBOARD");
            }
        } else {
            showToast("ENHANCED & COPIED TO CLIPBOARD");
        }
    }

    function commitChoiceById(altId, label) {
        if (typeof hostController !== "undefined") {
            hostController.confirmChoice(altId);
            showToast("INTENT CONFIRMED: " + (label ? label : altId));
        }
    }

    function commitChoice(choiceNumber) {
        if (typeof hostController !== "undefined" && hostController.activeAlternatives.length >= choiceNumber) {
            const alt = hostController.activeAlternatives[choiceNumber - 1];
            commitChoiceById(alt.id, alt.label);
            return;
        }

        let compiled = "";
        const opt1Label = "FloatingCard.qml — Responsive action button row";
        const opt2Label = "ScreenPlacement.cpp — Multi-monitor bounding clamp";
        const chosenLabel = (choiceNumber === 1) ? opt1Label : opt2Label;
        const chosenTarget = (choiceNumber === 1) ? "FloatingCard.qml" : "ScreenPlacement.cpp";
        const hasCtx = (typeof hostController !== "undefined" && hostController.hasHostContext);
        const ctxLine = hasCtx ? ("/* HOST CONTEXT: " + hostController.hostContextCategory + " (" + hostController.hostContextDisplayName + ") */\n") : "";
        const baseTask = isCompiledPreview ? originalDraft.trim() : promptInput.text.trim();

        if (activeScope === "local") {
            compiled = "/* ROCLINT [SCOPE: SINGLE FILE] */\n" +
                       "/* TARGET: " + chosenTarget + " */\n" +
                       ctxLine +
                       "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n" +
                       "/* RULE: STRICT SINGLE-FILE LOCK — DO NOT MODIFY FILES OUTSIDE " + chosenTarget + " */\n" +
                       "/* RESOLVED INTENT: Option " + choiceNumber + " (" + chosenLabel + ") */\n\n" +
                       baseTask + "\n\n" +
                       "/* CONSTRAINTS: KEEP EXISTING PUBLIC APIS UNCHANGED */";
        } else {
            compiled = "/* ROCLINT [SCOPE: ENTIRE PROJECT] */\n" +
                       "/* PRIMARY FOCUS: " + chosenTarget + " */\n" +
                       ctxLine +
                       "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n" +
                       "/* RESOLVED INTENT: Option " + choiceNumber + " (" + chosenLabel + ") */\n\n" +
                       baseTask + "\n\n" +
                       "/* CONSTRAINTS: VERIFY CHANGES ACROSS ALL REFERENCED MODULES */";
        }

        if (!isCompiledPreview) {
            originalDraft = promptInput.text;
        }
        promptInput.text = compiled;
        isCompiledPreview = true;

        if (typeof hostController !== "undefined") {
            hostController.commitPrompt(compiled);
            hostController.copyToClipboard(compiled);
        }
        showToast("COMPILED & COPIED: OPTION " + choiceNumber);
        isClarificationActive = false;
    }

    function openFileDropdown() {
        activeScope = "local";
        projectDropdown.visible = false;
        modelDropdown.visible = false;
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

    Timer {
        id: analyzeDebounceTimer
        interval: 180
        repeat: false
        onTriggered: {
            if (typeof hostController !== "undefined" && !rootWindow.isCompiledPreview && typeof promptInput !== "undefined") {
                hostController.analyzePrompt(promptInput.text);
            }
        }
    }

    // Telemetry Jitter Simulation (Matches HTML prototype)
    Timer {
        interval: 1400
        running: true
        repeat: true
        onTriggered: {
            rootWindow.liveRam = Number((39.8 + Math.random() * 0.8).toFixed(1));
            rootWindow.liveCpu = Number((0.01 + Math.random() * 0.03).toFixed(2));
            rootWindow.liveLatency = Number((4.6 + Math.random() * 0.5).toFixed(1));
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
            promptInput.forceActiveFocus();
        } else {
            hasBeenActive = false;
        }
    }

    // Keyboard Shortcuts
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (modelDropdown.visible) {
                modelDropdown.visible = false;
            } else if (projectDropdown.visible) {
                projectDropdown.visible = false;
            } else if (typeof fileDropdown !== "undefined" && fileDropdown.visible) {
                fileDropdown.visible = false;
            } else if (rootWindow.isClarificationActive) {
                rootWindow.isClarificationActive = false;
            } else {
                rootWindow.dismissCard();
            }
        }
    }

    Shortcut {
        sequences: ["Shift+Return", "Shift+Enter"]
        onActivated: rootWindow.executeHardBypass()
    }

    Shortcut {
        sequences: rootWindow.isMac ? ["Meta+Return", "Meta+Enter", "Ctrl+Return", "Ctrl+Enter"] : ["Ctrl+Return", "Ctrl+Enter"]
        onActivated: rootWindow.handleEnter()
    }

    Shortcut {
        sequences: ["Alt+1", "Ctrl+1"]
        enabled: rootWindow.isClarificationActive || rootWindow.hasAmbiguity
        onActivated: rootWindow.commitChoice(1)
    }

    Shortcut {
        sequences: ["Alt+2", "Ctrl+2"]
        enabled: rootWindow.isClarificationActive || rootWindow.hasAmbiguity
        onActivated: rootWindow.commitChoice(2)
    }

    Shortcut {
        sequences: ["Alt+3", "Ctrl+3"]
        enabled: rootWindow.isClarificationActive || rootWindow.hasAmbiguity
        onActivated: rootWindow.commitChoice(3)
    }

    Shortcut {
        sequences: ["Alt+M", "Ctrl+M"]
        onActivated: {
            rootWindow.telemetryMaximized = !rootWindow.telemetryMaximized;
            rootWindow.showToast(rootWindow.telemetryMaximized ? "PERFORMANCE: EXPANDED" : "PERFORMANCE: MINIMIZED");
        }
    }

    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: {
            if (typeof hostController !== "undefined" && hostController) {
                hostController.showDashboard();
            }
        }
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

                        MouseArea {
                            id: projectMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                modelDropdown.visible = false;
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

                    // Model Dropdown Selector Pill
                    Rectangle {
                        id: modelSelectorButton
                        visible: rootWindow.width >= 570
                        height: 20
                        width: Math.min(160, modelRow.implicitWidth + 12)
                        clip: true
                        color: (modelDropdown.visible || modelMouse.containsMouse) ? rootWindow.dimColor : "transparent"
                        border.color: modelDropdown.visible ? rootWindow.fgColor : rootWindow.borderColor
                        border.width: 1

                        RowLayout {
                            id: modelRow
                            anchors.centerIn: parent
                            spacing: 4

                            Text {
                                text: "MODEL:"
                                visible: rootWindow.width >= 760
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.mutedTextColor
                            }
                            Text {
                                text: (rootWindow.width >= 780 ? rootWindow.activeModel.name : "Llama-3.2") + " " + (modelDropdown.visible ? "▴" : "▾")
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                                elide: Text.ElideRight
                                Layout.maximumWidth: 110
                            }
                        }

                        MouseArea {
                            id: modelMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (!modelDropdown.visible) {
                                    var pt = modelSelectorButton.mapToItem(subBorderInset, 0, modelSelectorButton.height + 4);
                                    modelDropdown.x = pt.x;
                                    modelDropdown.y = pt.y;
                                    modelDropdown.visible = true;
                                } else {
                                    modelDropdown.visible = false;
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

                                MouseArea {
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
                                text: rootWindow.hasAmbiguity ?
                                      (rootWindow.width >= 660 ? "[ NEEDS CLARITY ]" : (rootWindow.width >= 560 ? "[ CLARIFY ]" : "[ ! ]")) :
                                      (rootWindow.width >= 660 ? "[ STATUS: OK ]" : (rootWindow.width >= 560 ? "[ OK ]" : "[ ✓ ]"))
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 9
                                color: rootWindow.hasAmbiguity ? "#FFB000" : "#7EE787"
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
                            MouseArea {
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
                            MouseArea {
                                id: setMouse
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
                            MouseArea {
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
                            MouseArea {
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
                    height: 1
                    color: rootWindow.borderColor
                    opacity: 0.3
                }

                // =============================================================
                // 1b. SCOPE GOVERNANCE STRIP (Global Codebase vs Local File)
                // =============================================================
                Rectangle {
                    Layout.fillWidth: true
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
                                    text: rootWindow.width >= 680 ? "ENTIRE PROJECT [DEFAULT]" : "ALL FILES"
                                    font.family: rootWindow.monoFont
                                    font.bold: rootWindow.activeScope === "global"
                                    font.pixelSize: 8
                                    color: rootWindow.activeScope === "global" ? rootWindow.bgColor : rootWindow.fgColor
                                }
                            }
                            MouseArea {
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
                            MouseArea {
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
                                        if (!rootWindow.targetLocalFile || rootWindow.targetLocalFile.length === 0) return "Select file...";
                                        var parts = rootWindow.targetLocalFile.split("/");
                                        return parts[parts.length - 1];
                                    }
                                    text: displayFileName + " " + (typeof fileDropdown !== "undefined" && fileDropdown.visible ? "▴" : "▾")
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                    elide: Text.ElideMiddle
                                    Layout.maximumWidth: Math.min(220, Math.max(90, rootWindow.width - 450))
                                }
                            }

                            MouseArea {
                                id: fileSelectorMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    projectDropdown.visible = false;
                                    modelDropdown.visible = false;
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
                                MouseArea {
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
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6



                        // Prompt Terminal Canvas (Double Bounded Inset)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: rootWindow.inputBgColor
                            border.color: promptInput.activeFocus ? rootWindow.fgColor : rootWindow.borderColor
                            border.width: 1
                            radius: 6

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 6

                                ScrollView {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true

                                    TextArea {
                                        id: promptInput
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
                                            if (typeof hostController !== "undefined" && !rootWindow.isCompiledPreview) {
                                                hostController.invalidateIntent();
                                            }
                                            analyzeDebounceTimer.restart();
                                        }

                                        Keys.onTabPressed: function(event) {
                                            if (rootWindow.hasAmbiguity) {
                                                rootWindow.commitChoice(1);
                                                event.accepted = true;
                                            } else {
                                                event.accepted = false;
                                            }
                                        }

                                        Keys.onReturnPressed: function(event) {
                                            if (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
                                                rootWindow.handleEnter();
                                                event.accepted = true;
                                            } else if (event.modifiers & Qt.ShiftModifier) {
                                                rootWindow.executeHardBypass();
                                                event.accepted = true;
                                            } else {
                                                event.accepted = false; // Normal multiline Return!
                                            }
                                        }

                                        Keys.onEnterPressed: function(event) {
                                            if (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) {
                                                rootWindow.handleEnter();
                                                event.accepted = true;
                                            } else if (event.modifiers & Qt.ShiftModifier) {
                                                rootWindow.executeHardBypass();
                                                event.accepted = true;
                                            } else {
                                                event.accepted = false;
                                            }
                                        }
                                    }
                                }

                                // Canvas Sub-bar
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 1
                                    color: rootWindow.borderColor
                                    opacity: 0.3
                                }

                                RowLayout {
                                    Layout.fillWidth: true
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
                                                if (rootWindow.isCompiledPreview) return "COMPILED SPECIFICATION (COPIED)";
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
                                            width: revertDraftText.contentWidth + 10
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
                                            MouseArea {
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
                                        text: rootWindow.isCompiledPreview ? "ENHANCED" : (rootWindow.hasAmbiguity ? "1 SUGGESTION" : "0 ISSUES")
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.isCompiledPreview ? "#7EE787" : (rootWindow.hasAmbiguity ? "#FFB000" : rootWindow.fgColor)
                                    }
                                    Text {
                                        text: rootWindow.isCompiledPreview ? "AMBIGUITY: 0%" : ("AMBIGUITY: " + (rootWindow.ambiguityScore * 100).toFixed(0) + "%")
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.isCompiledPreview ? "#7EE787" : rootWindow.fgColor
                                    }
                                }
                            }
                        }

                        // =============================================================
                        // 2b. SLICK LIVE LINTER & TRIAGE STRIP ("Swingly" C with A/B)
                        // =============================================================
                        Rectangle {
                            id: liveLinterStrip
                            Layout.fillWidth: true
                            height: (rootWindow.hasAmbiguity || (typeof hostController !== "undefined" && hostController.hasConfirmedIntent)) ? 30 : 22
                            color: {
                                if (rootWindow.hasAmbiguity) return "#140D04";
                                if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent) {
                                    return hostController.isIntentStale ? "#140D04" : "#08140B";
                                }
                                return rootWindow.inputBgColor;
                            }
                            border.color: {
                                if (rootWindow.hasAmbiguity) return "#FFB000";
                                if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent) {
                                    return hostController.isIntentStale ? "#F59E0B" : "#22C55E";
                                }
                                return rootWindow.borderColor;
                            }
                            border.width: 1
                            radius: 4

                            Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                            Behavior on color { ColorAnimation { duration: 160 } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8

                                // Indicator tag
                                Text {
                                    text: {
                                        if (rootWindow.hasAmbiguity) return "▲ AMBIGUITY:";
                                        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent) {
                                            return hostController.isIntentStale ? "⚠ STALE INTENT:" : "✓ INTENT RESOLVED:";
                                        }
                                        return "✦ STATUS:";
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: {
                                        if (rootWindow.hasAmbiguity) return "#FFB000";
                                        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent) {
                                            return hostController.isIntentStale ? "#F59E0B" : "#7EE787";
                                        }
                                        return "#7EE787";
                                    }
                                }

                                // Message
                                Text {
                                    text: {
                                        if (rootWindow.hasAmbiguity) {
                                            return (rootWindow.width >= 700 ? ("Multiple target components in [" + rootWindow.activeProject.name + "]") : "Target choice");
                                        }
                                        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent) {
                                            if (hostController.isIntentStale) return "PROMPT EDITED ∙ PREVIOUS INTENT INVALIDATED";
                                            return hostController.confirmedIntentSummary;
                                        }
                                        return (rootWindow.width >= 700 ? ("PROMPT CLEAR ∙ SCOPE: " + (rootWindow.activeScope === "global" ? "ENTIRE PROJECT" : "SINGLE FILE (" + rootWindow.targetLocalFile + ")")) : "PROMPT CLEAR");
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: !rootWindow.hasAmbiguity
                                    font.pixelSize: 8
                                    color: {
                                        if (rootWindow.hasAmbiguity) return rootWindow.fgColor;
                                        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent) {
                                            return hostController.isIntentStale ? "#F59E0B" : "#7EE787";
                                        }
                                        return "#7EE787";
                                    }
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: rootWindow.hasAmbiguity ? (rootWindow.width >= 760 ? 280 : 160) : 400
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                // Stale Intent Recovery Pill (C06 / B07)
                                RowLayout {
                                    visible: !rootWindow.hasAmbiguity && typeof hostController !== "undefined" && hostController.hasConfirmedIntent && hostController.isIntentStale
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 6

                                    Rectangle {
                                        height: 18
                                        width: staleReverifyText.implicitWidth + 12
                                        color: staleMouseArea.containsMouse ? "#F59E0B" : "transparent"
                                        border.color: "#F59E0B"
                                        border.width: 1
                                        radius: 3

                                        Text {
                                            id: staleReverifyText
                                            anchors.centerIn: parent
                                            text: "[ ↺ RE-VERIFY ]"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: staleMouseArea.containsMouse ? "#050505" : "#F59E0B"
                                        }

                                        MouseArea {
                                            id: staleMouseArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                if (typeof hostController !== "undefined") {
                                                    hostController.analyzePrompt(promptInput.text);
                                                }
                                            }
                                        }
                                    }
                                }

                                // Confirmed Intent Control & Target Profile Selector Pills (B07 / B08P)
                                RowLayout {
                                    visible: !rootWindow.hasAmbiguity && typeof hostController !== "undefined" && hostController.hasConfirmedIntent && !hostController.isIntentStale
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 6

                                    // Profile Selector Button (B08P sticky profile)
                                    Rectangle {
                                        height: 18
                                        width: profilePillText.implicitWidth + 12
                                        color: profileMouseArea.containsMouse ? rootWindow.fgColor : "transparent"
                                        border.color: "#7EE787"
                                        border.width: 1
                                        radius: 3

                                        Text {
                                            id: profilePillText
                                            anchors.centerIn: parent
                                            text: "[ ⛭ " + (typeof hostController !== "undefined" ? hostController.rewriteProfile : "GENERIC@1") + " ]"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: profileMouseArea.containsMouse ? rootWindow.bgColor : "#7EE787"
                                        }

                                        MouseArea {
                                            id: profileMouseArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                if (typeof hostController !== "undefined") {
                                                    const profiles = ["GENERIC@1", "ANTHROPIC_STRUCTURED@1", "OPENAI_STRUCTURED@1", "LOCAL_COMPACT@1"];
                                                    const current = hostController.rewriteProfile;
                                                    let nextIndex = (profiles.indexOf(current) + 1) % profiles.length;
                                                    hostController.setRewriteProfile(profiles[nextIndex]);
                                                    rootWindow.showToast("PROFILE: " + profiles[nextIndex]);
                                                }
                                            }
                                        }
                                    }

                                    // Looks Right Button (Explicit confirmation)
                                    Rectangle {
                                        height: 18
                                        width: looksRightText.implicitWidth + 12
                                        color: looksRightMouseArea.containsMouse ? "#7EE787" : "transparent"
                                        border.color: "#7EE787"
                                        border.width: 1
                                        radius: 3

                                        Text {
                                            id: looksRightText
                                            anchors.centerIn: parent
                                            text: "[ ✓ LOOKS RIGHT ]"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: looksRightMouseArea.containsMouse ? "#050505" : "#7EE787"
                                        }

                                        MouseArea {
                                            id: looksRightMouseArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                if (typeof hostController !== "undefined") {
                                                    hostController.confirmLooksRight();
                                                    rootWindow.showToast("INTENT CONFIRMED — READY TO REWRITE");
                                                }
                                            }
                                        }
                                    }

                                    // Keep Original Button
                                    Rectangle {
                                        height: 18
                                        width: keepOrigText.implicitWidth + 12
                                        color: keepOrigMouseArea.containsMouse ? rootWindow.fgColor : "transparent"
                                        border.color: rootWindow.borderColor
                                        border.width: 1
                                        radius: 3

                                        Text {
                                            id: keepOrigText
                                            anchors.centerIn: parent
                                            text: "[ ORIGINAL ]"
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 8
                                            color: keepOrigMouseArea.containsMouse ? rootWindow.bgColor : rootWindow.fgColor
                                        }

                                        MouseArea {
                                            id: keepOrigMouseArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                if (typeof hostController !== "undefined") {
                                                    hostController.keepOriginalPrompt();
                                                    rootWindow.showToast("KEEPING ORIGINAL PHRASING");
                                                }
                                            }
                                        }
                                    }
                                }

                                // Resolution Pills (Rendered when ambiguity detected - B06 dynamic)
                                RowLayout {
                                    visible: rootWindow.hasAmbiguity
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 6

                                    Text {
                                        visible: rootWindow.width >= 720 && typeof hostController !== "undefined"
                                        text: (typeof hostController !== "undefined" && hostController.activeQuestion.length > 0) ?
                                              hostController.activeQuestion : "RESOLVE TARGET:"
                                        font.family: rootWindow.monoFont
                                        font.bold: true
                                        font.pixelSize: 8
                                        color: "#FFB000"
                                    }

                                    Repeater {
                                        model: (typeof hostController !== "undefined" && hostController.activeAlternatives.length > 0)
                                               ? hostController.activeAlternatives
                                               : [
                                                     { id: "alt_1", label: "FloatingCard.qml" },
                                                     { id: "alt_2", label: "ScreenPlacement.cpp" }
                                                 ]
                                        delegate: Rectangle {
                                            height: 18
                                            width: pillText.implicitWidth + 12
                                            color: pillMouseArea.containsMouse ? rootWindow.fgColor : "transparent"
                                            border.color: "#FFB000"
                                            border.width: 1
                                            radius: 3

                                            Text {
                                                id: pillText
                                                anchors.centerIn: parent
                                                text: "[ " + (index + 1) + ": " + modelData.label + " ]"
                                                font.family: rootWindow.monoFont
                                                font.bold: true
                                                font.pixelSize: 8
                                                color: pillMouseArea.containsMouse ? rootWindow.bgColor : "#FFB000"
                                            }

                                            MouseArea {
                                                id: pillMouseArea
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (typeof hostController !== "undefined" && hostController.activeAlternatives.length > 0) {
                                                        rootWindow.commitChoiceById(modelData.id, modelData.label);
                                                    } else {
                                                        rootWindow.commitChoice(index + 1);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                // =============================================================
                // 3. INDUSTRIAL TELEMETRY GAUGES (Minimized / Maximized)
                // =============================================================
                Item {
                    id: telemetryContainer
                    Layout.fillWidth: true
                    Layout.preferredHeight: rootWindow.telemetryMaximized ? 46 : 22
                    Layout.maximumHeight: rootWindow.telemetryMaximized ? 48 : 22
                    Layout.fillHeight: false
                    clip: true

                    Behavior on Layout.preferredHeight {
                        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                    }

                    // MAXIMIZED VIEW (4 Detailed Gauge Columns)
                    RowLayout {
                        anchors.fill: parent
                        visible: rootWindow.telemetryMaximized
                        spacing: 8

                        // Gauge 1: RAM
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            Layout.fillHeight: false
                            color: "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 1

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "RAM:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                    Text { text: rootWindow.liveRam.toFixed(1) + " MB"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 9; color: rootWindow.fgColor }
                                }
                                Text {
                                    text: "[■■□□□□□□□□]"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: rootWindow.fgColor
                                }
                            }
                        }

                        // Gauge 2: Latency
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            Layout.fillHeight: false
                            color: "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 1

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "LATENCY:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                    Text { text: "REALTIME"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "LATENCY: " + rootWindow.liveLatency.toFixed(1) + " MS"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 10; color: rootWindow.fgColor }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                    Text { text: "±0.02"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }
                                }
                            }
                        }

                        // Gauge 3: Prompt Size
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            Layout.fillHeight: false
                            color: "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 1

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: rootWindow.width >= 640 ? "PROMPT SIZE:" : "SIZE:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                    Text {
                                        text: (rootWindow.width >= 640 ? (Math.ceil(promptInput.text.length / 4) + " / " + rootWindow.maxTokens + " TOKENS") : (Math.ceil(promptInput.text.length / 4) + "/" + rootWindow.maxTokens))
                                        font.family: rootWindow.monoFont
                                        font.bold: true
                                        font.pixelSize: 8
                                        color: (Math.ceil(promptInput.text.length / 4) > rootWindow.maxTokens) ? "#FF4D4D" : rootWindow.fgColor
                                    }
                                }
                                Text {
                                    text: {
                                        const count = Math.ceil(promptInput.text.length / 4);
                                        const total = 12;
                                        const filled = Math.min(total, Math.floor((count / rootWindow.maxTokens) * total));
                                        let s = "[";
                                        for (let i = 0; i < total; ++i) {
                                            s += (i < filled) ? "█" : "░";
                                        }
                                        return s + "]";
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: (Math.ceil(promptInput.text.length / 4) > rootWindow.maxTokens) ? "#FF4D4D" : rootWindow.fgColor
                                }
                            }
                        }

                        // Gauge 4: CPU + Quick Minimize Button
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            Layout.fillHeight: false
                            color: "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 1

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "CPU:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                    Text { text: rootWindow.liveCpu.toFixed(2) + "%"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 9; color: rootWindow.fgColor }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: rootWindow.width >= 640 ? "64 THREADS" : "64 TH"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 10; color: rootWindow.fgColor }
                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                    Rectangle {
                                        height: 12
                                        width: nomText.contentWidth + 6
                                        color: rootWindow.fgColor
                                        Text {
                                            id: nomText
                                            anchors.centerIn: parent
                                            text: "READY"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 7
                                            color: rootWindow.bgColor
                                        }
                                    }

                                    // Minimize Toggle Button
                                    Rectangle {
                                        height: 12
                                        width: minBtnText.contentWidth + 8
                                        color: minBtnMouse.containsMouse ? rootWindow.dimColor : "transparent"
                                        border.color: rootWindow.borderColor
                                        border.width: 1
                                        radius: 2

                                        Text {
                                            id: minBtnText
                                            anchors.centerIn: parent
                                            text: "▾ MIN"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 7
                                            color: rootWindow.fgColor
                                        }
                                        MouseArea {
                                            id: minBtnMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                rootWindow.telemetryMaximized = false;
                                                rootWindow.showToast("PERFORMANCE: MINIMIZED");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // MINIMIZED VIEW (Compact 1-line Ticker Strip)
                    Rectangle {
                        anchors.fill: parent
                        visible: !rootWindow.telemetryMaximized
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
                                text: "PERFORMANCE:"
                                visible: rootWindow.width >= 680
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.mutedTextColor
                            }

                            Text {
                                text: "RAM: " + rootWindow.liveRam.toFixed(1) + " MB"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                            }

                            Text { text: "│"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }

                            Text {
                                text: "LATENCY: " + rootWindow.liveLatency.toFixed(1) + " MS"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                            }

                            Text { text: "│"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }

                            Text {
                                text: (rootWindow.width >= 720 ? "LENGTH: " : "") + Math.ceil(promptInput.text.length / 4) + "/" + rootWindow.maxTokens + (rootWindow.width >= 720 ? " TOKENS" : "")
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                color: (Math.ceil(promptInput.text.length / 4) > rootWindow.maxTokens) ? "#FF4D4D" : rootWindow.fgColor
                            }

                            Text { text: "│"; visible: rootWindow.width >= 640; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.mutedTextColor }

                            Text {
                                visible: rootWindow.width >= 640
                                text: "CPU: " + rootWindow.liveCpu.toFixed(1) + "%" + (rootWindow.width >= 780 ? " (64 THREADS)" : "")
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                            }

                            Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                            // Expand Button on Ticker
                            Rectangle {
                                height: 14
                                width: maxBtnText.contentWidth + 8
                                color: maxBtnMouse.containsMouse ? rootWindow.dimColor : "transparent"
                                border.color: rootWindow.borderColor
                                border.width: 1
                                radius: 2

                                Text {
                                    id: maxBtnText
                                    anchors.centerIn: parent
                                    text: "▴ EXPAND"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 7
                                    color: rootWindow.fgColor
                                }
                                MouseArea {
                                    id: maxBtnMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        rootWindow.telemetryMaximized = true;
                                        rootWindow.showToast("PERFORMANCE: EXPANDED");
                                    }
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                rootWindow.telemetryMaximized = true;
                                rootWindow.showToast("PERFORMANCE: EXPANDED");
                            }
                        }
                    }
                }

                // =============================================================
                // 4. FOOTER BAR: Metrics, Hard-Shadow Tactile Action Buttons
                // =============================================================
                Rectangle {
                    Layout.fillWidth: true
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
                            width: cTag.contentWidth + 6
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
                                width: b1Text.contentWidth + 14
                                color: bypassMouse.pressed ? rootWindow.dimColor : (bypassMouse.containsMouse ? rootWindow.dimColor : "transparent")
                                border.color: rootWindow.isCompiledPreview ? "#FFB000" : rootWindow.borderColor
                                border.width: 1
                                radius: 3

                                Text {
                                    id: b1Text
                                    anchors.centerIn: parent
                                    text: {
                                        if (rootWindow.isCompiledPreview) return "[ ↺ EDIT DRAFT ]";
                                        return rootWindow.width >= 720 ? "[ SEND AS-IS ]" : "[ AS-IS ]";
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.isCompiledPreview ? "#FFB000" : rootWindow.fgColor
                                }
                                MouseArea {
                                    id: bypassMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (rootWindow.isCompiledPreview) {
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
                                width: b2Text.contentWidth + 16
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
                                    anchors.centerIn: parent
                                    text: {
                                        if (rootWindow.isCompiledPreview) return "[ ⏎ COPY AGAIN ]";
                                        if (rootWindow.hasAmbiguity) return rootWindow.width >= 720 ? "[ 1-2: RESOLVE ]" : "[ RESOLVE ]";
                                        if (typeof hostController !== "undefined" && hostController.hasConfirmedIntent && !hostController.isIntentStale) {
                                            return rootWindow.width >= 720 ? "[ ⏎ SYNTHESIZE REWRITE ]" : "[ ⏎ REWRITE ]";
                                        }
                                        return rootWindow.width >= 720 ? "[ ENHANCE & COPY ]" : "[ ENHANCE ]";
                                    }
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: triageMouse.pressed ? rootWindow.fgColor : (rootWindow.isCompiledPreview ? "#050505" : rootWindow.bgColor)
                                }
                                MouseArea {
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
                                width: hideTextLabel.contentWidth + 14
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
                                MouseArea {
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
                visible: modelDropdown.visible || projectDropdown.visible || (typeof fileDropdown !== "undefined" && fileDropdown.visible)
                z: 990
                onClicked: {
                    modelDropdown.visible = false;
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

                                MouseArea {
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

                            MouseArea {
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
                                    MouseArea {
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
                            Layout.fillHeight: true
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: fileDropdown.filteredFiles

                            ScrollBar.vertical: ScrollBar {
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
                                color: fItemMouse.containsMouse ? rootWindow.dimColor : (isSelected ? rootWindow.inputBgColor : "transparent")

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 12
                                    spacing: 8

                                    Text {
                                        text: isSelected ? "●" : "○"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: isSelected ? "#FFB000" : rootWindow.dimColor
                                    }

                                    ColumnLayout {
                                        spacing: 1
                                        Layout.fillWidth: true
                                        Text {
                                            text: fileName
                                            font.family: rootWindow.monoFont
                                            font.bold: isSelected
                                            font.pixelSize: 9
                                            color: isSelected ? "#FFB000" : rootWindow.fgColor
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: modelData
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 7
                                            color: rootWindow.mutedTextColor
                                            elide: Text.ElideMiddle
                                            Layout.fillWidth: true
                                        }
                                    }
                                }

                                MouseArea {
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

            // Neo-Brutalist Model Dropdown Menu
            Item {
                id: modelDropdown
                visible: false
                z: 999
                width: 320
                height: dropdownContent.implicitHeight

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
                    id: dropdownCard
                    anchors.fill: parent
                    color: rootWindow.bgColor
                    border.color: rootWindow.borderColor
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        id: dropdownContent
                        width: parent.width
                        spacing: 0

                        // Dropdown Title Strip
                        Rectangle {
                            Layout.fillWidth: true
                            height: 22
                            color: rootWindow.dimColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                Text {
                                    text: "SELECT AI MODEL"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text {
                                    text: "MODELS"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 7
                                    color: rootWindow.fgColor
                                    opacity: 0.7
                                }
                            }
                        }

                        // Model Item List
                        Repeater {
                            model: rootWindow.modelList
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                height: 32
                                color: (rootWindow.currentModelIndex === index)
                                       ? (itemMouse.containsMouse ? rootWindow.dimColor : rootWindow.inputBgColor)
                                       : (itemMouse.containsMouse ? rootWindow.dimColor : "transparent")

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    height: 1
                                    color: rootWindow.borderColor
                                    opacity: 0.2
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    // Active indicator
                                    Text {
                                        text: rootWindow.currentModelIndex === index ? "●" : "○"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 9
                                        color: rootWindow.currentModelIndex === index ? rootWindow.fgColor : rootWindow.dimColor
                                    }

                                    // Model Name
                                    Text {
                                        text: modelData.name
                                        font.family: rootWindow.monoFont
                                        font.bold: rootWindow.currentModelIndex === index
                                        font.pixelSize: 9
                                        color: rootWindow.fgColor
                                    }

                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                    // Quantization Tag
                                    Rectangle {
                                        height: 14
                                        width: quantText.contentWidth + 6
                                        color: rootWindow.currentModelIndex === index ? rootWindow.fgColor : "transparent"
                                        border.color: rootWindow.borderColor
                                        border.width: 1

                                        Text {
                                            id: quantText
                                            anchors.centerIn: parent
                                            text: modelData.quant
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 7
                                            color: rootWindow.currentModelIndex === index ? rootWindow.bgColor : rootWindow.fgColor
                                        }
                                    }
                                }

                                MouseArea {
                                    id: itemMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        rootWindow.currentModelIndex = index;
                                        rootWindow.showToast("MODEL: " + modelData.name);
                                        modelDropdown.visible = false;
                                    }
                                }
                            }
                        }

                        // Studio / HuggingFace Footer Link
                        Rectangle {
                            Layout.fillWidth: true
                            height: 26
                            color: hfMouse.containsMouse ? rootWindow.dimColor : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 6

                                Text {
                                    text: "[ + ]"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                }

                                Text {
                                    text: "DOWNLOAD MORE MODELS..."
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                    opacity: 0.8
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                Text {
                                    text: "STUDIO"
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 7
                                    color: rootWindow.mutedTextColor
                                }
                            }

                            MouseArea {
                                id: hfMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    modelDropdown.visible = false;
                                    rootWindow.showToast("OPENING DOWNLOAD PAGE...");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
