#include "theme.h"
#include "settings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>

// Accepts #RGB, #RRGGBB, #RRGGBBAA (CSS order) and color names.
static QColor parseColor(const QString &s)
{
    const QString v = s.trimmed();
    if (v.size() == 9 && v.startsWith('#')) {
        bool ok = false;
        const uint rgba = v.mid(1).toUInt(&ok, 16);
        if (ok)
            return QColor((rgba >> 24) & 0xff, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff);
    }
    return QColor::fromString(v);
}

static QString colorString(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name();
    return c.name(QColor::HexRgb) + QString::asprintf("%02x", c.alpha());
}

static QString cssColor(const QColor &c)
{
    if (c.alpha() == 255)
        return c.name();
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

static QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QString Theme::systemThemeFile() { return QDir::homePath() + "/.config/hypr/.current-theme"; }

bool Theme::systemDark()
{
    QFile f(systemThemeFile());
    if (!f.open(QIODevice::ReadOnly))
        return true;
    return f.readAll().trimmed() != "light";
}

Theme Theme::builtin(bool dark)
{
    QJsonObject pal;
    QFile f(QDir::homePath() + "/gruvbox_theme.css");
    if (f.open(QIODevice::ReadOnly))
        pal = QJsonDocument::fromJson(f.readAll()).object().value("colors").toObject()
                  .value(dark ? "dark" : "light").toObject();
    auto pick = [&](const char *k, const char *fb) {
        QColor c = parseColor(pal.value(QLatin1String(k)).toString());
        return c.isValid() ? c : QColor(fb);
    };

    Theme t;
    t.dark = dark;
    if (dark) {
        t.name = "gruvbox-dark";
        t.base = pick("bg0_hard", "#1d2021");
        t.card = pick("bg0_medium", "#282828");
        t.surface = pick("bg1", "#3c3836");
        t.surface2 = pick("bg2", "#504945");
        t.surface3 = pick("bg3", "#665c54");
        t.text = pick("fg1", "#ebdbb2");
        t.textStrong = pick("fg0", "#fbf1c7");
        t.muted = pick("grey", "#928374");
        t.accent = pick("yellow2", "#fabd2f");
        t.selection = pick("bg2", "#504945");
        t.h1 = pick("orange2", "#fe8019");
        t.h2 = pick("yellow2", "#fabd2f");
        t.h3 = pick("green2", "#b8bb26");
        t.h4 = pick("aqua2", "#8ec07c");
        t.h5 = pick("blue2", "#83a598");
        t.h6 = pick("purple2", "#d3869b");
        t.codeKeyword = pick("red2", "#fb4934");
        t.codeType = t.h2;
        t.codeString = t.h3;
        t.codeNumber = t.h6;
        t.codeComment = pick("grey", "#928374");
        t.link = pick("blue2", "#83a598");
        t.code = pick("aqua2", "#8ec07c");
    } else {
        t.name = "gruvbox-light";
        t.base = pick("bg0_soft", "#f2e5bc");
        t.card = pick("bg1", "#ebdbb2");
        t.surface = pick("bg2", "#d5c4a1");
        t.surface2 = pick("bg3", "#bdae93");
        t.surface3 = pick("bg4", "#a89984");
        t.text = pick("fg1", "#3c3836");
        t.textStrong = pick("fg0", "#282828");
        t.muted = pick("fg4", "#7c6f64");
        t.accent = pick("yellow2", "#b57614");
        t.selection = pick("bg2", "#d5c4a1");
        t.h1 = pick("orange2", "#af3a03");
        t.h2 = pick("yellow2", "#b57614");
        t.h3 = pick("green2", "#79740e");
        t.h4 = pick("aqua2", "#427b58");
        t.h5 = pick("blue2", "#076678");
        t.h6 = pick("purple2", "#8f3f71");
        t.codeKeyword = pick("red2", "#9d0006");
        t.codeType = t.h2;
        t.codeString = t.h3;
        t.codeNumber = t.h6;
        t.codeComment = pick("grey", "#928374");
        t.link = pick("blue2", "#076678");
        t.code = pick("aqua2", "#427b58");
    }
    t.cursor = t.accent;
    t.search = t.accent;
    t.search.setAlpha(70);
    t.hover = mix(t.base, t.surface, 0.5);
    return t;
}

static const char *kRoles[] = {"base", "card", "surface", "surface2", "surface3", "hover", "text",
                               "textStrong", "muted", "accent", "cursor", "selection", "search",
                               "h1", "h2", "h3", "h4", "h5", "h6", "link", "code",
                               "codeKeyword", "codeType", "codeString", "codeNumber", "codeComment"};

static QColor *role(Theme &t, const QString &k)
{
    static const QMap<QString, QColor Theme::*> m = {
        {"base", &Theme::base},         {"card", &Theme::card},
        {"surface", &Theme::surface},   {"surface2", &Theme::surface2},
        {"surface3", &Theme::surface3}, {"hover", &Theme::hover},
        {"text", &Theme::text},         {"textStrong", &Theme::textStrong},
        {"muted", &Theme::muted},       {"accent", &Theme::accent},
        {"cursor", &Theme::cursor},     {"selection", &Theme::selection},
        {"search", &Theme::search},
        {"h1", &Theme::h1}, {"h2", &Theme::h2}, {"h3", &Theme::h3},
        {"h4", &Theme::h4}, {"h5", &Theme::h5}, {"h6", &Theme::h6},
        {"link", &Theme::link},
        {"codeKeyword", &Theme::codeKeyword}, {"codeType", &Theme::codeType},
        {"codeString", &Theme::codeString},   {"codeNumber", &Theme::codeNumber},
        {"codeComment", &Theme::codeComment},
        {"code", &Theme::code},
    };
    auto it = m.find(k);
    return it == m.end() ? nullptr : &(t.*(*it));
}

QJsonObject Theme::toJson() const
{
    QJsonObject colors;
    Theme copy = *this;
    for (const char *k : kRoles)
        colors[k] = colorString(*role(copy, k));
    return QJsonObject{{"name", name}, {"dark", dark}, {"colors", colors}, {"css", extraCss}};
}

QString Theme::pathFor(const QString &nameOrPath)
{
    if (nameOrPath.contains('/') || nameOrPath.endsWith(".json"))
        return QFileInfo(Paths::expand(nameOrPath)).absoluteFilePath();
    return Paths::themesDir() + "/" + nameOrPath + ".json";
}

QStringList Theme::available()
{
    QStringList out;
    for (const auto &fi : QDir(Paths::themesDir()).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        out << fi.completeBaseName();
    return out;
}

Theme Theme::fromFile(const QString &path, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QString("tema não encontrado: %1").arg(path);
        return builtin(true);
    }
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (err)
            *err = QString("%1: %2 (posição %3)").arg(path, pe.errorString()).arg(pe.offset);
        return builtin(true);
    }
    const QJsonObject o = doc.object();
    Theme t = builtin(o.value("dark").toBool(true));
    t.name = o.value("name").toString(QFileInfo(path).completeBaseName());
    t.extraCss = o.value("css").toString();
    const QJsonObject colors = o.value("colors").toObject();
    const bool hasHover = colors.contains("hover");
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        QColor *c = role(t, it.key());
        const QColor v = parseColor(it.value().toString());
        if (!c) {
            if (err)
                *err = QString("%1: cor desconhecida \"%2\" ignorada").arg(path, it.key());
        } else if (!v.isValid()) {
            if (err)
                *err = QString("%1: valor inválido para \"%2\"").arg(path, it.key());
        } else {
            *c = v;
        }
    }
    if (!hasHover)
        t.hover = mix(t.base, t.surface, 0.5);
    if (!colors.contains("cursor"))
        t.cursor = t.accent;
    return t;
}

