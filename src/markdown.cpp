#include "markdown.h"
#include "theme.h"

#include <QFontMetricsF>
#include <QTextDocument>

namespace {

const QRegularExpression kHeading(R"(^(#{1,6})(\s+)(.*)$)");
const QRegularExpression kRule(R"(^\s*([-*_])(\s*\1){2,}\s*$)");
const QRegularExpression kQuote(R"(^(\s*>+\s?))");
const QRegularExpression kCode(R"(`([^`]+)`)");
const QRegularExpression kBold(R"((\*\*|__)(?=\S)(.+?)(?<=\S)\1)");
const QRegularExpression kItalic(R"((?<![*\w])([*_])(?=[^\s*_])(.+?)(?<=[^\s*_])\1(?![*\w]))");
const QRegularExpression kStrike(R"(~~(?=\S)(.+?)(?<=\S)~~)");

const double kHeadingScale[] = {1.0, 1.6, 1.4, 1.25, 1.12, 1.05, 1.0};

// ---- languages for fenced code blocks

struct Lang {
    QStringList names;
    QSet<QString> keywords, types;
    QString lineComment;
    bool slashStar = false;   // /* ... */
    bool backtick = false;    // `strings`
    bool caseInsensitive = false;
    bool keysBeforeColon = false; // json / yaml keys
};

QSet<QString> words(const char *s)
{
    QSet<QString> out;
    for (const auto &w : QString::fromLatin1(s).split(' ', Qt::SkipEmptyParts))
        out.insert(w);
    return out;
}

const QList<Lang> &langs()
{
    static const QList<Lang> l = {
        {{"c", "cpp", "c++", "cc", "cxx", "h", "hpp", "hh"},
         words("if else for while do switch case default break continue return goto struct class union enum "
               "typedef namespace using template typename public private protected virtual override final const "
               "constexpr consteval static inline extern volatile mutable new delete this sizeof alignof auto true "
               "false nullptr operator friend explicit noexcept try catch throw co_await co_return co_yield "
               "static_cast dynamic_cast reinterpret_cast const_cast #include #define #pragma #ifdef #ifndef #endif "
               "#if #else"),
         words("void int char short long float double bool unsigned signed size_t ssize_t uint8_t uint16_t "
               "uint32_t uint64_t int8_t int16_t int32_t int64_t std string vector map set unique_ptr shared_ptr "
               "QString QList QWidget QObject"),
         "//", true},
        {{"python", "py", "python3"},
         words("def class if elif else for while return import from as with try except finally raise pass break "
               "continue lambda yield in is not and or None True False global nonlocal async await del assert match "
               "case"),
         words("int str float bool list dict set tuple bytes object self cls"),
         "#"},
        {{"javascript", "js", "jsx", "mjs", "typescript", "ts", "tsx", "node"},
         words("function const let var if else for while do return class extends new this super import export from "
               "default async await try catch finally throw switch case break continue typeof instanceof in of null "
               "undefined true false yield static get set interface type enum implements readonly as"),
         words("string number boolean any void never unknown object Promise Array Object Map Set Record"),
         "//", true, true},
        {{"go", "golang"},
         words("func package import var const type struct interface map chan go defer if else for range return "
               "switch case default break continue select fallthrough goto nil true false iota"),
         words("int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 uintptr float32 float64 string bool "
               "byte rune error any complex64 complex128"),
         "//", true, true},
        {{"rust", "rs"},
         words("fn let mut pub struct enum impl trait use mod crate self Self super if else match for while loop "
               "return break continue as where move ref async await dyn true false const static unsafe extern type "
               "in"),
         words("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result "
               "Box Rc Arc HashMap"),
         "//", true},
        {{"java", "kotlin", "kt", "scala"},
         words("class interface public private protected static final abstract void new return if else for while "
               "do switch case default break continue try catch finally throw throws extends implements import "
               "package this super null true false fun val var when object enum record sealed override"),
         words("int long short byte double float boolean char String Integer Long Double Boolean List Map Set "
               "Object"),
         "//", true},
        {{"sh", "bash", "zsh", "fish", "shell", "console", "terminal"},
         words("if then else elif fi for in do done while until case esac function return export local readonly "
               "exit set unset end source alias echo cd sudo"),
         {}, "#"},
        {{"sql", "postgres", "postgresql", "mysql", "sqlite"},
         words("select from where insert into values update set delete create table drop alter join left right "
               "inner outer full on group by order having limit offset as and or not null is in like distinct "
               "union all primary key foreign references index default exists between case when then else end "
               "with returning asc desc"),
         words("int integer bigint smallint text varchar char boolean bool date timestamp numeric decimal real "
               "serial uuid json jsonb"),
         "--", true, false, true},
        {{"lua"},
         words("function local end if then else elseif for in do while repeat until return break nil true false "
               "and or not goto"),
         {}, "--"},
        {{"json", "jsonc"}, words("true false null"), {}, "", false, false, false, true},
        {{"yaml", "yml", "toml", "ini", "conf"}, words("true false null yes no on off"), {}, "#", false, false,
         false, true},
    };
    return l;
}

int langIndex(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n.isEmpty())
        return -1;
    const auto &l = langs();
    for (int i = 0; i < l.size(); ++i)
        if (l[i].names.contains(n))
            return i;
    return -1;
}

} // namespace

