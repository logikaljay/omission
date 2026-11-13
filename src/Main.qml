import QtQuick
import QtQuick.Window
import Omission

// The overview, a full-screen window. Its first frame is the focused
// workspace exactly as it is on screen, which then shrinks into its card
// while every other workspace fades in around it; closing plays that
// backwards into whichever workspace you picked.
Window {
    id: win
    color: "#101014"
    visible: false
    title: "omission"

    // 0 is the desktop as it is, 1 the overview.
    property real progress: 0
    property bool armed: false
    property bool closing: false
    property int heroId: 0
    property var spaces: []
    property string query: ""
    property string selected: ""
    property bool helpVisible: false
    property var tiles: ({})
    property var cards: ({})
    property var pendingAction: null
    property bool actionFirst: false

    // Dragging a window to another workspace.
    property string dragAddress: ""
    property string dragStableId: ""
    property point dragPoint: Qt.point(0, 0)
    property size dragSize: Qt.size(0, 0)
    property int dropTarget: 0

    readonly property int newId: backend.newWorkspaceId
    readonly property int cardCount: Math.max(1, spaces.length)
    readonly property real aspect: width > 0 && height > 0 ? width / height : 1.6
    readonly property real margin: Math.max(28, width * 0.035)
    readonly property real header: 88
    readonly property real gap: Math.max(20, width * 0.018)
    readonly property real labelHeight: 36
    readonly property var grid: backend.grid(cardCount, aspect, width - 2 * margin, height - header - margin,
                                             gap, labelHeight, width * 0.46)
    readonly property color accent: theme.accent
    readonly property int radius: theme.radius

    function lerp(a, b, t) { return a + (b - a) * t }

    function homeRect(index) {
        const columns = grid.columns
        const row = Math.floor(index / columns), column = index % columns
        const inRow = Math.min(columns, cardCount - row * columns)
        const rowWidth = inRow * grid.cardWidth + (inRow - 1) * gap
        const totalHeight = grid.rows * (grid.cardHeight + labelHeight) + (grid.rows - 1) * gap
        const x0 = (width - rowWidth) / 2
        const y0 = header + Math.max(0, (height - header - margin - totalHeight) / 2)
        return Qt.rect(x0 + column * (grid.cardWidth + gap), y0 + row * (grid.cardHeight + labelHeight + gap),
                       grid.cardWidth, grid.cardHeight)
    }

    function focusedSpace() {
        for (const s of spaces)
            if (s.focused)
                return s
        return spaces.length ? spaces[0] : null
    }

    function matches(w) {
        if (!query.length)
            return true
        const q = query.toLowerCase()
        return w.title.toLowerCase().indexOf(q) >= 0 || w.appClass.toLowerCase().indexOf(q) >= 0
    }

    // Every window in reading order: workspaces in order, and within one,
    // top to bottom then left to right.
    function orderedWindows() {
        const list = []
        for (const s of spaces) {
            const ws = s.windows.slice().sort((a, b) => (a.ry - b.ry) || (a.rx - b.rx))
            for (const w of ws)
                list.push({ window: w, space: s })
        }
        return list
    }

    function entryFor(address) {
        for (const e of orderedWindows())
            if (e.window.address === address)
                return e
        return null
    }

    function initialSelection() {
        // The window you were in before this one, so ↵ flips back to it
        // like Alt+Tab; otherwise the focused window.
        let best = null
        for (const e of orderedWindows())
            if (!best || e.window.focusHistory === 1 || (best.window.focusHistory !== 1 && e.window.focusHistory < best.window.focusHistory))
                best = e
        return best ? best.window.address : ""
    }

    function selectFirstMatch() {
        const current = entryFor(selected)
        if (current && matches(current.window))
            return
        for (const e of orderedWindows())
            if (matches(e.window)) {
                selected = e.window.address
                return
            }
        selected = ""
    }

    function cycle(step) {
        const list = orderedWindows().filter(e => matches(e.window))
        if (!list.length)
            return
        let i = list.findIndex(e => e.window.address === selected)
        i = i < 0 ? 0 : (i + step + list.length) % list.length
        selected = list[i].window.address
    }

    // Moves the selection to the nearest window in a direction, across
    // workspaces, judged by where the windows are on screen.
    function move(dx, dy) {
        const current = tiles[selected]
        if (!current) {
            cycle(1)
            return
        }
        const from = current.mapToItem(null, current.width / 2, current.height / 2)
        let best = "", bestScore = Infinity
        for (const address in tiles) {
            const tile = tiles[address]
            if (address === selected || !tile || !matches(tile.win))
                continue
            const p = tile.mapToItem(null, tile.width / 2, tile.height / 2)
            const along = (p.x - from.x) * dx + (p.y - from.y) * dy
            if (along <= 1)
                continue
            const across = Math.abs((p.x - from.x) * dy) + Math.abs((p.y - from.y) * dx)
            const score = along + across * 2
            if (score < bestScore) {
                bestScore = score
                best = address
            }
        }
        if (best)
            selected = best
    }

    // Closing: zoom the chosen workspace back to full screen, then hand
    // over. Going to another workspace, Hyprland switches first, which
    // hides this window along with the workspace it's on, so the last frame
    // of the zoom turns into the real thing. Staying on this workspace, the
    // window goes first so the action lands on the desktop beneath.
    function closeTo(spaceId, action) {
        if (closing)
            return
        closing = true
        helpVisible = false
        const here = focusedSpace()
        pendingAction = action
        actionFirst = !here || spaceId !== here.id
        if (!armed) {
            finish()
            return
        }
        heroId = spaceId
        zooming = false
        closeAnimation.from = progress
        closeAnimation.start()
    }

    function finish() {
        const action = pendingAction
        pendingAction = null
        if (action && actionFirst)
            action()
        backend.finishClose()
        if (action && !actionFirst)
            action()
    }

    function activate(address, summon) {
        const e = entryFor(address)
        if (!e)
            return
        if (summon) {
            const here = focusedSpace()
            closeTo(here ? here.id : e.space.id, () => backend.summonWindow(address))
        } else {
            closeTo(e.space.id, () => backend.focusWindow(address))
        }
    }

    function goTo(spaceId) {
        closeTo(spaceId, () => backend.focusWorkspace(spaceId))
    }

    function dismiss() {
        const here = focusedSpace()
        closeTo(here ? here.id : 0, null)
    }

    function closeSelected() {
        if (selected)
            backend.closeWindow(selected)
    }

    function arm() {
        if (armed || closing)
            return
        armed = true
        zoomClock = Date.now()
        zoomTime = 0
        zooming = true
        requestUpdate()
    }

    function cardAt(point) {
        for (const id in cards) {
            const card = cards[id]
            if (!card)
                continue
            const p = card.mapFromItem(null, point.x, point.y)
            const below = id === "new" ? 0 : labelHeight
            if (p.x >= 0 && p.y >= 0 && p.x <= card.width && p.y <= card.height + below)
                return card.spaceId
        }
        return 0
    }

    function drop() {
        const target = dropTarget
        const address = dragAddress
        const source = entryFor(address)
        dragAddress = ""
        dropTarget = 0
        if (target && source && target !== source.space.id)
            backend.moveWindow(address, target)
    }

    Connections {
        target: backend
        function onOpening() {
            win.zooming = false
            closeAnimation.stop()
            win.closing = false
            win.armed = false
            win.helpVisible = false
            win.query = ""
            win.dragAddress = ""
            win.dropTarget = 0
            win.spaces = backend.workspaces
            const here = win.focusedSpace()
            win.heroId = here ? here.id : 0
            win.progress = 0
            win.selected = win.initialSelection()
            keys.forceActiveFocus()
        }
        function onHeroReady() { win.arm() }
        function onCloseRequested() { win.dismiss() }
        function onStateChanged() {
            // Hold still while closing: the switch we just asked for would
            // otherwise reshuffle the cards mid-animation.
            if (!win.closing && backend.shown) {
                win.spaces = backend.workspaces
                if (!win.entryFor(win.selected))
                    win.selectFirstMatch()
            }
        }
    }

    // The zoom follows the clock rather than the frame count. An ordinary
    // animation moves one display refresh further with every frame it draws,
    // and each frame shows where it had got to when the frame before was
    // drawn. The first frames after the window appears come late (the
    // compositor is busy mapping it), so the fastest part of the zoom would
    // play in slow motion, a frame behind. Here every frame, as it's about
    // to be drawn, moves on by the time that really passed, so the first one
    // already shows the zoom under way. A late frame counts for two
    // refreshes at most: catching up all at once would skip most of the zoom.
    property bool zooming: false
    property double zoomClock: 0
    property double zoomTime: 0
    readonly property int zoomDuration: 100
    onAfterAnimating: {
        if (!zooming)
            return
        const now = Date.now()
        zoomTime += Math.min(now - zoomClock, 34)
        zoomClock = now
        const t = Math.min(1, zoomTime / zoomDuration)
        progress = 1 - Math.pow(1 - t, 5) // Easing.OutQuint
        if (t >= 1)
            zooming = false
    }

    NumberAnimation {
        id: closeAnimation
        target: win; property: "progress"
        to: 0; duration: 100
        easing.type: Easing.InOutCubic
        onFinished: win.finish()
    }

    onQueryChanged: selectFirstMatch()

    Item {
        id: keys
        anchors.fill: parent
        focus: true
        opacity: win.armed ? 1 : 0
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Keys.onPressed: event => {
            if (win.closing)
                return
            const ctrl = event.modifiers & Qt.ControlModifier
            const shift = event.modifiers & Qt.ShiftModifier
            event.accepted = true
            switch (event.key) {
            case Qt.Key_Escape:
                if (win.helpVisible) win.helpVisible = false
                else if (win.query.length) win.query = ""
                else win.dismiss()
                return
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (win.selected) win.activate(win.selected, shift)
                return
            case Qt.Key_Left: win.move(-1, 0); return
            case Qt.Key_Right: win.move(1, 0); return
            case Qt.Key_Up: win.move(0, -1); return
            case Qt.Key_Down: win.move(0, 1); return
            case Qt.Key_Tab: win.cycle(1); return
            case Qt.Key_Backtab: win.cycle(-1); return
            case Qt.Key_Delete: win.closeSelected(); return
            case Qt.Key_Backspace:
                if (ctrl) win.query = ""
                else win.query = win.query.slice(0, -1)
                return
            }
            if (ctrl && event.key === Qt.Key_W) {
                win.closeSelected()
                return
            }
            if (ctrl || (event.modifiers & Qt.AltModifier) || (event.modifiers & Qt.MetaModifier)) {
                event.accepted = false
                return
            }
            const text = event.text
            if (!win.query.length && text === "?") {
                win.helpVisible = !win.helpVisible
                return
            }
            if (!win.query.length && text >= "1" && text <= "9" && text.length === 1) {
                win.goTo(parseInt(text))
                return
            }
            if (text.length === 1 && text >= " " && !(win.query.length === 0 && text === " ")) {
                win.query += text
                return
            }
            event.accepted = false
        }

        // The blurred wallpaper: a tiny decode stretched across the screen is
        // a free blur, and costs nothing per frame.
        Item {
            id: backdrop
            anchors.fill: parent
            opacity: win.progress
            Rectangle { anchors.fill: parent; color: "#101014" }
            Image {
                anchors.fill: parent
                source: backend.wallpaper
                sourceSize.width: 40
                fillMode: Image.PreserveAspectCrop
                smooth: true
                cache: true
            }
            Rectangle { anchors.fill: parent; color: "#000000"; opacity: 0.5 }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: win.dismiss()
        }

        Repeater {
            model: win.spaces
            delegate: WorkspaceCard {
                required property var modelData
                required property int index
                space: modelData
                home: win.homeRect(index)
                overview: win
            }
        }

        NewWorkspaceCard {
            overview: win
        }

        SearchBar {
            overview: win
            anchors.horizontalCenter: parent.horizontalCenter
            y: 26 - (1 - win.progress) * 20
            opacity: win.progress
        }

        // The window being dragged follows the pointer above everything.
        Item {
            visible: win.dragAddress !== ""
            x: win.dragPoint.x - width / 2
            y: win.dragPoint.y - height / 2
            width: win.dragSize.width
            height: win.dragSize.height
            z: 100
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                color: "transparent"
                border.color: win.accent
                border.width: 2
                radius: win.radius
            }
            WindowThumb { anchors.fill: parent; windowId: win.dragStableId; opacity: 0.92 }
        }

        HelpOverlay {
            anchors.fill: parent
            overview: win
            visible: win.helpVisible
            z: 200
        }

        Rectangle {
            visible: backend.error !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
            width: errorText.implicitWidth + 32
            height: errorText.implicitHeight + 16
            radius: Math.min(4, win.radius)
            color: "#cc3a1d1d"
            z: 150
            Text {
                id: errorText
                anchors.centerIn: parent
                text: backend.error
                color: "white"
                font.pixelSize: 14
            }
        }
    }
}
