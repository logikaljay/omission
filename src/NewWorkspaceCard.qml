import QtQuick

// The next free workspace, top right: click to go there, or drop a window
// on it. While a window is being dragged it grows into a proper target.
Rectangle {
    id: target

    property var overview

    readonly property int spaceId: overview.newId
    readonly property bool dragging: overview.dragAddress !== ""
    readonly property bool dropping: dragging && overview.dropTarget === spaceId

    width: dragging ? Math.max(220, overview.width * 0.16) : label.implicitWidth + 36
    height: dragging ? width / overview.aspect : 40
    x: overview.width - overview.margin - width
    y: 26 - (1 - overview.progress) * 20
    opacity: overview.progress
    radius: dragging ? Math.min(10, overview.radius) : Math.min(20, Math.max(4, overview.radius * 2))
    color: dropping ? Qt.alpha(overview.accent, 0.22) : hover.hovered ? "#33ffffff" : "#1affffff"
    border.width: dropping ? 3 : 1
    border.color: dropping ? overview.accent : "#40ffffff"
    Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    Component.onCompleted: overview.cards["new"] = target

    HoverHandler { id: hover }

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.min(implicitWidth, target.width - 16)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: target.dragging ? qsTr("Move to workspace %1").arg(target.spaceId) : qsTr("+  New workspace")
        color: target.dropping ? overview.accent : "#ffffff"
        opacity: target.dropping ? 1 : 0.75
        font.pixelSize: 13
    }

    MouseArea {
        anchors.fill: parent
        onClicked: overview.goTo(target.spaceId)
    }
}
