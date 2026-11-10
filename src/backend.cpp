#include "backend.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QQuickWindow>
#include <QScreen>
#include <QUrl>
#include <QtGui/qguiapplication_platform.h>

#include <wayland-client.h>

#include "capturer.h"
#include "hyprland.h"
#include "theme.h"
#include "thumbnails.h"

// Waits until the compositor has handled every request sent so far. Qt reads
// and dispatches its own events on its queues; this uses a private one.
static void syncWithCompositor() {
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_display *display = app ? app->display() : nullptr;
    if (!display)
        return;
    wl_event_queue *queue = wl_display_create_queue(display);
    wl_display_roundtrip_queue(display, queue);
    wl_event_queue_destroy(queue);
}

Backend::Backend(Hyprland *hyprland, Capturer *capturer, Theme *theme, QObject *parent)
    : QObject(parent), m_hyprland(hyprland), m_capturer(capturer), m_theme(theme) {
    if (!m_hyprland->available())
        setError(tr("Hyprland isn't running, so there are no workspaces to show."));

    connect(m_capturer, &Capturer::frame, this, [this](const QString &id, const QImage &image) {
        Thumbnails::instance()->set(id, image);
        onFrame(id);
    });
    connect(m_capturer, &Capturer::dmabufFrame, this, [this](const QString &id, const std::shared_ptr<CaptureBuffer> &buffer) {
        Thumbnails::instance()->setDmabuf(id, buffer);
        onFrame(id);
    });
    // A driver that can't import the compositor's buffers: picture that
    // window again, and everything after it, through shared memory.
    connect(Thumbnails::instance(), &Thumbnails::importFailed, this, [this](const QString &id) {
        QMetaObject::invokeMethod(m_capturer, "disableDmabuf", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_capturer, "capture", Qt::QueuedConnection, Q_ARG(QStringList, QStringList{id}), Q_ARG(bool, false));
    });
    connect(m_capturer, &Capturer::failed, this, &Backend::setError);
    connect(m_hyprland, &Hyprland::hyprEvent, this, &Backend::onEvent);

    m_refreshDebounce.setSingleShot(true);
    m_refreshDebounce.setInterval(30);
    connect(&m_refreshDebounce, &QTimer::timeout, this, [this] {
        if (!m_shown)
            return;
        refresh();
        // Windows that appeared while open start their live pictures too.
        QStringList fresh;
        for (const auto &w : m_state.allWindows())
            if (!m_captured.contains(w.stableId))
                fresh += w.stableId;
        m_captured.unite(QSet<QString>(fresh.begin(), fresh.end()));
        if (!fresh.isEmpty())
            QMetaObject::invokeMethod(m_capturer, "capture", Qt::QueuedConnection, Q_ARG(QStringList, fresh), Q_ARG(bool, false));
    });

    // Pictures of workspaces you left are taken once switching settles, so
    // the compositor never copies windows in the middle of a quick run of
    // switches.
    m_prewarmDebounce.setSingleShot(true);
    m_prewarmDebounce.setInterval(400);
    connect(&m_prewarmDebounce, &QTimer::timeout, this, [this] {
        if (m_shown)
            return;
        QList<int> left;
        for (int id : std::as_const(m_leftWorkspaces))
            if (!m_activeByMonitor.values().contains(id))
                left += id;
        m_leftWorkspaces.clear();
        prewarm(left);
    });

    m_trickleTimer.setSingleShot(true);
    connect(&m_trickleTimer, &QTimer::timeout, this, &Backend::trickle);


    m_presentTimer.setSingleShot(true);
    m_presentTimer.setInterval(90);
    connect(&m_presentTimer, &QTimer::timeout, this, &Backend::present);

    applyWindowRule();
    reloadWallpaper();
    refresh();

    // Picture every window once now, so even the first opening shows real
    // contents rather than placeholders.
    QStringList all;
    QSet<int> onScreen;
    for (const auto &m : m_state.monitors)
        onScreen.insert(m.activeWorkspace);
    for (const auto &w : m_state.workspaces)
        for (const auto &win : w.windows) {
            all += win.stableId;
            if (!onScreen.contains(w.id))
                m_exact.insert(win.stableId);
        }
    QMetaObject::invokeMethod(m_capturer, "capture", Qt::QueuedConnection, Q_ARG(QStringList, all), Q_ARG(bool, false));
}

