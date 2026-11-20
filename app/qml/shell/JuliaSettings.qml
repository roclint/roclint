import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: juliaSettingsRoot
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    spacing: 14
    readonly property int currentTab: 0
    readonly property string globalShortcutString: hc ? hostController.shortcutLabel : "Ctrl+Shift+L"
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Segoe UI"
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null

    property string currentTheme: "dark"
    RocTheme { id: theme; mode: juliaSettingsRoot.currentTheme }

    function showToast(msg) {
        if (typeof dashboardWindow !== "undefined" && dashboardWindow && typeof dashboardWindow.showToast === "function") {
            dashboardWindow.showToast(msg);
        }
    }

    component SectionCard: Rectangle {
        id: card
        default property alias content: cardColumn.data
        property string heading: ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: cardColumn.implicitHeight + 28
        color: theme.inset
        border.color: theme.border
        border.width: 1
        radius: 14
        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10
            Text {
                text: card.heading
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: theme.foreground
            }
        }
    }

    component Body: Text {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.family: juliaSettingsRoot.sansFont
        font.pixelSize: 12
        color: theme.muted
    }

    component ActionButton: Rectangle {
        id: btn
        property string label: ""
        property bool primary: false
        property bool active: true
        property bool selected: false
        signal clicked()
        enabled: active
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.onPressAction: if (active) clicked()
        Keys.onPressed: function(event) {
            if (active && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
                clicked();
                event.accepted = true;
            }
        }
        implicitHeight: 32
        implicitWidth: btnText.implicitWidth + 24
        radius: 8
        opacity: active ? 1.0 : 0.4
        color: primary || selected ? theme.accent : (btnMouse.containsMouse && active ? theme.hover : theme.surface)
        border.color: primary || selected ? theme.accent : theme.border
        border.width: activeFocus ? 2 : 1
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label
            font.family: juliaSettingsRoot.sansFont
            font.pixelSize: 12
            font.weight: btn.primary || btn.selected ? Font.DemiBold : Font.Medium
            color: btn.primary || btn.selected ? "#FFFFFF" : theme.foreground
        }
        MouseArea {
            id: btnMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: btn.active
            cursorShape: btn.active ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: { btn.forceActiveFocus(); btn.clicked(); }
        }
    }

    // =========================================================================
    // CARD 3: Model Storage
    // =========================================================================
    SectionCard {
        visible: juliaSettingsRoot.currentTab === 0
        heading: qsTr("Model storage")

        Body {
            text: juliaSettingsRoot.hc
                ? qsTr("Base folder: %1\n\n• S1 Classifier (/s1): %2\n  %3\n\n• S2 Granite 3B (/s2): %4\n  %5\n\nDownloads are stored in /s1 and /s2 automatically. You can also select custom folders below to bring your own models from any directory.")
                    .arg(hostController.modelsBaseDirectory)
                    .arg(hostController.juliaModelDirectory)
                    .arg(hostController.juliaDiskStatus)
                    .arg(hostController.nativeGraniteDirectory)
                    .arg(hostController.nativeGraniteDiskStatus)
                : ""
            wrapMode: Text.WrapAnywhere
        }

        Flow {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8
            ActionButton {
                label: qsTr("Models root folder…")
                active: juliaSettingsRoot.hc && !hostController.isInferring && !hostController.isDownloadingModel
                onClicked: hostController.chooseModelsBaseDirectory()
            }
        }
    }

    // =========================================================================
    // CARD 4: Stage 1 — Julia Classifier Runtime
    // =========================================================================
    SectionCard {
        visible: juliaSettingsRoot.currentTab === 0
        heading: qsTr("Stage 1 — Julia Classifier Runtime")

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            ActionButton {
                label: qsTr("Julia · local")
                selected: juliaSettingsRoot.hc && hostController.scorerEngine === "JULIA_LOCAL"
                onClicked: {
                    if (hostController.scorerEngine !== "JULIA_LOCAL" && hostController.setScorerEngine("julia")) {
                        hostController.refreshJuliaModel();
                        juliaSettingsRoot.showToast("S1 CLASSIFIER: LOCAL JULIA");
                    }
                }
            }
            ActionButton {
                label: qsTr("Jev · cloud")
                selected: juliaSettingsRoot.hc && hostController.scorerEngine === "JEV_CLOUD"
                onClicked: {
                    hostController.setScorerEngine("jev");
                    juliaSettingsRoot.showToast("JEV SELECTED; CLOUD TRANSPORT IS NOT CONFIGURED");
                }
            }
            Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
        }

        Body {
            text: juliaSettingsRoot.hc ? hostController.scorerStatus : ""
        }

        Body {
            text: qsTr("Julia runs on this computer and scores the choices produced by the analysis model. Scores never select an answer for you. If the selected scorer is unavailable, choices remain unranked. Jev requires a cloud integration, which is not configured in this build.")
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.scorerEngine === "JULIA_LOCAL"
            text: juliaSettingsRoot.hc ? hostController.juliaStatus : ""
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.scorerEngine === "JULIA_LOCAL"
            text: juliaSettingsRoot.hc
                ? qsTr("Model folder (S1): %1\nDisk status: %2\nRuntime status: %3\nPython: %4\nDevice override: %5")
                    .arg(hostController.juliaModelDirectory)
                    .arg(hostController.juliaDiskStatus)
                    .arg(hostController.juliaMemoryStatus)
                    .arg(hostController.juliaPython || qsTr("Not found"))
                    .arg(hostController.juliaDevice || qsTr("auto"))
                : ""
            wrapMode: Text.WrapAnywhere
        }

        // Persistent CPU-fallback advisory: shown whenever Julia is ready but running
        // on CPU FP32, so the user cannot miss it even without opening the log.
        Rectangle {
            visible: juliaSettingsRoot.hc
                && hostController.scorerEngine === "JULIA_LOCAL"
                && hostController.hasJuliaModel
                && hostController.juliaStatus.indexOf("CPU (FP32)") >= 0
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            height: cpuWarningText.implicitHeight + 16
            color: "#2a1a00"
            border.color: "#8a5a00"
            radius: 8

            Text {
                id: cpuWarningText
                anchors { fill: parent; margins: 8 }
                text: qsTr("⚠ GPU acceleration unavailable — Julia is running on CPU.\nInstall CUDA drivers (NVIDIA) or torch-directml (AMD/Intel/Qualcomm),\nor set Device override to force a specific backend.")
                color: "#d4a030"
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.scorerEngine === "JULIA_LOCAL"
            text: qsTr("Note: If the Julia classifier is installed on disk, click 'Load Julia' or submit a prompt to warm it into local GPU memory. If missing, click 'Download / repair' or click 'Model folder…' to bring your own.")
        }

        Flow {
            visible: juliaSettingsRoot.hc && hostController.scorerEngine === "JULIA_LOCAL"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8
            readonly property bool busy: juliaSettingsRoot.hc && ["STARTING", "LOADING", "DOWNLOADING", "SCORING", "TRIAGING", "STOPPING"].indexOf(hostController.juliaState) >= 0

            ActionButton {
                label: qsTr("Load Julia")
                active: !parent.busy
                onClicked: {
                    hostController.refreshJuliaModel();
                    juliaSettingsRoot.showToast("LOADING JULIA INTO GPU MEMORY...");
                }
            }
            ActionButton {
                label: qsTr("Download / repair · 600 MB")
                active: !parent.busy
                onClicked: {
                    hostController.downloadJuliaModel();
                    juliaSettingsRoot.showToast("DOWNLOADING AND VERIFYING JULIA-1...");
                }
            }
            ActionButton {
                label: qsTr("Model folder…")
                active: !parent.busy
                onClicked: hostController.chooseJuliaModelDirectory()
            }
            ActionButton {
                label: qsTr("Python…")
                active: !parent.busy
                onClicked: hostController.chooseJuliaPython()
            }
        }
    }

    // =========================================================================
    // CARD 5: Stage 2 — Granite Reasoning & Rewrite SLM (Consolidated)
    // =========================================================================
    SectionCard {
        visible: juliaSettingsRoot.currentTab === 0
        heading: qsTr("Stage 2 — Granite Reasoning & Rewrite SLM")

        // Row 1: Engine Mode Toggle
        Flow {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8

            ActionButton {
                label: qsTr("Native Granite 4.2 3B")
                active: juliaSettingsRoot.hc
                selected: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
                onClicked: {
                    hostController.setModelRuntimeSource(true);
                    juliaSettingsRoot.showToast("RUNTIME: NATIVE GRANITE 4.2 3B");
                }
            }
            ActionButton {
                label: qsTr("Ollama · development")
                active: juliaSettingsRoot.hc
                selected: juliaSettingsRoot.hc && !hostController.nativeGraniteSelected
                onClicked: {
                    hostController.setModelRuntimeSource(false);
                    juliaSettingsRoot.showToast("RUNTIME: OLLAMA DEVELOPMENT");
                }
            }
        }

        // Row 2: Runtime Actions (Rescan & Warm)
        Flow {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 10

            Rectangle {
                objectName: "settingsRescanRuntimeAction"
                implicitWidth: nativeBtnTxt.contentWidth + 24
                height: 32
                radius: 8
                color: rescanMouse.containsMouse ? theme.hover : theme.surface
                border.color: theme.border
                border.width: 1

                Text {
                    id: nativeBtnTxt
                    anchors.centerIn: parent
                    text: qsTr("⟳ Rescan Runtime")
                    font.family: juliaSettingsRoot.sansFont
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    color: theme.foreground
                }
                HudMouseArea {
                    id: rescanMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof hostController !== "undefined") {
                            hostController.refreshAvailableModels();
                            juliaSettingsRoot.showToast("RESCANNED LOCAL MODEL DIRECTORY");
                        }
                    }
                }
            }

            Rectangle {
                objectName: "settingsWarmModelAction"
                implicitWidth: liveBtnTxt.contentWidth + 24
                height: 32
                radius: 8
                color: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                       ? theme.accent
                       : (warmMouse.containsMouse ? theme.hover : theme.surface)
                border.color: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                              ? theme.accent
                              : theme.border
                border.width: 1

                Text {
                    id: liveBtnTxt
                    anchors.centerIn: parent
                    text: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                          ? qsTr("● Weights loaded in GPU (Ready)")
                          : (typeof hostController !== "undefined" && hostController.isCheckingModel)
                            ? qsTr("⟳ Warming into GPU memory…")
                            : (typeof hostController !== "undefined" && hostController.isNativeGraniteInstalled)
                              ? qsTr("⏵ Warm model into GPU")
                              : qsTr("○ No weights on disk (Download below)")
                    font.family: juliaSettingsRoot.sansFont
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: (typeof hostController !== "undefined" && hostController.useLiveWeights)
                           ? "#FFFFFF"
                           : theme.foreground
                }
                HudMouseArea {
                    id: warmMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (typeof hostController !== "undefined") {
                            if (hostController.isNativeGraniteInstalled && !hostController.useLiveWeights) {
                                hostController.warmSelectedModel();
                                juliaSettingsRoot.showToast("WARMING MODEL INTO GPU MEMORY...");
                            } else {
                                hostController.refreshAvailableModels();
                                juliaSettingsRoot.showToast("CHECKING LOCAL MODEL RUNTIME");
                            }
                        }
                    }
                }
            }
        }

        // Row 3: Status bar
        Rectangle {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            height: 28
            radius: 8
            color: theme.surface
            border.color: theme.border
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Rectangle {
                    width: 6; height: 6
                    radius: 3
                    color: (typeof hostController !== "undefined" && hostController.availableModels.length > 0) ? "#34D399" : "#FFB000"
                }

                Text {
                    objectName: "settingsModelEngineStatus"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: typeof hostController !== "undefined" ? hostController.modelEngineStatus : "LOADING STATUS..."
                    font.family: juliaSettingsRoot.sansFont
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: theme.foreground
                    elide: Text.ElideRight
                }

                Text {
                    visible: typeof hostController !== "undefined" && hostController.inferenceSpeed.length > 0
                    textFormat: Text.StyledText
                    text: {
                        if (typeof hostController === "undefined") return "";
                        if (hostController.tokenSpeedMode === "BOTH" && hostController.generationTokensPerSecond > 0 && hostController.decodeTokensPerSecond > 0) {
                            return "Speed: <font color='#34D399'>" + hostController.generationSpeedLabel + " (gen)</font> · <font color='#94A3B8'>" + hostController.decodeSpeedLabel + " (dec)</font>";
                        }
                        return "Speed: " + hostController.inferenceSpeed;
                    }
                    font.family: juliaSettingsRoot.sansFont
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: "#38BDF8"
                }
            }
        }

        // Row 4: Non-Invasive Model Folder Connection
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 6

            Text {
                text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                      ? qsTr("Connected Model Folder (S2 Granite Models):")
                      : qsTr("Connected Model Folder (Non-Invasive Access):")
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 11
                font.bold: true
                color: theme.muted
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    height: 32
                    radius: 8
                    color: theme.surface
                    border.color: theme.border
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10

                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: (typeof hostController !== "undefined" && hostController.effectiveModelDirectory.length > 0)
                                  ? hostController.effectiveModelDirectory
                                  : "No folder connected"
                            font.family: juliaSettingsRoot.monoFont
                            font.pixelSize: 11
                            color: theme.foreground
                            elide: Text.ElideMiddle
                        }
                    }
                }

                Rectangle {
                    implicitWidth: connBtnTxt.contentWidth + 20
                    height: 32
                    radius: 8
                    color: connectMouse.containsMouse ? theme.hover : theme.surface
                    border.color: theme.border
                    border.width: 1

                    Text {
                        id: connBtnTxt
                        anchors.centerIn: parent
                        text: qsTr("📂 Connect Folder…")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: theme.foreground
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
                                    juliaSettingsRoot.showToast("CONNECTED MODEL FOLDER: " + chosen);
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "settingsRescanFolderAction"
                    implicitWidth: refreshBtnTxt.contentWidth + 20
                    height: 32
                    radius: 8
                    color: refreshMouse.containsMouse ? theme.hover : theme.surface
                    border.color: theme.border
                    border.width: 1

                    Text {
                        id: refreshBtnTxt
                        anchors.centerIn: parent
                        text: qsTr("⟳ Rescan Folder")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: theme.foreground
                    }

                    HudMouseArea {
                        id: refreshMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof hostController !== "undefined") {
                                hostController.refreshAvailableModels();
                                juliaSettingsRoot.showToast("RESCANNED CONNECTED FOLDER");
                            }
                        }
                    }
                }
            }

            Text {
                objectName: "settingsFolderStatusMessage"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                wrapMode: Text.WordWrap
                text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                      ? (hostController.isNativeGraniteInstalled
                         ? "✓ Model weights verified on disk in this folder. Ready to load into " + (typeof hostController !== "undefined" ? hostController.acceleratorName : "GPU") + " GPU memory."
                         : "⚠️ No model weights found in this folder. Download Granite below or click '📂 Connect Folder…' to point to your existing models.")
                      : "* Non-invasive isolation: Roclint only reads models inside this specific folder. Your hard drive and personal files are never searched."
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 11
                color: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                       ? (hostController.isNativeGraniteInstalled ? "#34D399" : "#FFB000")
                       : theme.muted
            }
        }

        // Row 5: Detected Models
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 6

            Text {
                text: qsTr("Detected Local Models in Folder / Runner:")
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 11
                font.bold: true
                color: theme.muted
            }

            Repeater {
                model: (typeof hostController !== "undefined") ? hostController.availableModels : []

                delegate: Rectangle {
                    objectName: "detectedModelRow_" + index
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: Math.max(34, modelRowLayout.implicitHeight + 8)
                    radius: 8
                    color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                           ? theme.hover
                           : theme.surface
                    border.color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                  ? theme.accent
                                  : theme.border
                    border.width: 1

                    RowLayout {
                        id: modelRowLayout
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8

                        Rectangle {
                            implicitWidth: tagTxt.contentWidth + 12
                            height: 20
                            radius: 6
                            color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                   ? theme.accent
                                   : "transparent"
                            border.color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                          ? theme.accent
                                          : theme.border
                            border.width: 1

                            Text {
                                id: tagTxt
                                anchors.centerIn: parent
                                text: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                      ? (hostController.useLiveWeights ? qsTr("● Loaded in GPU") : qsTr("★ Selected"))
                                      : (modelData.requiresImport ? qsTr("Import") : qsTr("Select"))
                                font.family: juliaSettingsRoot.sansFont
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                color: (typeof hostController !== "undefined" && hostController.activeModel === modelData.name)
                                       ? "#FFFFFF"
                                       : theme.muted
                            }
                        }

                        Text {
                            text: modelData.name
                            font.family: juliaSettingsRoot.sansFont
                            font.pixelSize: 12
                            font.bold: true
                            color: theme.foreground
                        }

                        Text {
                            objectName: "detectedModelDetails_" + index
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: "(" + modelData.size + ", " + modelData.source + ", " + modelData.quantization + ") · Verified on disk"
                            font.family: juliaSettingsRoot.sansFont
                            font.pixelSize: 11
                            color: theme.muted
                            elide: Text.ElideRight
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
                                juliaSettingsRoot.showToast(modelData.requiresImport ? "IMPORTING LOCAL GGUF..." : "ACTIVE MODEL: " + modelData.name);
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
                height: 34
                radius: 8
                color: theme.surface
                border.color: theme.border
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12

                    Text {
                        text: qsTr("No models found. Connect your model directory or download a model below.")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        color: theme.muted
                    }
                }
            }
        }

        // Row 6: Download Actions
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 6

            Text {
                text: qsTr("Model Download & Repository Actions:")
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 11
                font.bold: true
                color: theme.muted
            }

            // Active download progress
            Rectangle {
                visible: typeof hostController !== "undefined" && hostController.isDownloadingModel
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                height: 48
                radius: 8
                color: theme.surface
                border.color: "#FFB000"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Text {
                            text: qsTr("Downloading model: ") + (typeof hostController !== "undefined" ? hostController.downloadProgress : 0) + "%"
                            font.family: juliaSettingsRoot.sansFont
                            font.pixelSize: 11
                            font.bold: true
                            color: "#FFB000"
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: qsTr("Cancel")
                            font.family: juliaSettingsRoot.sansFont
                            font.pixelSize: 11
                            font.bold: true
                            color: theme.foreground
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
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        color: theme.muted
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
                    objectName: "settingsDownloadQ4Action"
                    implicitWidth: dl0Txt.contentWidth + 24
                    height: 32
                    radius: 8
                    color: dl0Mouse.containsMouse ? theme.hover : theme.surface
                    border.color: theme.border
                    border.width: 1

                    Text {
                        id: dl0Txt
                        anchors.centerIn: parent
                        text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                              ? (hostController.isNativeGraniteQ4Installed
                                 ? qsTr("✓ Q4_K_M Installed (2.09 GiB) · Re-download")
                                 : qsTr("⬇ Download Granite 4.2 Q4_K_M (2.09 GiB) · Fastest"))
                              : qsTr("⬇ Pull Granite 4.2 3B Q4")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: theme.foreground
                    }

                    HudMouseArea {
                        id: dl0Mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof hostController !== "undefined") {
                                hostController.downloadModel(hostController.nativeGraniteSelected ? "native-granite:Q4_K_M" : "granite4.2:3b-q4_k_m");
                                juliaSettingsRoot.showToast("DOWNLOADING GRANITE 4.2 3B (Q4_K_M)...");
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "settingsDownloadQ8Action"
                    implicitWidth: dl1Txt.contentWidth + 24
                    height: 32
                    radius: 8
                    color: dl1Mouse.containsMouse ? theme.hover : theme.surface
                    border.color: theme.border
                    border.width: 1

                    Text {
                        id: dl1Txt
                        anchors.centerIn: parent
                        text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                              ? (hostController.isNativeGraniteQ8Installed
                                 ? qsTr("✓ Q8_0 Installed (3.63 GiB) · Re-download")
                                 : qsTr("⬇ Download Granite 4.2 Q8_0 (3.63 GiB)"))
                              : qsTr("⬇ Pull Granite 4.2 3B (3.63 GiB)")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: theme.foreground
                    }

                    HudMouseArea {
                        id: dl1Mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof hostController !== "undefined") {
                                hostController.downloadModel(hostController.nativeGraniteSelected ? "native-granite:Q8_0" : "granite4.2:3b-q8_0");
                                juliaSettingsRoot.showToast("DOWNLOADING GRANITE 4.2 3B (Q8_0)...");
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "settingsDownloadQ5Action"
                    implicitWidth: dl2Txt.contentWidth + 24
                    height: 32
                    radius: 8
                    color: dl2Mouse.containsMouse ? theme.hover : theme.surface
                    border.color: theme.border
                    border.width: 1

                    Text {
                        id: dl2Txt
                        anchors.centerIn: parent
                        text: (typeof hostController !== "undefined" && hostController.nativeGraniteSelected)
                              ? (hostController.isNativeGraniteQ5Installed
                                 ? qsTr("✓ Q5_K_M Installed (2.43 GiB) · Re-download")
                                 : qsTr("⬇ Download Granite 4.2 Q5_K_M (2.43 GiB)"))
                              : qsTr("⬇ Pull Granite 3.1 2B (1.5 GB)")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: theme.foreground
                    }

                    HudMouseArea {
                        id: dl2Mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof hostController !== "undefined") {
                                hostController.downloadModel(hostController.nativeGraniteSelected ? "native-granite:Q5_K_M" : "granite3.1-dense:2b");
                                juliaSettingsRoot.showToast(hostController.nativeGraniteSelected ? "DOWNLOADING PINNED GRANITE 4.2 (Q5_K_M)..." : "DOWNLOADING GRANITE 3.1 2B...");
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "settingsTestRuntimeAction"
                    implicitWidth: testInfTxt.contentWidth + 24
                    height: 32
                    radius: 8
                    color: testInfMouse.containsMouse ? theme.hover : theme.surface
                    border.color: theme.border
                    border.width: 1

                    Text {
                        id: testInfTxt
                        anchors.centerIn: parent
                        text: (typeof hostController !== "undefined" && hostController.isInferring)
                              ? qsTr("Working…")
                              : (typeof hostController !== "undefined" && hostController.useLiveWeights)
                                ? qsTr("● Model Ready (GPU)")
                                : qsTr("⟳ Test Model Runtime")
                        font.family: juliaSettingsRoot.sansFont
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        color: theme.foreground
                    }

                    HudMouseArea {
                        id: testInfMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (typeof hostController !== "undefined") {
                                juliaSettingsRoot.showToast("CHECKING AND WARMING THE SELECTED MODEL...");
                                hostController.warmSelectedModel();
                            }
                        }
                    }
                }
            }

            Text {
                objectName: "settingsStorageDisclaimer"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("Storage: Models are stored in your configured folder (S1 in /s1, S2 in /s2). Click '📂 Connect Folder…' above to use existing models from any directory.")
                font.family: juliaSettingsRoot.sansFont
                font.pixelSize: 11
                color: theme.muted
                wrapMode: Text.WordWrap
            }
        }

        // Row 7: Memory Retention & GPU Residency
        Text {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: qsTr("Memory retention & GPU residency:")
            font.family: juliaSettingsRoot.sansFont
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: theme.foreground
        }

        Flow {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8
            Repeater {
                model: [{label: qsTr("Keep warm"), value: "KEEP_WARM"},
                        {label: qsTr("Balanced"), value: "BALANCED"},
                        {label: qsTr("Low memory"), value: "LOW_MEMORY"}]
                ActionButton {
                    required property var modelData
                    objectName: "graniteResidency_" + modelData.value
                    label: modelData.label
                    selected: juliaSettingsRoot.hc && hostController.graniteResidencyPreset === modelData.value
                    active: juliaSettingsRoot.hc
                    onClicked: hostController.setGraniteResidencyPreset(modelData.value)
                }
            }
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: !juliaSettingsRoot.hc ? "" : hostController.graniteResidencyPreset === "KEEP_WARM"
                ? qsTr("Keep a recently loaded model ready. The operating system may release it when memory is low.")
                : hostController.graniteResidencyPreset === "LOW_MEMORY"
                ? qsTr("Release an unused model after 30 seconds. The next request warms it again.")
                : qsTr("After 5 idle minutes, release GPU weights or retain reclaimable CPU mappings. Release remaining mappings after 30 minutes. The next request may take longer to warm.")
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: juliaSettingsRoot.hc ? hostController.graniteResidencyStatus : ""
        }

        // Row 8: Inference Thinking
        Text {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: qsTr("Think before answering")
            font.family: juliaSettingsRoot.sansFont
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: theme.foreground
        }

        Flow {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8
            ActionButton {
                objectName: "thinkingModeOff"
                label: qsTr("Off")
                selected: juliaSettingsRoot.hc && !hostController.thinkingEnabled
                active: juliaSettingsRoot.hc
                onClicked: hostController.setThinkingEnabled(false)
            }
            ActionButton {
                objectName: "thinkingModeOn"
                label: qsTr("More thinking")
                selected: juliaSettingsRoot.hc && hostController.thinkingEnabled
                active: juliaSettingsRoot.hc
                onClicked: hostController.setThinkingEnabled(true)
            }
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: qsTr("Give Roclint extra time to work through a prompt. Experimental: more thinking can take longer and may not improve every suggestion.")
        }

        // Row 9: Inference Speed Metric
        Text {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: qsTr("Inference speed metric:")
            font.family: juliaSettingsRoot.sansFont
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: theme.foreground
        }

        Flow {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 8
            Repeater {
                model: [{label: qsTr("Both (Gen & Dec)"), value: "BOTH"},
                        {label: qsTr("Generation speed"), value: "GENERATION"},
                        {label: qsTr("Decode speed"), value: "DECODE"}]
                ActionButton {
                    required property var modelData
                    objectName: "tokenSpeedMode_" + modelData.value
                    label: modelData.label
                    selected: juliaSettingsRoot.hc && hostController.tokenSpeedMode === modelData.value
                    active: juliaSettingsRoot.hc
                    onClicked: hostController.setTokenSpeedMode(modelData.value)
                }
            }
        }

        Body {
            visible: juliaSettingsRoot.hc && hostController.nativeGraniteSelected
            text: !juliaSettingsRoot.hc ? "" : hostController.tokenSpeedMode === "BOTH"
                ? qsTr("Show both views of the same model output tokens. Generation (green) includes sampling, streaming and completed forward passes; decode measures completed forward passes only. Neither includes prompt preparation or model loading.")
                : hostController.tokenSpeedMode === "GENERATION"
                ? qsTr("Output tokens divided by generation-loop time, including sampling, detokenization, streaming and completed forward passes. Excludes model loading and prompt preparation.")
                : qsTr("The same output tokens divided by completed neural forward-pass time, including the GPU completion wait. This is compute throughput, not a second token count.")
        }
    }
}
