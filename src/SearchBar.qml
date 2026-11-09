import QtQuick

// What you've typed, or how to type something.
Rectangle {
    id: bar

    property var overview

    readonly property bool searching: overview.query.length > 0
    readonly property int matchCount: {
        let n = 0
        for (const s of overview.spaces)
            for (const w of s.windows)
                if (overview.matches(w))
                    ++n
        return n
    }

    width: Math.max(searching ? 360 : 0, content.implicitWidth + 36)
    height: 40
    radius: Math.min(20, Math.max(4, overview.radius * 2))
    color: searching ? "#cc0e0e12" : "transparent"
    border.width: searching ? 1 : 0
    border.color: Qt.alpha(overview.accent, 0.7)

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 12

        Text {
            visible: bar.searching
            text: overview.query
            color: "#ffffff"
            font.pixelSize: 17
        }
        Rectangle {
            visible: bar.searching
            width: 2
            height: 20
            anchors.verticalCenter: parent.verticalCenter
            color: overview.accent
            SequentialAnimation on opacity {
                running: bar.searching
                loops: Animation.Infinite
                NumberAnimation { to: 0; duration: 500 }
                NumberAnimation { to: 1; duration: 500 }
            }
        }
        Text {
            visible: bar.searching
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%n matches", "", bar.matchCount)
            color: bar.matchCount ? "#ffffff" : "#ff8a80"
            opacity: 0.6
            font.pixelSize: 13
        }
        Text {
            visible: !bar.searching
            text: qsTr("Type to find a window  ·  ↵ go  ·  ⇧↵ bring here  ·  drag to move  ·  ? help")
            color: "#ffffff"
            opacity: 0.5
            font.pixelSize: 13
        }
    }
}
