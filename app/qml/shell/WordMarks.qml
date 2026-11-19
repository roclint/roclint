import QtQuick
import QtQml.Models

// Real text geometry only. Separate thin windows leave editor text and controls
// clickable; unsupported providers retain the field badge without fake marks.
QtObject {
    id: marks
    readonly property var host: typeof hostController !== "undefined" ? hostController : null
    readonly property bool showing: host && typeof host.wordMarkVisible !== "undefined" && host.wordMarkVisible
    property Instantiator segments: Instantiator {
        model: marks.showing ? marks.host.wordMarkRects : []
        delegate: Window {
            id: segment
            required property var modelData
            required property int index
            title: "Roclint phrase mark"
            objectName: "wordMark_" + index
            flags: (Qt.platform.os === "windows" ? Qt.Tool : Qt.ToolTip)
                   | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
            // Windows can pass clicks through zero-alpha pixels in a layered
            // window. Keep the entire narrow underline strip hit-testable.
            color: Qt.platform.os === "windows" ? Qt.rgba(0, 0, 0, 0.02) : "transparent"
            x: modelData.x
            y: modelData.y + modelData.height - 1
            width: Math.max(1, modelData.width)
            height: 7
            visible: marks.showing
            Canvas {
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.clearRect(0, 0, width, height);
                    ctx.strokeStyle = "#91ABFF";
                    ctx.lineWidth = 1.5;
                    ctx.beginPath();
                    for (let n = 0; n <= width; ++n) {
                        const y = 2 + Math.sin(n * Math.PI / 4) * 0.8;
                        if (n === 0) ctx.moveTo(n, y); else ctx.lineTo(n, y);
                    }
                    ctx.stroke();
                }
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Roclint suggestion for %1").arg(marks.host.wordRefinementOriginal)
                Accessible.onPressAction: marks.host.activateAmbientIndicator()
                // Open on press before a focus or caret update can move the mark.
                onPressed: marks.host.activateAmbientIndicator()
            }
        }
    }
}
