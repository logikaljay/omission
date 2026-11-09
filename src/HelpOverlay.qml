import QtQuick

// Every shortcut, on ?.
Item {
    id: help

    property var overview

    readonly property var shortcuts: [
        ["↵", qsTr("Go to the selected window")],
        ["⇧ ↵", qsTr("Bring the selected window to this workspace")],
        ["← → ↑ ↓", qsTr("Select the nearest window that way")],
        ["Tab", qsTr("Select the next window")],
        ["1 … 9", qsTr("Go to that workspace")],
        [qsTr("Type"), qsTr("Find windows by title or app")],
        ["Del", qsTr("Close the selected window")],
        [qsTr("Drag"), qsTr("Move a window to another workspace, or to a new one")],
        [qsTr("Middle click"), qsTr("Close a window")],
        ["Esc", qsTr("Clear the search, or close the overview")],
        ["?", qsTr("Show or hide this help")]
    ]

    MouseArea {
        anchors.fill: parent
        onClicked: overview.helpVisible = false
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.55
    }

    Rectangle {
        anchors.centerIn: parent
        width: column.implicitWidth + 64
        height: column.implicitHeight + 56
        radius: overview.radius
        color: "#f0141418"
        border.color: Qt.alpha(overview.accent, 0.6)
        border.width: 1

        Column {
            id: column
            anchors.centerIn: parent
            spacing: 12

            Text {
                text: qsTr("Shortcuts")
                color: overview.accent
                font.pixelSize: 18
                font.weight: Font.DemiBold
                bottomPadding: 6
            }

            Repeater {
                model: help.shortcuts
                delegate: Row {
                    required property var modelData
                    spacing: 20
                    Text {
                        width: 130
                        text: modelData[0]
                        color: "#ffffff"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        text: modelData[1]
                        color: "#ffffff"
                        opacity: 0.75
                        font.pixelSize: 14
                    }
                }
            }
        }
    }
}
