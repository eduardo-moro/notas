#pragma once
#include <QColor>
#include <QFont>
#include <QRegularExpression>
#include <QSet>
#include <QSyntaxHighlighter>

struct Theme;

// Live-preview markdown (Obsidian style): the text stays plain markdown, only its look changes.
// Markup characters are hidden on lines without a cursor and shown on the lines being edited.
// Each block's userState tells the editor what to paint behind/over it (code background,
// table grid, rules, heading underline).
class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    enum Kind {
        Normal = 0,
        Rule = 1,
        TableRow = 2,
        TableSep = 3,
        TableHeader = 4,
        Heading = 10, // + level (11..16)
        FenceClose = 50,
        InFence = 100, // 100 + lang * 2 + (inside /* comment */)
    };
    static bool inFence(int state) { return state >= InFence; }
    static bool isCode(int state) { return state >= InFence || state == FenceClose; }
    static bool isTable(int state) { return state >= TableRow && state <= TableHeader; }
    static int headingLevel(int state) { return state > Heading && state <= Heading + 6 ? state - Heading : 0; }

    explicit MarkdownHighlighter(QTextDocument *doc);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }
    void setTheme(const Theme &t);
    void setBaseFont(const QFont &f);
    // blocks that hold a cursor: their markers stay visible
    void setActiveBlocks(const QSet<int> &blocks);

    // helpers shared with the editor
    static const QRegularExpression &listRe();     // indent, marker, space, optional task box
    static const QRegularExpression &taskRe();     // indent + marker, box state
    static const QRegularExpression &linkRe();     // [text](url)
    static const QRegularExpression &urlRe();      // bare http(s) url
    static const QRegularExpression &fenceRe();    // indent, ``` or ~~~, language
    static const QRegularExpression &tableRowRe(); // a line starting with |
    static const QRegularExpression &tableSepRe(); // |---|:---:|
    static QList<int> pipePositions(const QString &row); // unescaped | characters
    static QStringList tableCells(const QString &row);

protected:
    void highlightBlock(const QString &text) override;

private:
    void merge(int start, int len, const QTextCharFormat &f);
    void markup(int start, int len, bool active);  // hidden and collapsed off the cursor line
    void conceal(int start, int len, bool active); // hidden but keeps its width (tables, rules)
    // opening + closing markup of a span. In table rows the opening one collapses and the closing
    // one absorbs its width: the word starts at the column start and the cell keeps its width.
    void markPair(int open, int openLen, int close, int closeLen, bool active);
    int highlightCode(const QString &text, int lang, bool inComment); // returns the new comment state
    void highlightTable(const QString &text, bool active);

    bool m_enabled = false;
    bool m_keepWidth = false; // table rows: hidden markup must not shift the columns
    QSet<int> m_active;
    QFont m_font;
    QColor m_h[7];
    QColor m_link, m_code, m_codeBg, m_muted, m_accent, m_text, m_strong;
    QColor m_kw, m_type, m_str, m_num, m_comment;
};
