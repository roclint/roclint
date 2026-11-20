import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtCore

// The only automatic surface: a non-focusing entry point beside the field.
// Checking and recommendations stay behind a deliberate click.
Window {
    id: badge
    title: qsTr("Roclint indicator")
    readonly property bool hc: typeof hostController !== "undefined" && hostController !== null
    readonly property bool hasSuggestion: hc && !!hostController.wordReplacementReady
    readonly property string hint: hasSuggestion ? qsTr("Wording suggestion ready · Open prompt review") : qsTr("Review prompt")
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    readonly property int badgeSize: 28
    width: badgeSize
    height: badgeSize
    minimumWidth: badgeSize
    maximumWidth: badgeSize
    minimumHeight: badgeSize
    maximumHeight: badgeSize
    x: hc ? hostController.ambientBadgeX : 0
    y: hc ? hostController.ambientBadgeY : 0
    visible: false
    Settings { id: appearance; category: "appearance"; property bool reducedMotion: false }
    Rectangle {
        anchors.fill: parent
        radius: 9
        color: indicatorMouse.containsMouse ? "#1B315E" : "#09132E"
        border.color: badge.hasSuggestion ? "#A9BAFF" : "#405582"
        Behavior on color { ColorAnimation { duration: appearance.reducedMotion ? 0 : 110 } }
        Image {
            anchors.centerIn: parent
            source: "assets/roc-mark.png"
            width: 19; height: 18
            fillMode: Image.PreserveAspectFit
            smooth: false
            Accessible.ignored: true
        }
        Rectangle {
            objectName: "suggestionDot"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 1
            width: 8; height: 8; radius: 4
            visible: badge.hasSuggestion
            color: "#8BE5CA"
            border.color: "#09132E"
            border.width: 1
            Accessible.ignored: true
        }
        MouseArea {
            id: indicatorMouse
            objectName: "ambientIndicatorControl"
            Accessible.role: Accessible.Button
            Accessible.name: badge.hint
            Accessible.description: qsTr("Right-click to dismiss")
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            Accessible.onPressAction: if (badge.hc) hostController.showCard()
            onPressed: function(mouse) {
                if (badge.hc && mouse.button === Qt.LeftButton) hostController.showCard();
            }
            onClicked: function(mouse) {
                if (badge.hc && mouse.button === Qt.RightButton) hostController.dismissAmbientBadge();
            }
        }
    }

}
