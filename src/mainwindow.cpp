#include "mainwindow.h"
#include "editor.h"
#include "markdown.h"
#include "settingspanel.h"
#include "widgets.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWindow>

static bool moveFile(const QString &src, const QString &dst)
{
    if (QFile::rename(src, dst))
        return true;
    return QFile::copy(src, dst) && QFile::remove(src); // across filesystems
}

// the undo history follows its note when the file is moved/archived/deleted
static void moveHistory(const QString &from, const QString &to)
{
    const QString src = Paths::historyFile(from);
    if (!QFile::exists(src))
        return;
    const QString dst = Paths::historyFile(to);
    QFile::remove(dst);
    moveFile(src, dst);
}

static void loadHistory(Editor *ed, const QString &path)
{
    QFile f(Paths::historyFile(path));
    if (f.open(QIODevice::ReadOnly))
        ed->restoreHistory(QJsonDocument::fromJson(f.readAll()).object());
}

MainWindow::MainWindow(const AppSettings &s, QWidget *parent) : QWidget(parent), m_s(s)
{
    setWindowTitle("Notas");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    resize(900, 600);

    m_tabs = new QTabBar(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setMovable(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setElideMode(Qt::ElideRight);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->installEventFilter(this);

    m_drag = new DragArea(this);
    m_drag->setMinimumWidth(40);
    m_stack = new QStackedWidget(this);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);
    top->addWidget(m_tabs, 0);
    top->addWidget(m_drag, 1);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 2, 6, 6);
    root->setSpacing(0);
    root->addLayout(top);
    root->addWidget(m_stack, 1);

    m_find = new FindBar(this);
    m_find->hide();
    m_panel = new SettingsPanel(this);

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i < 0 || i >= m_stack->count())
            return;
        if (Editor *prev = current(); prev && m_find->isVisible())
            prev->setSearchHighlights({});
        m_stack->setCurrentIndex(i);
        if (Editor *ed = editorAt(i); ed && !m_panel->isVisible()) {
            if (m_find->isVisible())
                refreshFind();
            else
                ed->setFocus();
        }
        m_sessionTimer.start();
    });
    connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *w = m_stack->widget(from);
        m_stack->removeWidget(w);
        m_stack->insertWidget(to, w);
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        m_sessionTimer.start();
    });
    connect(m_drag, &DragArea::doubleClicked, this, [this] { newTab(); });

    connect(m_find, &FindBar::changed, this, &MainWindow::findIncremental);
    connect(m_find, &FindBar::next, this, &MainWindow::findNext);
    connect(m_find, &FindBar::closed, this, &MainWindow::closeFind);
    connect(m_find, &FindBar::selectAll, this, [this] {
        const QString n = m_find->edit->text();
        Editor *ed = current();
        closeFind();
        if (ed && !n.isEmpty())
            ed->selectAllOccurrences(n, {});
    });

    connect(m_panel, &SettingsPanel::changed, this, [this](const AppSettings &ns) { applySettings(ns, false); });
    connect(m_panel, &SettingsPanel::closed, this, [this] {
        if (Editor *ed = current())
            ed->setFocus();
    });

    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(400);
    connect(&m_sessionTimer, &QTimer::timeout, this, &MainWindow::saveSession);

    // live reload of config.json, themes/*.json and the Hyprland theme state
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(150);
    connect(&m_reloadTimer, &QTimer::timeout, this, &MainWindow::reloadFromDisk);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    rewatch();

    // ---------------------------------------------------------------- shortcuts
    auto sc = [this](const QList<QKeySequence> &keys, auto fn) {
        for (const auto &k : keys) {
            auto *s = new QShortcut(k, this);
            s->setContext(Qt::WindowShortcut);
            connect(s, &QShortcut::activated, this, fn);
        }
    };
    using K = QKeySequence;
    sc({K("Ctrl+T"), K("Ctrl+N")}, [this] { newTab(); });
    sc({K("Ctrl+W"), K("Ctrl+F4")}, [this] { if (m_tabs->currentIndex() >= 0) closeTab(m_tabs->currentIndex()); });
    sc({K("Ctrl+Shift+T")}, [this] { reopenClosed(); });
    sc({K("Ctrl+Tab"), K("Ctrl+PgDown")}, [this] {
        if (m_tabs->count()) m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    });
    sc({K("Ctrl+Shift+Tab"), K(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), K("Ctrl+PgUp")}, [this] {
        if (m_tabs->count()) m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
    });
    sc({K("Ctrl+Shift+PgDown")}, [this] {
        const int i = m_tabs->currentIndex();
        if (i >= 0 && i < m_tabs->count() - 1) m_tabs->moveTab(i, i + 1);
    });
    sc({K("Ctrl+Shift+PgUp")}, [this] {
        const int i = m_tabs->currentIndex();
        if (i > 0) m_tabs->moveTab(i, i - 1);
    });
    for (int n = 1; n <= 9; ++n)
        sc({K(QString("Alt+%1").arg(n))}, [this, n] {
            if (m_tabs->count()) m_tabs->setCurrentIndex(n == 9 ? m_tabs->count() - 1 : qMin(n - 1, m_tabs->count() - 1));
        });
    sc({K("Ctrl+F")}, [this] { openFind(); });
    sc({K("F3")}, [this] { if (!m_find->edit->text().isEmpty()) findNext(false); else openFind(); });
    sc({K("Shift+F3")}, [this] { if (!m_find->edit->text().isEmpty()) findNext(true); else openFind(); });
    sc({K("Ctrl+="), K("Ctrl++"), K("Ctrl+Shift+=")}, [this] { zoom(1); });
    sc({K("Ctrl+-")}, [this] { zoom(-1); });
    sc({K("Ctrl+0")}, [this] { zoom(0); });
    sc({K("Ctrl+S")}, [this] { saveEverything(); });
    sc({K("Ctrl+,")}, [this] { toggleSettings(); });
    sc({K("Ctrl+Shift+,"), K("Ctrl+<"), K("Ctrl+Shift+<")}, [this] { openFile(Paths::configFile()); });
    sc({K("Ctrl+Shift+M")}, [this] {
        AppSettings ns = m_s;
        ns.markdown = !ns.markdown;
        applySettings(ns, false);
    });
    sc({K("Ctrl+Q")}, [this] { close(); });

    applyLook();
    loadSession();
}

