import QtQuick

// Preserve the upstream tactile controls while adding keyboard and accessibility activation.
MouseArea {
    id: control
    property color focusColor: "#F59E0B"
    property real focusRadius: (parent && typeof parent.radius === "number") ? parent.radius : 0
    property bool showFocusRing: true
    property bool showFocusOnTabOnly: true
    property bool focusVisible: false
    property string controlName: labelFor(parent, 0) || qsTr("Activate control")
    activeFocusOnTab: enabled && visible
    Accessible.role: Accessible.Button
    Accessible.name: controlName
    Accessible.onPressAction: control.clicked(null)
    onPressed: {
        focusVisible = false;
        forceActiveFocus();
    }
    onActiveFocusChanged: {
        if (!activeFocus) {
            focusVisible = false;
        } else if (!pressed) {
            focusVisible = true;
        }
    }
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            control.clicked(null);
            event.accepted = true;
        }
    }
    function labelFor(item, depth) {
        if (!item || depth > 3) return "";
        if (typeof item.text === "string" && item.text.length) return item.text;
        for (let i = 0; i < item.children.length; ++i) {
            if (item.children[i] === control) continue;
            const label = labelFor(item.children[i], depth + 1);
            if (label.length) return label;
        }
        return "";
    }
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: control.focusColor
        border.width: 2
        radius: control.focusRadius
        visible: control.showFocusRing && control.activeFocus && (!control.showFocusOnTabOnly || control.focusVisible)
    }
}
