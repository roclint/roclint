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
        { name: "Llama-3.2-3B-Instruct", quant: "Q4_K_M" },
        { name: "DeepSeek-Coder-6.7B", quant: "Q4_K_M" },
        { name: "Qwen-2.5-Coder-7B", quant: "Q5_K_M" },
        { name: "Mistral-7B-v0.3", quant: "Q4_K_M" }
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
    property string targetLocalFile: "SessionManager.cpp"
    property bool telemetryMaximized: false

    // Real-Time Linter & Ambiguity State
    readonly property bool hasAmbiguity: {
        const txt = typeof promptInput !== "undefined" ? promptInput.text.toLowerCase() : "";
        const ambiguousWords = ["refactor", "clean", "fix", "improve", "update", "change", "optimize", "rewrite"];
        return ambiguousWords.some(w => txt.includes(w));
    }
    readonly property real ambiguityScore: hasAmbiguity ? 0.84 : 0.08

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
    property bool pinToTop: false
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

    function handleEnter() {
        if (promptInput.text.trim().length === 0) {
            showToast("PLEASE TYPE A PROMPT FIRST");
            return;
        }

        if (hasAmbiguity) {
            // Resolve based on active scope
            if (activeScope === "local") {
                commitChoice(1);
            } else {
                commitChoice(2);
            }
        } else {
            let compiled = "";
            const fileTarget = (targetLocalFile.trim().length > 0) ? targetLocalFile.trim() : "SessionManager.cpp";
            const hasCtx = (typeof hostController !== "undefined" && hostController.hasHostContext);
            const ctxLine = hasCtx ? ("/* HOST CONTEXT: " + hostController.hostContextCategory + " (" + hostController.hostContextDisplayName + ") */\n") : "";

            if (activeScope === "local") {
                compiled = "/* ROCLINT [SCOPE: CURRENT FILE] */\n" +
                           "/* TARGET: " + fileTarget + " */\n" +
                           ctxLine +
                           "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n\n" +
                           promptInput.text.trim();
            } else {
                compiled = "/* ROCLINT [SCOPE: ENTIRE PROJECT] */\n" +
                           ctxLine +
                           "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n\n" +
                           promptInput.text.trim();
            }
            if (typeof hostController !== "undefined") {
                hostController.commitPrompt(compiled);
                hostController.copyToClipboard(compiled);
            }
            showToast("PROMPT COPIED TO CLIPBOARD");
        }
    }

    function commitChoice(choiceNumber) {
        let compiled = "";
        const fileTarget = (targetLocalFile.trim().length > 0) ? targetLocalFile.trim() : "SessionManager.cpp";
        const hasCtx = (typeof hostController !== "undefined" && hostController.hasHostContext);
        const ctxLine = hasCtx ? ("/* HOST CONTEXT: " + hostController.hostContextCategory + " (" + hostController.hostContextDisplayName + ") */\n") : "";

        if (choiceNumber === 1) {
            compiled = "/* ROCLINT [SCOPE: CURRENT FILE] */\n" +
                       "/* TARGET: " + fileTarget + " */\n" +
                       ctxLine +
                       "/* PROJECT: " + activeProject.path + " */\n" +
                       "/* RULE: DO NOT MODIFY FILES OUTSIDE " + fileTarget + " */\n\n" +
                       promptInput.text.trim() + "\n\n" +
                       "/* CONSTRAINTS: KEEP EXISTING PUBLIC APIS UNCHANGED */";
        } else {
            compiled = "/* ROCLINT [SCOPE: ENTIRE PROJECT] */\n" +
                       ctxLine +
                       "/* PROJECT: " + activeProject.name + " (" + activeProject.path + ") */\n\n" +
                       promptInput.text.trim() + "\n\n" +
                       "/* CONSTRAINTS: VERIFY CHANGES ACROSS ALL REFERENCED MODULES */";
        }

        if (typeof hostController !== "undefined") {
            hostController.commitPrompt(compiled);
            hostController.copyToClipboard(compiled);
        }
        showToast("PROMPT COPIED (" + (choiceNumber === 1 ? "THIS FILE" : "ENTIRE PROJECT") + ")");
        isClarificationActive = false;
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
        sequences: ["Alt+M", "Ctrl+M"]
        onActivated: {
            rootWindow.telemetryMaximized = !rootWindow.telemetryMaximized;
            rootWindow.showToast(rootWindow.telemetryMaximized ? "PERFORMANCE: EXPANDED" : "PERFORMANCE: MINIMIZED");
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
                        width: projectRow.width + 12
                        color: (projectDropdown.visible || projectMouse.containsMouse) ? rootWindow.dimColor : "transparent"
                        border.color: projectDropdown.visible ? rootWindow.fgColor : rootWindow.borderColor
                        border.width: 1

                        RowLayout {
                            id: projectRow
                            anchors.centerIn: parent
                            spacing: 5

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
                        width: modelRow.width + 12
                        color: (modelDropdown.visible || modelMouse.containsMouse) ? rootWindow.dimColor : "transparent"
                        border.color: modelDropdown.visible ? rootWindow.fgColor : rootWindow.borderColor
                        border.width: 1

                        RowLayout {
                            id: modelRow
                            anchors.centerIn: parent
                            spacing: 5

                            Text {
                                text: "MODEL:"
                                visible: rootWindow.width >= 760
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.mutedTextColor
                            }
                            Text {
                                text: (rootWindow.width >= 780 ? (rootWindow.activeModel.name + " (" + rootWindow.activeModel.quant + ")") : (rootWindow.width >= 720 ? rootWindow.activeModel.name : "Llama-3.2")) + " " + (modelDropdown.visible ? "▴" : "▾")
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.fgColor
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
                        width: ctxRow.implicitWidth + 16
                        color: "transparent"
                        border.color: rootWindow.borderColor
                        border.width: 1

                        Row {
                            id: ctxRow
                            anchors.centerIn: parent
                            spacing: 6

                            Text {
                                text: (rootWindow.width >= 700 ? "FROM: " : "") + (typeof hostController !== "undefined" ? hostController.hostContextDisplayName.toUpperCase() : "")
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                font.bold: true
                                color: rootWindow.fgColor
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

                    // Status Badge
                    Rectangle {
                        height: 20
                        width: statusTextLabel.contentWidth + 12
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

                    // Pin Toggle [PIN]
                    Rectangle {
                        width: pinText.contentWidth + 10
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

                    // Window Minimize [-]
                    Rectangle {
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
                                    localFileInput.forceActiveFocus();
                                    rootWindow.showToast("SCOPE: SINGLE FILE ACTIVE");
                                }
                            }
                        }

                        // Local File Text Input (Active when local is selected)
                        Rectangle {
                            visible: rootWindow.activeScope === "local"
                            height: 18
                            Layout.preferredWidth: Math.min(190, Math.max(90, rootWindow.width - 430))
                            color: rootWindow.inputBgColor
                            border.color: localFileInput.activeFocus ? rootWindow.fgColor : rootWindow.borderColor
                            border.width: 1
                            radius: 2

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 4
                                anchors.rightMargin: 4
                                spacing: 4

                                Text {
                                    text: "FILE:"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.mutedTextColor
                                }

                                TextInput {
                                    id: localFileInput
                                    Layout.fillWidth: true
                                    text: rootWindow.targetLocalFile
                                    color: rootWindow.fgColor
                                    font.family: rootWindow.monoFont
                                    font.pixelSize: 8
                                    selectByMouse: true
                                    onTextChanged: rootWindow.targetLocalFile = text
                                }
                            }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        // Active Scope Target Tag
                        Text {
                            visible: rootWindow.width >= 690
                            text: rootWindow.activeScope === "global" ?
                                  "TARGET: ALL FILES" :
                                  "TARGET: " + (rootWindow.targetLocalFile.length > 0 ? rootWindow.targetLocalFile : "...")
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 8
                            color: rootWindow.activeScope === "global" ? "#7EE787" : "#FFB000"
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



                        // Prompt Terminal Canvas (Double Bounded Inset + Corner Crosshairs)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: rootWindow.inputBgColor
                            border.color: promptInput.activeFocus ? rootWindow.fgColor : rootWindow.borderColor
                            border.width: 1
                            radius: 6

                            // Corner Crosshairs
                            Text { text: "+"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4 }
                            Text { text: "+"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4 }
                            Text { text: "+"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 4 }
                            Text { text: "+"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4 }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
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
                                            text: rootWindow.hasAmbiguity ? "NEEDS CLARITY" : "READY"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: rootWindow.hasAmbiguity ? "#FFB000" : rootWindow.fgColor
                                        }
                                        Text {
                                            text: "█"
                                            font.family: rootWindow.monoFont
                                            font.pixelSize: 8
                                            color: rootWindow.fgColor
                                            SequentialAnimation on opacity {
                                                loops: Animation.Infinite
                                                NumberAnimation { from: 1; to: 0; duration: 420 }
                                                NumberAnimation { from: 0; to: 1; duration: 420 }
                                            }
                                        }
                                    }

                                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                    Text {
                                        visible: rootWindow.width >= 620
                                        text: "INPUT: VALID"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.fgColor
                                    }
                                    Text {
                                        text: rootWindow.hasAmbiguity ? "1 SUGGESTION" : "0 ISSUES"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.hasAmbiguity ? "#FFB000" : rootWindow.fgColor
                                    }
                                    Text {
                                        text: "AMBIGUITY: " + (rootWindow.ambiguityScore * 100).toFixed(0) + "%"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.fgColor
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
                            height: rootWindow.hasAmbiguity ? 30 : 22
                            color: rootWindow.hasAmbiguity ? "#140D04" : rootWindow.inputBgColor
                            border.color: rootWindow.hasAmbiguity ? "#FFB000" : rootWindow.borderColor
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
                                    text: rootWindow.hasAmbiguity ? "▲ SUGGESTION:" : "✦ STATUS:"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.hasAmbiguity ? "#FFB000" : "#7EE787"
                                }

                                // Message
                                Text {
                                    text: rootWindow.hasAmbiguity ?
                                          (rootWindow.width >= 700 ? ("Vague request ∙ Choose target in [" + rootWindow.activeProject.name + "]") : "Clarify target") :
                                          (rootWindow.width >= 700 ? ("PROMPT IS CLEAR ∙ SCOPE: " + (rootWindow.activeScope === "global" ? "ENTIRE PROJECT" : "SINGLE FILE (" + rootWindow.targetLocalFile + ")")) : "PROMPT CLEAR")
                                    font.family: rootWindow.monoFont
                                    font.bold: !rootWindow.hasAmbiguity
                                    font.pixelSize: 8
                                    color: rootWindow.hasAmbiguity ? rootWindow.fgColor : "#7EE787"
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: rootWindow.hasAmbiguity ? (rootWindow.width >= 760 ? 280 : 160) : 400
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                                // Resolution Pills (Rendered when ambiguity detected)
                                RowLayout {
                                    visible: rootWindow.hasAmbiguity
                                    spacing: 6

                                    Text {
                                        visible: rootWindow.width >= 740
                                        text: "CHOOSE SCOPE:"
                                        font.family: rootWindow.monoFont
                                        font.bold: true
                                        font.pixelSize: 8
                                        color: "#FFB000"
                                    }

                                    // Pill 1: Single File Lock
                                    Rectangle {
                                        height: 18
                                        width: res1Text.contentWidth + 10
                                        color: res1Mouse.containsMouse ? rootWindow.fgColor : "transparent"
                                        border.color: "#FFB000"
                                        border.width: 1
                                        radius: 3

                                        Text {
                                            id: res1Text
                                            anchors.centerIn: parent
                                            text: {
                                                const fn = rootWindow.activeScope === "local" ? rootWindow.targetLocalFile : "SessionManager.cpp";
                                                if (rootWindow.width >= 760) return "[ 1: THIS FILE (" + fn + ") ]";
                                                if (rootWindow.width >= 640) return "[ 1: " + fn + " ]";
                                                return "[ 1: FILE ]";
                                            }
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: res1Mouse.containsMouse ? rootWindow.bgColor : "#FFB000"
                                        }
                                        MouseArea {
                                            id: res1Mouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: rootWindow.commitChoice(1)
                                        }
                                    }

                                    // Pill 2: Entire Project
                                    Rectangle {
                                        height: 18
                                        width: res2Text.contentWidth + 10
                                        color: res2Mouse.containsMouse ? rootWindow.fgColor : "transparent"
                                        border.color: "#FFB000"
                                        border.width: 1
                                        radius: 3

                                        Text {
                                            id: res2Text
                                            anchors.centerIn: parent
                                            text: rootWindow.width >= 680 ? "[ 2: ENTIRE PROJECT ]" : "[ 2: ALL ]"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: res2Mouse.containsMouse ? rootWindow.bgColor : "#FFB000"
                                        }
                                        MouseArea {
                                            id: res2Mouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: rootWindow.commitChoice(2)
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

                        // Live Character & Word Counts (Visible on wider layouts)
                        Rectangle {
                            visible: rootWindow.width >= 660
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
                            visible: rootWindow.width >= 640
                            text: "CHARS: " + promptInput.text.length
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        Text {
                            visible: rootWindow.width >= 660
                            text: "|"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 9
                            color: rootWindow.mutedTextColor
                        }

                        Text {
                            visible: rootWindow.width >= 660
                            text: "WORDS: " + rootWindow.getWordCount(promptInput.text)
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        Text {
                            visible: rootWindow.width >= 660
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
                            Layout.maximumWidth: Math.max(120, rootWindow.width - 320)
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        // Clear Action Buttons (No vague glyphs, clean tactile styling)
                        // Button 1: AS-IS
                        Rectangle {
                            id: bypassBtn
                            height: 22
                            width: b1Text.contentWidth + 14
                            color: bypassMouse.pressed ? rootWindow.dimColor : (bypassMouse.containsMouse ? rootWindow.dimColor : "transparent")
                            border.color: rootWindow.borderColor
                            border.width: 1
                            radius: 3

                            Text {
                                id: b1Text
                                anchors.centerIn: parent
                                text: rootWindow.width >= 720 ? "[ SEND AS-IS ]" : "[ AS-IS ]"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.fgColor
                            }
                            MouseArea {
                                id: bypassMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: rootWindow.executeHardBypass()
                            }
                        }

                        // Button 2: ENHANCE (Primary Action)
                        Rectangle {
                            id: triageBtn
                            height: 22
                            width: b2Text.contentWidth + 16
                            color: triageMouse.pressed ? rootWindow.dimColor : (triageMouse.containsMouse ? "#E2E8F0" : rootWindow.fgColor)
                            border.color: rootWindow.borderColor
                            border.width: 1
                            radius: 3

                            Text {
                                id: b2Text
                                anchors.centerIn: parent
                                text: rootWindow.width >= 720 ? "[ ENHANCE & COPY ]" : "[ ENHANCE ]"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: triageMouse.pressed ? rootWindow.fgColor : rootWindow.bgColor
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

            // Dropdown Backdrop: Click outside dropdowns to close
            MouseArea {
                id: dropdownBackdrop
                anchors.fill: parent
                visible: modelDropdown.visible || projectDropdown.visible
                z: 990
                onClicked: {
                    modelDropdown.visible = false;
                    projectDropdown.visible = false;
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
                                        rootWindow.showToast("PROJECT: " + modelData.name);
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
                                    rootWindow.showToast("PROJECT FOLDER SELECTED");
                                }
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
