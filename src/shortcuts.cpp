#include "shortcuts.h"

#include <QSocketNotifier>

#include <wayland-client.h>

#include "hyprland-global-shortcuts-v1-client.h"

#include <cstring>

namespace {

void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
    auto **manager = static_cast<hyprland_global_shortcuts_manager_v1 **>(data);
    if (!std::strcmp(interface, hyprland_global_shortcuts_manager_v1_interface.name))
        *manager = static_cast<hyprland_global_shortcuts_manager_v1 *>(
            wl_registry_bind(registry, name, &hyprland_global_shortcuts_manager_v1_interface, 1));
}

void onGlobalRemove(void *, wl_registry *, uint32_t) {}

// A key sends pressed and then released; the overview acts on the press.
template <void (Shortcuts::*Signal)()>
void onPressed(void *data, hyprland_global_shortcut_v1 *, uint32_t, uint32_t, uint32_t) {
    emit (static_cast<Shortcuts *>(data)->*Signal)();
}

void onReleased(void *, hyprland_global_shortcut_v1 *, uint32_t, uint32_t, uint32_t) {}

} // namespace

Shortcuts::Shortcuts(QObject *parent) : QObject(parent) {
    m_display = wl_display_connect(nullptr);
    if (!m_display)
        return;
    static const wl_registry_listener registryListener = {onGlobal, onGlobalRemove};
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &registryListener, &m_manager);
    if (wl_display_roundtrip(m_display) < 0 || !m_manager) {
        disconnectDisplay(); // not Hyprland, or one without the protocol
        return;
    }

    static const hyprland_global_shortcut_v1_listener toggleListener = {onPressed<&Shortcuts::toggle>, onReleased};
    static const hyprland_global_shortcut_v1_listener showListener = {onPressed<&Shortcuts::show>, onReleased};
    static const hyprland_global_shortcut_v1_listener hideListener = {onPressed<&Shortcuts::hide>, onReleased};
    const struct {
        const char *id;
        const char *description;
        const hyprland_global_shortcut_v1_listener *listener;
    } shortcuts[] = {
        {"toggle", "Open or close the overview", &toggleListener},
        {"show", "Open the overview", &showListener},
        {"hide", "Close the overview", &hideListener},
    };
    for (const auto &shortcut : shortcuts) {
        auto *registered = hyprland_global_shortcuts_manager_v1_register_shortcut(m_manager, shortcut.id, "omission",
                                                                                  shortcut.description, "");
        hyprland_global_shortcut_v1_add_listener(registered, shortcut.listener, this);
        m_shortcuts += registered;
    }
    // Another omission already holds the names: Hyprland answers with a
    // protocol error, which ends this connection and nothing else.
    if (wl_display_roundtrip(m_display) < 0) {
        disconnectDisplay();
        return;
    }

    m_notifier = new QSocketNotifier(wl_display_get_fd(m_display), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Shortcuts::dispatch);
}

Shortcuts::~Shortcuts() {
    disconnectDisplay();
}

void Shortcuts::dispatch() {
    if (!m_display)
        return;
    if (wl_display_prepare_read(m_display) == 0 && wl_display_read_events(m_display) < 0) {
        disconnectDisplay(); // Hyprland went away
        return;
    }
    wl_display_dispatch_pending(m_display);
    wl_display_flush(m_display);
}

void Shortcuts::disconnectDisplay() {
    delete m_notifier;
    m_notifier = nullptr;
    // Closing the connection takes every object on it along.
    m_shortcuts.clear();
    m_manager = nullptr;
    m_registry = nullptr;
    if (m_display)
        wl_display_disconnect(m_display);
    m_display = nullptr;
}
