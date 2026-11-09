import QtQuick

// One workspace: its monitor's wallpaper with its windows where they really
// are. The focused workspace is the one that zooms to and from full screen.
Item {
    id: card

    property var space
    property rect home
    property var overview

    readonly property int spaceId: space.id
    readonly property bool zooms: space.onFocusedMonitor && space.id === overview.heroId
    readonly property real t: overview.progress
    readonly property bool dropping: overview.dragAddress !== "" && overview.dropTarget === space.id

    x: zooms ? overview.lerp(0, home.x, t) : home.x
    y: zooms ? overview.lerp(0, home.y, t) : home.y
    width: zooms ? overview.lerp(overview.width, home.width, t) : home.width
    height: zooms ? overview.lerp(overview.height, home.height, t) : home.height
    z: zooms ? 2 : 1
    opacity: zooms ? 1 : t
    scale: (zooms ? 1 : 0.94 + 0.06 * t) * (dropping ? 1.03 : 1)
    Behavior on scale { enabled: card.t === 1; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    Component.onCompleted: overview.cards[space.id] = card
    Component.onDestruction: if (overview.cards[space.id] === card) delete overview.cards[space.id]

    HoverHandler { id: hover }

    // The monitor's shape, letterboxed when the workspace lives on a
    // monitor with another aspect ratio.
    Item {
        id: screen
        readonly property real aspect: card.space.aspect > 0 ? card.space.aspect : 1.6
        width: Math.min(card.width, card.height * aspect)
        height: width / aspect
        anchors.centerIn: parent

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 3 * card.t
            anchors.bottomMargin: -6 * card.t
            color: "#000000"
            opacity: 0.35 * card.t
            radius: Math.min(10, overview.radius)
        }

        Image {
            anchors.fill: parent
            source: backend.wallpaper
            sourceSize.width: 1440
            fillMode: Image.PreserveAspectCrop
            asynchronous: false
            cache: true
        }

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.18 * card.t
        }

        MouseArea {
            anchors.fill: parent
            onClicked: overview.goTo(card.space.id)
        }

        Repeater {
            model: card.space.windows
            delegate: WindowTile {
                required property var modelData
                win: modelData
                area: screen
                overview: card.overview
            }
        }

        Text {
            visible: card.space.windows.length === 0
            anchors.centerIn: parent
            text: qsTr("Empty")
            color: "#ffffff"
            opacity: 0.45 * card.t
            font.pixelSize: Math.max(11, Math.min(18, screen.height * 0.07))
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: card.dropping ? -3 : -2
            color: "transparent"
            radius: Math.min(10, overview.radius)
            border.width: card.dropping ? 3 : 2
            border.color: card.dropping ? overview.accent : "#ffffff"
            opacity: card.t * (card.dropping ? 1 : card.space.focused ? 0.55 : hover.hovered ? 0.3 : 0)
            Behavior on opacity { enabled: card.t === 1; NumberAnimation { duration: 120 } }
        }
    }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: screen.y + screen.height + 10
        spacing: 8
        opacity: card.t
        Text {
            text: card.space.label
            color: card.space.focused ? overview.accent : "#ffffff"
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
        Text {
            anchors.baseline: parent.children[0].baseline
            text: qsTr("%n windows", "", card.space.windows.length)
            color: "#ffffff"
            opacity: 0.55
            font.pixelSize: 12
        }
    }
}
