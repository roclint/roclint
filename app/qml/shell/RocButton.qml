import QtQuick
import QtQuick.Controls

Button {
    id: control
    property string label: ""
    property string iconName: ""
    property string tip: label
    property string currentTheme: "dark"
    property bool primary: false
    property bool quiet: false
    property bool active: true
    property bool compact: false
    property bool reduceMotion: false
    enabled: active
    text: label.replace(/&/g, "&&")
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    implicitHeight: compact ? 30 : 40
    implicitWidth: label.length ? Math.max(72, contentItem.implicitWidth + 28) : implicitHeight
    leftPadding: label.length ? 14 : 7
    rightPadding: leftPadding
    topPadding: 7
    bottomPadding: 7
    font.family: theme.sansFont
    font.pixelSize: compact ? 12 : 14
    font.weight: primary ? Font.DemiBold : Font.Medium
    icon.source: iconName.length ? "assets/icons/" + iconName + ".svg" : ""
    icon.color: primary ? "#FFFFFF" : theme.foreground
    icon.width: compact ? 16 : 18
    icon.height: compact ? 16 : 18
    display: label.length ? AbstractButton.TextBesideIcon : AbstractButton.IconOnly
    opacity: enabled ? 1 : 0.42
    Accessible.name: label.length ? label : tip
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 650
    ToolTip.text: tip
    RocTheme { id: theme; mode: control.currentTheme }
    palette.buttonText: primary ? "#FFFFFF" : theme.foreground
    background: Rectangle {
        radius: control.compact ? 7 : 10
        color: control.primary ? (control.down ? Qt.darker(theme.accent, 1.18) : control.hovered ? Qt.lighter(theme.accent, 1.12) : theme.accent)
                               : control.down || control.hovered ? theme.hover : control.quiet ? "transparent" : theme.surface
        border.color: control.activeFocus ? theme.focus : control.primary ? theme.accent : control.quiet ? "transparent" : theme.border
        border.width: control.activeFocus ? 2 : 1
        Behavior on color { ColorAnimation { duration: control.reduceMotion ? 0 : 110 } }
    }
}