Theme Theme::resolve(const AppSettings &s, QString *err)
{
    QString name;
    if (s.theme == "auto")
        name = systemDark() ? s.themeDark : s.themeLight;
    else if (s.theme == "dark")
        name = s.themeDark;
    else if (s.theme == "light")
        name = s.themeLight;
    else
        name = s.theme;

    const QString path = pathFor(name);
    if (!QFile::exists(path)) {
        const bool dark = s.theme == "light" ? false : s.theme == "dark" ? true : systemDark();
        if (err)
            *err = QString("tema \"%1\" não encontrado, usando o embutido").arg(name);
        return builtin(dark);
    }
    return fromFile(path, err);
}

void Theme::installDefaults()
{
    QDir().mkpath(Paths::themesDir());
    for (bool dark : {true, false}) {
        const Theme t = builtin(dark);
        const QString path = pathFor(t.name);
        if (QFile::exists(path))
            continue;
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(t.toJson()).toJson(QJsonDocument::Indented));
            f.commit();
        }
    }
}

QString Theme::styleSheet(int radius) const
{
    QString css = R"CSS(
* { outline: 0; }
QToolTip { background: {card}; color: {text}; border: 1px solid {surface}; border-radius: 6px; padding: 4px 8px; }

QTabBar { background: transparent; border: none; }
QTabBar::tab { background: transparent; color: {muted}; border: none; border-radius: {rs}px;
               padding: 4px 6px 4px 12px; margin: 6px 3px 5px 0px; min-width: 40px; }
QTabBar::tab:selected { background: {surface}; color: {textStrong}; }
QTabBar::tab:hover:!selected { background: {hover}; color: {text}; }
QTabBar::scroller { width: 24px; }
QTabBar::tear { width: 0px; border: none; }
QTabBar QToolButton { background: {base}; border: none; border-radius: 6px; margin: 6px 1px; color: {text}; }
QTabBar QToolButton:hover { background: {surface}; }

QPlainTextEdit { background: transparent; border: none; color: {text};
                 selection-background-color: {selection}; selection-color: {textStrong}; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px 4px 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px 4px 2px 4px; }
QScrollBar::handle { background: {surface}; border-radius: 3px; }
QScrollBar::handle:hover { background: {surface2}; }
QScrollBar::handle:vertical { min-height: 32px; }
QScrollBar::handle:horizontal { min-width: 32px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QFrame#card { background: {card}; border: 1px solid {surface}; border-radius: {r}px; }
QFrame#findbar { background: {card}; border: 1px solid {surface}; border-radius: {rs}px; }
QFrame#findbar QLineEdit { border: none; background: transparent; padding: 2px 4px; }

QLabel { color: {text}; background: transparent; }
QLabel#title { color: {textStrong}; font-weight: 600; font-size: 13pt; }
QLabel#muted { color: {muted}; }
QLabel#key { color: {accent}; }
QLabel#section { color: {muted}; font-weight: 600; padding-top: 10px; }

QLineEdit, QComboBox { background: {base}; color: {text}; border: 1px solid {surface};
    border-radius: 6px; padding: 4px 8px; selection-background-color: {selection}; selection-color: {textStrong}; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid {accent}; }
QSpinBox { background: {base}; color: {text}; border: 1px solid {surface}; border-radius: 6px;
    padding: 0px; padding-left: 6px; min-height: 26px; selection-background-color: {selection}; selection-color: {textStrong}; }
QSpinBox::up-button, QSpinBox::down-button { subcontrol-origin: border; width: 0px; height: 0px; border: none; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: {card}; color: {text}; border: 1px solid {surface};
    selection-background-color: {surface}; selection-color: {textStrong}; outline: 0; padding: 4px; }

QCheckBox { color: {text}; spacing: 8px; background: transparent; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 4px; border: 1px solid {surface3}; background: {base}; }
QCheckBox::indicator:checked { background: {accent}; border: 1px solid {accent}; }

QMenu { background: {card}; color: {text}; border: 1px solid {surface}; border-radius: 6px; padding: 4px; }
QMenu::item { padding: 5px 18px; border-radius: 4px; }
QMenu::item:selected { background: {surface}; color: {textStrong}; }
QMenu::item:disabled { color: {muted}; }
QMenu::separator { height: 1px; background: {surface}; margin: 4px 6px; }

QPushButton { background: {surface}; color: {text}; border: none; border-radius: 6px; padding: 5px 12px; }
QPushButton:hover { background: {surface2}; }

QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
)CSS";

    Theme copy = *this;
    for (const char *k : kRoles)
        css.replace(QString("{%1}").arg(k), cssColor(*role(copy, k)));
    css.replace("{rs}", QString::number(qMax(0, radius - 3)));
    css.replace("{r}", QString::number(radius));
    return css + "\n" + extraCss;
}
