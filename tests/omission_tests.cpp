#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "downscale.h"
#include "model.h"
#include "theme.h"
#include "translator.h"

class OmissionTests : public QObject {
    Q_OBJECT

private slots:
    void parsesWorkspacesAndPlacesWindows()
    {
        const auto state = Model::parse(monitors(), workspaces(), clients());
        QCOMPARE(state.monitors.size(), 1);
        QCOMPARE(state.monitors[0].width, 1440.0); // 2880 px at scale 2
        QCOMPARE(state.monitors[0].height, 932.0);

        // Normal workspaces in order, then specials; the empty special is gone.
        QCOMPARE(state.workspaces.size(), 3);
        QCOMPARE(state.workspaces[0].id, 1);
        QCOMPARE(state.workspaces[1].id, 3);
        QCOMPARE(state.workspaces[2].id, -98);

        const auto &one = state.workspaces[0].windows;
        QCOMPARE(one.size(), 2);
        // The floating window is drawn on top of the tiled one.
        QCOMPARE(one[0].appClass, QString("foot"));
        QCOMPARE(one[1].appClass, QString("pavucontrol"));
        QCOMPARE(one[0].stableId, QString("18000001"));
    }

    void hidesUnmappedAndHiddenWindows()
    {
        const auto state = Model::parse(monitors(), workspaces(), clients());
        for (const auto &w : state.allWindows())
            QVERIFY(w.appClass != "ghost");
    }

    void leavesItselfOut()
    {
        auto list = clients();
        list.append(client("18000009", "omission", 1, {0, 0}, {1440, 932}, true, 0));
        const auto state = Model::parse(monitors(), workspaces(), list);
        for (const auto &w : state.allWindows())
            QVERIFY(w.appClass != "omission");
        QCOMPARE(state.workspaces[0].windows.size(), 2);
    }

    void variantUsesMonitorRelativeUnits()
    {
        const auto list = Model::toVariant(Model::parse(monitors(), workspaces(), clients()));
        const auto first = list[0].toMap();
        QCOMPARE(first["focused"].toBool(), true);
        QCOMPARE(first["onFocusedMonitor"].toBool(), true);
        const auto foot = first["windows"].toList()[0].toMap();
        QCOMPARE(foot["rx"].toDouble(), 10.0 / 1440.0);
        QCOMPARE(foot["rw"].toDouble(), 0.5);
        const auto special = list[2].toMap();
        QCOMPARE(special["label"].toString(), QString("scratchpad"));
        QCOMPARE(special["special"].toBool(), true);
    }

    void activeWorkspaceAlwaysHasACard()
    {
        QJsonArray none;
        const auto state = Model::parse(monitors(), none, none);
        QCOMPARE(state.workspaces.size(), 1);
        QCOMPARE(state.workspaces[0].id, 1);
    }

    void findsTheFirstFreeWorkspace()
    {
        const auto state = Model::parse(monitors(), workspaces(), clients());
        QCOMPARE(Model::firstFreeWorkspace(state), 2);
    }

    void gridPrefersWideRowsAndFits()
    {
        // Bigger cards win: three fit larger as two rows of two.
        const auto three = Model::fitGrid(3, 1.6, 1400, 800, 20, 30, 10000);
        QCOMPARE(three.columns, 2);
        QCOMPARE(three.rows, 2);

        // When every layout hits the cap, they sit side by side.
        const auto tied = Model::fitGrid(3, 1.6, 1400, 800, 20, 30, 400);
        QCOMPARE(tied.columns, 3);
        QCOMPARE(tied.rows, 1);

        const auto six = Model::fitGrid(6, 1.6, 1400, 800, 20, 30, 10000);
        QCOMPARE(six.columns * six.rows >= 6, true);
        QVERIFY(six.columns * six.cardWidth + (six.columns - 1) * 20 <= 1400.5);
        QVERIFY(six.rows * (six.cardHeight + 30) + (six.rows - 1) * 20 <= 800.5);

        const auto capped = Model::fitGrid(1, 1.6, 1400, 800, 20, 30, 500);
        QCOMPARE(capped.cardWidth, 500.0);
    }

