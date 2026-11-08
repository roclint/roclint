import QtQuick
import QtQuick.Controls
import QtQuick.Window

// Reference artwork with ambient pixel motion, stronger working motion and a
// readable completion tail. Movement is feedback; the numbers remain measured.
Item {
    id: meter
    objectName: "generationMeter"
    implicitWidth: 250
    implicitHeight: 40
    property double tokensPerSecond: 0
    property double generationTokensPerSecond: 0
    property double decodeTokensPerSecond: 0
    property string tokenSpeedMode: "BOTH"
    property double tokenCount: 0
    property bool processing: false
    property bool preparing: false
    property bool metricsAvailable: true
    property bool reducedMotion: false
    property string currentTheme: "dark"
    property double seenTokens: 0
    property double displayedRate: 0
    property double displayedGenRate: 0
    property double displayedDecRate: 0
    property double rateUpdatedAt: 0
    readonly property color genColor: theme.mode === "light" ? "#047857" : "#34D399"
    readonly property color decColor: theme.mode === "light" ? "#475569" : "#94A3B8"
    property double activityStartedAt: 0
    property double holdUntil: 0
    property double fadeUntil: 0
    property double paintedAt: 0
    property double lastFrameAt: 0
    property real wavePhase: 0
    readonly property bool surfaceVisible: visible && Window.window !== null && Window.window.visible && Window.window.visibility !== Window.Minimized
    readonly property bool workingMotion: processing || recentActivity
    readonly property real wavePulse: (Math.sin(wavePhase * Math.PI * 2) + 1) / 2
    property real activityLevel: 0
    readonly property bool recentActivity: activityLevel > 0
    readonly property string rateText: {
        if (preparing) return metricsAvailable ? qsTr("Preparing…") : qsTr("Working…");
        if (processing && (!seenTokens || (displayedRate <= 0 && displayedGenRate <= 0 && displayedDecRate <= 0)))
            return metricsAvailable ? qsTr("Preparing…") : qsTr("Working…");
        if (!seenTokens || (displayedRate <= 0 && displayedGenRate <= 0 && displayedDecRate <= 0))
            return qsTr("— tok/s");

        const prefix = processing ? "" : qsTr("Last · ");
        if (tokenSpeedMode === "BOTH" && (displayedGenRate > 0 || displayedDecRate > 0)) {
            const g = displayedGenRate > 0 ? displayedGenRate.toFixed(1) : (displayedRate > 0 ? displayedRate.toFixed(1) : "—");
            const d = displayedDecRate > 0 ? displayedDecRate.toFixed(1) : (displayedRate > 0 ? displayedRate.toFixed(1) : "—");
            if (meter.width < 165) {
                return prefix + "<font color='" + genColor + "'>" + g + "g</font> · <font color='" + decColor + "'>" + d + "d</font>";
            }
            return prefix + "<font color='" + genColor + "'>" + g + " gen</font> · <font color='" + decColor + "'>" + d + " dec</font>";
        } else if (tokenSpeedMode === "DECODE") {
            const rate = displayedDecRate > 0 ? displayedDecRate : displayedRate;
            return prefix + "<font color='" + decColor + "'>" + rate.toFixed(1) + " tok/s (dec)</font>";
        } else {
            const rate = displayedGenRate > 0 ? displayedGenRate : displayedRate;
            return prefix + "<font color='" + genColor + "'>" + rate.toFixed(1) + " tok/s</font>";
        }
    }
    Accessible.role: Accessible.StaticText
    Accessible.focusable: false
    Accessible.focused: false
    Accessible.name: {
        if (preparing || (processing && !seenTokens)) return metricsAvailable ? qsTr("Preparing…") : qsTr("Working…");
        if (!seenTokens || (displayedRate <= 0 && displayedGenRate <= 0 && displayedDecRate <= 0)) return qsTr("No generation speed measured yet");
        if (tokenSpeedMode === "BOTH" && (displayedGenRate > 0 || displayedDecRate > 0)) {
            return (processing ? "" : "Last: ") + displayedGenRate.toFixed(1) + " tok/s generation, " + displayedDecRate.toFixed(1) + " tok/s decode";
        } else if (tokenSpeedMode === "DECODE") {
            const rate = displayedDecRate > 0 ? displayedDecRate : displayedRate;
            return (processing ? "" : "Last: ") + rate.toFixed(1) + " tok/s decode";
        } else {
            const rate = displayedGenRate > 0 ? displayedGenRate : displayedRate;
            return (processing ? "" : "Last: ") + rate.toFixed(1) + " tok/s generation";
        }
    }
    Accessible.description: qsTr("Measured generation speed with an animated pixel-wave activity indicator")
    RocTheme { id: theme; mode: meter.currentTheme }

    function resetPresentation() {
        seenTokens = tokenCount;
        displayedRate = 0;
        displayedGenRate = 0;
        displayedDecRate = 0;
        rateUpdatedAt = 0;
    }
    function refreshRate(now, force) {
        if (seenTokens > 0 && (tokensPerSecond > 0 || generationTokensPerSecond > 0 || decodeTokensPerSecond > 0) &&
            (force || rateUpdatedAt === 0 || now < rateUpdatedAt || now - rateUpdatedAt >= 500)) {
            displayedRate = tokensPerSecond > 0 ? tokensPerSecond : (generationTokensPerSecond > 0 ? generationTokensPerSecond : decodeTokensPerSecond);
            displayedGenRate = generationTokensPerSecond > 0 ? generationTokensPerSecond : displayedRate;
            displayedDecRate = decodeTokensPerSecond > 0 ? decodeTokensPerSecond : displayedRate;
            rateUpdatedAt = now;
        }
    }
    function keepActivity(now) {
        if (reducedMotion) return;
        if (!recentActivity) activityStartedAt = now;
        holdUntil = now + 1800;
        fadeUntil = holdUntil + 700;
        activityLevel = 1;
    }
    function recordProgress(now) {
        if (tokenCount < seenTokens) {
            // Native subrequests reset their counters while the pipeline keeps
            // working. Clear measurements without cutting off the visual feedback.
            resetPresentation();
            return;
        }
        if (tokenCount > seenTokens) {
            seenTokens = tokenCount;
            keepActivity(now);
        }
        refreshRate(now, !processing);
    }
    function advance(now) {
        const elapsed = lastFrameAt > 0 ? Math.max(0, Math.min(100, now - lastFrameAt)) : 0;
        lastFrameAt = now;
        paintedAt = now;
        if (!reducedMotion && surfaceVisible)
            wavePhase = (wavePhase + elapsed / (3200 - 2100 * activityLevel)) % 1;
        activityLevel = reducedMotion ? 0 : processing ? 1
            : now <= holdUntil && holdUntil > 0 ? 1
            : Math.max(0, Math.min(1, (fadeUntil - now) / 700));
        refreshRate(now, !processing);
    }
    onTokenCountChanged: recordProgress(Date.now())
    onTokensPerSecondChanged: refreshRate(Date.now(), !processing)
    onGenerationTokensPerSecondChanged: refreshRate(Date.now(), !processing)
    onDecodeTokensPerSecondChanged: refreshRate(Date.now(), !processing)
    onTokenSpeedModeChanged: refreshRate(Date.now(), true)
    onProcessingChanged: {
        if (processing) resetPresentation();
        else recordProgress(Date.now());
        keepActivity(Date.now()); // Includes Julia checks and warmup with zero tokens.
    }
    onSurfaceVisibleChanged: lastFrameAt = Date.now()
    onReducedMotionChanged: if (reducedMotion) activityLevel = 0
    Component.onCompleted: { lastFrameAt = Date.now(); recordProgress(Date.now()); }
    Timer {
        interval: 50
        running: meter.surfaceVisible && !meter.reducedMotion
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
        Image {
            anchors.fill: parent
            source: "assets/pixel-signal.png"
            sourceClipRect: Qt.rect(230, 250, 1900, 200)
            fillMode: Image.Stretch
            smooth: false
            opacity: meter.reducedMotion ? 0 : 0.25
            Accessible.ignored: true
        }
        Image {
            objectName: "referencePixelWave"
            anchors.bottom: parent.bottom
            x: meter.reducedMotion ? 0 : Math.round(2 * Math.sin(meter.wavePhase * Math.PI * 2 + 1.2))
            width: parent.width
            height: meter.reducedMotion ? parent.height : Math.round(parent.height *
                (0.79 + 0.21 * meter.wavePulse - 0.17 * meter.activityLevel * (1 - meter.wavePulse)))
            source: "assets/pixel-signal.png"
            sourceClipRect: Qt.rect(230, 250, 1900, 200)
            fillMode: Image.Stretch
            smooth: false
            opacity: meter.reducedMotion ? 0.65 : 0.75 + 0.20 * meter.activityLevel
            Accessible.ignored: true
        }
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: meter.metricsAvailable
        ? (meter.processing ? qsTr("Working locally · %1 tokens generated").arg(meter.tokenCount)
                            : meter.seenTokens > 0 ? qsTr("Last measured speed. The pixel wave settles after each response.")
                                                   : qsTr("Ready for a prompt. The pixel wave moves gently while idle."))
        : qsTr("Live token metrics are available with the native local model.")
}
