// omission — Mission Control for Omarchy: every workspace and every window
// on it, live, in one keystroke.

#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QUrl>

#include "backend.h"
#include "capturer.h"
#include "hyprland.h"
#include "shortcuts.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

static QByteArray socketPath() {
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    return (runtime.isEmpty() ? QByteArray("/tmp") : runtime) + "/omission.sock";
}

// The fast path: hand the command to the running overview with plain POSIX
// calls, before any Qt machinery starts, so a keypress costs one tiny
// process and a socket write.
static bool sendToRunning(const char *command) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray path = socketPath();
    std::strncpy(address.sun_path, path.constData(), sizeof(address.sun_path) - 1);
    bool sent = ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    if (sent) {
        const size_t length = std::strlen(command);
        sent = ::write(fd, command, length) == ssize_t(length) && ::write(fd, "\n", 1) == 1;
    }
    ::close(fd);
    return sent;
}

static void usage() {
    std::puts("Usage: omission [toggle|show|hide|daemon|quit]\n\n"
              "  toggle   Open the overview, or close it if open (the default)\n"
              "  show     Open the overview\n"
              "  hide     Close the overview\n"
              "  daemon   Start in the background without opening, for autostart\n"
              "  quit     Stop the running overview");
}

int main(int argc, char *argv[]) {
    const QByteArray command = argc > 1 ? QByteArray(argv[1]) : QByteArray("toggle");
    if (command == "-h" || command == "--help" || command == "help") {
        usage();
        return 0;
    }
    static const QList<QByteArray> commands = {"toggle", "show", "hide", "daemon", "quit"};
    if (!commands.contains(command)) {
        std::fprintf(stderr, "omission: unknown command '%s'\n", command.constData());
        usage();
        return 1;
    }
    if (command != "daemon" && sendToRunning(command.constData()))
        return 0;
    if (command == "hide" || command == "quit")
        return 0; // Nothing running, nothing to do.

    QGuiApplication app(argc, argv);
    app.setApplicationName("omission");
    app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("omission");
    app.setWindowIcon(QIcon::fromTheme("omission"));
    app.setQuitOnLastWindowClosed(false);

    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Hyprland hyprland;
    Capturer capturer;
    Backend backend(&hyprland, &capturer, &theme);

    QLocalServer::removeServer(QString::fromLocal8Bit(socketPath()));
    QLocalServer server;
    if (!server.listen(QString::fromLocal8Bit(socketPath()))) {
        std::fprintf(stderr, "omission: can't listen on %s\n", socketPath().constData());
        return 1;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (QLocalSocket *client = server.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            QObject::connect(client, &QLocalSocket::readyRead, client, [&, client] {
                while (client->canReadLine()) {
                    const QByteArray line = client->readLine().trimmed();
                    if (line == "toggle")
                        backend.toggle();
                    else if (line == "show")
                        backend.show();
                    else if (line == "hide")
                        backend.close();
                    else if (line == "quit")
                        app.quit();
                }
            });
        }
    });

    // Keys bound to Hyprland's global shortcuts ("omission:toggle") arrive
    // here without a process being started for them.
    Shortcuts shortcuts;
    QObject::connect(&shortcuts, &Shortcuts::toggle, &backend, &Backend::toggle);
    QObject::connect(&shortcuts, &Shortcuts::show, &backend, &Backend::show);
    QObject::connect(&shortcuts, &Shortcuts::hide, &backend, &Backend::close);

    qRegisterMetaType<std::shared_ptr<CaptureBuffer>>();
    qmlRegisterType<WindowThumb>("Omission", 1, 0, "WindowThumb");

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window)
        return 1;
    backend.setWindow(window);

    if (command != "daemon")
        backend.show();

    const int code = app.exec();
    QLocalServer::removeServer(QString::fromLocal8Bit(socketPath()));
    return code;
}
