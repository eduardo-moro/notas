#pragma once
#include "settings.h"
#include "theme.h"
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QTimer>
#include <QWidget>

class Editor;
class QTabBar;
class QStackedWidget;
class DragArea;
class FindBar;
class SettingsPanel;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(const AppSettings &s, QWidget *parent = nullptr);

    // {"files": [...], "texts": [...], "new": bool} — from argv or another `notas` invocation.
    void handleCommand(const QJsonObject &cmd);
    void saveEverything();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    Editor *editorAt(int i) const;
    Editor *current() const;
    int indexOfId(const QString &id) const;
    int indexOfPath(const QString &path) const;
    QString pathOf(const Editor *ed) const;
    QString newId() const;

    Editor *createTab(const QString &id, const QString &path, const QString &text, int index = -1);
    void newTab(const QString &text = QString());
    void closeTab(int i);
    void reopenClosed();
    void openFile(const QString &path);
    void updateTabTitle(Editor *ed);

    void saveNote(Editor *ed);
    void saveAll();
    void saveSession();
    void loadSession();
    void loadLooseFiles();
    void switchNotesDir(const QString &dir, bool moveFiles);

    void applySettings(AppSettings ns, bool fromFile);
    void applyLook();
    void reloadFromDisk();
    void rewatch();
    void zoom(int delta);

    void openFind();
    void closeFind();
    void findNext(bool backwards);
    void findIncremental();
    void refreshFind();

    void toggleSettings();
    void placeOverlays();
    Qt::Edges edgesAt(const QPoint &p) const;

    AppSettings m_s;
    Theme m_theme;
    QString m_lastCss, m_lastWarn;
    QStringList m_closed;

    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    DragArea *m_drag;
    FindBar *m_find;
    SettingsPanel *m_panel;

    QTimer m_sessionTimer;
    QTimer m_reloadTimer;
    QFileSystemWatcher m_watcher;
};
