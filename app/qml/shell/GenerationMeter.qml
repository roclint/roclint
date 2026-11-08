import QtQuick
import QtQuick.Controls
import QtQuick.Window

// Two measured views of the same native output tokens. Motion only follows
// telemetry arrivals; idle, preparation and stalled streams remain still.
Item {
    id: meter
    objectName: "generationMeter"
    implicitWidth: 250
    implicitHeight: 52
    property double tokensPerSecond: 0 // Legacy binding; never substituted for either measured rate.
    property double generationTokensPerSecond: 0
    property double decodeTokensPerSecond: 0
    property double generationElapsedMs: 0
    property double decodeElapsedMs: 0
    property string tokenSpeedMode: "BOTH"
    property double tokenCount: 0
    property bool processing: false
    property bool preparing: false
    property bool metricsAvailable: true
    property bool reducedMotion: false
    property string currentTheme: "dark"
    property double seenTokens: 0
    property double seenGenerationMs: 0
    property double seenDecodeMs: 0
    property double displayedGenRate: 0
    property double displayedDecRate: 0
    property double rateUpdatedAt: 0
    property double holdUntil: 0
    property double fadeUntil: 0
    property real activityLevel: 0
    property real generationLevel: 0
    property real decodeLevel: 0
    property real generationPosition: 0
    property real decodePosition: 0
    readonly property bool surfaceVisible: visible && Window.window !== null && Window.window.visible && Window.window.visibility !== Window.Minimized
    readonly property bool recentActivity: activityLevel > 0
    readonly property color genColor: theme.mode === "light" ? "#047857" : "#34D399"
    readonly property color decColor: theme.mode === "light" ? "#475569" : "#94A3B8"
    function rate(value) { return value > 0 && isFinite(value) ? value.toFixed(1) : "—"; }
    readonly property string rateText: {
        if (preparing || (processing && !seenTokens)) return metricsAvailable ? qsTr("Preparing…") : qsTr("Working…");
        if (!seenTokens) return qsTr("— tok/s");
        const prefix = processing ? "" : qsTr("Last · ");
        const g = rate(displayedGenRate), d = rate(displayedDecRate);
        if (tokenSpeedMode === "BOTH") {
            const suffixG = meter.width < 165 ? "g" : " gen";
            const suffixD = meter.width < 165 ? "d" : " dec";
            return prefix + "<font color='" + genColor + "'>" + g + suffixG + "</font> · <font color='" + decColor + "'>" + d + suffixD + "</font>" + (meter.width >= 215 ? " tok/s" : "");
        }
        return tokenSpeedMode === "DECODE"
            ? prefix + "<font color='" + decColor + "'>" + d + " tok/s (dec)</font>"
            : prefix + "<font color='" + genColor + "'>" + g + " tok/s</font>";
    }
    Accessible.role: Accessible.StaticText
    Accessible.focusable: false
    Accessible.focused: false
    Accessible.name: {
        if (preparing || (processing && !seenTokens)) return metricsAvailable ? qsTr("Preparing…") : qsTr("Working…");
        if (!seenTokens) return qsTr("No generation speed measured yet");
        const prefix = processing ? "" : qsTr("Last: ");
        if (tokenSpeedMode === "BOTH") return prefix + rate(displayedGenRate) + " tok/s generation, " + rate(displayedDecRate) + " tok/s decode";
        return prefix + (tokenSpeedMode === "DECODE" ? rate(displayedDecRate) + " tok/s decode" : rate(displayedGenRate) + " tok/s generation");
    }
    Accessible.description: qsTr("Generation and decode traces respond only to measured native token progress")
    RocTheme { id: theme; mode: meter.currentTheme }

    function resetPresentation() {
        seenTokens = 0;
        seenGenerationMs = 0;
        seenDecodeMs = 0;
        displayedGenRate = 0;
        displayedDecRate = 0;
        rateUpdatedAt = 0;
        holdUntil = 0;
        fadeUntil = 0;
        activityLevel = 0;
        generationLevel = 0;
        decodeLevel = 0;
    }
    function refreshRate(now, force) {
        if (seenTokens > 0 && (force || rateUpdatedAt === 0 || now < rateUpdatedAt || now - rateUpdatedAt >= 500)) {
            displayedGenRate = isFinite(generationTokensPerSecond) && generationTokensPerSecond > 0 ? generationTokensPerSecond : 0;
            displayedDecRate = isFinite(decodeTokensPerSecond) && decodeTokensPerSecond > 0 ? decodeTokensPerSecond : 0;
            rateUpdatedAt = now;
        }
    }
    function recordProgress(now) {
        if (tokenCount < seenTokens || generationElapsedMs < seenGenerationMs || decodeElapsedMs < seenDecodeMs)
            resetPresentation();
        if (tokenCount > seenTokens && generationElapsedMs > 0 && decodeElapsedMs > 0 && decodeElapsedMs <= generationElapsedMs) {
            const delta = tokenCount - seenTokens;
            const genMs = generationElapsedMs - seenGenerationMs;
            const decMs = decodeElapsedMs - seenDecodeMs;
            if (genMs <= 0 || decMs <= 0) return;
            // Bounded height encodes the measured batch rate. Positions advance
            // from each clock, not a continuously running cosmetic oscillator.
            const genRate = delta * 1000 / genMs, decRate = delta * 1000 / decMs;
            generationLevel = genRate / (genRate + 60);
            decodeLevel = decRate / (decRate + 60);
            generationPosition = Math.round(4 * Math.sin(generationElapsedMs / 180));
            decodePosition = Math.round(4 * Math.sin(decodeElapsedMs / 180));
            seenTokens = tokenCount;
            seenGenerationMs = generationElapsedMs;
            seenDecodeMs = decodeElapsedMs;
            if (!reducedMotion) {
                holdUntil = now + 350;
                fadeUntil = holdUntil + 350;
                activityLevel = 1;
            }
        }
        refreshRate(now, !processing);
    }
    function recordLatestProgress() { recordProgress(Date.now()); }
    function scheduleProgress() { Qt.callLater(recordLatestProgress); }
    function advance(now) {
        activityLevel = reducedMotion ? 0 : now <= holdUntil && holdUntil > 0 ? 1
            : Math.max(0, Math.min(1, (fadeUntil - now) / 350));
        refreshRate(now, !processing);
    }
    // Coalesce bindings from a single controller snapshot before reading counts,
    // clocks and rates together; never combine a new count with an old duration.
    onTokenCountChanged: scheduleProgress()
    onGenerationElapsedMsChanged: scheduleProgress()
    onDecodeElapsedMsChanged: scheduleProgress()
    onGenerationTokensPerSecondChanged: scheduleProgress()
    onDecodeTokensPerSecondChanged: scheduleProgress()
    onTokenSpeedModeChanged: refreshRate(Date.now(), true)
    onProcessingChanged: {
        if (processing) resetPresentation();
        else { recordProgress(Date.now()); refreshRate(Date.now(), true); }
    }
    onReducedMotionChanged: if (reducedMotion) activityLevel = 0
    Component.onCompleted: scheduleProgress()
    Timer {
        interval: 50
        running: meter.surfaceVisible && !meter.reducedMotion && meter.recentActivity
        repeat: true
        onTriggered: meter.advance(Date.now())
    }
    Text {
        objectName: "generationRateLabel"
        anchors.right: parent.right
        anchors.top: parent.top
        width: Math.min(implicitWidth, meter.width)
        elide: Text.ElideRight
        visible: meter.width >= 65
        textFormat: Text.StyledText
        text: meter.rateText
        color: meter.processing ? theme.foreground : theme.muted
        font.family: theme.monoFont
        font.pixelSize: meter.width < 125 ? 10 : 12
    }
    Item {
        id: wave
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.max(12, meter.height - 14)
        clip: true
        visible: meter.width >= 28
        Repeater {
            model: meter.tokenSpeedMode === "BOTH" ? 2 : 1
            Item {
                required property int index
                readonly property bool generation: meter.tokenSpeedMode !== "DECODE" && index === 0
                readonly property real level: generation ? meter.generationLevel : meter.decodeLevel
                width: wave.width
                height: meter.tokenSpeedMode === "BOTH" ? (wave.height - 2) / 2 : wave.height
                y: index * (height + 2)
                clip: true
                Text {
                    text: parent.generation ? "G" : "D"
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    color: parent.generation ? meter.genColor : meter.decColor
                    font.family: theme.monoFont
                    font.pixelSize: 8
                    Accessible.ignored: true
                }
                Image {
                    objectName: parent.generation ? "referencePixelWave" : "decodePixelWave"
                    anchors.bottom: parent.bottom
                    x: 12 + (meter.reducedMotion || !meter.recentActivity ? 0 : parent.generation ? meter.generationPosition : meter.decodePosition)
                    width: parent.width - 16
                    height: meter.reducedMotion || !meter.recentActivity ? parent.height : Math.max(3, Math.round(parent.height * (0.65 + 0.35 * parent.level)))
                    source: "assets/pixel-signal.png"
                    sourceClipRect: Qt.rect(230, 250, 1900, 200)
                    fillMode: Image.Stretch
                    smooth: false
                    opacity: meter.reducedMotion ? 0.55 : 0.35 + 0.65 * meter.activityLevel
                    Behavior on x { enabled: meter.surfaceVisible && !meter.reducedMotion; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                    Behavior on height { enabled: meter.surfaceVisible && !meter.reducedMotion; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                    Accessible.ignored: true
                }
            }
        }
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: meter.metricsAvailable
        ? qsTr("G: output tokens / generation-loop time. D: the same tokens / completed forward-pass time. Includes structured model output for this native request, not only visible rewrite words. Traces move only when token measurements arrive; Last is the completed request.")
        : qsTr("Live token metrics are available with the native local model.")
}
