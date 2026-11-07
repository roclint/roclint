import QtQuick
import QtQuick.Controls

// A token-arrival history, not an ornamental/equalizer animation. No timer
// fabricates activity while idle, pre-filling, stalled, or in reduced motion.
Item {
    id: meter
    objectName: "generationMeter"
    implicitWidth: 185
    implicitHeight: 32
    property double tokensPerSecond: 0
    property double tokenCount: 0
    property bool processing: false
    property bool metricsAvailable: true
    property bool reducedMotion: false
    property string currentTheme: "dark"
    property var history: Array(32).fill(0)
    property double sampledTokens: 0
    property double sampledAt: 0
    readonly property string rateText: tokensPerSecond > 0
        ? qsTr("%1 tok/s").arg(tokensPerSecond.toFixed(1))
        : processing ? (metricsAvailable ? qsTr("Preparing…") : qsTr("Working…")) : qsTr("— tok/s")
    readonly property double peak: Math.max(10, Math.max.apply(null, history))
    Accessible.role: Accessible.StaticText
    Accessible.focusable: false
    Accessible.focused: false
    Accessible.name: rateText
    Accessible.description: qsTr("Live local generation speed and token activity")
    RocTheme { id: theme; mode: meter.currentTheme }

    function resetHistory() {
        history = Array(32).fill(0);
        sampledTokens = tokenCount;
        sampledAt = Date.now();
    }
    function sample(now) {
        if (!processing || reducedMotion) { resetHistory(); return; }
        const elapsed = now - sampledAt;
        if (sampledAt <= 0 || elapsed <= 0 || tokenCount < sampledTokens) { resetHistory(); return; }
        const rate = Math.max(0, tokenCount - sampledTokens) * 1000 / elapsed;
        history = history.slice(1).concat([rate]);
        sampledTokens = tokenCount;
        sampledAt = now;
    }
    onProcessingChanged: resetHistory()
    onReducedMotionChanged: resetHistory()
    onTokenCountChanged: if (tokenCount < sampledTokens) resetHistory()
    Component.onCompleted: resetHistory()
    Timer {
        interval: 200
        running: meter.processing && !meter.reducedMotion && meter.visible
        repeat: true
        onTriggered: meter.sample(Date.now())
    }
    Text {
        objectName: "generationRateLabel"
        anchors.right: parent.right
        anchors.top: parent.top
        width: Math.min(implicitWidth, meter.width)
        elide: Text.ElideRight
        visible: meter.width >= 65
        text: meter.rateText
        color: meter.processing ? theme.foreground : theme.muted
        font.family: theme.monoFont
        font.pixelSize: meter.width < 90 ? 10 : 12
    }
    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 17
        spacing: 1
        visible: meter.width >= 28
        Repeater {
            model: 32
            Rectangle {
                required property int index
                width: Math.max(0, (meter.width - 31) / 32)
                height: Math.max(2, Math.round(17 * meter.history[index] / meter.peak))
                y: 17 - height
                color: theme.accent
                opacity: meter.history[index] > 0 ? 0.95 : 0.25
            }
        }
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 500
    ToolTip.text: meter.metricsAvailable
        ? (meter.processing ? qsTr("%1 tokens · live decode speed. Bars follow token arrivals; flat means no new tokens.")
                            : qsTr("%1 tokens · last decode speed. Bars are idle until the next response.")).arg(meter.tokenCount)
        : qsTr("Live token metrics are available with the native local model.")
}
