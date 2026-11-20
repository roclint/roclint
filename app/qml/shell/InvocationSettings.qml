import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
ColumnLayout {
    id: dashboardWindow
    Layout.minimumWidth: 0
    spacing: 8
    readonly property int currentTab: 1
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
        radius: 14
        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10
            Text {
                text: card.heading
                font.family: dashboardWindow.sansFont
                font.pixelSize: 14
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
        implicitHeight: 32
        implicitWidth: btnText.implicitWidth + 24
        radius: 8
        opacity: active ? 1.0 : 0.4
        color: primary || selected ? theme.accent : (btnMouse.containsMouse && active ? theme.hover : theme.surface)
        border.color: primary || selected ? theme.accent : theme.border
        border.width: activeFocus ? 2 : 1
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label
            font.family: dashboardWindow.sansFont
            font.pixelSize: 12
            font.weight: btn.primary || btn.selected ? Font.DemiBold : Font.Medium
            color: btn.primary || btn.selected ? "#FFFFFF" : theme.foreground
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
                            visible: dashboardWindow.currentTab === 1
                            heading: qsTr("Keyboard shortcut")
                            Body {
                                text: qsTr("Press %1 anywhere to open Roclint.").arg(dashboardWindow.globalShortcutString)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                TextField {
                                    id: invocationShortcutInput
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    color: theme.foreground
                                    placeholderTextColor: theme.muted
                                    font.family: dashboardWindow.monoFont
                                    background: Rectangle {
                                        color: theme.inset
                                        border.color: invocationShortcutInput.activeFocus ? theme.foreground : theme.border
                                        border.width: invocationShortcutInput.activeFocus ? 2 : 1
                                        radius: 4
                                    }
                                    text: dashboardWindow.hc ? hostController.invocationShortcut : "Ctrl+Shift+L"
                                    placeholderText: qsTr("Ctrl+Shift+L")
                                    Accessible.name: qsTr("Global invocation shortcut")
                                    onAccepted: if (dashboardWindow.hc) hostController.setInvocationShortcut(text)
                                }
                                ActionButton {
                                    label: qsTr("Apply")
                                    onClicked: if (dashboardWindow.hc) hostController.setInvocationShortcut(invocationShortcutInput.text)
                                }
                            }
                            Body {
                                text: qsTr("Use a modified letter, digit, F1–F12 or Space. Ctrl denotes Command on macOS. If another app owns it, your previous shortcut is kept.")
                            }
                            Body {
                                visible: text.length > 0
                                color: theme.warning
                                text: dashboardWindow.hc ? hostController.shortcutWarning : ""
                            }
                        }
                        SectionCard {
                            visible: dashboardWindow.currentTab === 1
                            heading: qsTr("Host context hint")
                            CheckBox {
                                id: hostHintToggle
                                indicator: Rectangle {
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    y: (hostHintToggle.height - height) / 2
                                    color: theme.inset
                                    border.color: theme.foreground
                                    border.width: hostHintToggle.activeFocus ? 3 : 1
                                    radius: 2
                                    Text { anchors.centerIn: parent; text: hostHintToggle.checked ? "✓" : ""; color: theme.foreground }
                                }
                                text: qsTr("Include the frontmost app category when I invoke Roclint")
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                contentItem: Text {
                                    text: hostHintToggle.text
                                    leftPadding: hostHintToggle.indicator.width + hostHintToggle.spacing
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.WordWrap
                                    color: theme.foreground
                                    font.family: dashboardWindow.sansFont
                                    font.pixelSize: 12
                                }
                                checked: dashboardWindow.hc && hostController.hostContextEnabled
                                onToggled: if (dashboardWindow.hc) hostController.hostContextEnabled = checked
                            }
                            Body {
                                text: qsTr("Only a coarse category such as browser or code editor is sent to the local model. Window titles, URLs and document paths are not captured. You can also clear the hint in the widget before Analyze.")
                            }
                        }
    SectionCard {
        visible: dashboardWindow.currentTab === 1
        heading: qsTr("Live prompt capture")
        CheckBox {
            id: ambientCaptureToggle
            objectName: "ambientCaptureToggle"
            Layout.fillWidth: true
            text: qsTr("Watch my focused prompt field in Codex and supported IDEs")
            checked: dashboardWindow.hc && hostController.ambientModeEnabled
            onToggled: if (dashboardWindow.hc) hostController.ambientModeEnabled = checked
            indicator: Rectangle {
                implicitWidth: 18; implicitHeight: 18
                y: (ambientCaptureToggle.height - height) / 2
                color: theme.inset
                border.color: theme.foreground
                border.width: ambientCaptureToggle.activeFocus ? 3 : 1
                radius: 2
                Text { anchors.centerIn: parent; text: ambientCaptureToggle.checked ? "✓" : ""; color: theme.foreground }
            }
            contentItem: Text {
                text: ambientCaptureToggle.text
                leftPadding: ambientCaptureToggle.indicator.width + ambientCaptureToggle.spacing
                wrapMode: Text.WordWrap
                color: theme.foreground
                font.family: dashboardWindow.sansFont
                font.pixelSize: 12
            }
        }
        CheckBox {
            objectName: "ambientBadgeSuggestionsOnly"
            Layout.fillWidth: true
            text: qsTr("Show the Roc badge only when a wording suggestion is ready")
            enabled: ambientCaptureToggle.checked
            checked: dashboardWindow.hc && hostController.ambientBadgeSuggestionsOnly
            onToggled: if (dashboardWindow.hc) hostController.ambientBadgeSuggestionsOnly = checked
        }
        Body {
            text: qsTr("Click an underlined phrase for a wording suggestion. Press %1 for a full prompt review.").arg(dashboardWindow.globalShortcutString)
        }
        Body { text: dashboardWindow.hc ? hostController.ambientStatus : "" }
    }
    ActionButton {
        visible: dashboardWindow.hc && hostController.accessibilityStatus === "NOT_GRANTED"
        label: qsTr("Grant accessibility permission")
        onClicked: hostController.requestAccessibilityPermission()
    }
}