// ---------------------------------------------------------------- shared regexes

const QRegularExpression &MarkdownHighlighter::listRe()
{
    static const QRegularExpression re(R"(^(\s*)([-*+]|\d+[.)])(\s+)(\[[ xX]\]\s+)?)");
    return re;
}

const QRegularExpression &MarkdownHighlighter::taskRe()
{
    static const QRegularExpression re(R"(^(\s*[-*+]\s+)\[([ xX])\])");
    return re;
}

const QRegularExpression &MarkdownHighlighter::linkRe()
{
    static const QRegularExpression re(R"(\[([^\]]+)\]\(([^)\s]+)\))");
    return re;
}

const QRegularExpression &MarkdownHighlighter::urlRe()
{
    static const QRegularExpression re(R"(https?://[^\s<>()\[\]]+)");
    return re;
}

const QRegularExpression &MarkdownHighlighter::fenceRe()
{
    static const QRegularExpression re(R"(^(\s*)(```|~~~)\s*([\w+#.-]*)\s*$)");
    return re;
}

const QRegularExpression &MarkdownHighlighter::tableRowRe()
{
    static const QRegularExpression re(R"(^\s*\|)");
    return re;
}

const QRegularExpression &MarkdownHighlighter::tableSepRe()
{
    static const QRegularExpression re(R"(^\s*\|?(\s*:?-+:?\s*\|)+\s*(:?-+:?\s*)?$)");
    return re;
}

QList<int> MarkdownHighlighter::pipePositions(const QString &row)
{
    QList<int> out;
    for (int i = 0; i < row.size(); ++i) {
        if (row[i] == '\\') {
            ++i;
            continue;
        }
        if (row[i] == '|')
            out << i;
    }
    return out;
}

QStringList MarkdownHighlighter::tableCells(const QString &row)
{
    const QList<int> p = pipePositions(row);
    QStringList cells;
    if (p.isEmpty())
        return {row.trimmed()};
    // text between consecutive pipes; a trailing segment counts only when there's no closing pipe
    for (int i = 0; i + 1 < p.size(); ++i)
        cells << row.mid(p[i] + 1, p[i + 1] - p[i] - 1).trimmed();
    const QString tail = row.mid(p.last() + 1).trimmed();
    if (!tail.isEmpty())
        cells << tail;
    return cells;
}

// ---------------------------------------------------------------- highlighter

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}

void MarkdownHighlighter::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    rehighlight();
}

void MarkdownHighlighter::setTheme(const Theme &t)
{
    m_h[0] = t.text;
    m_h[1] = t.h1;
    m_h[2] = t.h2;
    m_h[3] = t.h3;
    m_h[4] = t.h4;
    m_h[5] = t.h5;
    m_h[6] = t.h6;
    m_link = t.link;
    m_code = t.code;
    m_codeBg = t.surface;
    m_muted = t.muted;
    m_accent = t.accent;
    m_text = t.text;
    m_strong = t.textStrong;
    m_kw = t.codeKeyword;
    m_type = t.codeType;
    m_str = t.codeString;
    m_num = t.codeNumber;
    m_comment = t.codeComment;
    if (m_enabled)
        rehighlight();
}

