import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: rootWindow
    title: qsTr("Roclint")
    width: 780
    height: 480
    visible: false
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"

    property bool isClarificationActive: false
    property string toastMessage: ""

    function dismissCard() {
        isClarificationActive = false;
        toastMessage = "";
        if (typeof hostController !== "undefined") {
            hostController.hideCard()
        } else {
            rootWindow.hide()
        }
    }

    function executeHardBypass() {
        if (promptInput.text.trim().length === 0) return;
        showToast("[ ✓ 1-SHOT BYPASS COPIED TO CLIPBOARD ]");
        if (typeof hostController !== "undefined") {
            hostController.hardBypass(promptInput.text);
        }
    }

    function handleEnter() {
        if (promptInput.text.trim().length === 0) return;

        if (isClarificationActive) {
            commitChoice(1);
            return;
        }

        const lower = promptInput.text.toLowerCase();
        const ambiguousWords = ["refactor", "clean", "fix", "improve", "update", "change", "optimize", "rewrite"];
        const isAmbiguous = ambiguousWords.some(w => lower.includes(w));

        if (isAmbiguous) {
            isClarificationActive = true;
        } else {
            if (typeof hostController !== "undefined") {
                hostController.commitPrompt(promptInput.text);
                hostController.copyToClipboard(promptInput.text);
            }
            showToast("[ ✓ 1-SHOT PROMPT SEALED & COPIED ]");
            dismissTimer.start();
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
        showToast("[ ✓ INTENT SEALED // OPTION " + choiceNumber + " COPIED ]");
        isClarificationActive = false;
        dismissTimer.start();
    }

    function showToast(msg) {
        toastMessage = msg;
    }

    Timer {
        id: dismissTimer
        interval: 900
        repeat: false
        onTriggered: rootWindow.dismissCard()
    }

    Component.onCompleted: {
        x = (Screen.width - width) / 2
        y = (Screen.height - height) / 2
    }

    onVisibleChanged: {
        if (visible) {
            promptInput.forceActiveFocus();
        }
    }

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
        sequence: "Shift+Return"
        onActivated: rootWindow.executeHardBypass()
    }
    Shortcut {
        sequence: "Shift+Enter"
        onActivated: rootWindow.executeHardBypass()
    }

    // Outer Obsidian Container with Double ASCII Border (Zero Border Radius)
    Rectangle {
        id: cardRoot
        anchors.fill: parent
        color: "#07080A" // Pitch Obsidian Black
        border.color: isClarificationActive ? "#FFB000" : "#30363D"
        border.width: 1

        // Ambient ASCII character matrix dithering across the whole background
        Item {
            anchors.fill: parent
            anchors.margins: 6
            clip: true
            opacity: 0.06

            Text {
                readonly property int columns: 48
                readonly property int rows: 24
                text: {
                    const lines = []
                    for (let r = 0; r < rows; ++r) {
                        let line = ""
                        for (let c = 0; c < columns; ++c) {
                            const i = r * columns + c
                            line += (i % 7 === 0) ? "x" : ((i % 3 === 0) ? "+" : ((i % 5 === 0) ? ":" : "·"))
                        }
                        lines.push(line)
                    }
                    return lines.join("\n")
                }
                textFormat: Text.PlainText
                font.family: rootWindow.monoFont
                font.pixelSize: 11
                font.letterSpacing: 8
                lineHeightMode: Text.FixedHeight
                lineHeight: 20
                color: "#89DCEB"
            }
        }

        MouseArea {
            id: windowDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: rootWindow.startSystemMove()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            // ================================================================
            // HEADER BAR: CADROC // DEVELOPER // TOOL // V0.9.3 / ROCLINT HUD
            // ================================================================
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: "#0F1115"
                border.color: "#21262D"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Text {
                        text: "ROCLINT // INTENT INTERCEPTOR // HUD MODE v0.1.0"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 12
                        color: "#C9D1D9"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                    Rectangle {
                        height: 20
                        width: 75
                        color: "#161B22"
                        border.color: isClarificationActive ? "#FFB000" : "#238636"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: isClarificationActive ? "[ ! WARN ]" : "[ STATUS: OK ]"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 10
                            color: isClarificationActive ? "#FFB000" : "#7EE787"
                        }
                    }

                    Rectangle {
                        height: 20
                        width: 65
                        color: "#161B22"
                        border.color: "#30363D"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "[ 0.0 MS ]"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 10
                            color: "#8B949E"
                        }
                    }

                    // Window controls
                    Rectangle {
                        width: 24
                        height: 20
                        color: minMouse.containsMouse ? "#21262D" : "#161B22"
                        border.color: "#30363D"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "[-]"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 10
                            color: "#C9D1D9"
                        }
                        MouseArea {
                            id: minMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: rootWindow.isMac ? rootWindow.dismissCard() : rootWindow.showMinimized()
                        }
                    }

                    Rectangle {
                        width: 24
                        height: 20
                        color: closeMouse.containsMouse ? "#DA3633" : "#161B22"
                        border.color: "#30363D"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "[x]"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 10
                            color: "#C9D1D9"
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

            // ================================================================
            // PROMPT INPUT EDITOR
            // ================================================================
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0A0C10"
                border.color: "#21262D" // Zero blue: strict terminal border
                border.width: 1

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 10
                    clip: true

                    TextArea {
                        id: promptInput
                        placeholderText: qsTr("// $ prompt input ...\n// Enter raw developer prompt draft here.\n// Example: 'Refactor SessionManager token timeout handling and update tests'")
                        placeholderTextColor: "#3A424E"
                        color: "#E6EDF3"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 13
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        focus: true

                        Keys.onReturnPressed: function(event) {
                            if (event.modifiers & Qt.ShiftModifier) {
                                rootWindow.executeHardBypass();
                                event.accepted = true;
                            } else {
                                rootWindow.handleEnter();
                                event.accepted = true;
                            }
                        }

                        Keys.onDigit1Pressed: function(event) {
                            if (rootWindow.isClarificationActive && !(event.modifiers & Qt.ShiftModifier)) {
                                rootWindow.commitChoice(1);
                                event.accepted = true;
                            } else {
                                event.accepted = false;
                            }
                        }

                        Keys.onDigit2Pressed: function(event) {
                            if (rootWindow.isClarificationActive && !(event.modifiers & Qt.ShiftModifier)) {
                                rootWindow.commitChoice(2);
                                event.accepted = true;
                            } else {
                                event.accepted = false;
                            }
                        }
                    }
                }
            }

            // ================================================================
            // S1 TRIAGE INTERVENTION MICRO-CARD (CONDITION B: 1-SHOT GOVERNANCE)
            // ================================================================
            Rectangle {
                id: clarificationPanel
                visible: rootWindow.isClarificationActive
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                color: "#120E05" // Dark amber chamber
                border.color: "#FFB000"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "╔═[ [!] S1 TRIAGE :: MATERIAL AMBIGUITY DETECTED: 0.88 ]"
                            font.family: rootWindow.monoFont
                            font.bold: true
                            font.pixelSize: 11
                            color: "#FFB000"
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#442C00" }
                        Text {
                            text: "[ ESC: CANCEL ]═╗"
                            font.family: rootWindow.monoFont
                            font.pixelSize: 10
                            color: "#8B949E"
                        }
                    }

                    // Choice 1 Card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        color: c1Mouse.containsMouse ? "#2A200A" : "#1A1508"
                        border.color: c1Mouse.containsMouse ? "#FFB000" : "#593C00"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8
                            Text {
                                text: "[ 1 ]"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 12
                                color: "#89DCEB"
                            }
                            Text {
                                text: "MODULE SCOPE: Confine edits to active module. [AST: RevisionState.hpp:14]"
                                font.family: rootWindow.monoFont
                                font.pixelSize: 11
                                color: "#E6EDF3"
                            }
                        }
                        MouseArea {
                            id: c1Mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: rootWindow.commitChoice(1)
                        }
                    }

                    // Choice 2 Card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        color: c2Mouse.containsMouse ? "#2A200A" : "#1A1508"
                        border.color: c2Mouse.containsMouse ? "#FFB000" : "#593C00"
                        border.width: 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8
                            Text {
                                text: "[ 2 ]"
                                font.family: rootWindow.monoFont
                                font.bold: true
                                font.pixelSize: 12
                                color: "#89DCEB"
                            }
                            Text {
                                text: "WORKSPACE REFACTOR: Full project edits authorized with verification rollback proof."
                                font.family: rootWindow.monoFont
                                font.pixelSize: 11
                                color: "#E6EDF3"
                            }
                        }
                        MouseArea {
                            id: c2Mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: rootWindow.commitChoice(2)
                        }
                    }

                    Text {
                        text: "╚" + "═".repeat(80) + "╝"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 8
                        color: "#593C00"
                    }
                }
            }

            // ================================================================
            // RESOURCE PROFILER & TELEMETRY GAUGES (Grounding in Concept 4 & Slide 3)
            // ================================================================
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                color: "#0A0C10"
                border.color: "#21262D"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 12

                    Text {
                        text: "RESIDENT RAM: [███░░░░░░░░░] 40.2 MB"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 10
                        color: "#7EE787"
                    }

                    Rectangle { width: 1; height: 16; color: "#21262D" }

                    Text {
                        text: "LATENCY: 4.8 MS"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 10
                        color: "#89DCEB"
                    }

                    Rectangle { width: 1; height: 16; color: "#21262D" }

                    Text {
                        text: "CONTEXT BUDGET: [████████░░] 248 / 350 TOKENS"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 10
                        color: "#C9D1D9"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                    Text {
                        text: "IDLE CPU: 0.02%"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 10
                        color: "#7EE787"
                    }
                }
            }

            // ================================================================
            // FOOTER BAR: SHORTCUT GUIDE & ACTION STATUS
            // ================================================================
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                color: "#0F1115"
                border.color: "#21262D"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Text {
                        text: "[ CHARS: " + promptInput.length + " ]"
                        font.family: rootWindow.monoFont
                        font.pixelSize: 10
                        color: "#6E7681"
                    }

                    // Toast Feedback
                    Text {
                        visible: rootWindow.toastMessage.length > 0
                        text: rootWindow.toastMessage
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 11
                        color: "#7EE787"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "transparent" }

                    Text {
                        text: rootWindow.isClarificationActive ?
                              "[ 1 / 2: SELECT INTENT | ESC: CANCEL ]" :
                              "[ SHIFT+ENTER: HARD BYPASS | ENTER: 1-SHOT TRIAGE | ESC: HIDE ]"
                        font.family: rootWindow.monoFont
                        font.bold: true
                        font.pixelSize: 10
                        color: rootWindow.isClarificationActive ? "#FFB000" : "#89DCEB"
                    }
                }
            }
        }
    }
}
