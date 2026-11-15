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
    property double reasoningTokenCount: 0
    property double answerTokenCount: 0
    property string generationPhase: "idle"
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
    property int waveSampleCount: 0
    property double progressTick: 0
    property double previousProgressTick: 0
    readonly property bool surfaceVisible: visible && Window.window !== null && Window.window.visible && Window.window.visibility !== Window.Minimized
    readonly property bool recentActivity: activityLevel > 0
    readonly property color genColor: theme.mode === "light" ? "#047857" : "#34D399"
    readonly property color decColor: theme.mode === "light" ? "#475569" : "#94A3B8"
    function rate(value) { return value > 0 && isFinite(value) ? value.toFixed(1) : "—"; }
    readonly property string rateText: {
        if (preparing || (processing && !seenTokens)) return metricsAvailable ? qsTr("Preparing…") : qsTr("Working…");
        if (!seenTokens) return qsTr("— tok/s");
        const prefix = processing ? (generationPhase === "thinking" ? qsTr("Thinking · ") : "") : qsTr("Last · ");
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
    Accessible.description: qsTr("Measured tokens: %1 thinking, %2 answer. Generation and decode traces follow their combined measured progress.").arg(reasoningTokenCount).arg(answerTokenCount)
    RocTheme { id: theme; mode: meter.currentTheme }

    function emptyHistory() { return Array(historySize).fill(0); }
    function levelForRate(value) {
        return isFinite(value) && value > 0 ? Math.min(1, value / waveScaleTokensPerSecond) : 0;
    }
    function blendedHistory(previous, current) {
        return current.map(function(value, index) { return previous[index] + (value - previous[index]) * historyBlend; });
    }
    function appendHistory(history, value) {
        // Store actual recent-rate samples. No repeated pulse/crest template.
        const next = history.slice(1);
        next.push(value);
        return next;
    }
    function updateWaveforms() {
        // One shared rolling waveform, updated from each real progress sample.
        const visibleGen = blendedHistory(previousGenerationHistory, generationHistory);
        const visibleDec = blendedHistory(previousDecodeHistory, decodeHistory);
        waveformTransition.stop();
        previousGenerationHistory = visibleGen;
        previousDecodeHistory = visibleDec;
        previousProgressTick = previousProgressTick + (progressTick - previousProgressTick) * historyBlend;
        progressTick += 1;
        generationHistory = appendHistory(generationHistory, generationLevel);
        decodeHistory = appendHistory(decodeHistory, decodeLevel);
        waveSampleCount = Math.min(historySize, waveSampleCount + 1);
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
        waveSampleCount = 0;
        progressTick = 0;
        previousProgressTick = 0;
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
            // Request averages hide changes during a stream. The waveform uses
            // the newest token/timing deltas; numeric labels retain their exact
            // request-average definition. Both views still use measured tokens.
            const delta = tokenCount - seenTokens;
            generationLevel = levelForRate(delta * 1000 / genMs);
            decodeLevel = levelForRate(delta * 1000 / decMs);
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
    Text {
        objectName: "generationTokenSplitLabel"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 16
        visible: meter.height >= 45 && meter.width >= 180 && (meter.reasoningTokenCount + meter.answerTokenCount > 0)
        text: qsTr("Thinking %1 · Answer %2").arg(meter.reasoningTokenCount).arg(meter.answerTokenCount)
        color: theme.muted
        font.family: theme.monoFont
        font.pixelSize: 9
    }
    NumberAnimation {
        id: waveformTransition
        target: meter
        property: "historyBlend"
        from: 0
        to: 1
        duration: 140
        easing.type: Easing.OutCubic
    }
    Canvas {
        id: pixelWave
        objectName: "tokenPixelWaveform"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: Math.max(12, meter.height - (meter.height >= 45 && meter.reasoningTokenCount + meter.answerTokenCount > 0 ? 28 : 14))
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
            const cell = 5, row = 2;
            const columns = Math.max(1, Math.floor(width / cell));
            const baseline = Math.floor((height - 1) / row) * row;
            const intensity = meter.reducedMotion ? 0 : meter.activityLevel;
            ctx.fillStyle = meter.currentTheme === "light" ? "#CBD5F5" : "#172652";
            ctx.fillRect(0, baseline, columns * cell, 1);
            if (intensity <= 0) return;
            const gen = meter.blendedHistory(meter.previousGenerationHistory, meter.generationHistory);
            const dec = meter.blendedHistory(meter.previousDecodeHistory, meter.decodeHistory);
            // Interpolate the measured contour without averaging away its peaks.
            // Short histories spread across the plot; full histories roll left.
            const first = Math.max(0, meter.historySize - meter.waveSampleCount);
            function envelope(history, column) {
                if (meter.waveSampleCount === 0) return 0;
                const position = first + column * (history.length - 1 - first) / Math.max(1, columns - 1);
                const left = Math.floor(position), right = Math.min(history.length - 1, left + 1);
                return history[left] + (history[right] - history[left]) * (position - left);
            }
            const maxRows = Math.max(1, Math.floor(baseline / row));
            const phase = meter.previousProgressTick + (meter.progressTick - meter.previousProgressTick) * meter.historyBlend;
            for (let x = 0; x < columns; ++x) {
                // Unequal traveling ripples supply the pixel-wave style. Their
                // phase advances only on real incoming progress, while measured
                // recent rates determine height. This is activity feedback, not
                // a claim that individual GPU timings form a sinusoidal trace.
                const a = Math.sin(x * 0.13 + phase * 0.33);
                const b = Math.sin(x * 0.29 - phase * 0.21 + 1.4);
                const ripple = 0.16 + 0.48 * a * a + 0.36 * b * b;
                const g = envelope(gen, x) * ripple, d = envelope(dec, x) * ripple;
                const both = meter.tokenSpeedMode === "BOTH";
                const level = both ? (g + d) / 2 : meter.tokenSpeedMode === "DECODE" ? d : g;
                const rows = Math.round(level * intensity * maxRows);
                const genRows = meter.tokenSpeedMode === "DECODE" ? 0 : both
                    ? Math.min(rows, Math.round(g * 0.5 * intensity * maxRows)) : rows;
                // Both rates affect the outer size. Using max(gen, decode)
                // concealed generation changes whenever decode stayed faster.
                for (let y = 0; y < rows; ++y) {
                    const decodeEdge = y >= genRows;
                    if (decodeEdge) ctx.fillStyle = y === rows - 1 ? "#9EAEFF" : "#657FE8";
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
        ? qsTr("Thinking: %1 tokens; answer: %2 tokens. One wave: cobalt = generation; periwinkle = decode. Incoming tokens move the contour; recent measured speeds set its height on a shared 0–100 tok/s scale. Both speeds affect its overall size. Numbers are request averages and include structured model output. Last is the completed request.").arg(reasoningTokenCount).arg(answerTokenCount)
        : qsTr("Live token metrics are available with the native local model.")
}
