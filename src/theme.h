#pragma once
#include <QColor>
#include <QJsonObject>
#include <QString>

struct AppSettings;

struct Theme {
    QString name;
    bool dark = true;
    QColor base, card, surface, surface2, surface3, hover;
    QColor text, textStrong, muted, accent, cursor, selection, search;
    QString extraCss;

    // Built-in palette, derived from ~/gruvbox_theme.css when present.
    static Theme builtin(bool dark);
    // Resolves settings.theme (auto/dark/light/name/path) into a Theme. err gets a warning, if any.
    static Theme resolve(const AppSettings &s, QString *err = nullptr);
    static Theme fromFile(const QString &path, QString *err);
    static QString pathFor(const QString &nameOrPath);
    static QStringList available();
    // Writes gruvbox-dark.json / gruvbox-light.json into the themes dir if they don't exist.
    static void installDefaults();

    static QString systemThemeFile();
    static bool systemDark();

    QJsonObject toJson() const;
    QString styleSheet(int radius) const;
};
