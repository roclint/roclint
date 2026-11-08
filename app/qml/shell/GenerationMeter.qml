import QtQuick
import QtQuick.Controls

// Measured speed plus a deliberately readable activity afterglow. The artwork
// stays faithful to the reference; the sweep is feedback, not a throughput chart.
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
    property real activityLevel: 0
    readonly property bool recentActivity: activityLevel > 0
    readonly property real sweepX: Math.floor((((paintedAt - activityStartedAt) % 900) / 900 * (width + 64)) / 6) * 6 - 64
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
    Accessible.description: qsTr("Measured generation speed with a brief recent-activity afterglow")
    RocTheme { id: theme; mode: meter.currentTheme }

    function resetPresentation() {
        seenTokens = tokenCount;
        displayedRate = 0;
        displayedGenRate = 0;
        displayedDecRate = 0;
        rateUpdatedAt = 0;
        activityStartedAt = 0;
        holdUntil = 0;
        fadeUntil = 0;
        activityLevel = 0;
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
    function recordProgress(now) {
        if (tokenCount < seenTokens) { resetPresentation(); return; }
        if (tokenCount > seenTokens) {
            seenTokens = tokenCount;
            if (!reducedMotion) {
                if (!recentActivity) activityStartedAt = now;
                holdUntil = now + 1800;
                fadeUntil = holdUntil + 700;
                paintedAt = now;
                activityLevel = 1;
            }
        }
        refreshRate(now, !processing);
    }
    function advance(now) {
        paintedAt = now;
        activityLevel = reducedMotion ? 0 : now <= holdUntil && holdUntil > 0 ? 1
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
        else recordProgress(Date.now()); // Completing a fast burst must not erase it.
    }
    onReducedMotionChanged: if (reducedMotion) activityLevel = 0
    Component.onCompleted: recordProgress(Date.now())
    Timer {
        interval: 50
        running: meter.visible && !meter.reducedMotion && meter.recentActivity
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
            objectName: "referencePixelWave"
            anchors.fill: parent
            source: "assets/pixel-signal.png"
            sourceClipRect: Qt.rect(230, 250, 1900, 200)
            fillMode: Image.PreserveAspectFit
            smooth: false
            opacity: 0.65 + 0.15 * meter.activityLevel
            Accessible.ignored: true
        }
        Item {
            x: meter.sweepX
            width: 64
            height: parent.height
            clip: true
            opacity: meter.activityLevel
            Image {
                x: -parent.x
                width: wave.width
                height: wave.height
                source: "assets/pixel-signal.png"
                sourceClipRect: Qt.rect(230, 250, 1900, 200)
                fillMode: Image.PreserveAspectFit
                smooth: false
                Accessible.ignored: true
            }
        }
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: meter.metricsAvailable
        ? (meter.processing ? qsTr("%1 tokens · live decode speed. The pixel sweep follows token activity.")
                            : qsTr("%1 tokens · last decode speed. A brief afterglow shows recent activity; generation has finished.")).arg(meter.tokenCount)
        : qsTr("Live token metrics are available with the native local model.")
}
