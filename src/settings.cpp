#include "settings.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cstdio>
#include <unistd.h>

void warn(const QString &msg)
{
    fprintf(stderr, "notas: %s\n", qUtf8Printable(msg));
}

QString AppSettings::notesDirOverride;
bool AppSettings::configBroken = false;

// ---------------------------------------------------------------- Paths

static QString s_configFile;

static QString xdg(const char *var, const QString &fallback)
{
    const QString v = qEnvironmentVariable(var);
    return v.isEmpty() ? QDir::homePath() + fallback : v;
}

QString Paths::configDir() { return xdg("XDG_CONFIG_HOME", "/.config") + "/notas"; }
QString Paths::configFile() { return s_configFile.isEmpty() ? configDir() + "/config.json" : s_configFile; }
void Paths::setConfigFile(const QString &path) { s_configFile = QFileInfo(expand(path)).absoluteFilePath(); }
QString Paths::themesDir() { return configDir() + "/themes"; }

QString Paths::historyFile(const QString &filePath)
{
    const QByteArray key = QCryptographicHash::hash(QFileInfo(filePath).absoluteFilePath().toUtf8(),
                                                    QCryptographicHash::Sha1).toHex();
    return xdg("XDG_STATE_HOME", "/.local/state") + "/notas/historico/" + QString::fromLatin1(key) + ".json";
}

QString Paths::socketPath()
{
    const QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!rt.isEmpty())
        return rt + "/notas.sock";
    return QDir::tempPath() + QString("/notas-%1.sock").arg(getuid());
}

QString Paths::expand(const QString &p)
{
    QString s = p.trimmed();
    if (s == "~")
        return QDir::homePath();
    if (s.startsWith("~/"))
        s = QDir::homePath() + s.mid(1);
    return s.isEmpty() ? s : QDir::cleanPath(s);
}

QString Paths::contract(const QString &p)
{
    const QString home = QDir::homePath();
    if (p == home)
        return "~";
    if (p.startsWith(home + "/"))
        return "~" + p.mid(home.size());
    return p;
}

// ---------------------------------------------------------------- Store

QString Store::notePath(const QString &dir, const QString &id) { return dir + "/" + id + ".txt"; }
QString Store::closedDir(const QString &dir) { return dir + "/.fechadas"; }