    void downscaleHalvesAndAverages()
    {
        // 4×2 XRGB: a black/white checker in the left half, solid red right.
        const quint32 pixels[] = {0xff000000, 0xffffffff, 0xffff0000, 0xffff0000,
                                  0xffffffff, 0xff000000, 0xffff0000, 0xffff0000};
        const QImage image = downscale(reinterpret_cast<const uchar *>(pixels), 4, 2, 16, false, 2);
        QCOMPARE(image.size(), QSize(2, 1));
        QCOMPARE(image.format(), QImage::Format_RGB32);
        QCOMPARE(QColor(image.pixel(0, 0)).red(), 128);
        QCOMPARE(image.pixel(1, 0), 0xffff0000u);
    }

    void downscaleCopiesSmallImages()
    {
        quint32 pixels[] = {0x00123456, 0x00abcdef};
        const QImage image = downscale(reinterpret_cast<const uchar *>(pixels), 2, 1, 8, false, 100);
        pixels[0] = 0;
        QCOMPARE(image.size(), QSize(2, 1));
        // A copy, made opaque.
        QCOMPARE(image.pixel(0, 0), 0xff123456u);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        // omission asks again each time it opens rather than polling.
        QTest::qWait(100);
        theme.refreshRounding();
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
    }

    void shippedCatalogsCoverEveryString()
    {
        // Every catalog compiled into the app parses and translates the
        // strings English counts plurals for.
        const QDir dir(QFINDTESTDATA("../src/i18n"));
        QVERIFY(dir.exists());
        const auto english = QJsonDocument::fromJson(read(dir.filePath("en.json"))).object();
        for (const auto &file : dir.entryList({"*.json"})) {
            QJsonParseError error;
            const auto catalog = QJsonDocument::fromJson(read(dir.filePath(file)), &error).object();
            QVERIFY2(error.error == QJsonParseError::NoError, qPrintable(file + ": " + error.errorString()));
            if (file == "en.json")
                continue;
            for (auto it = english.begin(); it != english.end(); ++it)
                QVERIFY2(catalog.contains(it.key()), qPrintable(file + " lacks " + it.key()));
        }
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static QJsonArray monitors()
    {
        return QJsonArray{QJsonObject{{"id", 0}, {"name", "eDP-1"}, {"x", 0}, {"y", 0}, {"width", 2880}, {"height", 1864},
                                      {"scale", 2}, {"transform", 0}, {"focused", true},
                                      {"activeWorkspace", QJsonObject{{"id", 1}}},
                                      {"specialWorkspace", QJsonObject{{"id", 0}}}}};
    }

    static QJsonArray workspaces()
    {
        return QJsonArray{
            QJsonObject{{"id", 3}, {"name", "3"}, {"monitorID", 0}, {"windows", 1}},
            QJsonObject{{"id", -98}, {"name", "special:scratchpad"}, {"monitorID", 0}, {"windows", 1}},
            QJsonObject{{"id", -99}, {"name", "special:empty"}, {"monitorID", 0}, {"windows", 0}},
            QJsonObject{{"id", 1}, {"name", "1"}, {"monitorID", 0}, {"windows", 2}},
        };
    }

    static QJsonObject client(const QString &id, const QString &cls, int ws, QJsonArray at, QJsonArray size,
                              bool floating, int focus, bool hidden = false)
    {
        return QJsonObject{{"address", "0x" + id}, {"stableId", id}, {"class", cls}, {"title", cls}, {"at", at},
                           {"size", size}, {"floating", floating}, {"workspace", QJsonObject{{"id", ws}}},
                           {"monitor", 0}, {"focusHistoryID", focus}, {"mapped", true}, {"hidden", hidden}};
    }

    static QJsonArray clients()
    {
        return QJsonArray{
            client("18000002", "pavucontrol", 1, {400, 200}, {500, 400}, true, 1),
            client("18000001", "foot", 1, {10, 38}, {720, 884}, false, 0),
            client("18000003", "chromium", 3, {10, 38}, {1420, 884}, false, 2),
            client("18000004", "foot", -98, {200, 200}, {800, 500}, true, 3),
            client("18000005", "ghost", 3, {10, 38}, {100, 100}, false, 4, true),
        };
    }
};

QTEST_MAIN(OmissionTests)
#include "omission_tests.moc"