// ---------------------------------------------------------------- tab helpers

Editor *MainWindow::editorAt(int i) const { return qobject_cast<Editor *>(m_stack->widget(i)); }
Editor *MainWindow::current() const { return editorAt(m_tabs->currentIndex()); }

int MainWindow::indexOfId(const QString &id) const
{
    for (int i = 0; i < m_stack->count(); ++i)
        if (editorAt(i)->filePath.isEmpty() && editorAt(i)->noteId == id)
            return i;
    return -1;
}

int MainWindow::indexOfPath(const QString &path) const
{
    for (int i = 0; i < m_stack->count(); ++i)
        if (pathOf(editorAt(i)) == path)
            return i;
    return -1;
}

QString MainWindow::pathOf(const Editor *ed) const
{
    return ed->filePath.isEmpty() ? Store::notePath(m_s.notesDir, ed->noteId) : ed->filePath;
}

QString MainWindow::newId() const
{
    const QString base = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    QString id = base;
    for (int n = 2;; ++n) {
        if (indexOfId(id) < 0 && !QFile::exists(Store::notePath(m_s.notesDir, id)) &&
            !QFile::exists(Store::notePath(Store::closedDir(m_s.notesDir), id)))
            return id;
        id = QString("%1-%2").arg(base).arg(n);
    }
}

Editor *MainWindow::createTab(const QString &id, const QString &path, const QString &text, int index)
{
    auto *ed = new Editor;
    ed->noteId = id;
    ed->filePath = path;
    ed->loadContent(text);
    loadHistory(ed, pathOf(ed));
    ed->applySettings(m_s);
    ed->applyTheme(m_theme);

    connect(&ed->saveTimer, &QTimer::timeout, this, [this, ed] { saveNote(ed); });
    connect(ed, &Editor::edited, this, [this, ed] {
        ed->dirty = true;
        ed->saveTimer.start(m_s.autosaveMs);
        updateTabTitle(ed);
        if (m_find->isVisible() && ed == current())
            refreshFind();
    });
    connect(ed, &Editor::zoomRequested, this, &MainWindow::zoom);

    const int idx = index < 0 || index > m_tabs->count() ? m_tabs->count() : index;
    m_stack->insertWidget(idx, ed);
    m_tabs->insertTab(idx, QString());

    auto *btn = new CloseButton(m_tabs);
    btn->setColors(m_theme.muted, m_theme.surface2);
    connect(btn, &QAbstractButton::clicked, this, [this, btn] {
        for (int i = 0; i < m_tabs->count(); ++i)
            if (m_tabs->tabButton(i, QTabBar::RightSide) == btn) {
                closeTab(i);
                return;
            }
    });
    m_tabs->setTabButton(idx, QTabBar::RightSide, btn);
    updateTabTitle(ed);
    return ed;
}

