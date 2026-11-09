#include "hyprland.h"

#include <QTimer>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

Hyprland::Hyprland(QObject *parent) : QObject(parent) {
    const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature.isEmpty())
        return;
    m_directory = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/hypr/" + signature;

    connect(&m_events, &QLocalSocket::readyRead, this, [this] {
        while (m_events.canReadLine()) {
            const QString line = QString::fromUtf8(m_events.readLine()).trimmed();
            const int split = line.indexOf(">>");
            if (split > 0)
                emit hyprEvent(line.left(split), line.mid(split + 2));
        }
    });
    // Hyprland restarts drop the socket; keep trying quietly so a resident
    // omission recovers without being restarted itself.
    connect(&m_events, &QLocalSocket::disconnected, this, [this] { QTimer::singleShot(1000, this, &Hyprland::connectEvents); });
    connect(&m_events, &QLocalSocket::errorOccurred, this, [this] {
        if (m_events.state() == QLocalSocket::UnconnectedState)
            QTimer::singleShot(1000, this, &Hyprland::connectEvents);
    });
    connectEvents();
}

void Hyprland::connectEvents() {
    if (m_events.state() == QLocalSocket::UnconnectedState)
        m_events.connectToServer(m_directory + "/.socket2.sock");
}

QByteArray Hyprland::request(const QByteArray &command) const {
    if (m_directory.isEmpty())
        return {};
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return {};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray path = (m_directory + "/.socket.sock").toLocal8Bit();
    std::strncpy(address.sun_path, path.constData(), sizeof(address.sun_path) - 1);

    QByteArray reply;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0
        && ::write(fd, command.constData(), command.size()) == command.size()) {
        char buffer[65536];
        ssize_t n;
        while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
            reply.append(buffer, n);
    }
    ::close(fd);
    return reply;
}

bool Hyprland::dispatch(const QString &lua) const {
    return request("dispatch " + lua.toUtf8()).trimmed() == "ok";
}

bool Hyprland::eval(const QString &lua) const {
    return request("eval " + lua.toUtf8()).trimmed() == "ok";
}

QString Hyprland::quote(const QString &text) {
    QString escaped = text;
    escaped.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + escaped + '"';
}
