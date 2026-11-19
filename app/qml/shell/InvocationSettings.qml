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
    readonly property color bgColor: currentTheme === "light" ? "#F4F4F4" : currentTheme === "acid" ? "#070D08" : currentTheme === "amber" ? "#0F0902" : "#050505"
    readonly property color fgColor: currentTheme === "light" ? "#050505" : currentTheme === "acid" ? "#4ADE80" : currentTheme === "amber" ? "#FBBF24" : "#C9D1D9"
    readonly property color borderColor: currentTheme === "light" ? "#050505" : currentTheme === "acid" ? "#22C55E" : currentTheme === "amber" ? "#F59E0B" : "#6E7681"
    readonly property color dimColor: currentTheme === "light" ? "#D1D5DB" : currentTheme === "acid" ? "#14331B" : currentTheme === "amber" ? "#3B2207" : "#262626"
    readonly property color mutedTextColor: currentTheme === "light" ? "#4B5563" : currentTheme === "acid" ? "#4ADE80" : currentTheme === "amber" ? "#D97706" : "#8B949E"
    readonly property color inputBgColor: currentTheme === "light" ? "#E5E7EB" : currentTheme === "acid" ? "#0A140B" : currentTheme === "amber" ? "#140D04" : "#09090B"
    readonly property color okColor: currentTheme === "light" ? "#166534" : "#7EE787"
    readonly property color warnColor: "#F59E0B"
    readonly property color errorColor: "#FF7B72"

    component SectionCard: Rectangle {
        id: card
        default property alias content: cardColumn.data
        property string heading: ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: cardColumn.implicitHeight + 28
        color: dashboardWindow.inputBgColor
        border.color: dashboardWindow.borderColor
        border.width: 1
        radius: 6
        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8
            Text {
                text: card.heading
                font.family: dashboardWindow.sansFont
                font.pixelSize: 13
                font.bold: true
                color: dashboardWindow.fgColor
            }
        }
    }

    component Body: Text {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.family: dashboardWindow.sansFont
        font.pixelSize: 12
        color: dashboardWindow.mutedTextColor
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
        implicitHeight: 28
        implicitWidth: btnText.implicitWidth + 22
        radius: 4
        opacity: active ? 1.0 : 0.4
        color: primary || selected ? dashboardWindow.fgColor : (btnMouse.containsMouse && active ? dashboardWindow.dimColor : "transparent")
        border.color: dashboardWindow.fgColor
        border.width: activeFocus ? 3 : 1
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label
            font.family: dashboardWindow.sansFont
            font.pixelSize: 12
            font.bold: btn.primary || btn.selected
            color: btn.primary || btn.selected ? dashboardWindow.bgColor : dashboardWindow.fgColor
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
                                    color: dashboardWindow.fgColor
                                    placeholderTextColor: dashboardWindow.mutedTextColor
                                    font.family: dashboardWindow.monoFont
                                    background: Rectangle {
                                        color: dashboardWindow.inputBgColor
                                        border.color: invocationShortcutInput.activeFocus ? dashboardWindow.fgColor : dashboardWindow.borderColor
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
                                color: dashboardWindow.warnColor
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
                                    color: dashboardWindow.inputBgColor
                                    border.color: dashboardWindow.fgColor
                                    border.width: hostHintToggle.activeFocus ? 3 : 1
                                    radius: 2
                                    Text { anchors.centerIn: parent; text: hostHintToggle.checked ? "✓" : ""; color: dashboardWindow.fgColor }
                                }
                                text: qsTr("Include the frontmost app category when I invoke Roclint")
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                contentItem: Text {
                                    text: hostHintToggle.text
                                    leftPadding: hostHintToggle.indicator.width + hostHintToggle.spacing
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.WordWrap
                                    color: dashboardWindow.fgColor
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
                color: dashboardWindow.inputBgColor
                border.color: dashboardWindow.fgColor
                border.width: ambientCaptureToggle.activeFocus ? 3 : 1
                radius: 2
                Text { anchors.centerIn: parent; text: ambientCaptureToggle.checked ? "✓" : ""; color: dashboardWindow.fgColor }
            }
            contentItem: Text {
                text: ambientCaptureToggle.text
                leftPadding: ambientCaptureToggle.indicator.width + ambientCaptureToggle.spacing
                wrapMode: Text.WordWrap
                color: dashboardWindow.fgColor
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