void MainWindow::updateTabTitle(Editor *ed)
{
    const int i = m_stack->indexOf(ed);
    if (i < 0)
        return;
    QString t;
    if (!ed->filePath.isEmpty()) {
        t = QFileInfo(ed->filePath).fileName();
        m_tabs->setTabToolTip(i, Paths::contract(ed->filePath));
    } else {
        int n = 0;
        for (QTextBlock b = ed->document()->begin(); b.isValid() && n < 64; b = b.next(), ++n) {
            t = b.text().trimmed();
            if (!t.isEmpty())
                break;
        }
        if (m_s.markdown) { // "# Título" / "- [ ] item" -> "Título" / "item"
            static const QRegularExpression lead(R"(^(#{1,6}\s+|[-*+]\s+(\[[ xX]\]\s+)?|\d+[.)]\s+|>+\s*))");
            static const QRegularExpression emphasis(R"(\*\*|__|`|~~|(?<!\w)[*_]|[*_](?!\w))");
            if (MarkdownHighlighter::tableRowRe().match(t).hasMatch()) { // a note that starts with a table
                const QString cell = MarkdownHighlighter::tableCells(t).value(0).remove(emphasis).trimmed();
                t = cell.isEmpty() ? QStringLiteral("tabela") : "tabela - " + cell;
            } else {
                t.remove(lead);
                t.remove(emphasis);
                t = t.trimmed();
            }
        }
        if (t.isEmpty())
            t = "nova nota";
        if (t.size() > 26)
            t = t.left(25).trimmed() + QChar(0x2026);
    }
    t.replace("&", "&&");
    if (m_tabs->tabText(i) != t)
        m_tabs->setTabText(i, t);
    if (auto *b = qobject_cast<CloseButton *>(m_tabs->tabButton(i, QTabBar::RightSide)))
        b->setDirty(ed->dirty);
}

void MainWindow::newTab(const QString &text)
{
    const int idx = m_tabs->currentIndex() + 1;
    Editor *ed = createTab(newId(), QString(), QString(), idx);
    if (!text.isEmpty()) {
        ed->loadContent(text);
        ed->dirty = true;
        saveNote(ed);
    }
    m_tabs->setCurrentIndex(m_stack->indexOf(ed));
    ed->setFocus();
    m_sessionTimer.start();
}

void MainWindow::closeTab(int i)
{
    Editor *ed = editorAt(i);
    if (!ed)
        return;
    saveNote(ed);
    if (ed->filePath.isEmpty()) {
        const QString path = pathOf(ed);
        if (QFile::exists(path)) {
            if (ed->toPlainText().trimmed().isEmpty() || m_s.deleteOnClose) {
                QFile::remove(path);
                QFile::remove(Paths::historyFile(path));
            } else {
                const QString dir = Store::closedDir(m_s.notesDir);
                QDir().mkpath(dir);
                const QString dst = Store::notePath(dir, ed->noteId);
                QFile::remove(dst);
                if (moveFile(path, dst)) {
                    moveHistory(path, dst);
                    m_closed.removeAll(ed->noteId);
                    m_closed.append(ed->noteId); // no limit: every closed tab can be reopened
                }
            }
        }
    }
    m_stack->removeWidget(ed); // stack first, so the tab bar's currentChanged maps correctly
    m_tabs->removeTab(i);
    ed->deleteLater();
    if (m_tabs->count() == 0)
        newTab();
    saveSession();
}

