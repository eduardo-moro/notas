#include "mainwindow.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QStyleFactory>

#include <csignal>
#include <cstdio>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

#define NOTAS_VERSION "1.0.0"

static int g_sigFd[2];

static void onSignal(int)
{
    const char c = 1;
    [[maybe_unused]] auto r = ::write(g_sigFd[0], &c, 1);
}

// Commands that only read files and print: they must not open a display connection.
static bool isCliOnly(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a == "--")
            break;
        if (a == "-l" || a == "--list" || a == "-p" || a == "--print" || a.startsWith("--print=") ||
            a == "--path" || a == "-h" || a == "--help" || a == "--help-all" || a == "-v" || a == "--version")
            return true;
    }
    return false;
}

static void out(const QString &s)
{
    fputs(qUtf8Printable(s), stdout);
}

int main(int argc, char **argv)
{
    const bool cli = isCliOnly(argc, argv);
    std::unique_ptr<QCoreApplication> app(cli ? new QCoreApplication(argc, argv) : new QApplication(argc, argv));
    QCoreApplication::setApplicationName("notas");
    QCoreApplication::setApplicationVersion(NOTAS_VERSION);

    QCommandLineParser p;
    p.setApplicationDescription("Bloco de notas minimalista com abas e salvamento automático.\n"
                                "Sem argumentos abre a janela (ou traz para frente a que já está aberta).");
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption optList({"l", "list"}, "Lista as notas abertas: índice, id e título, separados por TAB.");
    QCommandLineOption optPrint({"p", "print"}, "Escreve o conteúdo da nota (id ou índice de --list) na saída padrão.", "nota");
    QCommandLineOption optPath("path", "Mostra a pasta das notas e sai.");
    QCommandLineOption optNew({"n", "new"}, "Abre uma aba vazia.");
    QCommandLineOption optText({"t", "text"}, "Cria uma nota com TEXTO (pode repetir).", "texto");
    QCommandLineOption optSettings("settings", "Abre o painel de configurações.");
    QCommandLineOption optDir("notes-dir", "Usa PASTA como pasta das notas nesta execução.", "pasta");
    QCommandLineOption optConfig("config", "Usa ARQUIVO como configuração em vez de $XDG_CONFIG_HOME/notas/config.json.", "arquivo");
    p.addOptions({optList, optPrint, optPath, optNew, optText, optSettings, optDir, optConfig});
    p.addPositionalArgument("arquivo", "Arquivos para abrir em abas. '-' lê a entrada padrão para uma nota nova.",
                            "[arquivo...]");
    p.process(*app); // handles --help / --version, exits on bad options

    if (p.isSet(optConfig))
        Paths::setConfigFile(p.value(optConfig));
    if (p.isSet(optDir))
        AppSettings::notesDirOverride = QFileInfo(Paths::expand(p.value(optDir))).absoluteFilePath();

    AppSettings settings;
    QString err;
    if (!settings.load(&err))
        warn(QString("%1: %2").arg(Paths::configFile(), err));

    // ------------------------------------------------------------ non-GUI commands
    if (cli) {
        if (p.isSet(optPath)) {
            out(settings.notesDir + "\n");
            return 0;
        }
        const QStringList ids = Store::orderedIds(settings.notesDir);
        if (p.isSet(optList)) {
            for (int i = 0; i < ids.size(); ++i) {
                QString t = Store::title(Store::readText(Store::notePath(settings.notesDir, ids[i])), 60);
                out(QString("%1\t%2\t%3\n").arg(i + 1).arg(ids[i], t));
            }
            return 0;
        }
        if (p.isSet(optPrint)) {
            const QString key = p.value(optPrint);
            QString id = key;
            bool isNum = false;
            const int n = key.toInt(&isNum);
            if (isNum && !ids.contains(key)) {
                if (n < 1 || n > ids.size()) {
                    warn(QString("índice fora do intervalo: %1 (há %2 notas)").arg(n).arg(ids.size()));
                    return 1;
                }
                id = ids[n - 1];
            }
            bool ok = false;
            const QString text = Store::readText(Store::notePath(settings.notesDir, id), &ok);
            if (!ok) {
                warn(QString("nota não encontrada: %1").arg(key));
                return 1;
            }
            out(text);
            return 0;
        }
        return 0;
    }

    // ------------------------------------------------------------ GUI command
    QJsonObject cmd;
    QJsonArray files, texts;
    for (const QString &a : p.positionalArguments()) {
        if (a == "-") {
            QFile in;
            if (in.open(stdin, QIODevice::ReadOnly)) {
                const QString s = QString::fromUtf8(in.readAll());
                if (!s.isEmpty())
                    texts.append(s);
            }
        } else {
            files.append(QFileInfo(Paths::expand(a)).absoluteFilePath());
        }
    }
    for (const QString &t : p.values(optText))
        texts.append(t);
    cmd["files"] = files;
    cmd["texts"] = texts;
    cmd["new"] = p.isSet(optNew);
    cmd["settings"] = p.isSet(optSettings);

    // single instance: hand the command to the running window
    const QString sock = Paths::socketPath();
    {
        QLocalSocket s;
        s.connectToServer(sock);
        if (s.waitForConnected(300)) {
            s.write(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
            s.flush();
            s.waitForBytesWritten(1000);
            s.disconnectFromServer();
            if (s.state() != QLocalSocket::UnconnectedState)
                s.waitForDisconnected(1000);
            return 0;
        }
    }

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QGuiApplication::setDesktopFileName("notas");
    Theme::installDefaults();

    MainWindow w(settings);
    w.show();
    w.handleCommand(cmd);

    QLocalServer::removeServer(sock);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(sock))
        warn(QString("não foi possível escutar em %1: %2").arg(sock, server.errorString()));
    QObject::connect(&server, &QLocalServer::newConnection, &w, [&] {
        while (QLocalSocket *c = server.nextPendingConnection()) {
            auto *buf = new QByteArray;
            QObject::connect(c, &QLocalSocket::readyRead, c, [c, buf] { buf->append(c->readAll()); });
            QObject::connect(c, &QLocalSocket::disconnected, &w, [c, buf, &w] {
                buf->append(c->readAll());
                w.handleCommand(QJsonDocument::fromJson(*buf).object());
                delete buf;
                c->deleteLater();
            });
        }
    });

    // SIGTERM / SIGINT / SIGHUP: save everything and quit cleanly
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_sigFd) == 0) {
        auto *sn = new QSocketNotifier(g_sigFd[1], QSocketNotifier::Read, &w);
        QObject::connect(sn, &QSocketNotifier::activated, &w, [] {
            char c;
            [[maybe_unused]] auto r = ::read(g_sigFd[1], &c, 1);
            QCoreApplication::quit();
        });
        struct sigaction sa = {};
        sa.sa_handler = onSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
    }
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &w, [&w] { w.saveEverything(); });

    return app->exec();
}