void Backend::setWindow(QQuickWindow *window) {
    m_window = window;
    // OMISSION_DEBUG=1 prints the gaps between presented frames during each
    // opening, which is what smoothness comes down to.
    if (qEnvironmentVariableIsSet("OMISSION_DEBUG")) {
        connect(window, &QQuickWindow::frameSwapped, this, [this] {
            if (m_shown && m_openClock.isValid() && m_openClock.elapsed() < 600)
                m_frameTimes += m_openClock.elapsed();
        }, Qt::DirectConnection);
    }
}

// The overview opens on the focused monitor.
void Backend::placeOnFocusedMonitor() {
    const Model::Monitor *focused = m_state.focusedMonitor();
    if (!focused)
        return;
    for (QScreen *screen : QGuiApplication::screens())
        if (screen->name() == focused->name) {
            m_window->setScreen(screen);
            return;
        }
}

// The overview is a full-screen window rather than a layer surface: Hyprland
// stops drawing what's under a full-screen window, while under a layer it
// redraws every window each frame, which on some machines halves the frame
// rate. The rule keeps it floating (so it never disturbs tiling), without
// borders, titlebar or Hyprland's open animation, which would smear the zoom.
// It's added at runtime, and again after every config reload, instead of
// editing the user's config.
void Backend::applyWindowRule() {
    m_hyprland->eval(R"(o.window("^omission$", { float = true, fullscreen = true, no_anim = true, border_size = 0, rounding = 0, no_shadow = true, no_blur = true, decorate = false }))");
}

void Backend::reloadWallpaper() {
    const QString link = qEnvironmentVariableIsSet("OMISSION_WALLPAPER")
        ? qEnvironmentVariable("OMISSION_WALLPAPER")
        : QDir::homePath() + "/.local/state/omarchy/current/background";
    const QString target = QFileInfo(link).canonicalFilePath();
    const QString url = target.isEmpty() ? QString() : QUrl::fromLocalFile(target).toString();
    if (url != m_wallpaper) {
        m_wallpaper = url;
        emit wallpaperChanged();
    }
}

void Backend::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Backend::refresh() {
    if (!m_hyprland->available()) {
        setState(m_state);
        return;
    }
    const QJsonArray monitors = m_hyprland->json("monitors").array();
    const QJsonArray workspaces = m_hyprland->json("workspaces").array();
    const QJsonArray clients = m_hyprland->json("clients").array();
    setState(Model::parse(monitors, workspaces, clients));
}

void Backend::setState(const Model::State &state) {
    m_state = state;
    m_activeByMonitor.clear();
    for (const auto &m : m_state.monitors)
        m_activeByMonitor.insert(m.id, m.activeWorkspace);

    QSet<QString> ids;
    for (const auto &w : m_state.allWindows())
        ids.insert(w.stableId);
    Thumbnails::instance()->keepOnly(ids);

    m_workspaces = Model::toVariant(m_state);
    emit stateChanged();
}

QVariantMap Backend::grid(int count, double aspect, double width, double height, double gap, double label, double maxCardWidth) const {
    const auto g = Model::fitGrid(count, aspect, width, height, gap, label, maxCardWidth);
    return {{"columns", g.columns}, {"rows", g.rows}, {"cardWidth", g.cardWidth}, {"cardHeight", g.cardHeight}};
}

void Backend::toggle() {
    if (m_shown)
        close();
    else
        show();
}

