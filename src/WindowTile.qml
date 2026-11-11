import QtQuick
import Omission

// One window, live. Hover or arrow keys select it, a click goes to it,
// a drag takes it to another workspace, a middle click closes it.
Item {
    id: tile

    property var win
    property Item area
    property var overview

    readonly property string address: win.address
    readonly property bool selected: overview.selected === address
    readonly property bool match: overview.matches(win)
    readonly property real t: overview.progress
    property bool dragged: false
    property point pressPoint

    x: win.rx * area.width
    y: win.ry * area.height
    width: win.rw * area.width
    height: win.rh * area.height
    z: selected ? 10 : 0
    opacity: overview.dragAddress === address ? 0.25 : match ? 1 : 0.18
    scale: selected && t === 1 ? 1.025 : 1
    Behavior on opacity { NumberAnimation { duration: 120 } }
    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    Component.onCompleted: overview.tiles[address] = tile
    Component.onDestruction: if (overview.tiles[address] === tile) delete overview.tiles[address]

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 2
        anchors.bottomMargin: -5
        color: "#000000"
        opacity: 0.4 * tile.t
        radius: Math.min(8, overview.radius)
    }

    // Until the first picture of a window arrives: its name on a plain tile.
    Rectangle {
        anchors.fill: parent
        visible: !thumb.ready
        color: "#26262c"
        Text {
            anchors.centerIn: parent
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.win.appClass
            color: "#ffffff"
            opacity: 0.7
            font.pixelSize: Math.max(10, Math.min(16, tile.height * 0.12))
        }
    }

    WindowThumb {
        id: thumb
        anchors.fill: parent
        windowId: tile.win.stableId
        opacity: ready ? 1 : 0
        Behavior on opacity { enabled: tile.t > 0.5; NumberAnimation { duration: 140 } }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        visible: tile.selected
        color: "transparent"
        radius: Math.min(8, overview.radius)
        border.width: 3
        border.color: overview.accent
        opacity: tile.t
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        cursorShape: Qt.PointingHandCursor
        preventStealing: true

        onEntered: if (overview.dragAddress === "" && !overview.closing) overview.selected = tile.address
        onPressed: mouse => {
            tile.dragged = false
            tile.pressPoint = Qt.point(mouse.x, mouse.y)
        }
        onPositionChanged: mouse => {
            if (!(mouse.buttons & Qt.LeftButton))
                return
            const p = mapToItem(null, mouse.x, mouse.y)
            if (!tile.dragged) {
                if (Math.abs(mouse.x - tile.pressPoint.x) + Math.abs(mouse.y - tile.pressPoint.y) < 8)
                    return
                tile.dragged = true
                const w = Math.min(tile.width, 260)
                overview.dragSize = Qt.size(w, w * tile.height / Math.max(1, tile.width))
                overview.dragStableId = tile.win.stableId
                overview.dragAddress = tile.address
            }
            overview.dragPoint = p
            overview.dropTarget = overview.cardAt(p)
        }
        onReleased: mouse => {
            if (tile.dragged)
                overview.drop()
        }
        onClicked: mouse => {
            if (tile.dragged)
                return
            if (mouse.button === Qt.MiddleButton)
                backend.closeWindow(tile.address)
            else
                overview.activate(tile.address, mouse.modifiers & Qt.ShiftModifier)
        }
    }

    // Close, top-left like the traffic lights it borrows from.
    Rectangle {
        // The button's own hover counts too: it takes the hover from the tile
        // underneath, and without it the button would hide, hand the hover
        // back, show again, and flicker.
        visible: (mouse.containsMouse || closeArea.containsMouse) && tile.t === 1 && overview.dragAddress === "" && tile.width > 60
        x: -width / 3
        y: -height / 3
        width: 22
        height: 22
        radius: 11
        color: closeArea.containsMouse ? "#e5484d" : "#2a2a30"
        border.color: "#ffffff"
        border.width: 1
        Text {
            anchors.centerIn: parent
            text: "×"
            color: "#ffffff"
            font.pixelSize: 15
        }
        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: backend.closeWindow(tile.address)
        }
    }

    // The title, under the selected window.
    Rectangle {
        visible: tile.selected && tile.t > 0.6 && overview.dragAddress === ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        width: Math.min(titleText.implicitWidth + 20, Math.max(80, tile.width - 16))
        height: titleText.implicitHeight + 10
        radius: Math.min(4, overview.radius)
        color: "#e6141418"
        opacity: tile.t
        Text {
            id: titleText
            anchors.centerIn: parent
            width: parent.width - 20
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.win.title || tile.win.appClass
            color: "#ffffff"
            font.pixelSize: 13
        }
    }
}
