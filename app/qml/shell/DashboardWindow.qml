import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: dashboardWindow
    title: qsTr("Roclint — Settings")
    width: 940
    height: 620
    minimumWidth: 780
    minimumHeight: 520
    visible: false
    flags: Qt.Window
    color: dashboardWindow.bgColor
    property bool openedOnce: false
    property bool restoreMaximizedAfterFullscreen: false

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    FontLoader { id: interFont; source: "assets/InterVariable.ttf" }
    readonly property string sansFont: interFont.status === FontLoader.Ready ? interFont.name : (isMac ? "Helvetica Neue" : "Segoe UI")

    // Multi-Palette Theme System (Mirrors FloatingCard.qml exactly)
    Connections {
        target: typeof hostController !== "undefined" ? hostController : null
        function onClarificationDeferred(message, automatic) { dashboardWindow.showToast(message); }
        function onStorageCommandFinished(requestId, name, succeeded, errorCode, message) {
            if (name === "purgeHistory" && succeeded) dashboardWindow.showToast("HISTORY PURGED");
        }
    }

    property string currentTheme: "dark" // "dark", "light", "acid", "amber"

    RocTheme { id: theme; mode: dashboardWindow.currentTheme }
    readonly property color bgColor: theme.background
    readonly property color fgColor: theme.foreground
    readonly property color borderColor: theme.border
    readonly property color dimColor: theme.hover
    readonly property color mutedTextColor: theme.muted
    readonly property color commentColor: theme.muted
    readonly property color inputBgColor: theme.inset
    readonly property color shadowColor: "#030918"

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

    function isProfileActive(profId) {
        if (typeof hostController === "undefined" || !hostController || !hostController.rewriteProfile) return false;
        const cur = String(hostController.rewriteProfile).toUpperCase();
        const target = String(profId).toUpperCase();
        const curBase = cur.replace(/@\d+$/, "");
        const targetBase = target.replace(/@\d+$/, "");
        return cur === target || curBase === targetBase;
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

    function present(startMaximized) {
        if (openedOnce) {
            show();
            return;
        }
        openedOnce = true;
        if (startMaximized) showMaximized();
        else showFullScreen();
    }

    function toggleFullscreen() {
        if (visibility === Window.FullScreen) {
            if (restoreMaximizedAfterFullscreen) showMaximized();
            else showNormal();
        } else {
            restoreMaximizedAfterFullscreen = visibility === Window.Maximized;
            showFullScreen();
        }
    }

    Shortcut {
        sequence: "F11"
        onActivated: dashboardWindow.toggleFullscreen()
    }

    Shortcut {
        sequence: "Escape"
        enabled: dashboardWindow.visibility === Window.FullScreen
        onActivated: dashboardWindow.toggleFullscreen()
    }

    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: dashboardWindow.dismissDashboard()
    }

    Rectangle {
        anchors.fill: parent
        color: dashboardWindow.bgColor
        gradient: Gradient {
            GradientStop { position: 0; color: theme.light ? "#F1F5FF" : "#172746" }
            GradientStop { position: 0.62; color: dashboardWindow.bgColor }
        }

        Item {
            anchors.fill: parent
            anchors.margins: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 12

                // =============================================================
                // 1. TOP HEADER BAR
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 32
                    spacing: 10

                    Image {
                        source: "assets/roc-mark.png"
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 30
                        fillMode: Image.PreserveAspectFit
                    }

                    Text {
                        text: "roclint"
                        font.family: dashboardWindow.sansFont
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                        font.letterSpacing: -0.6
                        color: dashboardWindow.fgColor
                    }

                    Rectangle {
                        width: 4
                        height: 4
                        radius: 2
                        color: dashboardWindow.mutedTextColor
                        opacity: 0.5
                        Layout.leftMargin: 2
                        Layout.rightMargin: 2
                    }

                    Text {
                        text: qsTr("Settings")
                        font.family: dashboardWindow.sansFont
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: dashboardWindow.mutedTextColor
                    }

                    Item { Layout.fillWidth: true }

                    // Host resident status chip (modern rounded pill)
                    Rectangle {
                        height: 24
                        implicitWidth: resRow.implicitWidth + 20
                        radius: 12
                        color: theme.surface
                        border.color: theme.border
                        border.width: 1

                        RowLayout {
                            id: resRow
                            anchors.centerIn: parent
                            spacing: 6

                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: "#34D399"
                            }

                            Text {
                                id: resText
                                text: qsTr("Running locally")
                                font.family: dashboardWindow.sansFont
                                font.weight: Font.Medium
                                font.pixelSize: 11
                                color: dashboardWindow.fgColor
                            }
                        }
                    }

                    RocButton {
                        visible: dashboardWindow.visibility === Window.FullScreen
                        label: qsTr("Exit full screen")
                        compact: true
                        currentTheme: dashboardWindow.currentTheme
                        onClicked: dashboardWindow.toggleFullscreen()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    height: 1
                    color: dashboardWindow.borderColor
                    opacity: 0.5
                }

                // =============================================================
                // 2. MAIN BODY (Sidebar Navigation Matrix + Content Tabs)
                // =============================================================
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.fillHeight: true
                    spacing: 14

                    // LEFT SIDEBAR: TAB SELECTION MATRIX
                    Rectangle {
                        Layout.preferredWidth: 230
                        Layout.fillHeight: true
                        color: dashboardWindow.inputBgColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1
                        radius: 14

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 6

                            Text {
                                text: qsTr("NAVIGATION")
                                font.family: dashboardWindow.sansFont
                                font.pixelSize: 10
                                font.bold: true
                                font.letterSpacing: 1.0
                                color: dashboardWindow.mutedTextColor
                                Layout.leftMargin: 6
                                Layout.topMargin: 4
                                Layout.bottomMargin: 4
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: dashboardWindow.borderColor
                                opacity: 0.5
                                Layout.bottomMargin: 6
                            }

                            Repeater {
                                model: [
                                    { index: 0, tag: "01", label: qsTr("Models & Rewrite") },
                                    { index: 1, tag: "02", label: qsTr("Integration & Theme") },
                                    { index: 2, tag: "03", label: qsTr("SQLite History") },
                                    { index: 3, tag: "04", label: qsTr("Prompt Actions Guide") }
                                ]

                                delegate: Rectangle {
                                    id: navTabBtn
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.preferredHeight: Math.max(38, navLabel.implicitHeight + 14)
                                    radius: 9
                                    color: dashboardWindow.currentTab === modelData.index
                                        ? theme.accent
                                        : (tabMouse.containsMouse ? theme.hover : "transparent")
                                    border.color: dashboardWindow.currentTab === modelData.index
                                        ? theme.accent
                                        : (tabMouse.containsMouse ? theme.border : "transparent")
                                    border.width: 1

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 8

                                        Rectangle {
                                            implicitWidth: 26
                                            height: 20
                                            radius: 6
                                            color: dashboardWindow.currentTab === modelData.index
                                                ? Qt.rgba(255, 255, 255, 0.22)
                                                : theme.surface
                                            border.color: dashboardWindow.currentTab === modelData.index
                                                ? "transparent"
                                                : theme.border
                                            border.width: 1

                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.tag
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: dashboardWindow.currentTab === modelData.index
                                                    ? "#FFFFFF"
                                                    : dashboardWindow.mutedTextColor
                                            }
                                        }

                                        Text {
                                            id: navLabel
                                            objectName: "settingsNavLabel" + modelData.index
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: modelData.label
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            font.weight: dashboardWindow.currentTab === modelData.index ? Font.DemiBold : Font.Medium
                                            color: dashboardWindow.currentTab === modelData.index
                                                ? "#FFFFFF"
                                                : dashboardWindow.fgColor
                                            wrapMode: Text.WordWrap
                                        }
                                    }

                                    HudMouseArea {
                                        id: tabMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        focusRadius: navTabBtn.radius
                                        focusColor: theme.accent
                                        onClicked: dashboardWindow.currentTab = modelData.index
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }


                        }
                    }

                    // RIGHT CONTENT AREA
                    ScrollView {
                        id: rightScrollView
                        objectName: "settingsScrollView"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.fillHeight: true
                        clip: true
                        rightPadding: settingsScrollBar.width + 8
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical: HudScrollBar {
                            id: settingsScrollBar
                            objectName: "settingsScrollBar"
                            parent: rightScrollView
                            orientation: Qt.Vertical
                            x: rightScrollView.width - width
                            y: 0
                            width: implicitWidth
                            height: rightScrollView.height
                        }

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
                                    text: qsTr("Inference Runtime & Prompt Compiler")
                                    font.family: dashboardWindow.sansFont
                                    font.pixelSize: 18
                                    font.weight: Font.DemiBold
                                    color: dashboardWindow.fgColor
                                }

                                // Card 1: Dual-Stage Local Inference Pipeline
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: card1Col.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: card1Col
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        RowLayout {
                                            Text {
                                                text: qsTr("Dual-Stage Local Inference Pipeline")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Rectangle {
                                                implicitWidth: modeChipTxt.contentWidth + 16
                                                height: 22
                                                radius: 11
                                                color: theme.surface
                                                border.color: theme.border
                                                border.width: 1
                                                Text {
                                                    id: modeChipTxt
                                                    anchors.centerIn: parent
                                                    text: (typeof hostController !== "undefined" && hostController.hasJuliaModel && (hostController.useLiveWeights || hostController.nativeGraniteResidency === "LOADED"))
                                                          ? qsTr("● Both Models Loaded")
                                                          : qsTr("100% On-Device · Private")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 11
                                                    color: (typeof hostController !== "undefined" && hostController.hasJuliaModel && (hostController.useLiveWeights || hostController.nativeGraniteResidency === "LOADED"))
                                                           ? "#34D399"
                                                           : dashboardWindow.mutedTextColor
                                                }
                                            }
                                        }

                                        // Stage 1 (Classifier) & Stage 2 (Reasoning SLM) Status & Quick-Warm
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 10

                                            // Stage 1 Row
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                spacing: 10

                                                Rectangle {
                                                    implicitWidth: s1TagTxt.contentWidth + 14
                                                    height: 24
                                                    radius: 6
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1
                                                    Text {
                                                        id: s1TagTxt
                                                        anchors.centerIn: parent
                                                        text: qsTr("Stage 1 · Classifier")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.DemiBold
                                                        color: dashboardWindow.mutedTextColor
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: juliaStatusTxt.contentWidth + 18
                                                    height: 24
                                                    radius: 12
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1

                                                    Text {
                                                        id: juliaStatusTxt
                                                        anchors.centerIn: parent
                                                        text: (typeof hostController !== "undefined" && hostController.hasJuliaModel)
                                                              ? qsTr("● Julia Loaded (%1)").arg(hostController.acceleratorName.length > 0 ? hostController.acceleratorName : "MPS")
                                                              : (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled)
                                                                ? qsTr("⏸ S1 Ready on disk")
                                                                : qsTr("○ S1 Not downloaded")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.Medium
                                                        color: (typeof hostController !== "undefined" && hostController.hasJuliaModel) ? "#34D399" : dashboardWindow.fgColor
                                                    }
                                                }

                                                Rectangle {
                                                    visible: typeof hostController !== "undefined" && !hostController.hasJuliaModel && ["NOT_LOADED", "FAILED"].indexOf(hostController.juliaState) >= 0
                                                    implicitWidth: dlJuliaCard1Txt.contentWidth + 20
                                                    height: 24
                                                    radius: 6
                                                    color: dlJuliaCard1Mouse.containsMouse ? theme.hover : theme.surface
                                                    border.color: (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled) ? "#38BDF8" : theme.border
                                                    border.width: 1

                                                    Text {
                                                        id: dlJuliaCard1Txt
                                                        anchors.centerIn: parent
                                                        text: (typeof hostController !== "undefined" && hostController.isJuliaModelInstalled)
                                                              ? qsTr("⟳ Load Julia into GPU")
                                                              : qsTr("⬇ Download Julia (600 MB)")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.Medium
                                                        color: dlJuliaCard1Mouse.containsMouse ? theme.foreground : ((typeof hostController !== "undefined" && hostController.isJuliaModelInstalled) ? "#38BDF8" : theme.foreground)
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
                                                                    dashboardWindow.showToast("LOADING JULIA INTO GPU MEMORY...");
                                                                } else {
                                                                    hostController.downloadJuliaModel();
                                                                    dashboardWindow.showToast("DOWNLOADING AND VERIFYING JULIA-1...");
                                                                }
                                                            }
                                                        }
                                                    }
                                                }

                                                Item { Layout.fillWidth: true }
                                            }

                                            // Stage 2 Row
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                spacing: 10

                                                Rectangle {
                                                    implicitWidth: s2TagTxt.contentWidth + 14
                                                    height: 24
                                                    radius: 6
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1
                                                    Text {
                                                        id: s2TagTxt
                                                        anchors.centerIn: parent
                                                        text: qsTr("Stage 2 · Reasoning SLM")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.DemiBold
                                                        color: dashboardWindow.mutedTextColor
                                                    }
                                                }

                                                Rectangle {
                                                    implicitWidth: graniteTopStatusTxt.contentWidth + 18
                                                    height: 24
                                                    radius: 12
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1

                                                    Text {
                                                        id: graniteTopStatusTxt
                                                        anchors.centerIn: parent
                                                        text: {
                                                            if (typeof hostController === "undefined") return qsTr("○ S2 Offline");
                                                            if (hostController.useLiveWeights || hostController.nativeGraniteResidency === "LOADED" || hostController.nativeGraniteMemoryStatus.indexOf("Loaded in GPU memory") !== -1) {
                                                                return qsTr("● Granite Loaded (%1)").arg(hostController.acceleratorName);
                                                            }
                                                            if (hostController.isCheckingModel || hostController.nativeGraniteMemoryStatus.indexOf("Warming") !== -1) {
                                                                return qsTr("⟳ Warming Granite...");
                                                            }
                                                            if (hostController.isNativeGraniteInstalled) {
                                                                return qsTr("⏸ S2 Ready on disk");
                                                            }
                                                            return qsTr("○ S2 Not downloaded");
                                                        }
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.Medium
                                                        color: {
                                                            if (typeof hostController === "undefined") return dashboardWindow.mutedTextColor;
                                                            if (hostController.useLiveWeights || hostController.nativeGraniteResidency === "LOADED" || hostController.nativeGraniteMemoryStatus.indexOf("Loaded in GPU memory") !== -1) {
                                                                return "#34D399";
                                                            }
                                                            if (hostController.isCheckingModel || hostController.nativeGraniteMemoryStatus.indexOf("Warming") !== -1) {
                                                                return "#FBBF24";
                                                            }
                                                            return dashboardWindow.fgColor;
                                                        }
                                                    }
                                                }

                                                Rectangle {
                                                    visible: typeof hostController !== "undefined" && hostController.nativeGraniteSelected && !hostController.useLiveWeights && hostController.nativeGraniteResidency !== "LOADED" && hostController.nativeGraniteMemoryStatus.indexOf("Loaded in GPU memory") === -1 && hostController.isNativeGraniteInstalled && !hostController.isCheckingModel
                                                    implicitWidth: loadGraniteCard1Txt.contentWidth + 20
                                                    height: 24
                                                    radius: 6
                                                    color: loadGraniteCard1Mouse.containsMouse ? theme.hover : theme.surface
                                                    border.color: "#38BDF8"
                                                    border.width: 1

                                                    Text {
                                                        id: loadGraniteCard1Txt
                                                        anchors.centerIn: parent
                                                        text: qsTr("⟳ Load Granite into GPU")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.Medium
                                                        color: loadGraniteCard1Mouse.containsMouse ? theme.foreground : "#38BDF8"
                                                    }

                                                    HudMouseArea {
                                                        id: loadGraniteCard1Mouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                hostController.warmSelectedModel();
                                                                dashboardWindow.showToast("WARMING GRANITE INTO GPU MEMORY...");
                                                            }
                                                        }
                                                    }
                                                }

                                                Item { Layout.fillWidth: true }
                                            }
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            wrapMode: Text.WordWrap
                                            text: qsTr("Julia classifies first: below 0.70 keeps the original; otherwise Granite supplies choices. Confirm intent explicitly.")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            color: dashboardWindow.mutedTextColor
                                        }
                                    }
                                }

                                // Card 2: Rewrite Target Profile
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: rewriteProfCol.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: rewriteProfCol
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        Text {
                                            text: qsTr("Rewrite target profile")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: dashboardWindow.fgColor
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            wrapMode: Text.WordWrap
                                            text: qsTr("Choose the structural format Roclint generates when clicking Rewrite prompt:")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            color: dashboardWindow.mutedTextColor
                                        }

                                        Flow {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 8
                                            Repeater {
                                                model: [
                                                    { id: "OPENAI_STRUCTURED", label: qsTr("OpenAI · Markdown") },
                                                    { id: "ANTHROPIC_STRUCTURED", label: qsTr("Anthropic · XML") },
                                                    { id: "GENERIC", label: qsTr("Generic Markdown") },
                                                    { id: "LOCAL_COMPACT", label: qsTr("Compact") }
                                                ]

                                                Rectangle {
                                                    required property var modelData
                                                    implicitHeight: 32
                                                    implicitWidth: targetBtnTxt.implicitWidth + 24
                                                    radius: 8
                                                    color: isProfileActive(modelData.id) ? theme.accent : theme.surface
                                                    border.color: isProfileActive(modelData.id) ? theme.accent : theme.border
                                                    border.width: 1

                                                    Text {
                                                        id: targetBtnTxt
                                                        anchors.centerIn: parent
                                                        text: modelData.label
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 12
                                                        font.weight: isProfileActive(modelData.id) ? Font.DemiBold : Font.Medium
                                                        color: isProfileActive(modelData.id) ? "#FFFFFF" : dashboardWindow.fgColor
                                                    }

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            if (typeof hostController !== "undefined") {
                                                                hostController.setRewriteProfile(modelData.id, hostController.stateVersion);
                                                                dashboardWindow.showToast("REWRITE PROFILE: " + modelData.label);
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 6

                                            Repeater {
                                                model: [
                                                    { id: "generic", label: "GENERIC", desc: qsTr("Clean standardized markdown envelope suitable for any AI assistant.") },
                                                    { id: "anthropic_structured", label: "ANTHROPIC_STRUCTURED", desc: qsTr("Strict XML tags (<task>, <context>, <constraints>) optimized for Anthropic.") },
                                                    { id: "openai_structured", label: "OPENAI_STRUCTURED", desc: qsTr("Markdown heading hierarchy (# Role, # Goal, # Constraints) optimized for OpenAI.") },
                                                    { id: "local_compact", label: "LOCAL_COMPACT", desc: qsTr("Telegraphic, token-budget optimized encoding for local models.") }
                                                ]

                                                delegate: Text {
                                                    objectName: "settingsProfileDesc_" + modelData.id
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    wrapMode: Text.WordWrap
                                                    text: "• " + modelData.desc
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 11
                                                    color: isProfileActive(modelData.id) ? dashboardWindow.fgColor : dashboardWindow.mutedTextColor
                                                }
                                            }
                                        }
                                    }
                                }

                                JuliaSettings {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    currentTheme: dashboardWindow.currentTheme
                                }
                            }

                            // =================================================
                            // TAB 1: INTEGRATION & THEME
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 1
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 14

                                Text {
                                    text: qsTr("Editor Integration & Theme")
                                    font.family: dashboardWindow.sansFont
                                    font.pixelSize: 18
                                    font.weight: Font.DemiBold
                                    color: dashboardWindow.fgColor
                                }

                                // Direct Editor Injection
                                Rectangle {
                                    objectName: "editorInjectionCard"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: injectionLayout.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: injectionLayout
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            Text {
                                                objectName: "editorInjectionHeading"
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                                text: qsTr("Direct In-Place Editor Injection (Accessibility API)")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                                color: dashboardWindow.fgColor
                                                wrapMode: Text.WordWrap
                                            }
                                            Text {
                                                objectName: "ambientCaptureStatus"
                                                Layout.alignment: Qt.AlignTop
                                                text: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? "[STATUS: ENABLED]" : "[STATUS: DISABLED]"
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? "#7EE787" : dashboardWindow.mutedTextColor
                                            }
                                        }

                                        Flow {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 10
                                            Rectangle {
                                                implicitWidth: injEnTxt.contentWidth + 24
                                                height: 32
                                                radius: 8
                                                color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? theme.accent : theme.surface
                                                border.color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? theme.accent : theme.border
                                                border.width: 1

                                                Text {
                                                    id: injEnTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("Enabled (Captured Field)")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? Font.DemiBold : Font.Medium
                                                    color: (typeof hostController !== "undefined" && hostController.ambientModeEnabled) ? "#FFFFFF" : dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    objectName: "ambientEnableAction"
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.ambientModeEnabled = true;
                                                            dashboardWindow.showToast("DIRECT INJECTION: ENABLED");
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: injDisTxt.contentWidth + 24
                                                height: 32
                                                radius: 8
                                                color: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? theme.accent : theme.surface
                                                border.color: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? theme.accent : theme.border
                                                border.width: 1

                                                Text {
                                                    id: injDisTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("Disabled (Clipboard Fallback)")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? Font.DemiBold : Font.Medium
                                                    color: (typeof hostController !== "undefined" && !hostController.ambientModeEnabled) ? "#FFFFFF" : dashboardWindow.fgColor
                                                }
                                                HudMouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    objectName: "ambientDisableAction"
                                                    onClicked: {
                                                        if (typeof hostController !== "undefined") {
                                                            hostController.ambientModeEnabled = false;
                                                            dashboardWindow.showToast("DIRECT INJECTION: DISABLED (CLIPBOARD MODE)");
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            objectName: "editorInjectionHelp"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: (typeof hostController !== "undefined" && hostController.accessibilityTrusted)
                                                ? qsTr("Live capture watches the focused editable field. Insert replaces that captured draft after approval.")
                                                : qsTr("Accessibility is unavailable. Copy the approved prompt, then paste it into your editor.")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            color: dashboardWindow.mutedTextColor
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }

                                // Color Palette Theme
                                Rectangle {
                                    objectName: "settingsThemeCard"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: themeLayout.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: themeLayout
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        Text {
                                            text: qsTr("Color Palette Theme")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: dashboardWindow.fgColor
                                        }

                                        Flow {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 10
                                            Repeater {
                                                model: [
                                                    { id: "dark", label: qsTr("Obsidian Slate"), dotColor: "#345CFF" },
                                                    { id: "acid", label: qsTr("Acid Phosphor"), dotColor: "#33B78D" },
                                                    { id: "amber", label: qsTr("Amber Glow CRT"), dotColor: "#D79124" },
                                                    { id: "light", label: qsTr("Clean Light"), dotColor: "#94A3B8" }
                                                ]
                                                delegate: Rectangle {
                                                    id: thmBtn
                                                    objectName: "settingsThemeChoice_" + modelData.id
                                                    implicitWidth: thmRow.implicitWidth + 24
                                                    height: 32
                                                    radius: 8
                                                    color: dashboardWindow.currentTheme === modelData.id ? theme.accent : (thmMouse.containsMouse ? theme.hover : theme.surface)
                                                    border.color: dashboardWindow.currentTheme === modelData.id ? theme.accent : theme.border
                                                    border.width: 1

                                                    RowLayout {
                                                        id: thmRow
                                                        anchors.centerIn: parent
                                                        spacing: 8

                                                        Rectangle {
                                                            width: 8
                                                            height: 8
                                                            radius: 4
                                                            color: modelData.dotColor
                                                        }

                                                        Text {
                                                            id: thmTxt
                                                            text: modelData.label
                                                            font.family: dashboardWindow.sansFont
                                                            font.pixelSize: 12
                                                            font.weight: dashboardWindow.currentTheme === modelData.id ? Font.DemiBold : Font.Medium
                                                            color: dashboardWindow.currentTheme === modelData.id ? "#FFFFFF" : dashboardWindow.fgColor
                                                        }
                                                    }

                                                    HudMouseArea {
                                                        id: thmMouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
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
                                    text: qsTr("SQLite History & Prompt Lineage")
                                    font.family: dashboardWindow.sansFont
                                    font.pixelSize: 18
                                    font.weight: Font.DemiBold
                                    color: dashboardWindow.fgColor
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: dbStatCol.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: dbStatCol
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        Text {
                                            text: qsTr("SQLite Database Status · WAL Mode ACID Storage")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: dashboardWindow.fgColor
                                        }

                                        RowLayout {
                                            Text {
                                                text: qsTr("Location:")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 100
                                            }
                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.databasePath.length > 0) ? hostController.databasePath : "Storage unavailable"
                                                font.family: dashboardWindow.monoFont
                                                font.pixelSize: 11
                                                color: dashboardWindow.fgColor
                                                elide: Text.ElideMiddle
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
                                            }
                                        }

                                        RowLayout {
                                            Text {
                                                text: qsTr("Storage Stats:")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: dashboardWindow.mutedTextColor
                                                Layout.preferredWidth: 100
                                            }
                                            Text {
                                                text: (typeof hostController !== "undefined" && hostController.databaseStats.length > 0) ? hostController.databaseStats : "Storage unavailable"
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                color: dashboardWindow.fgColor
                                            }
                                        }

                                        RowLayout {
                                            spacing: 10
                                            Rectangle {
                                                implicitWidth: vacTxt.contentWidth + 24
                                                height: 32
                                                radius: 8
                                                color: vacMouse.containsMouse ? theme.hover : theme.surface
                                                border.color: theme.border
                                                border.width: 1

                                                Text {
                                                    id: vacTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("⟳ Refresh Stats")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: Font.Medium
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
                                                implicitWidth: delHistTxt.contentWidth + 24
                                                height: 32
                                                radius: 8
                                                color: delHistMouse.containsMouse ? "#8B1E1E" : "transparent"
                                                border.color: "#8B1E1E"
                                                border.width: 1

                                                Text {
                                                    id: delHistTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("Purge History")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: Font.Medium
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
                                    text: qsTr("Recent Prompt Sessions & Revision Lineage")
                                    font.family: dashboardWindow.sansFont
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
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
                                        height: 64
                                        radius: 10
                                        color: theme.surface
                                        border.color: theme.border
                                        border.width: 1

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 12
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
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        color: dashboardWindow.mutedTextColor
                                                    }
                                                }

                                                Text {
                                                    text: (typeof modelData.latestPrompt !== "undefined" && modelData.latestPrompt.length > 0) ? modelData.latestPrompt : "(empty prompt)"
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    color: dashboardWindow.fgColor
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                }
                                            }

                                            Rectangle {
                                                implicitWidth: restTxt.contentWidth + 18
                                                height: 26
                                                radius: 6
                                                color: restMouse.containsMouse ? theme.hover : theme.surface
                                                border.color: "#227B62"
                                                border.width: 1

                                                Text {
                                                    id: restTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("Restore")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 11
                                                    font.weight: Font.Medium
                                                    color: dashboardWindow.fgColor
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
                                                implicitWidth: delItemTxt.contentWidth + 18
                                                height: 26
                                                radius: 6
                                                color: delItemMouse.containsMouse ? "#8B1E1E" : theme.surface
                                                border.color: "#8B1E1E"
                                                border.width: 1

                                                Text {
                                                    id: delItemTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("Delete")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 11
                                                    font.weight: Font.Medium
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

                            // =================================================
                            // TAB 3: PROMPT ACTIONS & REWRITE GUIDE
                            // =================================================
                            ColumnLayout {
                                visible: dashboardWindow.currentTab === 3
                                width: rightScrollView.availableWidth
                                spacing: 14

                                Text {
                                    text: qsTr("Prompt Actions, Rewrite Profiles & Workflow Guide")
                                    font.family: dashboardWindow.sansFont
                                    font.pixelSize: 18
                                    font.weight: Font.DemiBold
                                    color: dashboardWindow.fgColor
                                }

                                // Card 1: Prompt Actions Reference
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: guideCol.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: guideCol
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 14

                                        RowLayout {
                                            Text {
                                                text: qsTr("Prompt Actions Reference · Clarify vs. Rewrite vs. Edit")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Rectangle {
                                                implicitWidth: coreChipTxt.contentWidth + 14
                                                height: 20
                                                radius: 10
                                                color: theme.surface
                                                border.color: theme.border
                                                border.width: 1
                                                Text {
                                                    id: coreChipTxt
                                                    anchors.centerIn: parent
                                                    text: qsTr("Core Workflow")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 10
                                                    font.weight: Font.Medium
                                                    color: dashboardWindow.mutedTextColor
                                                }
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 1
                                            color: dashboardWindow.borderColor
                                            opacity: 0.5
                                        }

                                        // Clarify block
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            RowLayout {
                                                spacing: 8
                                                Rectangle {
                                                    implicitWidth: clrTagTxt.contentWidth + 14
                                                    height: 22
                                                    radius: 6
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1
                                                    Text {
                                                        id: clrTagTxt
                                                        anchors.centerIn: parent
                                                        text: qsTr("Clarify")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.DemiBold
                                                        color: dashboardWindow.fgColor
                                                    }
                                                }
                                                Text {
                                                    text: qsTr("Discovery & Progressive Interview")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: Font.DemiBold
                                                    color: dashboardWindow.fgColor
                                                }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("Diagnose before treating. The local reasoning model (currently Granite) scans your draft for missing context, ambiguities, or contradictory requirements. It asks 1–2 focused questions with concrete trade-off options before any rewrite occurs, ensuring your intent is pinned down without guessing.")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 12
                                                color: dashboardWindow.mutedTextColor
                                                wrapMode: Text.WordWrap
                                            }
                                            Text {
                                                text: qsTr("→ Use when: Your idea is raw, incomplete, or contains conflicting requirements.")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 11
                                                color: dashboardWindow.mutedTextColor
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 1
                                            color: dashboardWindow.borderColor
                                            opacity: 0.5
                                        }

                                        // Rewrite block
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            RowLayout {
                                                spacing: 8
                                                Rectangle {
                                                    implicitWidth: rwTagTxt.contentWidth + 14
                                                    height: 22
                                                    radius: 6
                                                    color: theme.accent
                                                    border.color: theme.accent
                                                    border.width: 1
                                                    Text {
                                                        id: rwTagTxt
                                                        anchors.centerIn: parent
                                                        text: qsTr("Rewrite prompt")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.bold: true
                                                        color: "#FFFFFF"
                                                    }
                                                }
                                                Text {
                                                    text: qsTr("Contract Synthesis & Hardening")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: Font.DemiBold
                                                    color: dashboardWindow.fgColor
                                                }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("Direct translation into a production prompt contract. Restructures your draft for downstream AI (Anthropic, OpenAI, Google). Strictly preserves negative prohibitions ('do not touch X'), code identifiers, and quoted data without altering your core task. Validated by an independent adversarial Goldfish audit pass.")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 12
                                                color: dashboardWindow.mutedTextColor
                                                wrapMode: Text.WordWrap
                                            }
                                            Text {
                                                text: qsTr("→ Use when: Your intent is clear and you want clean, structured prompt engineering.")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 11
                                                color: dashboardWindow.mutedTextColor
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 1
                                            color: dashboardWindow.borderColor
                                            opacity: 0.5
                                        }

                                        // Edit draft block
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            RowLayout {
                                                spacing: 8
                                                Rectangle {
                                                    implicitWidth: edTagTxt.contentWidth + 14
                                                    height: 22
                                                    radius: 6
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1
                                                    Text {
                                                        id: edTagTxt
                                                        anchors.centerIn: parent
                                                        text: qsTr("Edit draft")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.DemiBold
                                                        color: dashboardWindow.fgColor
                                                    }
                                                }
                                                Text {
                                                    text: qsTr("Manual Control")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: Font.DemiBold
                                                    color: dashboardWindow.fgColor
                                                }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("In-place editing. Pauses model inference and lets you modify the raw prompt text directly inside Roclint without running any local neural passes.")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 12
                                                color: dashboardWindow.mutedTextColor
                                                wrapMode: Text.WordWrap
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 1
                                            color: dashboardWindow.borderColor
                                            opacity: 0.5
                                        }

                                        // Workspace options block
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            RowLayout {
                                                spacing: 8
                                                Rectangle {
                                                    implicitWidth: wsTagTxt.contentWidth + 14
                                                    height: 22
                                                    radius: 6
                                                    color: theme.surface
                                                    border.color: theme.border
                                                    border.width: 1
                                                    Text {
                                                        id: wsTagTxt
                                                        anchors.centerIn: parent
                                                        text: qsTr("Workspace options")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.DemiBold
                                                        color: dashboardWindow.fgColor
                                                    }
                                                }
                                                Text {
                                                    text: qsTr("Local Code Grounding")
                                                    font.family: dashboardWindow.sansFont
                                                    font.pixelSize: 12
                                                    font.weight: Font.DemiBold
                                                    color: dashboardWindow.fgColor
                                                }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("Repository symbol awareness. Extracts matching class and function declarations from your active workspace so rewritten prompts don't guess names. (100% on-device; zero code leaves your machine).")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 12
                                                color: dashboardWindow.mutedTextColor
                                                wrapMode: Text.WordWrap
                                            }
                                        }
                                    }
                                }

                                // Card 2: Rewrite Profiles Selector
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: profCardCol.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: profCardCol
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        RowLayout {
                                            Text {
                                                text: qsTr("Rewrite Profiles · Choose Target Prompt Format")
                                                font.family: dashboardWindow.sansFont
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                                color: dashboardWindow.fgColor
                                            }
                                            Item { Layout.fillWidth: true }
                                            Rectangle {
                                                implicitWidth: activeProfRow2.implicitWidth + 16
                                                height: 22
                                                radius: 11
                                                color: theme.surface
                                                border.color: theme.border
                                                border.width: 1
                                                RowLayout {
                                                    id: activeProfRow2
                                                    anchors.centerIn: parent
                                                    spacing: 6
                                                    Rectangle {
                                                        width: 6; height: 6; radius: 3
                                                        color: theme.accent
                                                    }
                                                    Text {
                                                        id: activeProfTxt2
                                                        text: qsTr("Active: ") + (typeof hostController !== "undefined" ? hostController.rewriteProfile.toUpperCase() : "GENERIC")
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        font.weight: Font.Medium
                                                        color: dashboardWindow.fgColor
                                                    }
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            spacing: 8
                                            Repeater {
                                                model: [
                                                    { id: "openai_structured", label: "OPENAI_STRUCTURED", desc: "Markdown heading hierarchy (# Role, # Goal, # Constraints) optimized for OpenAI" },
                                                    { id: "anthropic_structured", label: "ANTHROPIC_STRUCTURED", desc: "Strict XML tags (<task>, <context>, <constraints>) optimized for Anthropic" },
                                                    { id: "generic", label: "GENERIC", desc: "Clean standardized markdown envelope with standardized instructions and constraints" },
                                                    { id: "local_compact", label: "LOCAL_COMPACT", desc: "Telegraphic, token-budget optimized encoding for local models" }
                                                ]

                                                delegate: RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 10

                                                    Rectangle {
                                                        implicitWidth: profBtnTxt2.contentWidth + 20
                                                        height: 28
                                                        radius: 8
                                                        color: isProfileActive(modelData.id) ? theme.accent : theme.surface
                                                        border.color: isProfileActive(modelData.id) ? theme.accent : theme.border
                                                        border.width: 1

                                                        Text {
                                                            id: profBtnTxt2
                                                            anchors.centerIn: parent
                                                            text: modelData.label
                                                            font.family: dashboardWindow.sansFont
                                                            font.pixelSize: 11
                                                            font.weight: isProfileActive(modelData.id) ? Font.DemiBold : Font.Medium
                                                            color: isProfileActive(modelData.id) ? "#FFFFFF" : dashboardWindow.fgColor
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
                                                        Layout.fillWidth: true
                                                        text: modelData.desc
                                                        font.family: dashboardWindow.sansFont
                                                        font.pixelSize: 11
                                                        color: dashboardWindow.mutedTextColor
                                                        wrapMode: Text.WordWrap
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // Card 3: Two-stage engine & delivery
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    implicitHeight: delivCol.implicitHeight + 28
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.borderColor
                                    border.width: 1
                                    radius: 14

                                    ColumnLayout {
                                        id: delivCol
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        Text {
                                            text: qsTr("Universal Editor Delivery & 100% Private Execution")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: dashboardWindow.fgColor
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("• In-Place Injection: Accepting a rewrite replaces your draft directly inside your active editor, IDE, or chat box (supports Antigravity, VS Code, Cursor, Codex, Xcode, terminal CLIs, and more) with the finalized contract.")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            color: dashboardWindow.fgColor
                                            wrapMode: Text.WordWrap
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("• Non-Destructive: Click 'Keep original' or press Esc at any time to preserve your draft untouched.")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            color: dashboardWindow.mutedTextColor
                                            wrapMode: Text.WordWrap
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("• 100% Offline & Local: S1 Julia-1 and S2 Granite are currently supported out of the box, running completely on your local device hardware (GPU / CPU) with extensible support for additional models. Zero telemetry, zero cloud calls, zero prompt leakage.")
                                            font.family: dashboardWindow.sansFont
                                            font.pixelSize: 12
                                            color: dashboardWindow.fgColor
                                            wrapMode: Text.WordWrap
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
                        height: 26
                        implicitWidth: toastTextLabel.contentWidth + 20
                        radius: 13
                        color: dashboardWindow.dimColor
                        border.color: dashboardWindow.borderColor
                        border.width: 1
                        visible: dashboardWindow.toastMessage.length > 0

                        Text {
                            id: toastTextLabel
                            anchors.centerIn: parent
                            text: dashboardWindow.toastMessage.length ? dashboardWindow.toastMessage : (typeof hostController !== "undefined" ? hostController.lastError : "")
                            font.family: dashboardWindow.sansFont
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            color: dashboardWindow.fgColor
                        }
                    }
                }
            }
        }
    }
}
