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
    Connections {
        target: typeof hostController !== "undefined" ? hostController : null
        function onClarificationDeferred(message, automatic) { dashboardWindow.showToast(message); }
        function onStorageCommandFinished(requestId, name, succeeded, errorCode, message) {
            if (name === "purgeHistory" && succeeded) dashboardWindow.showToast("HISTORY PURGED");
        }
    }

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

    readonly property string scorerMode: (typeof hostController !== "undefined" ? (hostController.scorerEngine === "JULIA_LOCAL" ? "julia" : "jev") : "julia")
    readonly property string globalShortcutString: typeof hostController !== "undefined" ? hostController.shortcutLabel : (isMac ? "Cmd+Shift+L" : "Ctrl+Shift+L")
    property string toastMessage: ""
    property bool confirmPurge: false

    function showToast(msg) {
        toastMessage = msg;
        toastTimer.restart();
    }

    function scrollToPosition(pos) {
        rightScrollView.ScrollBar.vertical.position = pos;
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
                    Layout.minimumWidth: 0
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
                        implicitWidth: resText.contentWidth + 16
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
                        HudMouseArea {
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
                        HudMouseArea {
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
                        HudMouseArea {
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
                    Layout.minimumWidth: 0
                    height: 1
                    color: dashboardWindow.borderColor
                }

                // =============================================================
                // 2. MAIN BODY (Sidebar Navigation Matrix + Content Tabs)
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
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
                                Layout.minimumWidth: 0
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
                                    Layout.minimumWidth: 0
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
                                            Layout.minimumWidth: 0
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

                                    HudMouseArea {
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
                                Layout.minimumWidth: 0
                                height: 1
                                color: dashboardWindow.borderColor
                            }

                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
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
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical: HudScrollBar {}
                        id: rightScrollView
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
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
                                    Layout.minimumWidth: 0
                                    Layout.preferredHeight: 175
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        RowLayout {
                                            Text {
                                                text: "S1 PROMPT CLASSIFIER :: LOCAL JULIA-1"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: dashboardWindow.scorerMode === "jev" ? "[MODE: JEV CLOUD]" : "[MODE: LOCAL JULIA]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.mutedTextColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                implicitWidth: juliaTxt.contentWidth + 16
                                                height: 26
                                                color: dashboardWindow.scorerMode === "julia" ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: juliaTxt
                                                    anchors.centerIn: parent
                                                    text: dashboardWindow.scorerMode === "julia" ? "[ ● LOCAL JULIA-1 (AUTO GPU / CPU) ]" : "[ ○ LOCAL JULIA-1 (AUTO GPU / CPU) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: dashboardWindow.scorerMode === "julia"
                                                    color: dashboardWindow.scorerMode === "julia" ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {

                                                        if (typeof hostController !== "undefined") { hostController.setScorerEngine("julia"); hostController.refreshJuliaModel(); }
                                                        dashboardWindow.showToast("S1 CLASSIFIER: LOCAL JULIA-1");
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: jevTxt.contentWidth + 16
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
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {

                                                        if (typeof hostController !== "undefined") hostController.setScorerEngine("jev");
                                                        dashboardWindow.showToast("JEV SELECTED; CLOUD TRANSPORT IS NOT CONFIGURED");
                                                    }
                                                }
                                            }
                                        }

                                        // Julia Model Weights Status & Download Action
                                        RowLayout {
                                            spacing: 8
                                            visible: dashboardWindow.scorerMode === "julia"

                                            // Status Badge
                                            Rectangle {
                                                implicitWidth: juliaStatusTxt.contentWidth + 14
                                                height: 22
                                                color: "transparent"
                                                border.color: (typeof hostController !== "undefined" && hostController.hasJuliaModel)
                                                              ? "#7EE787"
                                                              : (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled)
                                                                ? "#38BDF8"
                                                                : "#FFB000"
                                                border.width: 1

                                                Text {
                                                    id: juliaStatusTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.hasJuliaModel)
                                                          ? "[ ● JULIA LOADED (MPS) ]"
                                                          : (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled)
                                                            ? "[ ⏸ S1 READY ON DISK ]"
                                                            : "[ ○ S1 NOT DOWNLOADED ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: (typeof hostController !== "undefined" && hostController.hasJuliaModel)
                                                           ? "#7EE787"
                                                           : (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled)
                                                             ? "#38BDF8"
                                                             : "#FFB000"
                                                }
                                            }

                                            // Action button: If installed on disk, load it; if missing, download it
                                            Rectangle {
                                                visible: typeof hostController !== "undefined" && !hostController.hasJuliaModel && ["NOT_LOADED", "FAILED"].indexOf(hostController.juliaState) >= 0
                                                implicitWidth: dlJuliaCard1Txt.contentWidth + 14
                                                height: 22
                                                color: dlJuliaCard1Mouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                border.color: (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled) ? "#38BDF8" : dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: dlJuliaCard1Txt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled)
                                                          ? "[ ⟳ LOAD JULIA-1 INTO GPU ]"
                                                          : "[ ⬇ DOWNLOAD JULIA-1 (600 MB) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: dlJuliaCard1Mouse.containsMouse ? dashboardWindow.bgColor : ((typeof hostController !== "undefined" && hostController.isJuliaModelInstalled) ? "#38BDF8" : dashboardWindow.fgColor)
                                                }

                                                HudMouseArea {
                                                    id: dlJuliaCard1Mouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            if (hostController.isJuliaModelInstalled) {
                                                                hostController.refreshJuliaModel();
                                                                dashboardWindow.showToast("LOADING JULIA-1 INTO GPU MEMORY...");
                                                            } else {
                                                                hostController.downloadJuliaModel();
                                                                dashboardWindow.showToast("DOWNLOADING AND VERIFYING JULIA-1...");
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            // Real-time downloading indicator
                                            Text {
                                                visible: typeof hostController !== "undefined" && ["LOADING", "DOWNLOADING"].indexOf(hostController.juliaState) >= 0
                                                text: typeof hostController !== "undefined" ? hostController.juliaState : ""
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 8
                                                font.bold: true
                                                color: "#FFB000"
                                            }
                                        }

                                        Text {
                                            text: dashboardWindow.scorerMode === "jev"
                                                ? "Jev is not configured. The S1-first flow requires local Julia."
                                                : "Julia classifies first: below 0.70 keeps the original; otherwise Granite supplies choices. Confirm intent explicitly."
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            color: dashboardWindow.commentColor
                                        }
                                    }
                                }

                                JuliaSettings {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    currentTheme: dashboardWindow.currentTheme
                                }

                                // Card 2: Rewrite Profiles (Prompt Compiler)
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
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
                                                        implicitWidth: profBtnTxt.contentWidth + 14
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

                                                        HudMouseArea {
                                                            anchors.fill: parent
                                                            cursorShape: Qt.PointingHandCursor
                                                            onClicked: {
                                                                if (typeof hostController !== "undefined") {
                                                                    hostController.setRewriteProfile(modelData.id, hostController.stateVersion);
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

                                // Card 3: IBM Granite SLM & Non-Invasive Model Repository
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: graniteCardCol.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1

                                    ColumnLayout {
                                        id: graniteCardCol
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        RowLayout {
                                            Text {
                                                text: "IBM GRANITE SLM :: LOCAL INFERENCE & WEIGHTS MANAGER"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                                      ? ((typeof hostController !== "undefined" && hostController.inferenceSpeed.length > 0)
                                                          ? "[LIVE WEIGHTS IN GPU · " + hostController.inferenceSpeed + "]"
                                                          : "[LIVE WEIGHTS IN GPU]")
                                                      : (typeof hostController !== "undefined" && hostController.isCheckingModel)
                                                        ? "[WARMING INTO GPU...]"
                                                        : (typeof hostController !== "undefined" && hostController.isNativeGraniteInstalled)
                                                          ? "[STANDBY ON DISK · READY]"
                                                          : "[NOT DOWNLOADED]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                                       ? "#7EE787"
                                                       : (typeof hostController !== "undefined" && (hostController.isCheckingModel || hostController.isNativeGraniteInstalled))
                                                         ? "#38BDF8"
                                                         : dashboardWindow.mutedTextColor
                                            }
                                        }

                                        // Row 1: Engine Mode Toggle
                                        RowLayout {
                                            spacing: 10

                                            Rectangle {
                                                implicitWidth: nativeBtnTxt.contentWidth + 16
                                                height: 26
                                                color: "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: nativeBtnTxt
                                                    anchors.centerIn: parent
                                                    text: "[ ⟳ RESCAN RUNTIME ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: true
                                                    color: dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.refreshAvailableModels();
                                                            dashboardWindow.showToast("RESCANNED LOCAL MODEL DIRECTORY");
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: liveBtnTxt.contentWidth + 16
                                                height: 26
                                                color: (typeof hostController !== "undefined" && hostController.useLiveWeights) ? dashboardWindow.fgColor : "transparent"
                                                border.color: (typeof hostController !== "undefined" && (hostController.useLiveWeights || hostController.isNativeGraniteInstalled)) ? "#7EE787" : dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: liveBtnTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                                          ? "[ ● WEIGHTS LOADED IN GPU (READY) ]"
                                                          : (typeof hostController !== "undefined" && hostController.isCheckingModel)
                                                            ? "[ ⟳ WARMING INTO GPU MEMORY... ]"
                                                            : (typeof hostController !== "undefined" && hostController.isNativeGraniteInstalled)
                                                              ? "[ ⏸ WEIGHTS ON DISK (CLICK TO WARM) ]"
                                                              : "[ ○ NO WEIGHTS ON DISK (DOWNLOAD BELOW) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: (typeof hostController !== "undefined" && (hostController.useLiveWeights || hostController.isNativeGraniteInstalled))
                                                    color: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                                           ? dashboardWindow.bgColor
                                                           : (typeof hostController !== "undefined" && hostController.isNativeGraniteInstalled)
                                                             ? "#7EE787"
                                                             : dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            if (hostController.isNativeGraniteInstalled && !hostController.useLiveWeights) {
                                                                hostController.warmSelectedModel();
                                                                dashboardWindow.showToast("WARMING MODEL INTO GPU MEMORY...");
                                                            } else {
                                                                hostController.refreshAvailableModels();
                                                                dashboardWindow.showToast("CHECKING LOCAL MODEL RUNTIME");
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Status bar
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            height: 24
                                            color: dashboardWindow.dimColor
                                            border.color: dashboardWindow.borderColor
                                            border.width: 1

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: 8
                                                anchors.rightMargin: 8
                                                spacing: 6

                                                Rectangle {
                                                    width: 6; height: 6
                                                    color: (typeof hostController !== "undefined" && hostController.availableModels.length > 0) ? "#7EE787" : "#FFB000"
                                                }

                                                Text {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    text: typeof hostController !== "undefined" ? hostController.modelEngineStatus : "LOADING STATUS..."
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: dashboardWindow.fgColor
                                                    elide: Text.ElideRight
                                                }

                                                Text {
                                                    visible: typeof hostController !== "undefined" && hostController.inferenceSpeed.length > 0
                                                    text: "SPEED: " + (typeof hostController !== "undefined" ? hostController.inferenceSpeed : "")
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: "#38BDF8"
                                                }
                                            }
                                        }

                                        // Row 2: Non-Invasive Model Folder Connection
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 4

                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                                                      ? "CONNECTED MODEL FOLDER (S2 GRANITE MODELS):"
                                                      : "CONNECTED MODEL FOLDER (NON-INVASIVE ACCESS):"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 8
                                                font.bold: true
                                                color: dashboardWindow.mutedTextColor
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                spacing: 8

                                                Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    height: 28
                                                    color: dashboardWindow.bgColor
                                                    border.color: dashboardWindow.borderColor
                                                    border.width: 1

                                                    RowLayout {
                                                        anchors.fill: parent
                                                        anchors.leftMargin: 8
                                                        anchors.rightMargin: 8

                                                        Text {
                                                            Layout.fillWidth: true
                                                            Layout.minimumWidth: 0
                                                            text: (typeof hostController !== "undefined" && hostController.effectiveModelDirectory.length > 0)
                                                                  ? hostController.effectiveModelDirectory
                                                                  : "No folder connected"
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 9
                                                            color: dashboardWindow.fgColor
                                                            elide: Text.ElideMiddle
                                                        }
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: connBtnTxt.contentWidth + 16
                                                    height: 28
                                                    color: connectMouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                    border.color: dashboardWindow.borderColor
                                                    border.width: 1

                                                    Text {
                                                        id: connBtnTxt
                                                        anchors.centerIn: parent
                                                        text: "[ 📂 CONNECT FOLDER... ]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        font.bold: true
                                                        color: connectMouse.containsMouse ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                    }

                                                    HudMouseArea {
                                                        id: connectMouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                const chosen = hostController.openModelFolderDialog();
                                                                if (chosen && chosen.length > 0) {
                                                                    dashboardWindow.showToast("CONNECTED MODEL FOLDER: " + chosen);
                                                                }
                                                            }
                                                        }
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: refreshBtnTxt.contentWidth + 14
                                                    height: 28
                                                    color: refreshMouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                    border.color: dashboardWindow.borderColor
                                                    border.width: 1

                                                    Text {
                                                        id: refreshBtnTxt
                                                        anchors.centerIn: parent
                                                        text: "[ ⟳ RESCAN ]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        font.bold: true
                                                        color: refreshMouse.containsMouse ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                    }

                                                    HudMouseArea {
                                                        id: refreshMouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                hostController.refreshAvailableModels();
                                                                dashboardWindow.showToast("RESCANNED CONNECTED FOLDER");
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                                                      ? (hostController.isNativeGraniteInstalled
                                                         ? "✓ Model weights verified on disk in this folder. Ready to load into " + (typeof hostController !== "undefined" ? hostController.acceleratorName : "GPU") + " GPU memory."
                                                         : "⚠️ No model weights found in this folder. Download Granite below or click '[ 📂 CONNECT FOLDER... ]' to point to your existing models.")
                                                      : "* Non-invasive isolation: Roclint only reads models inside this specific folder. Your hard drive and personal files are never searched."
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 8
                                                color: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                                                       ? (hostController.isNativeGraniteInstalled ? "#7EE787" : "#FFB000")
                                                       : dashboardWindow.commentColor
                                            }
                                        }

                                        // Row 3: Detected Models
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 6

                                            Text {
                                                text: "DETECTED LOCAL MODELS IN FOLDER / RUNNER:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 8
                                                font.bold: true
                                                color: dashboardWindow.mutedTextColor
                                            }

                                            Repeater {
                                                model: (typeof hostController !== "undefined") ? hostController.availableModels : []

                                                delegate: Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    height: 26
                                                    color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                                           ? dashboardWindow.dimColor
                                                           : "transparent"
                                                    border.color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                                                  ? dashboardWindow.fgColor
                                                                  : dashboardWindow.borderColor
                                                    border.width: 1

                                                    RowLayout {
                                                        anchors.fill: parent
                                                        anchors.leftMargin: 8
                                                        anchors.rightMargin: 8
                                                        spacing: 8

                                                        Text {
                                                            text: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                                                  ? (hostController.useLiveWeights ? "[ ● LOADED IN GPU ]" : "[ ★ SELECTED (STANDBY) ]")
                                                                  : (modelData.requiresImport ? "[ IMPORT ]" : "[ ○ SELECT ]")
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 8
                                                            font.bold: true
                                                            color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                                                   ? (hostController.useLiveWeights ? "#7EE787" : "#38BDF8")
                                                                   : dashboardWindow.mutedTextColor
                                                        }

                                                        Text {
                                                            text: modelData.name
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 9
                                                            font.bold: true
                                                            color: dashboardWindow.fgColor
                                                        }

                                                        Text {
                                                            text: "(" + modelData.size + ", " + modelData.source + ", " + modelData.quantization + ") · [✓ Verified on disk]"
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 8
                                                            color: dashboardWindow.commentColor
                                                        }

                                                        Item { Layout.fillWidth: true }
                                                    }

                                                    HudMouseArea {
                                                        anchors.fill: parent
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                if (modelData.requiresImport) hostController.importLocalModel(modelData.path);
                                                                else hostController.setActiveModel(modelData.name);
                                                                dashboardWindow.showToast(modelData.requiresImport ? "IMPORTING LOCAL GGUF..." : "ACTIVE MODEL: " + modelData.name);
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            // Fallback banner when 0 models
                                            Rectangle {
                                                visible: (typeof hostController !== "undefined" && hostController.availableModels.length === 0)
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                height: 32
                                                color: dashboardWindow.dimColor
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 10

                                                    Text {
                                                        text: "NO MODELS FOUND. Connect your model directory or download a model below."
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        color: dashboardWindow.mutedTextColor
                                                    }
                                                }
                                            }
                                        }

                                        // Row 4: Download / Pull Models & Live Test
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 6

                                            Text {
                                                text: "MODEL DOWNLOAD & REPOSITORY ACTIONS:"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 8
                                                font.bold: true
                                                color: dashboardWindow.mutedTextColor
                                            }

                                            // Active download progress
                                            Rectangle {
                                                visible: typeof hostController !== "undefined" && hostController.isDownloadingModel
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                height: 44
                                                color: dashboardWindow.dimColor
                                                border.color: "#FFB000"
                                                border.width: 1

                                                ColumnLayout {
                                                    anchors.fill: parent
                                                    anchors.margins: 6
                                                    spacing: 4

                                                    RowLayout {
                                                        Text {
                                                            text: "DOWNLOADING MODEL: " + (typeof hostController !== "undefined" ? hostController.downloadProgress : 0) + "%"
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 8
                                                            font.bold: true
                                                            color: "#FFB000"
                                                        }
                                                        Item { Layout.fillWidth: true }
                                                        Text {
                                                            text: "[ CANCEL ]"
                                                            font.family: dashboardWindow.monoFont
                                                            font.pixelSize: 8
                                                            font.bold: true
                                                            color: dashboardWindow.fgColor
                                                            HudMouseArea {
                                                                anchors.fill: parent
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: {
                                                                    if (typeof hostController !== "undefined") {
                                                                        hostController.cancelModelDownload();
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }

                                                    Text {
                                                        text: typeof hostController !== "undefined" ? hostController.downloadStatusMessage : ""
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        color: dashboardWindow.commentColor
                                                        elide: Text.ElideRight
                                                    }
                                                }
                                            }

                                            // Download Action Buttons
                                            Flow {
                                                visible: typeof hostController === "undefined" || !hostController.isDownloadingModel
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                spacing: 8

                                                Rectangle {
                                                    implicitWidth: dl0Txt.contentWidth + 14
                                                    height: 24
                                                    color: dl0Mouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                    border.color: (typeof hostController !== "undefined" && hostController.isNativeGraniteQ4Installed) ? "#7EE787" : dashboardWindow.borderColor
                                                    border.width: 1

                                                    Text {
                                                        id: dl0Txt
                                                        anchors.centerIn: parent
                                                        text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                                                              ? (hostController.isNativeGraniteQ4Installed
                                                                 ? "[ ✓ Q4_K_M INSTALLED (2.09 GiB) · RE-DOWNLOAD ]"
                                                                 : "[ ⬇ DOWNLOAD GRANITE 4.2 Q4_K_M (2.09 GiB) · FASTEST ]")
                                                              : "[ ⬇ PULL GRANITE 4.2 3B Q4 ]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        font.bold: true
                                                        color: dl0Mouse.containsMouse ? dashboardWindow.bgColor : ((typeof hostController !== "undefined" && hostController.isNativeGraniteQ4Installed) ? "#7EE787" : dashboardWindow.fgColor)
                                                    }

                                                    HudMouseArea {
                                                        id: dl0Mouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                hostController.downloadModel(hostController.nativeGraniteSelected ? "native-granite:Q4_K_M" : "granite4.2:3b-q4_k_m");
                                                                dashboardWindow.showToast("DOWNLOADING GRANITE 4.2 3B (Q4_K_M)...");
                                                            }
                                                        }
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: dl1Txt.contentWidth + 14
                                                    height: 24
                                                    color: dl1Mouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                    border.color: (typeof hostController !== "undefined" && hostController.isNativeGraniteQ8Installed) ? "#7EE787" : dashboardWindow.borderColor
                                                    border.width: 1

                                                    Text {
                                                        id: dl1Txt
                                                        anchors.centerIn: parent
                                                        text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                                                              ? (hostController.isNativeGraniteQ8Installed
                                                                 ? "[ ✓ Q8_0 INSTALLED (3.89 GiB) · RE-DOWNLOAD ]"
                                                                 : "[ ⬇ DOWNLOAD GRANITE 4.2 Q8_0 (3.62 GiB) ]")
                                                              : "[ ⬇ PULL GRANITE 4.2 3B (3.9 GB) ]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        font.bold: true
                                                        color: dl1Mouse.containsMouse ? dashboardWindow.bgColor : ((typeof hostController !== "undefined" && hostController.isNativeGraniteQ8Installed) ? "#7EE787" : dashboardWindow.fgColor)
                                                    }

                                                    HudMouseArea {
                                                        id: dl1Mouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                hostController.downloadModel(hostController.nativeGraniteSelected ? "native-granite:Q8_0" : "granite4.2:3b-q8_0");
                                                                dashboardWindow.showToast("DOWNLOADING GRANITE 4.2 3B (Q8_0)...");
                                                            }
                                                        }
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: dl2Txt.contentWidth + 14
                                                    height: 24
                                                    color: dl2Mouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                    border.color: (typeof hostController !== "undefined" && hostController.isNativeGraniteQ5Installed) ? "#7EE787" : dashboardWindow.borderColor
                                                    border.width: 1

                                                    Text {
                                                        id: dl2Txt
                                                        anchors.centerIn: parent
                                                        text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                                                              ? (hostController.isNativeGraniteQ5Installed
                                                                 ? "[ ✓ Q5_K_M INSTALLED (2.43 GiB) · RE-DOWNLOAD ]"
                                                                 : "[ ⬇ DOWNLOAD GRANITE 4.2 Q5_K_M (2.43 GiB) ]")
                                                              : "[ ⬇ PULL GRANITE 3.1 2B (1.5 GB) ]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        font.bold: true
                                                        color: dl2Mouse.containsMouse ? dashboardWindow.bgColor : ((typeof hostController !== "undefined" && hostController.isNativeGraniteQ5Installed) ? "#7EE787" : dashboardWindow.fgColor)
                                                    }

                                                    HudMouseArea {
                                                        id: dl2Mouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                hostController.downloadModel(hostController.nativeGraniteSelected ? "native-granite:Q5_K_M" : "granite3.1-dense:2b");
                                                                dashboardWindow.showToast(hostController.nativeGraniteSelected ? "DOWNLOADING PINNED GRANITE 4.2 (Q5_K_M)..." : "DOWNLOADING GRANITE 3.1 2B...");
                                                            }
                                                        }
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: testInfTxt.contentWidth + 14
                                                    height: 24
                                                    color: testInfMouse.containsMouse ? dashboardWindow.fgColor : "transparent"
                                                    border.color: "#7EE787"
                                                    border.width: 1

                                                    Text {
                                                        id: testInfTxt
                                                        anchors.centerIn: parent
                                                        text: (typeof hostController !== "undefined" && hostController.isInferring)
                                                              ? "[ WORKING... ]"
                                                              : (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                                                ? "[ ● MODEL READY (GPU) ]"
                                                                : "[ CHECK & WARM MODEL ]"
                                                        font.family: dashboardWindow.monoFont
                                                        font.pixelSize: 8
                                                        font.bold: true
                                                        color: testInfMouse.containsMouse ? dashboardWindow.bgColor : "#7EE787"
                                                    }

                                                    HudMouseArea {
                                                        id: testInfMouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                dashboardWindow.showToast("CHECKING AND WARMING THE SELECTED MODEL...");
                                                                hostController.warmSelectedModel();
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            Text {
                                                text: "Storage: Models are stored in your configured folder (S1 in /s1, S2 in /s2). Click '[ 📂 CONNECT FOLDER... ]' above to use existing models from any directory."
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 8
                                                color: dashboardWindow.commentColor
                                                wrapMode: Text.WordWrap
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
                                    Layout.minimumWidth: 0
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
                                                objectName: "ambientCaptureStatus"
                                                text: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? "[STATUS: ENABLED]" : "[STATUS: DISABLED]"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? "#7EE787" : dashboardWindow.mutedTextColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                implicitWidth: injEnTxt.contentWidth + 14
                                                height: 24
                                                color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: injEnTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? "[ ● ENABLED (Captured Field) ]" : "[ ○ ENABLED (Captured Field) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: (typeof hostController !== "undefined" && hostController.ambientModeEnabled)
                                                    color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.setAmbientModeEnabled(true);
                                                            dashboardWindow.showToast("DIRECT INJECTION: ENABLED");
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: injDisTxt.contentWidth + 14
                                                height: 24
                                                color: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? dashboardWindow.fgColor : "transparent"
                                                border.color: dashboardWindow.borderColor
                                                border.width: 1

                                                Text {
                                                    id: injDisTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? "[ ● DISABLED (Clipboard Fallback) ]" : "[ ○ DISABLED (Clipboard Fallback) ]"
                                                    font.family: dashboardWindow.monoFont
                                                    font.pixelSize: 9
                                                    font.bold: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled)
                                                    color: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? dashboardWindow.bgColor : dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.setAmbientModeEnabled(false);
                                                            dashboardWindow.showToast("DIRECT INJECTION: DISABLED (CLIPBOARD MODE)");
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            text: (typeof hostController !== "undefined" && hostController.accessibilityTrusted)
                                                ? "Live capture watches the focused editable field. Insert replaces that captured draft after approval."
                                                : "Accessibility is unavailable. Copy the approved prompt, then paste it into your editor."
                                            font.family: dashboardWindow.monoFont
                                            font.pixelSize: 9
                                            color: dashboardWindow.commentColor
                                        }
                                    }
                                }

                                // Color Palette Theme
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
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
                                                    implicitWidth: thmTxt.contentWidth + 20
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
                                                    HudMouseArea {
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
                                InvocationSettings {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    currentTheme: dashboardWindow.currentTheme
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
                                    Layout.minimumWidth: 0
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
                                                text: (typeof hostController !== "undefined" && hostController.databasePath.length > 0) ? hostController.databasePath : "Storage unavailable"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                color: dashboardWindow.fgColor
                                                elide: Text.ElideMiddle
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
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
                                                text: (typeof hostController !== "undefined" && hostController.databaseStats.length > 0) ? hostController.databaseStats : "Storage unavailable"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: "#7EE787"
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                implicitWidth: vacTxt.contentWidth + 14
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
                                                HudMouseArea {
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
                                                implicitWidth: delHistTxt.contentWidth + 14
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
                                                HudMouseArea {
                                                    id: delHistMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            if (!dashboardWindow.confirmPurge) { dashboardWindow.confirmPurge = true; dashboardWindow.showToast("CLICK PURGE AGAIN TO CONFIRM"); return; }
                                                            if (!hostController.purgeHistory()) return;
                                                            dashboardWindow.confirmPurge = false;
                                                            return;
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
                                    Layout.minimumWidth: 0
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
                                                Layout.minimumWidth: 0
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
                                                    Layout.minimumWidth: 0
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: restTxt.contentWidth + 12
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
                                                HudMouseArea {
                                                    id: restMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            if (hostController.restoreSessionForReview(modelData.sessionId, hostController.stateVersion))
                                                                hostController.hideDashboard();
                                                            else dashboardWindow.showToast(hostController.lastError);
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: delItemTxt.contentWidth + 12
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
                                                HudMouseArea {
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
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 30
                    spacing: 10

                    Rectangle {
                        height: 24
                        implicitWidth: toastTextLabel.contentWidth + 16
                        color: dashboardWindow.dimColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1
                        visible: dashboardWindow.toastMessage.length > 0

                        Text {
                            id: toastTextLabel
                            anchors.centerIn: parent
                            text: dashboardWindow.toastMessage.length ? dashboardWindow.toastMessage : (typeof hostController !== "undefined" ? hostController.lastError : "")
                            font.family: dashboardWindow.monoFont
                            font.pixelSize: 9
                            font.bold: true
                            color: dashboardWindow.fgColor
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // [ CLOSE ]
                    Rectangle {
                        implicitWidth: closeBtnTxt.contentWidth + 18
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
                        HudMouseArea {
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
