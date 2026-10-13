import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("ROCLINT // RETROFUTURIST POSTER HUD")
    width: 800
    height: 490
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
        return "#F5F5F7";
    }

    readonly property color borderColor: {
        if (currentTheme === "light") return "#050505";
        if (currentTheme === "acid") return "#22C55E";
        if (currentTheme === "amber") return "#F59E0B";
        return "#F5F5F7";
    }

    readonly property color dimColor: {
        if (currentTheme === "light") return "#D1D5DB";
        if (currentTheme === "acid") return "#14331B";
        if (currentTheme === "amber") return "#3B2207";
        return "#262626";
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
        return "#F5F5F7";
    }

    property bool isClarificationActive: false
    property string toastMessage: ""
    property real liveLatency: 4.8
    property real liveCpu: 0.02
    property real liveRam: 40.2
    property int maxTokens: 350

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
        showToast("PIPELINE BYPASSED // DIRECT DISPATCH");
        if (typeof hostController !== "undefined") {
            hostController.hardBypass(promptInput.text);
        }
    }

    function handleEnter() {
        if (promptInput.text.trim().length === 0) {
            showToast("ERROR: PROMPT BUFFER EMPTY");
            return;
        }

        if (isClarificationActive) {
            commitChoice(1);
            return;
        }

        const lower = promptInput.text.toLowerCase();
        const ambiguousWords = ["refactor", "clean", "fix", "improve", "update", "change", "optimize", "rewrite"];
        const isAmbiguous = ambiguousWords.some(w => lower.includes(w));

        if (isAmbiguous) {
            isClarificationActive = true;
            showToast("AMBIGUITY DETECTED // S1 CLARIFICATION OPEN");
        } else {
            if (typeof hostController !== "undefined") {
                hostController.commitPrompt(promptInput.text);
                hostController.copyToClipboard(promptInput.text);
            }
            showToast("1-SHOT PROMPT SEALED & COPIED");
        }
    }

    function commitChoice(choiceNumber) {
        let compiled = "";
        if (choiceNumber === 1) {
            compiled = "/* ROCLINT [SCOPE_LOCK: MODULE_LOCAL] */\n" +
                       "/* AST ATTACHED: roclint::core::RevisionState */\n" +
                       promptInput.text.trim() + "\n" +
                       "/* CONSTRAINTS: ZERO BREAKING CHANGES OUTSIDE ACTIVE MODULE */";
        } else {
            compiled = "/* ROCLINT [SCOPE_LOCK: WORKSPACE_REFACTOR] */\n" +
                       "/* AST ATTACHED: Workspace AST Trigram Index */\n" +
                       promptInput.text.trim() + "\n" +
                       "/* CONSTRAINTS: DETERMINISTIC ROLLBACK PROOF REQUIRED */";
        }

        if (typeof hostController !== "undefined") {
            hostController.commitPrompt(compiled);
            hostController.copyToClipboard(compiled);
        }
        showToast("INTENT SEALED // OPTION " + choiceNumber + " COPIED");
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

    function injectPreset(key) {
        if (key === "refactor") {
            promptInput.text = "Refactor SessionManager token timeout handling and update active test suite with boundary assertions.";
        } else if (key === "cuda") {
            promptInput.text = "Optimize CUDA warp dispatch matrix for FP16 tensor broadcast on SM_90 architecture with zero bank conflict.";
        } else if (key === "auth") {
            promptInput.text = "Inject cryptographic HMAC-SHA256 signature verification filter before route controller dispatch.";
        } else if (key === "bench") {
            promptInput.text = "Benchmark resident heap allocation under sustained 20k RPS throughput; output telemetry breakdown.";
        }
        showToast("PRESET LOADED: [" + key.toUpperCase() + "]");
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
        }
    }

    // Keyboard Shortcuts
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (rootWindow.isClarificationActive) {
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
        enabled: rootWindow.isClarificationActive
        onActivated: rootWindow.commitChoice(1)
    }

    Shortcut {
        sequences: ["Alt+2", "Ctrl+2"]
        enabled: rootWindow.isClarificationActive
        onActivated: rootWindow.commitChoice(2)
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

        // SUB-BORDER INSET (Double Bounding Frame)
        Rectangle {
            id: subBorderInset
            anchors.fill: parent
            anchors.margins: 10
            color: rootWindow.bgColor
            border.color: rootWindow.borderColor
            border.width: 1
            radius: 10

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
                        text: "ROCLINT"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 10
                        font.letterSpacing: 1
                        color: rootWindow.fgColor
                    }

                    // Model Hot-Swap Pill (Ollama / Unsloth style)
                    Rectangle {
                        height: 20
                        width: modelRow.width + 12
                        color: modelMouse.containsMouse ? rootWindow.dimColor : "transparent"
                        border.color: rootWindow.borderColor
                        border.width: 1

                        RowLayout {
                            id: modelRow
                            anchors.centerIn: parent
                            spacing: 5

                            Text {
                                text: "MODEL:"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 8
                                color: rootWindow.dimColor
                            }
                            Text {
                                text: rootWindow.activeModel.name + " (" + rootWindow.activeModel.quant + ") ▾"
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
                                rootWindow.currentModelIndex = (rootWindow.currentModelIndex + 1) % rootWindow.modelList.length;
                                rootWindow.showToast("HOT-SWAPPED: " + rootWindow.activeModel.name);
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                    // Status Badge
                    Rectangle {
                        height: 20
                        width: statusTextLabel.contentWidth + 12
                        color: "transparent"
                        border.color: isClarificationActive ? "#FFB000" : rootWindow.borderColor
                        border.width: 1

                        Text {
                            id: statusTextLabel
                            anchors.centerIn: parent
                            text: isClarificationActive ? "[ STATUS: WARN ]" : "[ STATUS: OK ]"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: isClarificationActive ? "#FFB000" : rootWindow.fgColor
                        }
                    }

                    // Latency Pill
                    Rectangle {
                        height: 20
                        width: 76
                        color: "transparent"
                        border.color: rootWindow.borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "[ " + rootWindow.liveLatency.toFixed(1) + " MS ]"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }
                    }

                    // Multi-Palette Theme Switchers (MONO, LIGHT, ACID, AMBR)
                    Rectangle {
                        height: 20
                        width: themeRow.width + 4
                        color: "transparent"
                        border.color: rootWindow.borderColor
                        border.width: 1

                        Row {
                            id: themeRow
                            anchors.centerIn: parent
                            spacing: 2

                            Repeater {
                                model: [
                                    { name: "dark", label: "MONO" },
                                    { name: "light", label: "LIGHT" },
                                    { name: "acid", label: "ACID" },
                                    { name: "amber", label: "AMBR" }
                                ]
                                delegate: Rectangle {
                                    width: tText.contentWidth + 6
                                    height: 16
                                    color: rootWindow.currentTheme === modelData.name ? rootWindow.fgColor : "transparent"

                                    Text {
                                        id: tText
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        font.family: rootWindow.monoFont
                                        font.bold: true
                                        font.pixelSize: 8
                                        color: rootWindow.currentTheme === modelData.name ? rootWindow.bgColor : rootWindow.fgColor
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            rootWindow.currentTheme = modelData.name;
                                            rootWindow.showToast("THEME: " + modelData.label + " APPLIED");
                                        }
                                    }
                                }
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
                // 2. CENTRAL PROMPT INGESTION & INTERACTION MATRIX
                // =============================================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6

                    // Presets & Ingestion Header Strip
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: "PROMPT INGESTION"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            font.letterSpacing: 1
                            color: rootWindow.fgColor
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        Text {
                            text: "PRESETS:"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 8
                            color: rootWindow.fgColor
                            opacity: 0.6
                        }

                        // Preset Buttons
                        Repeater {
                            model: ["refactor", "cuda", "auth", "bench"]
                            delegate: Rectangle {
                                height: 18
                                width: pText.contentWidth + 10
                                color: pMouse.containsMouse ? rootWindow.fgColor : "transparent"
                                border.color: rootWindow.borderColor
                                border.width: 1

                                    Text {
                                        id: pText
                                        anchors.centerIn: parent
                                        text: modelData.toUpperCase()
                                        font.family: rootWindow.monoFont
                                        font.bold: true
                                        font.pixelSize: 8
                                        color: pMouse.containsMouse ? rootWindow.bgColor : rootWindow.fgColor
                                    }
                                    MouseArea {
                                        id: pMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: rootWindow.injectPreset(modelData)
                                    }
                                }
                            }
                        }

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
                                        placeholderText: qsTr("// $ prompt input ...\n// Enter raw developer prompt draft here.\n// Example: 'Refactor SessionManager token timeout handling and update active tests'")
                                        placeholderTextColor: rootWindow.dimColor
                                        color: rootWindow.fgColor
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 12
                                        wrapMode: TextEdit.Wrap
                                        selectByMouse: true
                                        focus: true

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
                                            text: "DAEMON_INTERCEPT:"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: rootWindow.fgColor
                                        }
                                        Text {
                                            text: isClarificationActive ? "TRIAGE_WARNING" : "LISTENING"
                                            font.family: rootWindow.monoFont
                                            font.bold: true
                                            font.pixelSize: 8
                                            color: isClarificationActive ? "#FFB000" : rootWindow.fgColor
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
                                        text: "SYNTAX: VALID"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.fgColor
                                    }
                                    Text {
                                        text: isClarificationActive ? "LINT: 1 AMBIGUITY" : "LINT: 0 WARNINGS"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: isClarificationActive ? "#FFB000" : rootWindow.fgColor
                                    }
                                    Text {
                                        text: "ENTROPY: 0.7842 λ"
                                        font.family: rootWindow.monoFont
                                        font.pixelSize: 8
                                        color: rootWindow.fgColor
                                    }
                                }
                            }
                        }

                        // S1 Clarification Choices (When Active)
                        ColumnLayout {
                            visible: rootWindow.isClarificationActive
                            Layout.fillWidth: true
                            spacing: 4

                            Rectangle {
                                Layout.fillWidth: true
                                height: 26
                                color: opt1Mouse.containsMouse ? rootWindow.dimColor : "transparent"
                                border.color: "#FFB000"
                                border.width: 1

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    spacing: 6
                                    Text { text: "[ 1 / Alt+1 ]"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 9; color: "#FFB000" }
                                    Text { text: "MODULE SCOPE: Confine edits to active module [AST: RevisionState.hpp:14]"; font.family: rootWindow.monoFont; font.pixelSize: 9; color: rootWindow.fgColor }
                                }
                                MouseArea {
                                    id: opt1Mouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: rootWindow.commitChoice(1)
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 26
                                color: opt2Mouse.containsMouse ? rootWindow.dimColor : "transparent"
                                border.color: "#FFB000"
                                border.width: 1

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    spacing: 6
                                    Text { text: "[ 2 / Alt+2 ]"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 9; color: "#FFB000" }
                                    Text { text: "WORKSPACE REFACTOR: Full project edits authorized with rollback proof"; font.family: rootWindow.monoFont; font.pixelSize: 9; color: rootWindow.fgColor }
                                }
                                MouseArea {
                                    id: opt2Mouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: rootWindow.commitChoice(2)
                                }
                            }
                        }
                    }

                // =============================================================
                // 3. INDUSTRIAL TELEMETRY GAUGES (4 Columns)
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 46
                    Layout.maximumHeight: 48
                    Layout.fillHeight: false
                    spacing: 8

                    // Gauge 1: Resident RAM
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
                                Text { text: "RESIDENT RAM:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
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
                                Text { text: "±0.02"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.6 }
                            }
                        }
                    }

                    // Gauge 3: Context Budget
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
                                Text { text: "CONTEXT BUDGET:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text {
                                    text: Math.ceil(promptInput.text.length / 4) + " / " + rootWindow.maxTokens + " TOKENS"
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

                    // Gauge 4: Idle CPU
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
                                Text { text: "IDLE CPU:"; font.family: rootWindow.monoFont; font.pixelSize: 8; color: rootWindow.fgColor; opacity: 0.75 }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Text { text: rootWindow.liveCpu.toFixed(2) + "%"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 9; color: rootWindow.fgColor }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: "64 THREADS"; font.family: rootWindow.monoFont; font.bold: true; font.pixelSize: 10; color: rootWindow.fgColor }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }
                                Rectangle {
                                    height: 12
                                    width: nomText.contentWidth + 6
                                    color: rootWindow.fgColor
                                    Text {
                                        id: nomText
                                        anchors.centerIn: parent
                                        text: "NOMINAL"
                                        font.family: rootWindow.monoFont
                                        font.bold: true
                                        font.pixelSize: 7
                                        color: rootWindow.bgColor
                                    }
                                }
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

                        // Live Character & Word Counts
                        Rectangle {
                            height: 16
                            width: cTag.contentWidth + 6
                            color: rootWindow.fgColor
                            Text {
                                id: cTag
                                anchors.centerIn: parent
                                text: "COUNT"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 8
                                color: rootWindow.bgColor
                            }
                        }

                        Text {
                            text: "CHARS: " + promptInput.text.length
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        Text {
                            text: "|"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 9
                            color: rootWindow.dimColor
                        }

                        Text {
                            text: "WORDS: " + rootWindow.getWordCount(promptInput.text)
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        // System Toast Banner
                        Text {
                            visible: rootWindow.toastMessage.length > 0
                            text: "✦ " + rootWindow.toastMessage
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: rootWindow.fgColor
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                        // Tactile Neo-Brutalist Hard-Shadow Buttons
                        // Button 1: SHIFT+ENTER: HARD BYPASS
                        Item {
                            width: bypassInner.width + 4
                            height: 24

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 0
                                anchors.leftMargin: 3
                                anchors.topMargin: 3
                                color: rootWindow.shadowColor
                            }

                            Rectangle {
                                id: bypassInner
                                width: b1Text.contentWidth + 14
                                height: 22
                                x: bypassMouse.pressed ? 2 : (bypassMouse.containsMouse ? -1 : 0)
                                y: bypassMouse.pressed ? 2 : (bypassMouse.containsMouse ? -1 : 0)
                                color: rootWindow.bgColor
                                border.color: rootWindow.borderColor
                                border.width: 1

                                Text {
                                    id: b1Text
                                    anchors.centerIn: parent
                                    text: "[ SHIFT+ENTER: HARD BYPASS ]"
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
                        }

                        // Button 2: ENTER: 1-SHOT TRIAGE
                        Item {
                            width: triageInner.width + 4
                            height: 24

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 3
                                anchors.topMargin: 3
                                color: rootWindow.shadowColor
                            }

                            Rectangle {
                                id: triageInner
                                width: b2Text.contentWidth + 14
                                height: 22
                                x: triageMouse.pressed ? 2 : (triageMouse.containsMouse ? -1 : 0)
                                y: triageMouse.pressed ? 2 : (triageMouse.containsMouse ? -1 : 0)
                                color: rootWindow.fgColor
                                border.color: rootWindow.borderColor
                                border.width: 1

                                Text {
                                    id: b2Text
                                    anchors.centerIn: parent
                                    text: "[ ENTER: 1-SHOT TRIAGE ]"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.bgColor
                                }
                                MouseArea {
                                    id: triageMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: rootWindow.handleEnter()
                                }
                            }
                        }

                        // Button 3: COPY_PAYLOAD
                        Item {
                            width: copyInner.width + 4
                            height: 24

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 3
                                anchors.topMargin: 3
                                color: rootWindow.shadowColor
                            }

                            Rectangle {
                                id: copyInner
                                width: b3Text.contentWidth + 14
                                height: 22
                                x: copyMouse.pressed ? 2 : (copyMouse.containsMouse ? -1 : 0)
                                y: copyMouse.pressed ? 2 : (copyMouse.containsMouse ? -1 : 0)
                                color: rootWindow.bgColor
                                border.color: rootWindow.borderColor
                                border.width: 1

                                Text {
                                    id: b3Text
                                    anchors.centerIn: parent
                                    text: "[ COPY_PAYLOAD ]"
                                    font.family: rootWindow.monoFont
                                    font.bold: true
                                    font.pixelSize: 8
                                    color: rootWindow.fgColor
                                }
                                MouseArea {
                                    id: copyMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (promptInput.text.trim().length === 0) {
                                            rootWindow.showToast("BUFFER EMPTY: NOTHING TO COPY");
                                            return;
                                        }
                                        if (typeof hostController !== "undefined") {
                                            hostController.copyToClipboard(promptInput.text);
                                        }
                                        rootWindow.showToast("PAYLOAD COPIED TO SYSTEM CLIPBOARD");
                                    }
                                }
                            }
                        }

                        // Button 4: ESC: HIDE
                        Rectangle {
                            height: 22
                            width: hideTextLabel.contentWidth + 12
                            color: hideMouse.containsMouse ? rootWindow.dimColor : "transparent"
                            border.color: rootWindow.borderColor
                            border.width: 1

                            Text {
                                id: hideTextLabel
                                anchors.centerIn: parent
                                text: "[ ESC: HIDE ]"
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
    }
}