void MainWindow::reopenClosed()
{
    const QString dir = Store::closedDir(m_s.notesDir);
    while (!m_closed.isEmpty()) {
        QString id = m_closed.takeLast();
        const QString src = Store::notePath(dir, id);
        if (!QFile::exists(src))
            continue;
        if (QFile::exists(Store::notePath(m_s.notesDir, id)) || indexOfId(id) >= 0)
            id = newId();
        if (!moveFile(src, Store::notePath(m_s.notesDir, id)))
            continue;
        moveHistory(src, Store::notePath(m_s.notesDir, id)); // before createTab, which loads it
        Editor *ed = createTab(id, QString(), Store::readText(Store::notePath(m_s.notesDir, id)),
                               m_tabs->currentIndex() + 1);
        m_tabs->setCurrentIndex(m_stack->indexOf(ed));
        saveSession();
        return;
    }
}

void MainWindow::openFile(const QString &path)
{
    const QString abs = QFileInfo(Paths::expand(path)).absoluteFilePath();
    if (int i = indexOfPath(abs); i >= 0) {
        m_tabs->setCurrentIndex(i);
        return;
    }
    Editor *ed;
    const QFileInfo fi(abs);
    if (fi.absolutePath() == QFileInfo(m_s.notesDir).absoluteFilePath() && fi.suffix() == "txt")
        ed = createTab(fi.completeBaseName(), QString(), Store::readText(abs), m_tabs->currentIndex() + 1);
    else
        ed = createTab(QString(), abs, Store::readText(abs), m_tabs->currentIndex() + 1);
    m_tabs->setCurrentIndex(m_stack->indexOf(ed));
    m_sessionTimer.start();
}

// ---------------------------------------------------------------- persistence

void MainWindow::saveNote(Editor *ed)
{
    ed->saveTimer.stop();
    if (!ed->dirty && !ed->historyDirty)
        return;
    const QString path = pathOf(ed);
    const QString text = ed->toPlainText();
    if (text.isEmpty() && !QFile::exists(path)) { // never create empty files
        ed->dirty = false;
        updateTabTitle(ed);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(text.toUtf8());
        if (f.commit())
            ed->dirty = false;
    }
    if (ed->dirty) {
        warn(QString("não foi possível salvar %1: %2").arg(path, f.errorString()));
    } else if (ed->historyDirty) {
        // saved right after the text, so the history's hash matches what's on disk
        const QString hp = Paths::historyFile(path);
        QDir().mkpath(QFileInfo(hp).absolutePath());
        QSaveFile h(hp);
        if (h.open(QIODevice::WriteOnly)) {
            h.write(QJsonDocument(ed->historyJson()).toJson(QJsonDocument::Compact));
            if (h.commit())
                ed->historyDirty = false;
        }
        if (ed->historyDirty)
            warn(QString("não foi possível salvar o histórico de %1: %2").arg(path, h.errorString()));
    }
    updateTabTitle(ed);
}

void MainWindow::saveAll()
{
    for (int i = 0; i < m_stack->count(); ++i)
        saveNote(editorAt(i));
}

void MainWindow::saveEverything()
{
    saveAll();
    saveSession();
}

