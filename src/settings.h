#pragma once
#include <QJsonObject>
#include <QString>

// Prints "notas: <msg>" to stderr (GNU style diagnostics).
void warn(const QString &msg);

// XDG paths and note storage helpers shared by the GUI and the CLI.
namespace Paths {
QString configDir();
QString configFile();
void setConfigFile(const QString &path);
QString themesDir();
QString socketPath();
// undo history of a file: $XDG_STATE_HOME/notas/historico/<sha1 of the absolute path>.json
QString historyFile(const QString &filePath);
QString expand(const QString &p);   // "~/x" -> "/home/u/x"
QString contract(const QString &p); // "/home/u/x" -> "~/x"
}

namespace Store {
QString notePath(const QString &dir, const QString &id);
QString closedDir(const QString &dir);
QJsonObject readSession(const QString &dir);
QStringList orderedIds(const QString &dir); // session order, then loose *.txt files
QString readText(const QString &path, bool *ok = nullptr);
QString title(const QString &text, int max = 26);
}

struct AppSettings {
    QString notesDir;
    int autosaveMs = 1000;
    QString fontFamily;
    int fontSize = 11;
    int padH = 28;
    int padV = 14;
    int tabWidth = 4;
    bool useSpaces = false;
    bool autoIndent = true;
    bool wordWrap = true;
    QString theme = "auto"; // auto | dark | light | <theme name> | <path.json>
    QString themeDark = "gruvbox-dark";
    QString themeLight = "gruvbox-light";
    int windowRadius = 12;  // corners of the window itself
    int elementRadius = 8;  // tabs, panels, find bar
    bool deleteOnClose = false;
    bool markdown = true; // live-preview markdown (WYSIWYG)

    // Loads config.json; writes a default one if missing.
    // Returns false (and fills error) on a syntax error; values stay at defaults/previous.
    bool load(QString *error = nullptr);
    bool save() const;

    static QString notesDirOverride; // --notes-dir (never persisted)
    static bool configBroken;        // don't overwrite a config the user is mid-editing
};
