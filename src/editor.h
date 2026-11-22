#pragma once
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTimer>
#include <functional>

struct AppSettings;
struct Theme;

// Plain text editor with VSCode-like multi-cursor editing.
class Editor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit Editor(QWidget *parent = nullptr);

    void applySettings(const AppSettings &s);
    void applyTheme(const Theme &t);

    QString noteId;   // note stored in the notes dir (empty for external files)
    QString filePath; // external file opened from the command line
    bool dirty = false;
    int pendingScroll = -1;
    QTimer saveTimer;

    QList<QTextCursor> allCursors() const;
    void setAllCursors(QList<QTextCursor> cs, int primary = 0);
    void clearExtraCursors();
    bool hasExtraCursors() const { return !m_extra.isEmpty(); }

    void setSearchHighlights(const QList<QTextCursor> &hits);
    void selectAllOccurrences(const QString &needle = QString(),
                              QTextDocument::FindFlags flags = QTextDocument::FindCaseSensitively);

signals:
    void zoomRequested(int delta);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    struct Group { int first, last; };

    void edit(const std::function<void(QTextCursor &)> &fn);
    void move(QTextCursor::MoveOperation op, bool shift);
    void addCursorVertical(int dir);
    void selectNextOccurrence();
    void newline();
    void insertLine(bool above);
    void smartHome(bool shift);
    void tabKey();
    void indentLines(bool outdent);
    void moveLines(int dir);
    void duplicateLines(int dir);
    void deleteLines();
    void selectLines();
    void copy(bool cut);
    void paste();

    QList<Group> blockGroups() const;
    QString indentUnit(int column) const;
    QString leadingWs(const QTextBlock &b, int upTo = -1) const;
    int posAt(int blockNumber, int column) const;
    void refreshExtraSelections();
    void resetBlink();

    QList<QTextCursor> m_extra;
    QList<QTextCursor> m_search;
    QColor m_cursorColor, m_extraSel, m_searchColor;
    int m_tabWidth = 4;
    bool m_useSpaces = false;
    bool m_autoIndent = true;
    QTimer m_blink;
    bool m_blinkOn = true;
};
