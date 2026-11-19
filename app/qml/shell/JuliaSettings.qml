import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: dashboardWindow
    Layout.minimumWidth: 0
    spacing: 8
    readonly property int currentTab: 0
    readonly property string globalShortcutString: hc ? hostController.shortcutLabel : "Ctrl+Shift+L"
    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos"
    readonly property string monoFont: isMac ? "Menlo" : "Consolas"
    readonly property string sansFont: isMac ? "Helvetica Neue" : "Segoe UI"
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null

    property string currentTheme: "dark"
    RocTheme { id: theme; mode: dashboardWindow.currentTheme }

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
        radius: 6
        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8
            Text {
                text: card.heading
                font.family: dashboardWindow.sansFont
                font.pixelSize: 13
                font.bold: true
                color: theme.foreground
            }
        }
    }

    component Body: Text {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.family: dashboardWindow.sansFont
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
        implicitHeight: 28
        implicitWidth: btnText.implicitWidth + 22
        radius: 4
        opacity: active ? 1.0 : 0.4
        color: primary || selected ? theme.foreground : (btnMouse.containsMouse && active ? theme.hover : "transparent")
        border.color: theme.foreground
        border.width: activeFocus ? 3 : 1
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label
            font.family: dashboardWindow.sansFont
            font.pixelSize: 12
            font.bold: btn.primary || btn.selected
            color: btn.primary || btn.selected ? theme.background : theme.foreground
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
                        SectionCard {
                            visible: dashboardWindow.currentTab === 0
                            heading: qsTr("Model storage")
                            Body {
                                text: dashboardWindow.hc
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
                                    active: dashboardWindow.hc && !hostController.isInferring && !hostController.isDownloadingModel
                                    onClicked: hostController.chooseModelsBaseDirectory()
                                }
                            }
                        }
                        SectionCard {
                            visible: dashboardWindow.currentTab === 0
                            heading: qsTr("Analysis and rewrite runtime")
                            Flow {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 8
                                ActionButton {
                                    label: qsTr("Native Granite 4.2 3B")
                                    active: dashboardWindow.hc
                                    selected: dashboardWindow.hc && hostController.nativeGraniteSelected
                                    onClicked: hostController.setModelRuntimeSource(true)
                                }
                                ActionButton {
                                    label: qsTr("Ollama · development")
                                    active: dashboardWindow.hc
                                    selected: dashboardWindow.hc && !hostController.nativeGraniteSelected
                                    onClicked: hostController.setModelRuntimeSource(false)
                                }
                            }
                            Body {
                                text: dashboardWindow.hc && hostController.nativeGraniteSelected
                                    ? qsTr("Granite runs locally through the packaged native runtime. Download a pinned model below, then choose Analyze or Load model.")
                                    : qsTr("The Ollama development route uses a separately installed local service and an explicitly selected model.")
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: dashboardWindow.hc
                                    ? qsTr("Model folder (S2): %1\nDisk status: %2\nMemory status: %3")
                                        .arg(hostController.nativeGraniteDirectory)
                                        .arg(hostController.nativeGraniteDiskStatus)
                                        .arg(hostController.nativeGraniteMemoryStatus)
                                    : ""
                                wrapMode: Text.WrapAnywhere
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: qsTr("Note: 'Unloaded' means the model is safely stored and verified on disk, and will automatically warm into Apple Metal GPU memory on your first prompt (or when clicking 'Load model' below). Unloading frees system RAM while Roclint is idle.")
                            }
                            Flow {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 8
                                ActionButton {
                                    label: qsTr("Load model")
                                    active: dashboardWindow.hc && !hostController.isCheckingModel && !hostController.isInferring && !hostController.isDownloadingModel
                                    onClicked: hostController.warmSelectedModel()
                                }
                                ActionButton {
                                    label: qsTr("Pinned model folder…")
                                    active: dashboardWindow.hc && !hostController.isInferring
                                    onClicked: hostController.chooseNativeModelDirectory()
                                }
                            }
                            Flow {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
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
                                        selected: dashboardWindow.hc && hostController.graniteResidencyPreset === modelData.value
                                        active: dashboardWindow.hc
                                        onClicked: hostController.setGraniteResidencyPreset(modelData.value)
                                    }
                                }
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: !dashboardWindow.hc ? "" : hostController.graniteResidencyPreset === "KEEP_WARM"
                                    ? qsTr("Keep a recently loaded model ready. The operating system may release it when memory is low.")
                                    : hostController.graniteResidencyPreset === "LOW_MEMORY"
                                    ? qsTr("Release an unused model after 30 seconds. The next request warms it again.")
                                    : qsTr("After 5 idle minutes, release GPU weights or retain reclaimable CPU mappings. Release remaining mappings after 30 minutes. The next request may take longer to warm.")
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: dashboardWindow.hc ? hostController.graniteResidencyStatus : ""
                            }
                            Text {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: qsTr("Think before answering")
                                font.family: dashboardWindow.sansFont
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: theme.foreground
                            }
                            Flow {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                Layout.fillWidth: true
                                spacing: 8
                                ActionButton {
                                    objectName: "thinkingModeOff"
                                    label: qsTr("Off")
                                    selected: dashboardWindow.hc && !hostController.thinkingEnabled
                                    active: dashboardWindow.hc
                                    onClicked: hostController.setThinkingEnabled(false)
                                }
                                ActionButton {
                                    objectName: "thinkingModeOn"
                                    label: qsTr("More thinking")
                                    selected: dashboardWindow.hc && hostController.thinkingEnabled
                                    active: dashboardWindow.hc
                                    onClicked: hostController.setThinkingEnabled(true)
                                }
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: qsTr("Give Roclint extra time to work through a prompt. Experimental: more thinking can take longer and may not improve every suggestion.")
                            }
                            Text {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: qsTr("Inference speed metric:")
                                font.family: dashboardWindow.sansFont
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                color: theme.foreground
                            }
                            Flow {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
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
                                        selected: dashboardWindow.hc && hostController.tokenSpeedMode === modelData.value
                                        active: dashboardWindow.hc
                                        onClicked: hostController.setTokenSpeedMode(modelData.value)
                                    }
                                }
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.nativeGraniteSelected
                                text: !dashboardWindow.hc ? "" : hostController.tokenSpeedMode === "BOTH"
                                    ? qsTr("Show both views of the same model output tokens. Generation (green) includes sampling, streaming and completed forward passes; decode measures completed forward passes only. Neither includes prompt preparation or model loading.")
                                    : hostController.tokenSpeedMode === "GENERATION"
                                    ? qsTr("Output tokens divided by generation-loop time, including sampling, detokenization, streaming and completed forward passes. Excludes model loading and prompt preparation.")
                                    : qsTr("The same output tokens divided by completed neural forward-pass time, including the GPU completion wait. This is compute throughput, not a second token count.")
                            }
                        }
                        SectionCard {
                            visible: dashboardWindow.currentTab === 0
                            heading: qsTr("Julia runtime")
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                ActionButton {
                                    label: qsTr("Julia · local")
                                    selected: dashboardWindow.hc && hostController.scorerEngine === "JULIA_LOCAL"
                                    onClicked: {
                                        if (hostController.scorerEngine !== "JULIA_LOCAL" && hostController.setScorerEngine("julia"))
                                            hostController.refreshJuliaModel();
                                    }
                                }
                                ActionButton {
                                    label: qsTr("Jev · cloud")
                                    selected: dashboardWindow.hc && hostController.scorerEngine === "JEV_CLOUD"
                                    onClicked: hostController.setScorerEngine("jev")
                                }
                                Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
                            }
                            Body {
                                text: dashboardWindow.hc ? hostController.scorerStatus : ""
                            }
                            Body {
                                text: qsTr("Julia runs on this computer and scores the choices produced by the analysis model. Scores never select an answer for you. If the selected scorer is unavailable, choices remain unranked. Jev requires a cloud integration, which is not configured in this build.")
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.scorerEngine === "JULIA_LOCAL"
                                text: dashboardWindow.hc ? hostController.juliaStatus : ""
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.scorerEngine === "JULIA_LOCAL"
                                text: dashboardWindow.hc
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
                                visible: dashboardWindow.hc
                                    && hostController.scorerEngine === "JULIA_LOCAL"
                                    && hostController.hasJuliaModel
                                    && hostController.juliaStatus.indexOf("CPU (FP32)") >= 0
                                Layout.fillWidth: true
                                height: cpuWarningText.implicitHeight + 16
                                color: "#2a1a00"
                                border.color: "#8a5a00"
                                radius: 4
                                Text {
                                    id: cpuWarningText
                                    anchors { fill: parent; margins: 8 }
                                    text: qsTr("⚠ GPU acceleration unavailable — Julia is running on CPU.\nInstall CUDA drivers (NVIDIA) or torch-directml (AMD/Intel/Qualcomm),\nor set Device override to force a specific backend.")
                                    color: "#d4a030"
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                }
                            }
                            Body {
                                visible: dashboardWindow.hc && hostController.scorerEngine === "JULIA_LOCAL"
                                text: qsTr("Note: If the Julia classifier is installed on disk, click 'Load Julia' or submit a prompt to warm it into Apple Metal GPU memory. If missing, click 'Download / repair' or click 'Model folder…' to bring your own.")
                            }
                            Flow {
                                visible: dashboardWindow.hc && hostController.scorerEngine === "JULIA_LOCAL"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 8
                                readonly property bool busy: dashboardWindow.hc && ["STARTING", "LOADING", "DOWNLOADING", "SCORING", "TRIAGING", "STOPPING"].indexOf(hostController.juliaState) >= 0
                                ActionButton {
                                    label: qsTr("Load Julia")
                                    active: !parent.busy
                                    onClicked: hostController.refreshJuliaModel()
                                }
                                ActionButton {
                                    label: qsTr("Download / repair · 600 MB")
                                    active: !parent.busy
                                    onClicked: hostController.downloadJuliaModel()
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
}