void MainWindow::saveSession()
{
    m_sessionTimer.stop();
    QJsonArray tabs;
    for (int i = 0; i < m_stack->count(); ++i) {
        const Editor *ed = editorAt(i);
        QJsonObject t;
        if (ed->filePath.isEmpty())
            t["id"] = ed->noteId;
        else
            t["path"] = ed->filePath;
        t["cursor"] = ed->textCursor().position();
        t["anchor"] = ed->textCursor().anchor();
        t["scroll"] = ed->verticalScrollBar()->value();
        tabs.append(t);
    }
    QJsonObject o;
    o["version"] = 1;
    o["current"] = m_tabs->currentIndex();
    o["tabs"] = tabs;
    o["closed"] = QJsonArray::fromStringList(m_closed);

    QDir().mkpath(m_s.notesDir);
    QSaveFile f(m_s.notesDir + "/.session.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

void MainWindow::loadSession()
{
    QDir().mkpath(m_s.notesDir);
    const QJsonObject o = Store::readSession(m_s.notesDir);
    const int base = m_tabs->count();
    for (const auto &v : o.value("tabs").toArray()) {
        const QJsonObject t = v.toObject();
        Editor *ed = nullptr;
        if (t.contains("path")) {
            const QString p = t.value("path").toString();
            if (p.isEmpty() || indexOfPath(p) >= 0)
                continue;
            ed = createTab(QString(), p, Store::readText(p));
        } else {
            const QString id = t.value("id").toString();
            if (id.isEmpty() || indexOfId(id) >= 0)
                continue;
            ed = createTab(id, QString(), Store::readText(Store::notePath(m_s.notesDir, id)));
        }
        const int len = ed->document()->characterCount() - 1;
        QTextCursor c(ed->document());
        c.setPosition(qBound(0, t.value("anchor").toInt(), len));
        c.setPosition(qBound(0, t.value("cursor").toInt(), len), QTextCursor::KeepAnchor);
        ed->setTextCursor(c);
        ed->pendingScroll = t.value("scroll").toInt(-1);
    }
    loadLooseFiles();

    m_closed.clear();
    for (const auto &v : o.value("closed").toArray())
        if (QFile::exists(Store::notePath(Store::closedDir(m_s.notesDir), v.toString())))
            m_closed << v.toString();

    if (m_tabs->count() == 0)
        newTab();
    m_tabs->setCurrentIndex(qBound(0, base + o.value("current").toInt(0), m_tabs->count() - 1));
    if (Editor *ed = current())
        ed->setFocus();
}

// *.txt files dropped into the notes dir by other programs become tabs too.
void MainWindow::loadLooseFiles()
{
    const auto files = QDir(m_s.notesDir).entryInfoList({"*.txt"}, QDir::Files, QDir::Time | QDir::Reversed);
    for (const auto &fi : files)
        if (indexOfId(fi.completeBaseName()) < 0)
            createTab(fi.completeBaseName(), QString(), Store::readText(fi.absoluteFilePath()));
}

void MainWindow::switchNotesDir(const QString &dir, bool moveFiles)
{
    saveEverything();
    const QString old = m_s.notesDir;
    if (moveFiles) {
        if (!QDir().mkpath(dir)) {
            warn(QString("não foi possível criar %1").arg(dir));
            return;
        }
        for (int i = 0; i < m_stack->count(); ++i) {
            Editor *ed = editorAt(i);
            if (!ed->filePath.isEmpty())
                continue;
            const QString src = Store::notePath(old, ed->noteId);
            if (!QFile::exists(src))
                continue;
            if (QFile::exists(Store::notePath(dir, ed->noteId)))
                ed->noteId += "-" + QDateTime::currentDateTime().toString("HHmmsszzz");
            if (!moveFile(src, Store::notePath(dir, ed->noteId)))
                warn(QString("não foi possível mover %1").arg(src));
            else
                moveHistory(src, Store::notePath(dir, ed->noteId));
        }
        const QString oldClosed = Store::closedDir(old), newClosed = Store::closedDir(dir);
        for (const QString &id : std::as_const(m_closed)) {
            QDir().mkpath(newClosed);
            if (moveFile(Store::notePath(oldClosed, id), Store::notePath(newClosed, id)))
                moveHistory(Store::notePath(oldClosed, id), Store::notePath(newClosed, id));
        }
        QFile::remove(old + "/.session.json");
        QDir().rmdir(oldClosed); // only succeeds if empty
        m_s.notesDir = dir;
        loadLooseFiles();
        saveSession();
    } else {
        // switch: close note tabs (files stay where they are) and load the other dir
        for (int i = m_stack->count() - 1; i >= 0; --i) {
            Editor *ed = editorAt(i);
            if (!ed->filePath.isEmpty())
                continue;
            m_stack->removeWidget(ed);
            m_tabs->removeTab(i);
            ed->deleteLater();
        }
        m_s.notesDir = dir;
        loadSession();
    }
}

// ---------------------------------------------------------------- settings & theme

void MainWindow::applySettings(AppSettings ns, bool fromFile)
{
    if (QDir::cleanPath(ns.notesDir) != QDir::cleanPath(m_s.notesDir) && !ns.notesDir.isEmpty()) {
        if (!fromFile)
            switchNotesDir(ns.notesDir, true);
        else if (QDir(ns.notesDir).exists()) // edited by hand: only switch to dirs that exist
            switchNotesDir(ns.notesDir, false);
        else
            ns.notesDir = m_s.notesDir;
    }
    m_s = ns;
    if (!fromFile && !m_s.save())
        warn("config.json não foi salvo (corrija o erro de sintaxe primeiro)");
    applyLook();
}

void MainWindow::applyLook()
{
    QString err;
    m_theme = Theme::resolve(m_s, &err);
    if (err != m_lastWarn) {
        if (!err.isEmpty())
            warn(err);
        m_lastWarn = err;
    }
    const QString css = m_theme.styleSheet(m_s.elementRadius);
    if (css != m_lastCss) {
        qApp->setStyleSheet(css);
        m_lastCss = css;
    }
    for (int i = 0; i < m_stack->count(); ++i) {
        editorAt(i)->applySettings(m_s);
        editorAt(i)->applyTheme(m_theme);
        if (auto *b = qobject_cast<CloseButton *>(m_tabs->tabButton(i, QTabBar::RightSide)))
            b->setColors(m_theme.muted, m_theme.surface2);
    }
    m_panel->setRadius(m_s.windowRadius);
    update();
}

void MainWindow::rewatch()
{
    QStringList want = {Paths::configDir(), Paths::configFile(), Paths::themesDir(),
                        QFileInfo(Theme::systemThemeFile()).absolutePath(), Theme::systemThemeFile()};
    for (const QString &t : Theme::available())
        want << Theme::pathFor(t);
    const QString themePath = Theme::pathFor(m_s.theme);
    if (QFile::exists(themePath))
        want << themePath;
    QStringList missing;
    const QStringList have = m_watcher.files() + m_watcher.directories();
    for (const QString &p : want)
        if (!have.contains(p) && QFileInfo::exists(p))
            missing << p;
    if (!missing.isEmpty())
        m_watcher.addPaths(missing);
}

void MainWindow::reloadFromDisk()
{
    rewatch(); // atomic saves replace files, so watches must be re-added
    AppSettings ns;
    QString err;
    if (ns.load(&err)) {
        m_lastWarn.clear();
        applySettings(ns, true);
    } else {
        warn(QString("%1: %2").arg(Paths::configFile(), err));
        applyLook(); // the theme may still have changed
    }
    // keep an open config.json tab in sync when it was written by us
    if (int i = indexOfPath(Paths::configFile()); i >= 0) {
        Editor *ed = editorAt(i);
        const QString disk = Store::readText(Paths::configFile());
        if (!ed->dirty && disk != ed->toPlainText()) {
            const int pos = ed->textCursor().position();
            ed->setPlainText(disk);
            QTextCursor c(ed->document());
            c.setPosition(qMin(pos, ed->document()->characterCount() - 1));
            ed->setTextCursor(c);
            ed->dirty = false;
            ed->saveTimer.stop();
            updateTabTitle(ed);
        }
    }
}

void MainWindow::zoom(int delta)
{
    m_s.fontSize = delta == 0 ? 11 : qBound(6, m_s.fontSize + delta, 72);
    m_s.save();
    for (int i = 0; i < m_stack->count(); ++i)
        editorAt(i)->applySettings(m_s);
}

void MainWindow::toggleSettings()
{
    if (m_panel->isVisible()) {
        m_panel->hide();
        if (Editor *ed = current())
            ed->setFocus();
        return;
    }
    closeFind();
    m_panel->setGeometry(rect());
    m_panel->open(m_s);
}

// ---------------------------------------------------------------- find

void MainWindow::openFind()
{
    Editor *ed = current();
    if (!ed)
        return;
    const QString sel = ed->textCursor().selectedText();
    if (!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator))
        m_find->edit->setText(sel);
    placeOverlays();
    m_find->show();
    m_find->raise();
    m_find->edit->setFocus();
    m_find->edit->selectAll();
    refreshFind();
}

void MainWindow::closeFind()
{
    if (!m_find->isVisible())
        return;
    m_find->hide();
    if (Editor *ed = current()) {
        ed->setSearchHighlights({});
        ed->setFocus();
    }
}

void MainWindow::findNext(bool backwards)
{
    Editor *ed = current();
    const QString n = m_find->edit->text();
    if (!ed || n.isEmpty())
        return;
    const QTextCursor c = ed->textCursor();
    QTextDocument *doc = ed->document();
    QTextDocument::FindFlags fl;
    if (backwards)
        fl |= QTextDocument::FindBackward;
    QTextCursor f = doc->find(n, backwards ? c.selectionStart() : c.selectionEnd(), fl);
    if (f.isNull())
        f = doc->find(n, backwards ? doc->characterCount() - 1 : 0, fl);
    if (!f.isNull())
        ed->setAllCursors({f});
    refreshFind();
}

void MainWindow::findIncremental()
{
    Editor *ed = current();
    const QString n = m_find->edit->text();
    if (ed && !n.isEmpty()) {
        QTextCursor f = ed->document()->find(n, ed->textCursor().selectionStart());
        if (f.isNull())
            f = ed->document()->find(n, 0);
        if (!f.isNull())
            ed->setAllCursors({f});
    }
    refreshFind();
}

void MainWindow::refreshFind()
{
    Editor *ed = current();
    if (!ed)
        return;
    const QString n = m_find->edit->text();
    QList<QTextCursor> hits;
    if (!n.isEmpty()) {
        QTextCursor f = ed->document()->find(n, 0);
        while (!f.isNull() && hits.size() < 5000) {
            hits << f;
            f = ed->document()->find(n, f.selectionEnd());
        }
    }
    ed->setSearchHighlights(hits);
    const QTextCursor c = ed->textCursor();
    int idx = 0;
    for (int i = 0; i < hits.size(); ++i)
        if (hits[i].selectionStart() == c.selectionStart() && hits[i].selectionEnd() == c.selectionEnd()) {
            idx = i + 1;
            break;
        }
    if (n.isEmpty())
        m_find->info->clear();
    else if (idx)
        m_find->info->setText(QString("%1/%2").arg(idx).arg(hits.size()));
    else
        m_find->info->setText(QString::number(hits.size()));
}

// ---------------------------------------------------------------- window chrome

void MainWindow::placeOverlays()
{
    const QRect r = m_stack->geometry();
    m_find->adjustSize();
    m_find->move(r.right() - m_find->width() - 14, r.top() + 4);
    if (m_panel->isVisible())
        m_panel->setGeometry(rect());
}

void MainWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(m_theme.surface, 1));
    p.setBrush(m_theme.base);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), m_s.windowRadius, m_s.windowRadius);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    placeOverlays();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    saveEverything();
    e->accept();
}