void MarkdownHighlighter::setBaseFont(const QFont &f)
{
    if (f == m_font)
        return;
    m_font = f;
    if (m_enabled)
        rehighlight();
}

void MarkdownHighlighter::setActiveBlocks(const QSet<int> &blocks)
{
    if (blocks == m_active)
        return;
    const QSet<int> changed = (blocks - m_active) + (m_active - blocks);
    m_active = blocks;
    if (!m_enabled)
        return;
    for (int n : changed) {
        const QTextBlock b = document()->findBlockByNumber(n);
        if (b.isValid())
            rehighlightBlock(b);
    }
}

void MarkdownHighlighter::merge(int start, int len, const QTextCharFormat &f)
{
    for (int i = start; i < start + len; ++i) {
        QTextCharFormat cur = format(i);
        cur.merge(f);
        setFormat(i, 1, cur);
    }
}

void MarkdownHighlighter::markup(int start, int len, bool active)
{
    if (m_keepWidth) {
        conceal(start, len, active);
        return;
    }
    QTextCharFormat f;
    if (active) {
        f.setForeground(m_muted);
    } else {
        f.setForeground(Qt::transparent);
        f.setFontPointSize(1);
    }
    merge(start, len, f);
}

void MarkdownHighlighter::conceal(int start, int len, bool active)
{
    QTextCharFormat f;
    f.setForeground(active ? m_muted : QColor(Qt::transparent));
    merge(start, len, f);
}

void MarkdownHighlighter::markPair(int open, int openLen, int close, int closeLen, bool active)
{
    if (!m_keepWidth || active) {
        markup(open, openLen, active);
        markup(close, closeLen, active);
        return;
    }
    m_keepWidth = false;
    markup(open, openLen, false); // collapsed
    m_keepWidth = true;
    const qreal cw = QFontMetricsF(m_font).horizontalAdvance(QLatin1Char(' '));
    QTextCharFormat f;
    f.setForeground(Qt::transparent);
    f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    f.setFontLetterSpacing(openLen * cw / closeLen); // spread the opening width over the closing chars
    merge(close, closeLen, f);
}

int MarkdownHighlighter::highlightCode(const QString &text, int li, bool inComment)
{
    QTextCharFormat base;
    base.setFontFamilies({QStringLiteral("monospace")});
    base.setForeground(li < 0 ? m_code : m_text);
    merge(0, text.size(), base);
    if (li < 0)
        return 0;

    const Lang &lang = langs()[li];
    auto color = [this](int s, int l, const QColor &c, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (italic)
            f.setFontItalic(true);
        merge(s, l, f);
    };
    const int n = text.size();
    int i = 0;
    if (inComment) {
        const int end = text.indexOf("*/");
        if (end < 0) {
            color(0, n, m_comment, true);
            return 1;
        }
        color(0, end + 2, m_comment, true);
        i = end + 2;
    }
    while (i < n) {
        const QChar c = text[i];
        if (lang.slashStar && text.mid(i, 2) == "/*") {
            const int end = text.indexOf("*/", i + 2);
            if (end < 0) {
                color(i, n - i, m_comment, true);
                return 1;
            }
            color(i, end + 2 - i, m_comment, true);
            i = end + 2;
            continue;
        }
        if (!lang.lineComment.isEmpty() && text.mid(i, lang.lineComment.size()) == lang.lineComment) {
            color(i, n - i, m_comment, true);
            break;
        }
        if (c == '"' || c == '\'' || (lang.backtick && c == '`')) {
            int j = i + 1;
            while (j < n && text[j] != c)
                j += text[j] == '\\' ? 2 : 1;
            if (j >= n) { // unterminated: only the quote (rust lifetimes, apostrophes...)
                ++i;
                continue;
            }
            int k = j + 1;
            while (k < n && text[k].isSpace())
                ++k;
            const bool key = lang.keysBeforeColon && k < n && text[k] == ':';
            color(i, j + 1 - i, key ? m_type : m_str);
            i = j + 1;
            continue;
        }
        if (c.isDigit() && (i == 0 || (!text[i - 1].isLetterOrNumber() && text[i - 1] != '_'))) {
            int j = i;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == '.' || text[j] == '_'))
                ++j;
            color(i, j - i, m_num);
            i = j;
            continue;
        }
        if (c.isLetter() || c == '_' || c == '#') {
            int j = i + 1;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == '_'))
                ++j;
            QString w = text.mid(i, j - i);
            if (lang.caseInsensitive)
                w = w.toLower();
            if (lang.keywords.contains(w)) {
                QTextCharFormat f;
                f.setForeground(m_kw);
                f.setFontWeight(QFont::Bold);
                merge(i, j - i, f);
            } else if (lang.types.contains(w)) {
                color(i, j - i, m_type);
            } else if (lang.keysBeforeColon && li == langs().size() - 1) { // yaml/toml key
                int k = j;
                while (k < n && (text[k].isLetterOrNumber() || text[k] == '-' || text[k] == '.' || text[k] == '_'))
                    ++k;
                if (k < n && (text[k] == ':' || text[k] == '=') && text.left(i).trimmed().isEmpty())
                    color(i, k - i, m_type);
                j = qMax(j, k);
            }
            i = j;
            continue;
        }
        ++i;
    }
    return 0;
}

