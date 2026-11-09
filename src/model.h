#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QVariantList>

// Hyprland's state, reduced to what the overview draws: monitors, the
// workspaces on them, and each window's place on its monitor.
namespace Model {

struct Window {
    QString address;   // "0x55…", what dispatchers take
    QString stableId;  // matches the ext-foreign-toplevel identifier
    QString appClass;
    QString title;
    double x = 0, y = 0, width = 0, height = 0; // logical, relative to the monitor
    bool floating = false;
    int fullscreen = 0;
    int focusHistory = 0; // 0 is the focused window, 1 the one before, …
    int workspace = 0;
};

struct Monitor {
    int id = 0;
    QString name;
    double x = 0, y = 0, width = 0, height = 0; // logical layout coordinates
    int activeWorkspace = 0;
    int specialWorkspace = 0;
    bool focused = false;
};

struct Workspace {
    int id = 0;
    QString name;
    int monitor = 0;
    bool special = false;
    QList<Window> windows; // bottom to top
};

struct State {
    QList<Monitor> monitors;
    QList<Workspace> workspaces;

    const Monitor *monitor(int id) const;
    const Monitor *focusedMonitor() const;
    const Workspace *workspace(int id) const;
    QList<Window> allWindows() const;
};

State parse(const QJsonArray &monitors, const QJsonArray &workspaces, const QJsonArray &clients);

// What QML gets: one map per workspace with windows placed in 0…1 units of
// their monitor, so a card of any size draws them by multiplying.
QVariantList toVariant(const State &state);

// The lowest positive workspace id nothing uses yet, for "New workspace".
int firstFreeWorkspace(const State &state);

struct Grid {
    int columns = 1;
    int rows = 1;
    double cardWidth = 0;
    double cardHeight = 0;
};

// The largest cards of the given aspect that fit `count` of them in the
// area, leaving `gap` between cards and `label` under each row.
Grid fitGrid(int count, double aspect, double width, double height, double gap, double label, double maxCardWidth);

} // namespace Model