Qt::Edges MainWindow::edgesAt(const QPoint &p) const
{
    const int m = 6;
    Qt::Edges e;
    if (p.x() < m) e |= Qt::LeftEdge;
    if (p.x() > width() - m) e |= Qt::RightEdge;
    if (p.y() < m) e |= Qt::TopEdge;
    if (p.y() > height() - m) e |= Qt::BottomEdge;
    return e;
}

void MainWindow::mouseMoveEvent(QMouseEvent *e)
{
    const Qt::Edges ed = edgesAt(e->position().toPoint());
    if (ed == (Qt::LeftEdge | Qt::TopEdge) || ed == (Qt::RightEdge | Qt::BottomEdge))
        setCursor(Qt::SizeFDiagCursor);
    else if (ed == (Qt::RightEdge | Qt::TopEdge) || ed == (Qt::LeftEdge | Qt::BottomEdge))
        setCursor(Qt::SizeBDiagCursor);
    else if (ed & (Qt::LeftEdge | Qt::RightEdge))
        setCursor(Qt::SizeHorCursor);
    else if (ed & (Qt::TopEdge | Qt::BottomEdge))
        setCursor(Qt::SizeVerCursor);
    else
        unsetCursor();
}

void MainWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !windowHandle())
        return;
    const Qt::Edges ed = edgesAt(e->position().toPoint());
    if (ed)
        windowHandle()->startSystemResize(ed);
    else
        windowHandle()->startSystemMove();
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_tabs && e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::MiddleButton) {
            const int i = m_tabs->tabAt(me->position().toPoint());
            if (i >= 0)
                closeTab(i);
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

// ---------------------------------------------------------------- IPC / argv

void MainWindow::handleCommand(const QJsonObject &cmd)
{
    for (const auto &f : cmd.value("files").toArray())
        openFile(f.toString());
    const QJsonArray texts = cmd.value("texts").toArray();
    for (const auto &t : texts)
        newTab(t.toString());
    if (cmd.value("new").toBool())
        newTab();
    if (cmd.value("settings").toBool() && !m_panel->isVisible())
        toggleSettings();
    show();
    raise();
    activateWindow();
}
