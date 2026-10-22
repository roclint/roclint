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

    // Active Navigation Tab Index
    property int currentTab: 0

    // Settings Model Properties
    property string graniteQuantization: "Q8_0"
    property string graniteResidency: "IDLE_MAPPED"
    property int contextLength: 4096
    property string scorerMode: (typeof hostController !== "undefined" ? hostController.scorerEngine : "julia") // "julia" (default) or "jev"
    property string defaultRewriteProfile: "ANTHROPIC_STRUCTURED"
    property string defaultScopePreference: "global"
    property bool passiveHostContextEnabled: true
    property string globalShortcutString: isMac ? "Cmd+Shift+L" : "Ctrl+Shift+L"
    property bool telemetryOptIn: true
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
        // FRAMELESS WINDOW RESIZE HANDLES (All 4 Edges & Corners)
        // =====================================================================
        MouseArea {
            id: resizeBottomRight
            anchors.right: parent.right; anchors.bottom: parent.bottom
            width: 14; height: 14; z: 100
            cursorShape: Qt.SizeFDiagCursor
            onPressed: dashboardWindow.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
        }
        MouseArea {
            id: resizeRight
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 14; anchors.bottomMargin: 14
            width: 7; z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: dashboardWindow.startSystemResize(Qt.RightEdge)
        }
        MouseArea {
            id: resizeBottom
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 7; z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: dashboardWindow.startSystemResize(Qt.BottomEdge)
        }
        MouseArea {
            id: resizeBottomLeft
            anchors.left: parent.left; anchors.bottom: parent.bottom
            width: 14; height: 14; z: 100
            cursorShape: Qt.SizeBDiagCursor
            onPressed: dashboardWindow.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
        }
        MouseArea {
            id: resizeLeft
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 14; anchors.bottomMargin: 14
            width: 7; z: 100
            cursorShape: Qt.SizeHorCursor
            onPressed: dashboardWindow.startSystemResize(Qt.LeftEdge)
        }
        MouseArea {
            id: resizeTop
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 7; z: 100
            cursorShape: Qt.SizeVerCursor
            onPressed: dashboardWindow.startSystemResize(Qt.TopEdge)
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

        // =====================================================================
        // SUB-BORDER INSET (Double Bounding Frame)
        // =====================================================================
        Rectangle {
            id: subBorderInset
            anchors.fill: parent
            anchors.margins: dashboardWindow.visibility === Window.Maximized ? 6 : 10
            color: dashboardWindow.bgColor
            border.color: dashboardWindow.borderColor
            border.width: 1
            radius: dashboardWindow.visibility === Window.Maximized ? 0 : 10
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                // =============================================================
                // 1. TOP HEADER BAR: System Branding, Status, Window Controls
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    spacing: 8

                    // Pulsing status dot (green)
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
                        font.pixelSize: 10
                        font.letterSpacing: 1
                        color: dashboardWindow.fgColor
                    }

                    Rectangle {
                        width: 1; height: 14
                        color: dashboardWindow.borderColor
                    }

                    Text {
                        text: "CONTROL CENTER & PREFERENCES"
                        font.family: dashboardWindow.monoFont
                        font.bold: true
                        font.pixelSize: 9
                        font.letterSpacing: 1
                        color: dashboardWindow.mutedTextColor
                    }

                    Item { Layout.fillWidth: true }

                    // Status Badge
                    Rectangle {
                        id: hostStatusBadge
                        implicitHeight: 20
                        implicitWidth: hostStatusLabel.implicitWidth + 12
                        Layout.preferredHeight: 20
                        Layout.preferredWidth: implicitWidth
                        height: 20
                        width: implicitWidth
                        color: "transparent"
                        border.color: "#7EE787"
                        border.width: 1

                        Text {
                            id: hostStatusLabel
                            anchors.centerIn: parent
                            text: typeof hostController !== "undefined" ? "HOST: " + hostController.hostStateString : "HOST: RESIDENT"
                            font.family: dashboardWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: "#7EE787"
                        }
                    }

                    // Window Minimize [-]
                    Rectangle {
                        id: minButton
                        width: 22
                        height: 20
                        implicitWidth: 22
                        implicitHeight: 20
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 20
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

                    // Window Maximize / Restore Toggle [+] / [=]
                    Rectangle {
                        id: maxButton
                        width: 22
                        height: 20
                        implicitWidth: 22
                        implicitHeight: 20
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 20
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
                                    dashboardWindow.showToast("WINDOW RESTORED");
                                } else {
                                    dashboardWindow.showMaximized();
                                    dashboardWindow.showToast("WINDOW MAXIMIZED");
                                }
                            }
                        }
                    }

                    // Window Close [x]
                    Rectangle {
                        id: closeButton
                        width: 22
                        height: 20
                        implicitWidth: 22
                        implicitHeight: 20
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 20
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

                // Divider line
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: dashboardWindow.borderColor
                }

                // =============================================================
                // 2. MAIN BODY: LEFT SIDEBAR MATRIX + RIGHT CONTENT VIEWPORT
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 12

                    // LEFT SIDEBAR TABS (Brutalist Terminal Matrix)
                    Rectangle {
                        Layout.fillHeight: true
                        Layout.preferredWidth: 230
                        color: dashboardWindow.inputBgColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        // Corner crosshairs
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4 }
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4 }
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 4 }
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4 }

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
                                    { index: 0, tag: "01", label: "MODELS & RUNTIME" },
                                    { index: 1, tag: "02", label: "WORKSPACES & AST" },
                                    { index: 2, tag: "03", label: "APPEARANCE & KEYS" },
                                    { index: 3, tag: "04", label: "PERSISTENCE & SQL" },
                                    { index: 4, tag: "05", label: "STUDY & TELEMETRY" }
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

                            // System Info Footer in Sidebar
                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: dashboardWindow.borderColor
                            }

                            ColumnLayout {
                                spacing: 2
                                Text {
                                    text: "SYSTEM: ROCLINT CORE v0.1.0"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 8
                                    color: dashboardWindow.mutedTextColor
                                }
                                Text {
                                    text: typeof hostController !== "undefined"
                                        ? "REVISION: " + hostController.currentRevisionId + " (v" + hostController.stateVersion + ")"
                                        : "REVISION: rev_0 (v0)"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 8
                                    color: dashboardWindow.mutedTextColor
                                }
                                Text {
                                    text: "BUILD: C++20 / QT 6.11"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 8
                                    color: dashboardWindow.commentColor
                                }
                            }
                        }
                    }

                    // RIGHT CONTENT VIEWPORT
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: dashboardWindow.bgColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        // Corner crosshairs for viewport
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4 }
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4 }
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 4 }
                        Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4 }

                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 18
                            clip: true
                            contentWidth: availableWidth

                            // =================================================
                            // TAB 0: MODELS & INFERENCE RUNTIME
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 0
                                width: parent.width
                                spacing: 14

                                Text {
                                    text: "/* 01: IBM GRANITE 3B & DECISION INFERENCE RUNTIME */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                // Card 1: IBM Granite 3B Local Runtime
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 180
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    // Corner crosshairs
                                    Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4 }
                                    Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4 }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        RowLayout {
                                            Text {
                                                text: "LOCAL RUNTIME (llama.cpp) :: ibm-granite/granite-4.2-3b-GGUF"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: "[STATUS: READY]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: "#7EE787"
                                            }
                                        }

                                        // Quantization Selector
                                        RowLayout {
                                            spacing: 12
                                            Text {
                                                text: "QUANTIZATION:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 120
                                            }
                                            RowLayout {
                                                spacing: 8
                                                Repeater {
                                                    model: [
                                                        { id: "Q4_K_M", label: "[ Q4_K_M :: 2.1 GB :: FAST ]" },
                                                        { id: "Q5_K_M", label: "[ Q5_K_M :: 2.6 GB :: BALANCED ]" },
                                                        { id: "Q8_0", label: "[ Q8_0 :: 3.4 GB :: MAXIMUM FIDELITY ]" }
                                                    ]
                                                    delegate: Rectangle {
                                                        width: quantTxt.contentWidth + 16
                                                        height: 24
                                                        color: dashboardWindow.graniteQuantization === modelData.id ? dashboardWindow.fgColor : "transparent"
                                                        border.color: dashboardWindow.borderColor
                                                        border.width: 1

                                                        Text {
                                                            id: quantTxt
                                                            anchors.centerIn: parent
                                                            text: modelData.label
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 9
                                                            font.bold: dashboardWindow.graniteQuantization === modelData.id
                                                            color: dashboardWindow.graniteQuantization === modelData.id ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                        }
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            cursorShape: Qt.PointingHandCursor
                                                            onClicked: dashboardWindow.graniteQuantization = modelData.id
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Residency Policy Selector (B10R)
                                        RowLayout {
                                            spacing: 12
                                            Text {
                                                text: "RESIDENCY (B10R):"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 120
                                            }
                                            RowLayout {
                                                spacing: 8
                                                Repeater {
                                                    model: [
                                                        { id: "IDLE_MAPPED", label: "[ IDLE_MAPPED (MMAP) ]" },
                                                        { id: "HOT", label: "[ HOT (PINNED RAM) ]" },
                                                        { id: "UNLOADED", label: "[ UNLOADED ]" }
                                                    ]
                                                    delegate: Rectangle {
                                                        width: resTxt.contentWidth + 14
                                                        height: 24
                                                        color: dashboardWindow.graniteResidency === modelData.id ? dashboardWindow.fgColor : "transparent"
                                                        border.color: dashboardWindow.borderColor
                                                        border.width: 1

                                                        Text {
                                                            id: resTxt
                                                            anchors.centerIn: parent
                                                            text: modelData.label
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 9
                                                            font.bold: dashboardWindow.graniteResidency === modelData.id
                                                            color: dashboardWindow.graniteResidency === modelData.id ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                        }
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            cursorShape: Qt.PointingHandCursor
                                                            onClicked: dashboardWindow.graniteResidency = modelData.id
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Rewrite Profile Default (B08P)
                                        RowLayout {
                                            spacing: 12
                                            Text {
                                                text: "REWRITE PROFILE:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 120
                                            }
                                            Text {
                                                text: "ANTHROPIC_STRUCTURED (Strict XML envelope & scratchpads)"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                        }
                                    }
                                }

                                // Card 2: Semantic Decision Scorer (C03 / D51)
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    // Corner crosshairs
                                    Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4 }
                                    Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4 }

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
                            }

                            // =================================================
                            // TAB 1: WORKSPACES & AST SCOPE
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 1
                                width: parent.width
                                spacing: 14

                                Text {
                                    text: "/* 02: WORKSPACES & AST CARTRIDGE SCOPE */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 180
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.left: parent.left; anchors.top: parent.top; anchors.margins: 4 }
                                    Text { text: "+"; font.family: dashboardWindow.monoFont; font.pixelSize: 8; color: dashboardWindow.fgColor; opacity: 0.6; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4 }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        Text {
                                            text: "INDEXED PROJECTS & AST SYMBOLS"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        Repeater {
                                            model: [
                                                { name: "[ roclint ]", path: "/Users/jesse/roclint", status: "[INDEXED: 7 FILES, 142 SYMBOLS]" },
                                                { name: "[ Stardust_v4_2 ]", path: "/Users/jesse/Stardust_v4_2 2", status: "[INDEXED: 18 SPECS]" }
                                            ]
                                            delegate: RowLayout {
                                                Layout.fillWidth: true
                                                Text {
                                                    text: modelData.name
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: true
                                                    color: dashboardWindow.fgColor
                                                    Layout.preferredWidth: 140
                                                }
                                                Text {
                                                    text: modelData.path
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.mutedTextColor
                                                    Layout.fillWidth: true
                                                    elide: Text.ElideMiddle
                                                }
                                                Text {
                                                    text: modelData.status
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: "#7EE787"
                                                }
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: addDirTxt.contentWidth + 14
                                                height: 24
                                                color: addDirMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: addDirTxt
                                                    anchors.centerIn: parent
                                                    text: "[ + ADD PROJECT FOLDER ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    id: addDirMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.showToast("PROJECT SELECTOR OPENED")
                                                }
                                            }
                                            Rectangle {
                                                width: reindexTxt.contentWidth + 14
                                                height: 24
                                                color: reindexMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: reindexTxt
                                                    anchors.centerIn: parent
                                                    text: "[ RE-INDEX AST CARTRIDGES ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.mutedTextColor
                                                }
                                                MouseArea {
                                                    id: reindexMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.showToast("AST CARTRIDGES RE-INDEXED")
                                                }
                                            }
                                        }
                                    }
                                }

                                // Default Scope Behavior
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 8

                                        Text {
                                            text: "DEFAULT INVOCATION SCOPE"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: scopeGlobTxt.contentWidth + 14
                                                height: 24
                                                color: dashboardWindow.defaultScopePreference === "global" ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: scopeGlobTxt
                                                    anchors.centerIn: parent
                                                    text: "[ 2: ENTIRE CODEBASE (GLOBAL) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: dashboardWindow.defaultScopePreference === "global"
                                                    color: dashboardWindow.defaultScopePreference === "global" ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.defaultScopePreference = "global"
                                                }
                                            }

                                            Rectangle {
                                                width: scopeLocTxt.contentWidth + 14
                                                height: 24
                                                color: dashboardWindow.defaultScopePreference === "local" ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: scopeLocTxt
                                                    anchors.centerIn: parent
                                                    text: "[ 1: SINGLE TARGET FILE (LOCAL) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: dashboardWindow.defaultScopePreference === "local"
                                                    color: dashboardWindow.defaultScopePreference === "local" ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.defaultScopePreference = "local"
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // =================================================
                            // TAB 2: APPEARANCE & HOTKEYS
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 2
                                width: parent.width
                                spacing: 14

                                Text {
                                    text: "/* 03: COLOR PALETTE & GLOBAL SHORTCUT BINDING */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                // Theme Selector Card
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 130
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
                                                    height: 38
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

                                // Hotkeys & Host Context
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 155
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        Text {
                                            text: "GLOBAL INVOCATION SHORTCUT & PASSIVE CONTEXT (C09)"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Text {
                                                text: "ACTIVE SHORTCUT:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                            }
                                            Rectangle {
                                                width: hkTxt.contentWidth + 12
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
                                                text: "(Anchors floating HUD at active cursor screen coordinates)"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.commentColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 8
                                            Rectangle {
                                                width: 16; height: 16
                                                color: dashboardWindow.passiveHostContextEnabled ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: dashboardWindow.passiveHostContextEnabled ? "×" : ""
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: dashboardWindow.bgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.passiveHostContextEnabled = !dashboardWindow.passiveHostContextEnabled
                                                }
                                            }
                                            Text {
                                                text: "Capture one-shot passive frontmost-application context hint (advisory chip, no OS permissions)"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 8
                                            Rectangle {
                                                width: 16; height: 16
                                                color: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.directInjectionEnabled) ? "×" : ""
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: dashboardWindow.bgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.directInjectionEnabled = !hostController.directInjectionEnabled;
                                                            dashboardWindow.showToast("DIRECT INJECTION: " + (hostController.directInjectionEnabled ? "ENABLED" : "DISABLED (CLIPBOARD FALLBACK)"));
                                                        }
                                                    }
                                                }
                                            }
                                            Text {
                                                text: "Enable Direct Editor Injection (Replaces text in-place via Accessibility API; falls back to clipboard)"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                            }
                                        }
                                    }
                                }
                            }

                            // =================================================
                            // TAB 3: HISTORY & SQLITE STORAGE
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 3
                                width: parent.width
                                spacing: 14

                                Text {
                                    text: "/* 04: SQLITE PROMPT REVISION STORAGE & LINEAGE */"
                                    font.family: dashboardWindow.monoFont
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: dashboardWindow.fgColor
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 180
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        Text {
                                            text: "SQLITE DATABASE STATUS (C02 / A03 / A04)"
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
                                            Text {
                                                text: "RETENTION:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 100
                                            }
                                            Text {
                                                text: "[ PERSISTENT LOCAL WAL — ZERO CLOUD LEAKAGE ]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                width: expHistTxt.contentWidth + 14
                                                height: 24
                                                color: expHistMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: expHistTxt
                                                    anchors.centerIn: parent
                                                    text: "[ EXPORT JSON HISTORY ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.fgColor
                                                }
                                                MouseArea {
                                                    id: expHistMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.showToast("HISTORY EXPORTED")
                                                }
                                            }

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
                                                    color: dashboardWindow.mutedTextColor
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
                                    Layout.preferredHeight: 320
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
                                                    Text {
                                                        text: "[" + modelData.revisionCount + " revs]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 9
                                                        color: "#7EE787"
                                                    }
                                                }

                                                Text {
                                                    text: modelData.latestPrompt ? (modelData.latestPrompt.length > 70 ? (modelData.latestPrompt.substring(0, 70) + "...") : modelData.latestPrompt) : "(empty)"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    color: dashboardWindow.fgColor
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                }
                                            }

                                            Rectangle {
                                                width: 72
                                                height: 24
                                                color: restMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                                border.color: "#58A6FF"
                                                border.width: 1

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "RESTORE"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: "#58A6FF"
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
                                                width: 60
                                                height: 24
                                                color: delMouse.containsMouse ? "#8B1E1E" : "transparent"
                                                border.color: "#EF4444"
                                                border.width: 1

                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "DELETE"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: "#EF4444"
                                                }

                                                MouseArea {
                                                    id: delMouse
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

                            // =================================================
                            // TAB 4: TELEMETRY & STUDY
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 4
                                width: parent.width
                                spacing: 14

                                Text {
                                    text: "/* 05: BENCHMARK STUDY TELEMETRY & HARNESS LOGS */"
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
                                        spacing: 10

                                        Text {
                                            text: "STAGE 10 DOWNSTREAM EVALUATION LOGS (B11)"
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            spacing: 8
                                            Rectangle {
                                                width: 16; height: 16
                                                color: dashboardWindow.telemetryOptIn ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: dashboardWindow.telemetryOptIn ? "×" : ""
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: dashboardWindow.bgColor
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: dashboardWindow.telemetryOptIn = !dashboardWindow.telemetryOptIn
                                                }
                                            }
                                            Text {
                                                text: "Record local latency & disambiguation decision metrics for benchmark study"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                            }
                                        }

                                        Text {
                                            text: "Raw prompt text is strictly preserved locally unless consented. Tracks S1 latency, token counts, and accuracy."
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            color: dashboardWindow.commentColor
                                        }

                                        Rectangle {
                                            width: expCsvTxt.contentWidth + 14
                                            height: 24
                                            color: expCsvMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                                            border.color: dashboardWindow.borderColor
                                            border.width: 1

                                            Text {
                                                id: expCsvTxt
                                                anchors.centerIn: parent
                                                text: "[ EXPORT TASKS_COMPLETED.CSV ]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                            }
                                            MouseArea {
                                                id: expCsvMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: dashboardWindow.showToast("EXPORTED: tasks_completed.csv")
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // =============================================================
                // 3. BOTTOM FOOTER CONTROLS (Matching HUD action bar)
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    spacing: 10

                    // Toast message banner
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

                    // [ SAVE PREFERENCES ]
                    Rectangle {
                        width: saveTxt.contentWidth + 18
                        height: 24
                        color: saveMouse.containsMouse ? dashboardWindow.dimColor : "transparent"
                        border.color: dashboardWindow.borderColor
                        border.width: 1

                        Text {
                            id: saveTxt
                            anchors.centerIn: parent
                            text: "[ SAVE PREFERENCES ]"
                            font.family: dashboardWindow.monoFont
                            font.bold: true
                            font.pixelSize: 9
                            color: dashboardWindow.fgColor
                        }
                        MouseArea {
                            id: saveMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dashboardWindow.showToast("PREFERENCES PERSISTED")
                        }
                    }

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
