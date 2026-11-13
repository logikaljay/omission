#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantList>

#include "model.h"

class Capturer;
class Hyprland;
class QQuickWindow;
class Theme;

// The overview's state and everything it does to the desktop. It stays
// resident, so opening is a refresh of Hyprland's state (a couple of
// milliseconds) rather than a process start.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY stateChanged)
    Q_PROPERTY(int newWorkspaceId READ newWorkspaceId NOTIFY stateChanged)
    Q_PROPERTY(QString wallpaper READ wallpaper NOTIFY wallpaperChanged)
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    Backend(Hyprland *hyprland, Capturer *capturer, Theme *theme, QObject *parent = nullptr);

    void setWindow(QQuickWindow *window);

    QVariantList workspaces() const { return m_workspaces; }
    int newWorkspaceId() const { return Model::firstFreeWorkspace(m_state); }
    QString wallpaper() const { return m_wallpaper; }
    bool shown() const { return m_shown; }
    QString error() const { return m_error; }

    // Replaces Hyprland's state, for tests and the screenshot harness.
    void setState(const Model::State &state);

    Q_INVOKABLE QVariantMap grid(int count, double aspect, double width, double height, double gap, double label, double maxCardWidth) const;

public slots:
    void toggle();
    void show();
    // Asks QML to play the closing animation; QML calls finishClose().
    void close();
    void finishClose();

    void focusWindow(const QString &address);
    void focusWorkspace(int id);
    void moveWindow(const QString &address, int workspace);
    void summonWindow(const QString &address);
    void closeWindow(const QString &address);

signals:
    void stateChanged();
    void wallpaperChanged();
    void shownChanged();
    void errorChanged();
    // QML resets on opening and starts the zoom on heroReady, which comes
    // as the window appears.
    void opening();
    void heroReady();
    void closeRequested();

private slots:
    void setError(const QString &error);

private:
    void refresh();
    void onEvent(const QString &name, const QString &data);
    void onFrame(const QString &id);
    void trickle();
    void present();
    void placeOnFocusedMonitor();
    void setAnimated(const QStringList &addresses, bool animated);
    void applyWindowRule();
    void prewarm(const QList<int> &workspaces);
    void reloadWallpaper();

    Hyprland *m_hyprland;
    Capturer *m_capturer;
    Theme *m_theme;
    QPointer<QQuickWindow> m_window;

    Model::State m_state;
    QVariantList m_workspaces;
    QHash<int, int> m_activeByMonitor;
    QSet<QString> m_pendingHero;
    QSet<QString> m_captured;
    QString m_wallpaper;
    QString m_error;
    bool m_shown = false;
    QTimer m_refreshDebounce;
    QTimer m_prewarmDebounce;
    // Windows whose picture shows them exactly as they are: captured after
    // their workspace was left, and not on screen since.
    QSet<QString> m_exact;
    QTimer m_presentTimer;
    bool m_presented = false;
    QHash<QString, int> m_restoreFullscreen;
    // Windows under the overview, whose animations are off while it's open.
    QStringList m_stilled;
    QTimer m_trickleTimer;
    QStringList m_trickle;
    QSet<int> m_leftWorkspaces;
    QElapsedTimer m_openClock;
    QStringList m_frameProgress;
    QList<qint64> m_frameTimes; // written from the render thread while open
};