void Backend::show() {
    if (m_shown || !m_window)
        return;
    m_openClock.start();
    refresh();
    reloadWallpaper();
    m_theme->refreshRounding();

    const Model::Monitor *focused = m_state.focusedMonitor();
    placeOnFocusedMonitor();

    // Every capture costs the compositor real work, and Hyprland answers
    // capture requests whether or not a window changed, so ask for as little
    // as possible:
    //  - windows on workspaces that are on screen (the focused one first,
    //    because the zoom starts from it) are captured now and, while open,
    //    kept live;
    //  - windows on hidden workspaces can't change much (Hyprland doesn't
    //    let them draw), so their cached picture stands and is refreshed in
    //    the background one window at a time; only windows never pictured
    //    are captured straight away.
    // What changes on screen while the zoom plays costs frames, and every
    // capture is GPU work for the compositor, so during the zoom only the
    // focused workspace is pictured (it's what the zoom starts from).
    // Windows on hidden workspaces were pictured as you left them and can't
    // have changed since; only those whose picture isn't known to be exact
    // are refreshed, one at a time, once the zoom has finished.
    QStringList hero, later;
    m_trickle.clear();
    m_pendingHero.clear();
    QSet<int> onScreen;
    for (const auto &m : m_state.monitors) {
        onScreen.insert(m.activeWorkspace);
        if (m.specialWorkspace)
            onScreen.insert(m.specialWorkspace);
    }
    for (const auto &w : m_state.workspaces) {
        for (const auto &win : w.windows) {
            if (focused && w.id == focused->activeWorkspace) {
                hero += win.stableId;
                m_pendingHero.insert(win.stableId);
            } else if (onScreen.contains(w.id) || !m_exact.contains(win.stableId)) {
                later += win.stableId;
            }
        }
    }
    m_captured.clear();
    for (const auto &w : m_state.allWindows())
        m_captured.insert(w.stableId);
    m_trickle = later;
    if (!hero.isEmpty())
        QMetaObject::invokeMethod(m_capturer, "capture", Qt::QueuedConnection, Q_ARG(QStringList, hero), Q_ARG(bool, false));
    // Zoom (260 ms) plus a little.
    m_trickleTimer.start(320);

    // Whoever was full screen on this workspace gets that back on close.
    m_restoreFullscreen.clear();
    if (focused)
        if (const auto *w = m_state.workspace(focused->activeWorkspace))
            for (const auto &win : w->windows)
                if (win.fullscreen)
                    m_restoreFullscreen.insert(win.address, win.fullscreen);

    m_frameTimes.clear();
    if (qEnvironmentVariableIsSet("OMISSION_DEBUG"))
        QTimer::singleShot(650, this, [this] {
            QStringList gaps;
            for (int i = 1; i < m_frameTimes.size(); ++i)
                gaps += QString::number(m_frameTimes[i] - m_frameTimes[i - 1]);
            qInfo("omission: first frame at %lld ms, then gaps: %s", m_frameTimes.isEmpty() ? -1 : m_frameTimes.first(),
                  qPrintable(gaps.join(' ')));
        });
    m_shown = true;
    m_presented = false;
    emit shownChanged();
    emit opening();

    // The window appears when its first frame can show the focused workspace
    // as it is. Cached pictures are good enough for that (fresh ones replace
    // them a few frames into the zoom); otherwise wait briefly for captures.
    bool cached = true;
    for (const auto &id : std::as_const(hero))
        cached = cached && Thumbnails::instance()->has(id);
    if (cached)
        present();
    else
        m_presentTimer.start();
}

void Backend::present() {
    m_presentTimer.stop();
    if (!m_shown || m_presented)
        return;
    m_presented = true;
    m_window->showFullScreen();
    m_window->requestActivate();
    if (qEnvironmentVariableIsSet("OMISSION_DEBUG"))
        qInfo("omission: window shown %lld ms after open", m_openClock.elapsed());
    emit heroReady();
}

// Refreshes one cached picture of a hidden workspace at a time, so the
// compositor never has a pile of copies to make at once.
void Backend::trickle() {
    if (!m_shown || m_trickle.isEmpty()) {
        m_trickleTimer.stop();
        return;
    }
    m_trickleTimer.start(80);
    const QStringList one{m_trickle.takeFirst()};
    QMetaObject::invokeMethod(m_capturer, "capture", Qt::QueuedConnection, Q_ARG(QStringList, one), Q_ARG(bool, false));
}

void Backend::onFrame(const QString &id) {
    if (m_pendingHero.isEmpty() || !m_pendingHero.remove(id) || !m_pendingHero.isEmpty())
        return;
    if (qEnvironmentVariableIsSet("OMISSION_DEBUG"))
        qInfo("omission: focused workspace captured %lld ms after open", m_openClock.elapsed());
    present();
}

void Backend::close() {
    if (m_shown)
        emit closeRequested();
}