void MarkdownHighlighter::highlightTable(const QString &text, bool active)
{
    const bool sep = tableSepRe().match(text).hasMatch();
    if (sep) {
        setCurrentBlockState(TableSep);
        conceal(0, text.size(), active); // invisible, same height: nothing moves when the cursor passes
        return;
    }
    const QTextBlock next = currentBlock().next();
    const bool header = next.isValid() && tableSepRe().match(next.text()).hasMatch();
    setCurrentBlockState(header ? TableHeader : TableRow);
    if (header) {
        QTextCharFormat h;
        h.setFontWeight(QFont::Bold);
        h.setForeground(m_strong);
        merge(0, text.size(), h);
    }
    for (int p : pipePositions(text))
        conceal(p, 1, active);
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    const int prev = qMax(0, previousBlockState());
    if (!m_enabled) {
        setCurrentBlockState(Normal);
        return;
    }
    const bool active = m_active.contains(currentBlock().blockNumber());
    const auto fence = fenceRe().match(text);

    // inside a fenced code block
    if (inFence(prev)) {
        const int li = (prev - InFence) / 2 - 1; // -1: unknown language
        const bool inComment = (prev - InFence) % 2;
        if (fence.hasMatch() && fence.capturedLength(3) == 0) { // closing fence
            markup(0, text.size(), active);
            setCurrentBlockState(FenceClose);
            return;
        }
        const int comment = highlightCode(text, li, inComment);
        setCurrentBlockState(InFence + (li + 1) * 2 + comment);
        return;
    }
    // opening fence: ```lang
    if (fence.hasMatch()) {
        const int li = langIndex(fence.captured(3));
        markup(fence.capturedStart(2), fence.capturedLength(2), active);
        QTextCharFormat name;
        name.setForeground(m_muted);
        name.setFontItalic(true);
        merge(fence.capturedStart(3), fence.capturedLength(3), name);
        setCurrentBlockState(InFence + (li + 1) * 2);
        return;
    }
    setCurrentBlockState(Normal);
    m_keepWidth = false;

    int contentStart = 0;

    if (const auto m = kHeading.match(text); m.hasMatch()) {
        const int level = m.capturedLength(1);
        QTextCharFormat h;
        h.setForeground(m_h[level]);
        h.setFontWeight(level <= 3 ? QFont::Bold : QFont::DemiBold);
        if (m_font.pointSizeF() > 0)
            h.setFontPointSize(m_font.pointSizeF() * kHeadingScale[level]);
        merge(0, text.size(), h);
        markup(0, m.capturedLength(1) + m.capturedLength(2), active);
        contentStart = m.capturedStart(3);
        setCurrentBlockState(Heading + level);
    } else if (kRule.match(text).hasMatch()) {
        conceal(0, text.size(), active); // the editor draws the line across the whole width
        setCurrentBlockState(Rule);
        return;
    } else if (tableRowRe().match(text).hasMatch()) {
        highlightTable(text, active);
        if (currentBlockState() == TableSep)
            return;
        m_keepWidth = true;
    } else if (const auto q = kQuote.match(text); q.hasMatch()) {
        QTextCharFormat quote;
        quote.setForeground(m_muted);
        quote.setFontItalic(true);
        merge(0, text.size(), quote);
        QTextCharFormat bar;
        bar.setForeground(m_accent);
        merge(q.capturedStart(1), q.capturedLength(1), bar);
        contentStart = q.capturedEnd(1);
    } else if (const auto l = listRe().match(text); l.hasMatch()) {
        QTextCharFormat mark;
        mark.setForeground(m_accent);
        mark.setFontWeight(QFont::Bold);
        merge(l.capturedStart(2), l.capturedLength(2), mark);
        if (l.capturedLength(4) > 0) { // task box
            merge(l.capturedStart(4), 3, mark);
            const QChar state = text.at(l.capturedStart(4) + 1);
            if (state == 'x' || state == 'X') {
                QTextCharFormat done;
                done.setForeground(m_muted);
                done.setFontStrikeOut(true);
                merge(l.capturedEnd(4), text.size() - l.capturedEnd(4), done);
            }
        }
        contentStart = l.capturedEnd(0);
    }

    // inline spans (code first: nothing inside it is markdown)
    QList<QPair<int, int>> codeSpans;
    for (auto it = kCode.globalMatch(text, contentStart); it.hasNext();) {
        const auto m = it.next();
        QTextCharFormat c;
        c.setForeground(m_code);
        c.setBackground(m_codeBg);
        c.setFontFamilies({QStringLiteral("monospace")});
        merge(m.capturedStart(1), m.capturedLength(1), c);
        markPair(m.capturedStart(0), 1, m.capturedEnd(0) - 1, 1, active);
        codeSpans.append({m.capturedStart(0), m.capturedEnd(0)});
    }
    auto inCode = [&](int pos) {
        for (const auto &s : codeSpans)
            if (pos >= s.first && pos < s.second)
                return true;
        return false;
    };

    auto span = [&](const QRegularExpression &re, int markLen, const QTextCharFormat &f) {
        for (auto it = re.globalMatch(text, contentStart); it.hasNext();) {
            const auto m = it.next();
            if (inCode(m.capturedStart(0)))
                continue;
            merge(m.capturedStart(0) + markLen, m.capturedLength(0) - 2 * markLen, f);
            markPair(m.capturedStart(0), markLen, m.capturedEnd(0) - markLen, markLen, active);
        }
    };
    QTextCharFormat bold;
    bold.setFontWeight(QFont::Bold);
    span(kBold, 2, bold);
    QTextCharFormat italic;
    italic.setFontItalic(true);
    span(kItalic, 1, italic);
    QTextCharFormat strike;
    strike.setFontStrikeOut(true);
    span(kStrike, 2, strike);

    // links: [text](url) shows only the text; bare urls are underlined
    QList<QPair<int, int>> linkSpans;
    for (auto it = linkRe().globalMatch(text, contentStart); it.hasNext();) {
        const auto m = it.next();
        if (inCode(m.capturedStart(0)))
            continue;
        QTextCharFormat l;
        l.setForeground(m_link);
        l.setFontUnderline(true);
        merge(m.capturedStart(1), m.capturedLength(1), l);
        markPair(m.capturedStart(0), 1, m.capturedEnd(1), m.capturedEnd(0) - m.capturedEnd(1), active); // [ ](url)
        linkSpans.append({m.capturedStart(0), m.capturedEnd(0)});
    }
    for (auto it = urlRe().globalMatch(text, contentStart); it.hasNext();) {
        const auto m = it.next();
        bool inside = inCode(m.capturedStart(0));
        for (const auto &s : linkSpans)
            inside = inside || (m.capturedStart(0) >= s.first && m.capturedStart(0) < s.second);
        if (inside)
            continue;
        QTextCharFormat u;
        u.setForeground(m_link);
        u.setFontUnderline(true);
        merge(m.capturedStart(0), m.capturedLength(0), u);
    }
}
