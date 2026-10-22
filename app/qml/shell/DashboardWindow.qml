import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: dashboardWindow
    title: qsTr("ROCLINT // SYSTEM CONTROL CENTER & PREFERENCES")
    width: 940
    height: 620
    minimumWidth: 780
    minimumHeight: 520
    visible: false
    flags: Qt.FramelessWindowHint
    color: "transparent"

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Arial"

    // Multi-Palette Theme System (Mirrors FloatingCard.qml exactly)
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
        return "#C9D1D9";
    }

    readonly property color borderColor: {
        if (currentTheme === "light") return "#050505";
        if (currentTheme === "acid") return "#22C55E";
        if (currentTheme === "amber") return "#F59E0B";
        return "#6E7681";
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
        return "#8B949E";
    }

    readonly property color commentColor: {
        if (currentTheme === "light") return "#6B7280";
        if (currentTheme === "acid") return "#22C55E";
        if (currentTheme === "amber") return "#B45309";
        return "#8B949E";
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
        return "#3B4252";
    }

    // Active Navigation Tab Index (0: Models & Rewrite, 1: Integration & Theme, 2: SQLite History)
    property int currentTab: 0

    property string scorerMode: (typeof hostController !== "undefined" ? hostController.scorerEngine : "julia")
    property string globalShortcutString: isMac ? "Cmd+Shift+L" : "Ctrl+Shift+L"
    property string toastMessage: ""

    function showToast(msg) {
        toastMessage = msg;
        toastTimer.restart();
    }

    Timer {
        id: toastTimer
        interval: 2200
        onTriggered: dashboardWindow.toastMessage = ""
    }

    onClosing: function(closeEvent) {
        if (typeof hostController !== "undefined" && hostController) {
            hostController.syncDashboardVisibility(false);
        }
    }

    function dismissDashboard() {
        dashboardWindow.visible = false;
        if (typeof hostController !== "undefined" && hostController) {
            hostController.hideDashboard();
        }
    }

    Shortcut {
        sequence: "Escape"
        onActivated: dashboardWindow.dismissDashboard()
    }

    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: dashboardWindow.dismissDashboard()
    }

    // =========================================================================
    // POSTER BOARD CONTAINER (Matching FloatingCard.qml Double Bounding Box)
    // =========================================================================
    Rectangle {
        id: posterBoard
        anchors.fill: parent
        anchors.margins: 6
        color: dashboardWindow.bgColor
        border.color: dashboardWindow.borderColor
        border.width: 2
        radius: dashboardWindow.visibility === Window.Maximized ? 0 : 16

        MouseArea {
            id: windowDragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: dashboardWindow.startSystemMove()
            onDoubleClicked: function(mouse) {
                if (mouse.y <= 48) {
                    if (dashboardWindow.visibility === Window.Maximized) {
                        dashboardWindow.showNormal();
                        dashboardWindow.showToast("WINDOW RESTORED");
                    } else {
                        dashboardWindow.showMaximized();
                        dashboardWindow.showToast("WINDOW MAXIMIZED");
                    }
                }
            }
        }

        // =====================================================================
        // FRAMELESS WINDOW RESIZE HANDLES
        // =====================================================================
        MouseArea {
            id: resizeBottomRight
            anchors.right: parent.right; anchors.bottom: parent.bottom
            width: 14; height: 14; z: 100
            cursorShape: Qt.SizeFDiagCursor
            onPressed: dashboardWindow.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        }
        MouseArea {
            id: resizeBottomLeft
            anchors.left: parent.left; anchors.bottom: parent.bottom
            width: 14; height: 14; z: 100
            cursorShape: Qt.SizeBDiagCursor
            onPressed: dashboardWindow.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
        }
        MouseArea {
            id: resizeTopRight
            anchors.right: parent.right; anchors.top: parent.top
            width: 14; height: 14; z: 100
            cursorShape: Qt.SizeBDiagCursor
            onPressed: dashboardWindow.startSystemResize(Qt.RightEdge | Qt.TopEdge)
        }
        MouseArea {
            id: resizeTopLeft
            anchors.left: parent.left; anchors.top: parent.top
            width: 14; height: 14; z: 100
            cursorShape: Qt.SizeFDiagCursor
            onPressed: dashboardWindow.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
        }
        MouseArea {
            id: resizeRight
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 14; anchors.bottomMargin: 14
            width: 6; z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: dashboardWindow.startSystemResize(Qt.RightEdge)
        }
        MouseArea {
            id: resizeBottom
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 6; z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: dashboardWindow.startSystemResize(Qt.BottomEdge)
        }
        MouseArea {
            id: resizeLeft
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 14; anchors.bottomMargin: 14
            width: 6; z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: dashboardWindow.startSystemResize(Qt.LeftEdge)
        }
        MouseArea {
            id: resizeTop
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 6; z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: dashboardWindow.startSystemResize(Qt.TopEdge)
        }

        // Drop shadow inset border
        Rectangle {
            id: subBorderInset
            anchors.fill: parent
            anchors.margins: 5
            color: "transparent"
            border.color: dashboardWindow.borderColor
            border.width: 1
            radius: dashboardWindow.visibility === Window.Maximized ? 0 : 12

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                // =============================================================
                // 1. TOP HEADER BAR
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    spacing: 8

                    Rectangle {
                        width: 8
                        height: 8
                        color: dashboardWindow.fgColor
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            NumberAnimation { from: 1.0; to: 0.2; duration: 600; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: 0.2; to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
                        }
                    }

                    Text {
                        text: "ROCLINT"
                        font.family: dashboardWindow.monoFont
                        font.bold: true
                        font.pixelSize: 11
                        font.letterSpacing: 1
                        color: dashboardWindow.fgColor
                    }

                    Text { text: "│"; font.family: dashboardWindow.monoFont; font.pixelSize: 10; color: dashboardWindow.mutedTextColor }

                    Text {
                        text: "CONTROL CENTER & PREFERENCES"
                        font.family: dashboardWindow.monoFont
                        font.bold: true
                        font.pixelSize: 9
                        color: dashboardWindow.mutedTextColor
                    }

                    Item { Layout.fillWidth: true }

                    // Host resident status chip
                    Rectangle {
                        height: 20
                        width: resText.contentWidth + 16
                        color: "transparent"
                        border.color: "#7EE787"
                        border.width: 1

                        Text {
                            id: resText
                            anchors.centerIn: parent
                            text: "HOST: RESIDENT"
                            font.family: dashboardWindow.monoFont
                            font.bold: true
                            font.pixelSize: 8
                            color: "#7EE787"
                        }
                    }

                    // Window Minimize [-]
                    Rectangle {
                        id: minButton
                        width: 22
                        height: 20
                        color: minMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "[-]"
                            font.family: dashboardWindow.monoFont
                            font.pixelSize: 9
                            color: dashboardWindow.fgColor
                        }
                        MouseArea {
                            id: minMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dashboardWindow.showMinimized()
                        }
                    }

                    // Window Maximize [+] / [=]
                    Rectangle {
                        id: maxButton
                        width: 22
                        height: 20
                        color: maxMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: dashboardWindow.visibility === Window.Maximized ? "[=]" : "[+]"
                            font.family: dashboardWindow.monoFont
                            font.pixelSize: 9
                            color: dashboardWindow.fgColor
                        }
                        MouseArea {
                            id: maxMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (dashboardWindow.visibility === Window.Maximized) {
                                    dashboardWindow.showNormal();
                                } else {
                                    dashboardWindow.showMaximized();
                                }
                            }
                        }
                    }

                    // Window Close [x]
                    Rectangle {
                        id: closeButton
                        width: 22
                        height: 20
                        color: closeMouse.containsMouse ? "#8B1E1E" : "transparent"
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: "[x]"
                            font.family: dashboardWindow.monoFont
                            font.pixelSize: 9
                            color: dashboardWindow.fgColor
                        }
                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dashboardWindow.dismissDashboard()
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: dashboardWindow.borderColor
                }

                // =============================================================
                // 2. MAIN BODY (Sidebar Navigation Matrix + Content Tabs)
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 12

                    // LEFT SIDEBAR: TAB SELECTION MATRIX
                    Rectangle {
                        Layout.preferredWidth: 200
                        Layout.fillHeight: true
                        color: dashboardWindow.inputBgColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 8

                            Text {
                                text: "/* NAVIGATION MATRIX */"
                                font.family: dashboardWindow.monoFont
                                font.pixelSize: 9
                                font.bold: true
                                color: dashboardWindow.commentColor
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: dashboardWindow.borderColor
                                Layout.bottomMargin: 4
                            }

                            Repeater {
                                model: [
                                    { index: 0, tag: "01", label: "MODELS & REWRITE" },
                                    { index: 1, tag: "02", label: "INTEGRATION & THEME" },
                                    { index: 2, tag: "03", label: "SQLITE HISTORY" }
                                ]

                                delegate: Rectangle {
                                    id: navTabBtn
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 34
                                    color: dashboardWindow.currentTab === modelData.index
                                        ? dashboardWindow.fgColor
                                        : (tabMouse.containsMouse ? dashboardWindow.dimColor : "transparent")
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 8

                                        Text {
                                            text: "[" + modelData.tag + "]"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            font.bold: true
                                            color: dashboardWindow.currentTab === modelData.index
                                                ? dashboardWindow.bgColor
                                                : dashboardWindow.mutedTextColor
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.label
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            font.bold: dashboardWindow.currentTab === modelData.index
                                            color: dashboardWindow.currentTab === modelData.index
                                                ? dashboardWindow.bgColor
                                                : dashboardWindow.fgColor
                                            elide: Text.ElideRight
                                        }
                                    }

                                    MouseArea {
                                        id: tabMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: dashboardWindow.currentTab = modelData.index
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: dashboardWindow.borderColor
                            }

                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Text {
                                    text: "SYSTEM: ROCLINT CORE"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 8
                                    color: dashboardWindow.mutedTextColor
                                }
                                Text {
                                    text: "REVISION: v" + (typeof hostController !== "undefined" ? hostController.stateVersion : "0")
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 8
                                    color: dashboardWindow.mutedTextColor
                                }
                                Text {
                                    text: "BUILD: C++20 / QT 6.11"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 8
                                    color: dashboardWindow.mutedTextColor
                                }
                            }
                        }
                    }

                    // RIGHT CONTENT AREA
                    ScrollView {
                        id: rightScrollView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: rightScrollView.availableWidth
                            spacing: 14

                            // =================================================
                            // TAB 0: MODELS & REWRITE ENGINE
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 0
                                width: rightScrollView.availableWidth
                                spacing: 14

                                Text {
                                    text: "/* 01: INFERENCE RUNTIME & PROMPT COMPILER */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                // Card 1: Semantic Decision Scorer
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        RowLayout {
                                            Text {
                                                text: "SEMANTIC DECISION SCORER :: JEV CLOUD VS LOCAL JULIA-1"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: dashboardWindow.scorerMode === "jev" ? "[MODE: JEV CLOUD]" : "[MODE: LOCAL ONNX]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: juliaTxt.contentWidth + 16
                                                height: 26
                                                color: dashboardWindow.scorerMode === "julia" ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: juliaTxt
                                                    anchors.centerIn: parent
                                                    text: dashboardWindow.scorerMode === "julia" ? "[ ● LOCAL JULIA-1 (AIR-GAPPED CPU) ]" : "[ ○ LOCAL JULIA-1 (AIR-GAPPED CPU) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: dashboardWindow.scorerMode === "julia"
                                                    color: dashboardWindow.scorerMode === "julia" ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        dashboardWindow.scorerMode = "julia";
                                                        if (typeof hostController !== "undefined") hostController.setScorerEngine("julia");
                                                        dashboardWindow.showToast("S1 SCORER: LOCAL JULIA-1 (AIR-GAPPED DEFAULT)");
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                width: jevTxt.contentWidth + 16
                                                height: 26
                                                color: dashboardWindow.scorerMode === "jev" ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: jevTxt
                                                    anchors.centerIn: parent
                                                    text: dashboardWindow.scorerMode === "jev" ? "[ ● CLOUD JEV (BASELINE STUDY) ]" : "[ ○ CLOUD JEV (BASELINE STUDY) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: dashboardWindow.scorerMode === "jev"
                                                    color: dashboardWindow.scorerMode === "jev" ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        dashboardWindow.scorerMode = "jev";
                                                        if (typeof hostController !== "undefined") hostController.setScorerEngine("jev");
                                                        dashboardWindow.showToast("S1 SCORER: CLOUD JEV ACTIVATED");
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            text: dashboardWindow.scorerMode === "jev"
                                                ? "Cloud credentials stored in OS secure vault (macOS Keychain / WinCred). Satisfies D07."
                                                : "Local Julia-1 ONNX runs on CPU with <15ms latency. 100% offline air-gapped execution."
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            color: dashboardWindow.commentColor
                                        }
                                    }
                                }

                                // Card 2: Rewrite Profiles (Prompt Compiler)
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 170
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        RowLayout {
                                            Text {
                                                text: "REWRITE PROFILES (PROMPT COMPILER ARCHITECTURE)"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: "[ACTIVE: " + (typeof hostController !== "undefined" ? hostController.rewriteProfile.toUpperCase() : "GENERIC") + "]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: "#7EE787"
                                            }
                                        }

                                        ColumnLayout {
                                            spacing: 6

                                            Repeater {
                                                model: [
                                                    { id: "generic", label: "GENERIC", desc: "Clean markdown envelope with standardized instructions and constraints" },
                                                    { id: "anthropic_structured", label: "ANTHROPIC_STRUCTURED", desc: "Strict XML tags (<task>, <context>, <constraints>) for Claude 3.5 / 3.7" },
                                                    { id: "openai_structured", label: "OPENAI_STRUCTURED", desc: "Markdown heading hierarchy (# System / # Goal / # Instructions) for GPT-4o" },
                                                    { id: "local_compact", label: "LOCAL_COMPACT", desc: "Telegraphic, token-budget optimized encoding for local small models (3B)" }
                                                ]

                                                delegate: RowLayout {
                                                    spacing: 10

                                                    Rectangle {
                                                        width: profBtnTxt.contentWidth + 14
                                                        height: 22
                                                        color: (typeof hostController !== "undefined" && hostController.rewriteProfile.toLowerCase().indexOf(modelData.id) !== -1) ? dashboardWindow.fgColor : "transparent"
                                                        border.color: dashboardWindow.borderColor
                                                        border.width: 1

                                                        Text {
                                                            id: profBtnTxt
                                                            anchors.centerIn: parent
                                                            text: (typeof hostController !== "undefined" && hostController.rewriteProfile.toLowerCase().indexOf(modelData.id) !== -1) ? "[ ● " + modelData.label + " ]" : "[ ○ " + modelData.label + " ]"
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 8
                                                            font.bold: true
                                                            color: (typeof hostController !== "undefined" && hostController.rewriteProfile.toLowerCase().indexOf(modelData.id) !== -1) ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                        }

                                                        MouseArea {
                                                            anchors.fill: parent
                                                            cursorShape: Qt.PointingHandCursor
                                                            onClicked: {
                                                                if (typeof hostController !== "undefined") {
                                                                    hostController.setRewriteProfile(modelData.id);
                                                                    dashboardWindow.showToast("REWRITE PROFILE: " + modelData.label);
                                                                }
                                                            }
                                                        }
                                                    }

                                                    Text {
                                                        text: modelData.desc
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        color: dashboardWindow.commentColor
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // =================================================
                            // TAB 1: INTEGRATION & THEME
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 1
                                width: rightScrollView.availableWidth
                                spacing: 14

                                Text {
                                    text: "/* 02: EDITOR INTEGRATION & THEME */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                // Direct Editor Injection
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        RowLayout {
                                            Text {
                                                text: "DIRECT IN-PLACE EDITOR INJECTION (ACCESSIBILITY API)"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? "[STATUS: ENABLED]" : "[STATUS: DISABLED]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? "#7EE787" : dashboardWindow.mutedTextColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: injEnTxt.contentWidth + 14
                                                height: 24
                                                color: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: injEnTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? "[ ● ENABLED (Direct In-Place) ]" : "[ ○ ENABLED (Direct In-Place) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: (typeof hostController !== "undefined" && hostController.directInjectionEnabled)
                                                    color: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.directInjectionEnabled = true;
                                                            dashboardWindow.showToast("DIRECT INJECTION: ENABLED");
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                width: injDisTxt.contentWidth + 14
                                                height: 24
                                                color: (typeof hostController !== "undefined" && !hostController.directInjectionEnabled) ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: injDisTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && !hostController.directInjectionEnabled) ? "[ ● DISABLED (Clipboard Fallback) ]" : "[ ○ DISABLED (Clipboard Fallback) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: (typeof hostController !== "undefined" && !hostController.directInjectionEnabled)
                                                    color: (typeof hostController !== "undefined" && !hostController.directInjectionEnabled) ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.directInjectionEnabled = false;
                                                            dashboardWindow.showToast("DIRECT INJECTION: DISABLED (CLIPBOARD MODE)");
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            text: (typeof hostController !== "undefined" && hostController.hasAccessibilityPermission)
                                                ? "OS Accessibility API permission is granted. Prompts replace selected text directly in the active editor."
                                                : "When permissions are absent, Roclint cleanly copies enhanced prompts to your clipboard for instant Cmd+V paste."
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            color: dashboardWindow.commentColor
                                        }
                                    }
                                }

                                // Color Palette Theme
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        Text {
                                            text: "COLOR PALETTE THEME"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Repeater {
                                                model: [
                                                    { id: "dark", label: "[ OBSIDIAN SLATE ]", sample: "#050505" },
                                                    { id: "acid", label: "[ ACID PHOSPHOR ]", sample: "#070D08" },
                                                    { id: "amber", label: "[ AMBER GLOW CRT ]", sample: "#0F0902" },
                                                    { id: "light", label: "[ CLEAN LIGHT ]", sample: "#F4F4F4" }
                                                ]
                                                delegate: Rectangle {
                                                    width: thmTxt.contentWidth + 20
                                                    height: 34
                                                    color: modelData.sample
                                                    border.color: dashboardWindow.currentTheme === modelData.id ? dashboardWindow.fgColor : dashboardWindow.borderColor
                                                    border.width: dashboardWindow.currentTheme === modelData.id ? 2 : 1

                                                    Text {
                                                        id: thmTxt
                                                        anchors.centerIn: parent
                                                        text: modelData.label
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 9
                                                        font.bold: true
                                                        color: modelData.id === "light" ? "#050505" : "#C9D1D9"
                                                    }
                                                    MouseArea {
                                                        anchors.fill: parent
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            dashboardWindow.currentTheme = modelData.id;
                                                            dashboardWindow.showToast("THEME APPLIED: " + modelData.id.toUpperCase());
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // Global Shortcut Card
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 80
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Text {
                                            text: "GLOBAL INVOCATION SHORTCUT"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: hkTxt.contentWidth + 14
                                                height: 22
                                                color: dashboardWindow.dimColor
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1
                                                Text {
                                                    id: hkTxt
                                                    anchors.centerIn: parent
                                                    text: "[ " + dashboardWindow.globalShortcutString + " ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: true
                                                    color: dashboardWindow.fgColor
                                                }
                                            }
                                            Text {
                                                text: "Registered in OS native event loop. Anchors floating HUD at active cursor screen coordinates."
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.commentColor
                                            }
                                        }
                                    }
                                }
                            }

                            // =================================================
                            // TAB 2: SQLITE HISTORY & STORAGE
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 2
                                width: rightScrollView.availableWidth
                                spacing: 14

                                Text {
                                    text: "/* 03: SQLITE PROMPT REVISION STORAGE & LINEAGE */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 8

                                        Text {
                                            text: "SQLITE DATABASE STATUS (WAL MODE ACID STORAGE)"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            Text {
                                                text: "LOCATION:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 100
                                            }
                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.databasePath.length > 0) ? hostController.databasePath : "~/Library/Application Support/roclint/history.db"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                                elide: Text.ElideMiddle
                                                Layout.fillWidth: true
                                            }
                                        }

                                        RowLayout {
                                            Text {
                                                text: "STORAGE STATS:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 100
                                            }
                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.databaseStats.length > 0) ? hostController.databaseStats : "WAL MODE ENABLED  |  0 SESSIONS  |  SCHEMA v2"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: "#7EE787"
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: vacTxt.contentWidth + 14
                                                height: 24
                                                color: vacMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: vacTxt
                                                    anchors.centerIn: parent
                                                    text: "[ REFRESH STATS ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    id: vacMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.refreshStorageStats();
                                                        }
                                                        dashboardWindow.showToast("STORAGE STATS REFRESHED");
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                width: delHistTxt.contentWidth + 14
                                                height: 24
                                                color: delHistMouse.containsMouse ? "#8B1E1E" : "transparent"
                                                border.color: "#8B1E1E"
                                                border.width: 1

                                                Text {
                                                    id: delHistTxt
                                                    anchors.centerIn: parent
                                                    text: "[ PURGE HISTORY ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: "#EF4444"
                                                }
                                                MouseArea {
                                                    id: delHistMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.purgeHistory();
                                                        }
                                                        dashboardWindow.showToast("HISTORY PURGED");
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                Text {
                                    text: "/* RECENT PROMPT SESSIONS & REVISION LINEAGE */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                ListView {
                                    id: sessionListView
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 280
                                    clip: true
                                    spacing: 8
                                    model: (typeof hostController !== "undefined") ? hostController.getRecentSessions() : []

                                    Connections {
                                        target: typeof hostController !== "undefined" ? hostController : null
                                        function onSessionListChanged() {
                                            sessionListView.model = hostController.getRecentSessions();
                                        }
                                    }

                                    delegate: Rectangle {
                                        width: sessionListView.width
                                        height: 60
                                        color: dashboardWindow.inputBgColor
                                        border.color: dashboardWindow.borderColor
                                        border.width: 1

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 10
                                            spacing: 12

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 4

                                                RowLayout {
                                                    spacing: 8
                                                    Text {
                                                        text: modelData.sessionId
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 10
                                                        font.bold: true
                                                        color: "#58A6FF"
                                                    }
                                                    Text {
                                                        text: "• " + modelData.formattedTime
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 9
                                                        color: dashboardWindow.mutedTextColor
                                                    }
                                                }

                                                Text {
                                                    text: (typeof modelData.latestPrompt !== "undefined" && modelData.latestPrompt.length > 0) ? modelData.latestPrompt : "(empty prompt)"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.fgColor
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                }
                                            }

                                            Rectangle {
                                                width: restTxt.contentWidth + 12
                                                height: 22
                                                color: restMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: restTxt
                                                    anchors.centerIn: parent
                                                    text: "[ RESTORE ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    color: "#7EE787"
                                                }
                                                MouseArea {
                                                    id: restMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.restoreSession(modelData.sessionId);
                                                            dashboardWindow.showToast("SESSION RESTORED");
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                width: delItemTxt.contentWidth + 12
                                                height: 22
                                                color: delItemMouse.containsMouse ? "#8B1E1E" : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: delItemTxt
                                                    anchors.centerIn: parent
                                                    text: "[ DELETE ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    color: "#EF4444"
                                                }
                                                MouseArea {
                                                    id: delItemMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.deleteSession(modelData.sessionId);
                                                            dashboardWindow.showToast("SESSION DELETED");
                                                        }
                                                    }
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
                // 3. BOTTOM FOOTER CONTROLS
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    spacing: 10

                    Rectangle {
                        height: 24
                        width: toastTextLabel.contentWidth + 16
                        color: dashboardWindow.dimColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1
                        visible: dashboardWindow.toastMessage.length > 0

                        Text {
                            id: toastTextLabel
                            anchors.centerIn: parent
                            text: dashboardWindow.toastMessage
                            font.family: dashboardWindow.monoFont
                            font.pixelSize: 9
                            font.bold: true
                            color: dashboardWindow.fgColor
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // [ CLOSE ]
                    Rectangle {
                        width: closeBtnTxt.contentWidth + 18
                        height: 24
                        color: closeBtnMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        Text {
                            id: closeBtnTxt
                            anchors.centerIn: parent
                            text: "[ CLOSE ]"
                            font.family: dashboardWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: dashboardWindow.fgColor
                        }
                        MouseArea {
                            id: closeBtnMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dashboardWindow.dismissDashboard()
                        }
                    }
                }
            }
        }
    }
}
