import QtQuick
import QtQuick.Controls

ScrollBar {
    id: bar
    padding: 2
    background: Rectangle { color: "#09090B"; opacity: 0.8 }
    contentItem: Rectangle {
        implicitWidth: 8
        implicitHeight: 8
        radius: 3
        color: bar.pressed ? "#C9D1D9" : "#6E7681"
        opacity: bar.size < 1 ? 1 : 0
    }
}