QJsonObject Store::readSession(const QString &dir)
{
    QFile f(dir + "/.session.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QStringList Store::orderedIds(const QString &dir)
{
    QStringList ids;
    const QJsonArray tabs = readSession(dir).value("tabs").toArray();
    for (const auto &v : tabs) {
        const QString id = v.toObject().value("id").toString();
        if (!id.isEmpty() && !ids.contains(id))
            ids << id;
    }
    const auto files = QDir(dir).entryInfoList({"*.txt"}, QDir::Files, QDir::Time | QDir::Reversed);
    for (const auto &fi : files)
        if (!ids.contains(fi.completeBaseName()))
            ids << fi.completeBaseName();
    return ids;
}

QString Store::readText(const QString &path, bool *ok)
{
    QFile f(path);
    const bool good = f.open(QIODevice::ReadOnly);
    if (ok)
        *ok = good;
    return good ? QString::fromUtf8(f.readAll()) : QString();
}

QString Store::title(const QString &text, int max)
{
    QString t;
    for (const auto line : QStringView(text).split(u'\n')) {
        t = line.trimmed().toString();
        if (!t.isEmpty())
            break;
    }
    if (t.size() > max)
        t = t.left(max - 1).trimmed() + QChar(0x2026);
    return t;
}

// ---------------------------------------------------------------- AppSettings

static QJsonObject s_raw; // last parsed config, so unknown keys survive a save

bool AppSettings::load(QString *error)
{
    fontFamily = "monospace"; // fontconfig alias; works without a GUI connection
    notesDir = QDir::homePath() + "/Notas";

    QFile f(Paths::configFile());
    if (!f.exists()) {
        configBroken = false;
        s_raw = {};
        if (!notesDirOverride.isEmpty())
            notesDir = notesDirOverride;
        save();
        return true;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        configBroken = true;
        if (error)
            *error = QString("%1 (posição %2)").arg(pe.errorString()).arg(pe.offset);
        return false;
    }
    configBroken = false;
    s_raw = doc.object();
    const QJsonObject o = s_raw;
    const auto font = o.value("font").toObject();
    const auto pad = o.value("padding").toObject();
    const auto ind = o.value("indent").toObject();
    const auto th = o.value("theme");

    notesDir = Paths::expand(o.value("notesDir").toString(Paths::contract(notesDir)));
    autosaveMs = qBound(100, o.value("autosaveMs").toInt(autosaveMs), 600000);
    fontFamily = font.value("family").toString(fontFamily);
    fontSize = qBound(4, font.value("size").toInt(fontSize), 96);
    padH = qBound(0, pad.value("horizontal").toInt(padH), 1000);
    padV = qBound(0, pad.value("vertical").toInt(padV), 1000);
    tabWidth = qBound(1, ind.value("width").toInt(tabWidth), 16);
    useSpaces = ind.value("spaces").toBool(useSpaces);
    autoIndent = ind.value("auto").toBool(autoIndent);
    wordWrap = o.value("wordWrap").toBool(wordWrap);
    if (th.isObject()) {
        theme = th.toObject().value("mode").toString(theme);
        themeDark = th.toObject().value("dark").toString(themeDark);
        themeLight = th.toObject().value("light").toString(themeLight);
    } else if (th.isString()) {
        theme = th.toString();
    }
    if (o.value("radius").isObject()) {
        const auto r = o.value("radius").toObject();
        windowRadius = qBound(0, r.value("window").toInt(windowRadius), 40);
        elementRadius = qBound(0, r.value("elements").toInt(elementRadius), 40);
    } else if (o.value("radius").isDouble()) { // old format: single number
        elementRadius = qBound(0, o.value("radius").toInt(elementRadius), 40);
    }
    deleteOnClose = o.value("onClose").toString("archive") == "delete";
    markdown = o.value("markdown").toBool(markdown);

    if (!notesDirOverride.isEmpty())
        notesDir = notesDirOverride;
    return true;
}

bool AppSettings::save() const
{
    if (configBroken)
        return false;
    QJsonObject o = s_raw;
    if (notesDirOverride.isEmpty())
        o["notesDir"] = Paths::contract(notesDir);
    else if (!o.contains("notesDir"))
        o["notesDir"] = Paths::contract(QDir::homePath() + "/Notas");
    o["autosaveMs"] = autosaveMs;
    o["font"] = QJsonObject{{"family", fontFamily}, {"size", fontSize}};
    o["padding"] = QJsonObject{{"horizontal", padH}, {"vertical", padV}};
    o["indent"] = QJsonObject{{"width", tabWidth}, {"spaces", useSpaces}, {"auto", autoIndent}};
    o["wordWrap"] = wordWrap;
    o["theme"] = QJsonObject{{"mode", theme}, {"dark", themeDark}, {"light", themeLight}};
    o["radius"] = QJsonObject{{"window", windowRadius}, {"elements", elementRadius}};
    o["onClose"] = deleteOnClose ? "delete" : "archive";
    o["markdown"] = markdown;

    const QByteArray data = QJsonDocument(o).toJson(QJsonDocument::Indented);
    QFile cur(Paths::configFile());
    if (cur.open(QIODevice::ReadOnly) && cur.readAll() == data)
        return true; // unchanged: don't touch the file (avoids watcher churn)
    cur.close();

    QDir().mkpath(QFileInfo(Paths::configFile()).absolutePath());
    QSaveFile f(Paths::configFile());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    if (!f.commit())
        return false;
    s_raw = o;
    return true;
}
