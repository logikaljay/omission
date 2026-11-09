#include "model.h"

#include <QJsonObject>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace Model {

const Monitor *State::monitor(int id) const {
    for (const auto &m : monitors)
        if (m.id == id)
            return &m;
    return monitors.isEmpty() ? nullptr : &monitors.first();
}

const Monitor *State::focusedMonitor() const {
    for (const auto &m : monitors)
        if (m.focused)
            return &m;
    return monitors.isEmpty() ? nullptr : &monitors.first();
}

const Workspace *State::workspace(int id) const {
    for (const auto &w : workspaces)
        if (w.id == id)
            return &w;
    return nullptr;
}

QList<Window> State::allWindows() const {
    QList<Window> all;
    for (const auto &w : workspaces)
        all += w.windows;
    return all;
}

static QString specialName(const QString &name) {
    return name.startsWith("special:") ? name.mid(8) : name;
}

State parse(const QJsonArray &monitors, const QJsonArray &workspaces, const QJsonArray &clients) {
    State state;
    for (const auto &value : monitors) {
        const auto o = value.toObject();
        if (o.value("disabled").toBool())
            continue;
        Monitor m;
        m.id = o.value("id").toInt();
        m.name = o.value("name").toString();
        m.x = o.value("x").toDouble();
        m.y = o.value("y").toDouble();
        const double scale = std::max(0.1, o.value("scale").toDouble(1));
        double w = o.value("width").toDouble() / scale, h = o.value("height").toDouble() / scale;
        // Odd transforms rotate by 90°, swapping the logical width and height.
        if (o.value("transform").toInt() % 2 == 1)
            std::swap(w, h);
        m.width = std::round(w);
        m.height = std::round(h);
        m.activeWorkspace = o.value("activeWorkspace").toObject().value("id").toInt();
        m.specialWorkspace = o.value("specialWorkspace").toObject().value("id").toInt();
        m.focused = o.value("focused").toBool();
        state.monitors += m;
    }
    std::sort(state.monitors.begin(), state.monitors.end(), [](const Monitor &a, const Monitor &b) { return a.id < b.id; });

    for (const auto &value : workspaces) {
        const auto o = value.toObject();
        Workspace w;
        w.id = o.value("id").toInt();
        w.name = o.value("name").toString();
        w.monitor = o.value("monitorID").toInt();
        w.special = w.id < 0;
        // Special workspaces show only while they hold something; normal
        // ones show even empty, because Hyprland keeps them only while
        // they're active or persistent.
        if (w.special && o.value("windows").toInt() == 0)
            continue;
        state.workspaces += w;
    }

    // A monitor's active workspace always gets a card, even before Hyprland
    // lists it.
    for (const auto &m : state.monitors)
        if (m.activeWorkspace != 0 && !state.workspace(m.activeWorkspace)) {
            Workspace w;
            w.id = m.activeWorkspace;
            w.name = QString::number(w.id);
            w.monitor = m.id;
            state.workspaces += w;
        }

    for (const auto &value : clients) {
        const auto o = value.toObject();
        // Skip unmapped and hidden windows, and the overview itself.
        if (!o.value("mapped").toBool(true) || o.value("hidden").toBool() || o.value("class").toString() == "omission")
            continue;
        Window win;
        win.address = o.value("address").toString();
        win.stableId = o.value("stableId").toString();
        win.appClass = o.value("class").toString();
        win.title = o.value("title").toString();
        win.floating = o.value("floating").toBool();
        win.fullscreen = o.value("fullscreen").toInt();
        win.focusHistory = o.value("focusHistoryID").toInt();
        win.workspace = o.value("workspace").toObject().value("id").toInt();
        const auto at = o.value("at").toArray(), size = o.value("size").toArray();
        const Monitor *m = state.monitor(o.value("monitor").toInt());
        win.x = at.at(0).toDouble() - (m ? m->x : 0);
        win.y = at.at(1).toDouble() - (m ? m->y : 0);
        win.width = size.at(0).toDouble();
        win.height = size.at(1).toDouble();
        if (win.width <= 0 || win.height <= 0)
            continue;
        for (auto &w : state.workspaces)
            if (w.id == win.workspace) {
                w.windows += win;
                break;
            }
    }

    // Draw tiled windows under floating ones and fullscreen over both, the
    // least recently focused first, so the stacking matches the screen.
    for (auto &w : state.workspaces)
        std::stable_sort(w.windows.begin(), w.windows.end(), [](const Window &a, const Window &b) {
            const int la = a.fullscreen ? 2 : a.floating ? 1 : 0, lb = b.fullscreen ? 2 : b.floating ? 1 : 0;
            if (la != lb)
                return la < lb;
            return a.focusHistory > b.focusHistory;
        });

    std::stable_sort(state.workspaces.begin(), state.workspaces.end(), [](const Workspace &a, const Workspace &b) {
        if (a.special != b.special)
            return !a.special;
        if (a.special)
            return a.name < b.name;
        return a.id < b.id;
    });
    return state;
}

QVariantList toVariant(const State &state) {
    QVariantList list;
    const Monitor *focused = state.focusedMonitor();
    for (const auto &w : state.workspaces) {
        const Monitor *m = state.monitor(w.monitor);
        const double mw = m && m->width > 0 ? m->width : 1, mh = m && m->height > 0 ? m->height : 1;
        QVariantList windows;
        for (const auto &win : w.windows)
            windows += QVariantMap{
                {"address", win.address},
                {"stableId", win.stableId},
                {"appClass", win.appClass},
                {"title", win.title},
                {"rx", win.x / mw},
                {"ry", win.y / mh},
                {"rw", win.width / mw},
                {"rh", win.height / mh},
                {"floating", win.floating},
                {"focusHistory", win.focusHistory},
            };
        const bool numeric = w.name == QString::number(w.id);
        list += QVariantMap{
            {"id", w.id},
            {"name", w.special ? specialName(w.name) : w.name},
            {"label", w.special ? specialName(w.name) : numeric ? QString::number(w.id) : w.name},
            {"special", w.special},
            {"monitor", m ? m->name : QString()},
            {"aspect", mw / mh},
            {"active", m && (m->activeWorkspace == w.id || m->specialWorkspace == w.id)},
            {"onFocusedMonitor", m && focused && m->id == focused->id},
            {"focused", focused && focused->activeWorkspace == w.id},
            {"windows", windows},
        };
    }
    return list;
}

int firstFreeWorkspace(const State &state) {
    int id = 1;
    while (state.workspace(id))
        ++id;
    return id;
}

Grid fitGrid(int count, double aspect, double width, double height, double gap, double label, double maxCardWidth) {
    Grid best;
    count = std::max(1, count);
    aspect = aspect > 0 ? aspect : 16.0 / 10.0;
    for (int columns = 1; columns <= count; ++columns) {
        const int rows = (count + columns - 1) / columns;
        const double byWidth = (width - (columns - 1) * gap) / columns;
        const double byHeight = ((height - rows * label - (rows - 1) * gap) / rows) * aspect;
        const double cardWidth = std::min({byWidth, byHeight, maxCardWidth});
        // On a tie (every layout hits maxCardWidth) the wider grid wins, so
        // a few workspaces sit side by side rather than stacked.
        if (cardWidth >= best.cardWidth - 0.5) {
            best.columns = columns;
            best.rows = rows;
            best.cardWidth = std::max(0.0, cardWidth);
        }
    }
    best.cardHeight = best.cardWidth / aspect;
    return best;
}

} // namespace Model
