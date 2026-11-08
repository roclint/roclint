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
    readonly property real waveScaleTokensPerSecond: 100
    readonly property int historySize: 48
    property var generationHistory: emptyHistory()
    property var decodeHistory: emptyHistory()
    property var previousGenerationHistory: emptyHistory()
    property var previousDecodeHistory: emptyHistory()
    property real historyBlend: 1
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

    function emptyHistory() { return Array(historySize).fill(0); }
    function levelForRate(value) {
        return isFinite(value) && value > 0 ? Math.min(1, value / waveScaleTokensPerSecond) : 0;
    }
    function blendedHistory(previous, current) {
        return current.map(function(value, index) { return previous[index] + (value - previous[index]) * historyBlend; });
    }
    function appendHistory(history, value, slots) {
        const next = history.slice();
        for (let i = 0; i < slots; ++i) {
            // Each received batch contributes one crest, with measured rate
            // controlling its height. This is arrival feedback, not fabricated
            // per-token timestamps or a free-running decorative waveform.
            const crest = Math.sin(Math.PI * i / Math.max(1, slots - 1));
            next.push(value * crest * crest);
        }
        return next.slice(-historySize);
    }
    function updateWaveforms() {
        // Both measurements share one crest and the same fixed speed scale.
        // IPC batch size must not make an equally fast stream look larger.
        const slots = 8;
        const visibleGen = blendedHistory(previousGenerationHistory, generationHistory);
        const visibleDec = blendedHistory(previousDecodeHistory, decodeHistory);
        waveformTransition.stop();
        previousGenerationHistory = visibleGen;
        previousDecodeHistory = visibleDec;
        generationHistory = appendHistory(generationHistory, generationLevel, slots);
        decodeHistory = appendHistory(decodeHistory, decodeLevel, slots);
        historyBlend = 0;
        if (surfaceVisible && !reducedMotion) waveformTransition.restart();
        else historyBlend = 1;
        pixelWave.requestPaint();
    }
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
        if (waveformTransition) waveformTransition.stop();
        generationHistory = emptyHistory();
        decodeHistory = emptyHistory();
        previousGenerationHistory = emptyHistory();
        previousDecodeHistory = emptyHistory();
        historyBlend = 1;
        if (pixelWave) pixelWave.requestPaint();
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
            const genMs = generationElapsedMs - seenGenerationMs;
            const decMs = decodeElapsedMs - seenDecodeMs;
            if (genMs <= 0 || decMs <= 0) return;
            // Match the request-average speeds reported by the numeric meter.
            // A fixed linear scale makes twice the speed twice the amplitude
            // below the visual ceiling, without adding the two rates together.
            generationLevel = levelForRate(tokenCount * 1000 / generationElapsedMs);
            decodeLevel = levelForRate(tokenCount * 1000 / decodeElapsedMs);
            updateWaveforms();
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
    onProcessingChanged: {
        if (processing) resetPresentation();
        else { recordProgress(Date.now()); refreshRate(Date.now(), true); }
    }
    onActivityLevelChanged: if (pixelWave) pixelWave.requestPaint()
    onHistoryBlendChanged: if (pixelWave) pixelWave.requestPaint()
    onTokenSpeedModeChanged: { refreshRate(Date.now(), true); if (pixelWave) pixelWave.requestPaint(); }
    onCurrentThemeChanged: if (pixelWave) pixelWave.requestPaint()
    onReducedMotionChanged: {
        if (reducedMotion) { waveformTransition.stop(); historyBlend = 1; activityLevel = 0; }
        if (pixelWave) pixelWave.requestPaint();
    }
    onSurfaceVisibleChanged: {
        if (!surfaceVisible) { waveformTransition.stop(); historyBlend = 1; }
        else { advance(Date.now()); pixelWave.requestPaint(); }
    }
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
    NumberAnimation {
        id: waveformTransition
        target: meter
        property: "historyBlend"
        from: 0
        to: 1
        duration: 220
        easing.type: Easing.OutCubic
    }
    Canvas {
        id: pixelWave
        objectName: "tokenPixelWaveform"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.max(12, meter.height - 14)
        visible: meter.width >= 28
        renderStrategy: Canvas.Immediate
        Accessible.ignored: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.clearRect(0, 0, width, height);
            ctx.imageSmoothingEnabled = false;
            const cell = 5, row = 3;
            const columns = Math.max(1, Math.floor(width / cell));
            const baseline = Math.floor((height - 1) / row) * row;
            const intensity = meter.reducedMotion ? 0 : meter.activityLevel;
            ctx.fillStyle = meter.currentTheme === "light" ? "#CBD5F5" : "#172652";
            ctx.fillRect(0, baseline, columns * cell, 1);
            if (intensity <= 0) return;
            const gen = meter.blendedHistory(meter.previousGenerationHistory, meter.generationHistory);
            const dec = meter.blendedHistory(meter.previousDecodeHistory, meter.decodeHistory);
            // Neighbor smoothing joins cells into a stepped, filled waveform,
            // rather than isolated histogram bars. Vertical levels keep the
            // cobalt/periwinkle layers and hard pixel edges of the selected style.
            function envelope(history, column) {
                const position = column * (history.length - 1) / Math.max(1, columns - 1);
                const center = Math.round(position);
                let total = 0, weight = 0;
                for (let offset = -1; offset <= 1; ++offset) {
                    const index = center + offset;
                    if (index < 0 || index >= history.length) continue;
                    const w = 2 - Math.abs(offset);
                    total += history[index] * w;
                    weight += w;
                }
                return weight > 0 ? total / weight : 0;
            }
            const maxRows = Math.max(1, Math.floor(baseline / row));
            for (let x = 0; x < columns; ++x) {
                const genRows = meter.tokenSpeedMode === "DECODE" ? 0 : Math.round(envelope(gen, x) * intensity * maxRows);
                const decRows = meter.tokenSpeedMode === "GENERATION" ? 0 : Math.round(envelope(dec, x) * intensity * maxRows);
                const rows = Math.max(genRows, decRows);
                // One filled waveform: generation is the cobalt body; decode
                // extends its periwinkle edge on the identical baseline/scale.
                for (let y = 0; y < rows; ++y) {
                    const decodeEdge = y >= genRows || (meter.tokenSpeedMode === "BOTH" && y === decRows - 1);
                    if (decodeEdge) ctx.fillStyle = y === decRows - 1 ? "#9EAEFF" : "#657FE8";
                    else ctx.fillStyle = y === genRows - 1 ? "#5F81FF" : y >= genRows * 0.55 ? "#3E62F0" : "#2445C8";
                    ctx.globalAlpha = decodeEdge ? 0.80 : 0.97;
                    ctx.fillRect(x * cell, baseline - (y + 1) * row, cell, row);
                }
            }
            ctx.globalAlpha = 1;
        }
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: meter.metricsAvailable
        ? qsTr("One wave: cobalt body = generation speed; periwinkle edge = decode speed. Both heights use the same fixed 0–100 tok/s scale; faster rates stay at the visual ceiling while numbers remain exact. Includes structured model output for this native request. Crests arrive with token updates. Last is the completed request.")
        : qsTr("Live token metrics are available with the native local model.")
}
