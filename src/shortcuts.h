#pragma once

#include <QList>
#include <QObject>

class QSocketNotifier;
struct wl_display;
struct wl_registry;
struct hyprland_global_shortcuts_manager_v1;
struct hyprland_global_shortcut_v1;

// Lets Hyprland open the overview itself. A key bound to `omission toggle`
// starts a process for every press, and loading Qt's libraries into it takes
// about 25 ms before it can say anything. A key bound to Hyprland's global
// shortcut "omission:toggle" costs one Wayland event to the resident
// overview instead.
//
// The shortcuts live on a connection of their own: registering a name twice
// is a protocol error, and a second omission must lose only its shortcuts.
class Shortcuts : public QObject {
    Q_OBJECT

public:
    explicit Shortcuts(QObject *parent = nullptr);
    ~Shortcuts() override;

signals:
    void toggle();
    void show();
    void hide();

private:
    void dispatch();
    void disconnectDisplay();

    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    hyprland_global_shortcuts_manager_v1 *m_manager = nullptr;
    QList<hyprland_global_shortcut_v1 *> m_shortcuts;
    QSocketNotifier *m_notifier = nullptr;
};