void Backend::finishClose() {
    if (!m_shown)
        return;
    m_shown = false;
    m_pendingHero.clear();
    m_trickle.clear();
    m_trickleTimer.stop();
    m_presentTimer.stop();
    if (m_window) {
        m_window->hide();
        // The overview is a full-screen window. Until Hyprland has unmapped
        // it, focusing a window on this workspace makes that window take
        // over full screen (misc:on_focus_under_fullscreen = 1).
        syncWithCompositor();
    }
    for (auto it = m_restoreFullscreen.begin(); it != m_restoreFullscreen.end(); ++it)
        m_hyprland->dispatch(QString("hl.dsp.window.fullscreen({ mode = %1, action = \"set\", window = %2 })")
                                 .arg(Hyprland::quote(it.value() == 1 ? "maximized" : "fullscreen"),
                                      Hyprland::quote("address:" + it.key())));
    m_restoreFullscreen.clear();
    QMetaObject::invokeMethod(m_capturer, "rest", Qt::QueuedConnection);
    emit shownChanged();
}

void Backend::focusWindow(const QString &address) {
    m_hyprland->dispatch(QString("hl.dsp.focus({ window = %1 })").arg(Hyprland::quote("address:" + address)));
}

void Backend::focusWorkspace(int id) {
    for (const auto &w : m_state.workspaces)
        if (w.id == id && w.special) {
            m_hyprland->dispatch(QString("hl.dsp.workspace.toggle_special(%1)").arg(Hyprland::quote(w.name.mid(8))));
            return;
        }
    m_hyprland->dispatch(QString("hl.dsp.focus({ workspace = %1 })").arg(Hyprland::quote(QString::number(id))));
}

void Backend::moveWindow(const QString &address, int workspace) {
    QString target = QString::number(workspace);
    if (const auto *w = m_state.workspace(workspace); w && w->special)
        target = w->name;
    m_hyprland->dispatch(QString("hl.dsp.window.move({ workspace = %1, follow = false, window = %2 })")
                             .arg(Hyprland::quote(target), Hyprland::quote("address:" + address)));
    refresh();
}

void Backend::summonWindow(const QString &address) {
    const Model::Monitor *focused = m_state.focusedMonitor();
    if (!focused)
        return;
    moveWindow(address, focused->activeWorkspace);
    focusWindow(address);
}

void Backend::closeWindow(const QString &address) {
    m_hyprland->dispatch(QString("hl.dsp.window.close({ window = %1 })").arg(Hyprland::quote("address:" + address)));
}

void Backend::prewarm(const QList<int> &workspaces) {
    QStringList ids;
    for (int id : workspaces)
        if (const auto *w = m_state.workspace(id))
            for (const auto &win : w->windows) {
                ids += win.stableId;
                m_exact.insert(win.stableId);
            }
    if (!ids.isEmpty())
        QMetaObject::invokeMethod(m_capturer, "capture", Qt::QueuedConnection, Q_ARG(QStringList, ids), Q_ARG(bool, false));
}

void Backend::onEvent(const QString &name, const QString &) {
    if (name == "configreloaded") {
        applyWindowRule();
        return;
    }
    if (m_shown) {
        static const QSet<QString> relevant = {"openwindow", "closewindow", "movewindowv2", "windowtitlev2",
                                               "changefloatingmode", "fullscreen", "createworkspacev2",
                                               "destroyworkspacev2", "renameworkspace", "moveworkspacev2"};
        if (relevant.contains(name))
            m_refreshDebounce.start();
        return;
    }
    // While hidden, picture a workspace's windows the moment you leave it:
    // they can't change until you come back, so the next opening already
    // has them exactly as they were.
    if (name == "workspacev2" || name == "focusedmonv2") {
        const QHash<int, int> before = m_activeByMonitor;
        refresh();
        // Windows now on screen can change, so their pictures stop being exact.
        for (const auto &m : m_state.monitors)
            if (const auto *w = m_state.workspace(m.activeWorkspace))
                for (const auto &win : w->windows)
                    m_exact.remove(win.stableId);
        for (auto it = before.begin(); it != before.end(); ++it)
            if (m_activeByMonitor.value(it.key()) != it.value())
                m_leftWorkspaces.insert(it.value());
        if (!m_leftWorkspaces.isEmpty())
            m_prewarmDebounce.start();
    }
}
