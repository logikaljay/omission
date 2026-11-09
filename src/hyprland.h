#pragma once

#include <QJsonDocument>
#include <QLocalSocket>
#include <QObject>
#include <QString>

// Talks to Hyprland over its own sockets instead of spawning hyprctl, so a
// full state refresh costs a couple of milliseconds rather than three process
// launches.
class Hyprland : public QObject {
    Q_OBJECT

public:
    explicit Hyprland(QObject *parent = nullptr);

    bool available() const { return !m_directory.isEmpty(); }

    // One synchronous request on the command socket ("j/clients", "dispatch
    // ..."). Hyprland answers in well under a millisecond, so blocking is
    // cheaper than the bookkeeping of an asynchronous reply.
    QByteArray request(const QByteArray &command) const;
    QJsonDocument json(const QByteArray &command) const { return QJsonDocument::fromJson(request("j/" + command)); }

    // Dispatchers use the Lua syntax of Hyprland's Lua config
    // ("hl.dsp.focus({ workspace = \"2\" })").
    bool dispatch(const QString &lua) const;
    bool eval(const QString &lua) const;

    static QString quote(const QString &text);

signals:
    // A line from the event socket, split at ">>" ("workspacev2", "3,3").
    void hyprEvent(const QString &name, const QString &data);

private:
    void connectEvents();

    QString m_directory;
    QLocalSocket m_events;
};
