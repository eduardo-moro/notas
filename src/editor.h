#pragma once
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTimer>
#include <functional>

class MarkdownHighlighter;
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

    // Undo history that survives restarts (Qt's own undo stack can't be serialized).
    void loadContent(const QString &text); // replaces the text without an undo step; fresh history
    void undo();
    void redo();
    QJsonObject historyJson() const;
    bool restoreHistory(const QJsonObject &o); // false if it doesn't belong to the current text
    bool historyDirty = false;

    QList<QTextCursor> allCursors() const;
    void setAllCursors(QList<QTextCursor> cs, int primary = 0);
    void clearExtraCursors();
    bool hasExtraCursors() const { return !m_extra.isEmpty(); }

    void setSearchHighlights(const QList<QTextCursor> &hits);
    void selectAllOccurrences(const QString &needle = QString(),
                              QTextDocument::FindFlags flags = QTextDocument::FindCaseSensitively);

signals:
    void zoomRequested(int delta);
    void edited(); // the text really changed (textChanged also fires for markdown formatting)

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    struct Group { int first, last; };

    // one recorded change and one undo step (a step may hold several changes)
    struct Op { int pos; QString removed, added; };
    struct Step { QList<Op> ops; qint64 time = 0; };
    void onContentsChange();
    void applyStep(const Step &s, bool undo);
    QList<Step> m_undo, m_redo;
    QString m_shadow;       // text before the latest change, to compute what was removed
    bool m_applying = false; // changes made by undo/redo/load aren't recorded
    bool m_turnOpen = false; // changes in the same event-loop turn form one step
    bool m_breakMerge = false;

    // markdown live preview
    MarkdownHighlighter *m_md;
    bool m_formatOnly = false; // highlighter re-formatting: not a text change
    void updateActiveBlocks();
    bool markdownNewline(QTextCursor &c);
    bool markdownClick(QMouseEvent *e);
    void wrapSelection(const QString &marker);
    bool inCodeBlock(const QTextBlock &b) const;
    bool isTableLine(const QTextBlock &b) const;

    // tables: align columns, move between cells
    struct TableInfo { int first = -1, last = -1, sepRow = -1; QString indent; QList<int> widths; };
    TableInfo formatTable(const QTextBlock &inside);
    int cellAt(const QString &row, int col) const;
    QTextCursor cellCursor(const TableInfo &t, int row, int cell) const;
    void tableTab(bool back);
    void tableEnter();
    void tableLeave();
    bool tableArrow(int key); // Ctrl+arrows: cell navigation; false = not applicable here
    void tableDeleteRow(bool keepColumn);
    QTextCursor cursorInCell(const QTextBlock &b, int cell) const;
    QString emptyRow(const TableInfo &t) const;

    // painting behind / over the text
    void paintMarkdownUnder(QPainter &p);
    void paintMarkdownOver(QPainter &p);
    QColor m_codeBg, m_ruleColor;

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
