#include "editor.h"
#include "markdown.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QUrl>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QMenu>
#include <QClipboard>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QTextDocumentFragment>
#include <QTextLayout>
#include <algorithm>

static const char *kLineMime = "application/x-notas-line";

Editor::Editor(QWidget *parent) : QPlainTextEdit(parent)
{
    setFrameShape(QFrame::NoFrame);
    setCursorWidth(0); // cursors are painted by us (accent color + multi-cursor)
    setTabChangesFocus(false);
    saveTimer.setSingleShot(true);

    const int flash = QApplication::cursorFlashTime();
    m_blink.setInterval(flash > 0 ? flash / 2 : 0);
    connect(&m_blink, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        viewport()->update();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &Editor::resetBlink);

    setUndoRedoEnabled(false); // replaced by our own, persistable history
    connect(document(), &QTextDocument::contentsChange, this, [this](int, int, int) { onContentsChange(); });

    m_md = new MarkdownHighlighter(document());
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &Editor::updateActiveBlocks);
}

void Editor::updateActiveBlocks()
{
    if (!m_md->isEnabled())
        return;
    QSet<int> blocks;
    for (const auto &c : allCursors()) {
        const int a = document()->findBlock(c.selectionStart()).blockNumber();
        const int b = document()->findBlock(c.selectionEnd()).blockNumber();
        for (int n = a; n <= b && blocks.size() < 500; ++n)
            blocks.insert(n);
    }
    m_formatOnly = true;
    m_md->setActiveBlocks(blocks);
    m_formatOnly = false;
}

void Editor::applySettings(const AppSettings &s)
{
    QFont f(s.fontFamily);
    f.setPointSize(s.fontSize);
    f.setStyleHint(QFont::Monospace);
    setFont(f);
    m_tabWidth = s.tabWidth;
    m_useSpaces = s.useSpaces;
    m_autoIndent = s.autoIndent;
    setTabStopDistance(QFontMetricsF(f).horizontalAdvance(QLatin1Char(' ')) * s.tabWidth);
    document()->setDocumentMargin(s.padV);
    const int h = qMax(0, s.padH - s.padV);
    setViewportMargins(h, 0, h, 0);
    setLineWrapMode(s.wordWrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    saveTimer.setInterval(s.autosaveMs);
    m_formatOnly = true;
    m_md->setBaseFont(f);
    m_md->setEnabled(s.markdown);
    m_formatOnly = false;
    updateActiveBlocks();
}

void Editor::applyTheme(const Theme &t)
{
    m_cursorColor = t.cursor;
    m_extraSel = t.selection;
    m_searchColor = t.search;
    m_codeBg = t.card;
    m_ruleColor = t.surface2;
    m_formatOnly = true;
    m_md->setTheme(t);
    m_formatOnly = false;
    refreshExtraSelections();
    viewport()->update();
}

// ------------------------------------------------------------ cursor plumbing

QList<QTextCursor> Editor::allCursors() const
{
    QList<QTextCursor> l;
    l << textCursor();
    l += m_extra;
    return l;
}

void Editor::setAllCursors(QList<QTextCursor> cs, int primary)
{
    if (cs.isEmpty())
        return;
    primary = qBound(0, primary, int(cs.size()) - 1);
    const int primPos = cs[primary].position();

    std::sort(cs.begin(), cs.end(), [](const QTextCursor &a, const QTextCursor &b) {
        if (a.selectionStart() != b.selectionStart())
            return a.selectionStart() < b.selectionStart();
        return a.selectionEnd() < b.selectionEnd();
    });

    // merge duplicates / overlapping selections
    QList<QTextCursor> out;
    for (const auto &c : cs) {
        if (!out.isEmpty()) {
            QTextCursor &l = out.last();
            if (c.selectionStart() == l.selectionStart() && c.selectionEnd() == l.selectionEnd())
                continue;
            if (!l.hasSelection() && c.selectionStart() == l.selectionStart()) {
                l = c;
                continue;
            }
            if (c.selectionStart() < l.selectionEnd()) {
                const int s = l.selectionStart();
                const int e = qMax(l.selectionEnd(), c.selectionEnd());
                const bool forward = l.position() >= l.anchor();
                QTextCursor m(document());
                m.setPosition(forward ? s : e);
                m.setPosition(forward ? e : s, QTextCursor::KeepAnchor);
                l = m;
                continue;
            }
        }
        out.append(c);
    }

    int pi = 0;
    for (int i = 0; i < out.size(); ++i) {
        if (out[i].selectionStart() <= primPos && primPos <= out[i].selectionEnd()) {
            pi = i;
            break;
        }
    }
    QTextCursor p = out.takeAt(pi);
    m_extra = out;
    setTextCursor(p);
    refreshExtraSelections();
    updateActiveBlocks();
    resetBlink();
}

void Editor::clearExtraCursors()
{
    if (m_extra.isEmpty())
        return;
    m_extra.clear();
    refreshExtraSelections();
    viewport()->update();
}

void Editor::setSearchHighlights(const QList<QTextCursor> &hits)
{
    m_search = hits;
    refreshExtraSelections();
}

void Editor::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> sels;
    for (const auto &c : m_search) {
        QTextEdit::ExtraSelection s;
        s.cursor = c;
        s.format.setBackground(m_searchColor);
        sels << s;
    }
    for (const auto &c : m_extra) {
        if (!c.hasSelection())
            continue;
        QTextEdit::ExtraSelection s;
        s.cursor = c;
        s.format.setBackground(m_extraSel);
        sels << s;
    }
    setExtraSelections(sels);
}

void Editor::resetBlink()
{
    m_blinkOn = true;
    if (m_blink.interval() > 0 && hasFocus())
        m_blink.start();
    viewport()->update();
}

void Editor::edit(const std::function<void(QTextCursor &)> &fn)
{
    auto all = allCursors();
    QTextCursor block(document());
    block.beginEditBlock();
    for (auto &c : all)
        fn(c);
    block.endEditBlock();
    setAllCursors(all, 0);
}

void Editor::move(QTextCursor::MoveOperation op, bool shift)
{
    auto all = allCursors();
    for (auto &c : all) {
        if (!shift && c.hasSelection() && (op == QTextCursor::Left || op == QTextCursor::Right)) {
            c.setPosition(op == QTextCursor::Left ? c.selectionStart() : c.selectionEnd());
            continue;
        }
        c.movePosition(op, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    }
    setAllCursors(all, 0);
}

QString Editor::indentUnit(int column) const
{
    if (!m_useSpaces)
        return QStringLiteral("\t");
    return QString(m_tabWidth - column % m_tabWidth, QLatin1Char(' '));
}

QString Editor::leadingWs(const QTextBlock &b, int upTo) const
{
    const QString t = b.text();
    int i = 0;
    while (i < t.size() && (upTo < 0 || i < upTo) && (t[i] == ' ' || t[i] == '\t'))
        ++i;
    return t.left(i);
}

int Editor::posAt(int blockNumber, int column) const
{
    QTextBlock b = document()->findBlockByNumber(qBound(0, blockNumber, document()->blockCount() - 1));
    return b.position() + qBound(0, column, b.length() - 1);
}

// Contiguous block ranges touched by any cursor (selection ending at column 0 excludes that line).
QList<Editor::Group> Editor::blockGroups() const
{
    QList<Group> gs;
    for (const auto &c : allCursors()) {
        const QTextBlock sb = document()->findBlock(c.selectionStart());
        const QTextBlock eb = document()->findBlock(c.selectionEnd());
        int s = sb.blockNumber(), e = eb.blockNumber();
        if (c.hasSelection() && e > s && c.selectionEnd() == eb.position())
            --e;
        gs.append({s, e});
    }
    std::sort(gs.begin(), gs.end(), [](const Group &a, const Group &b) { return a.first < b.first; });
    QList<Group> merged;
    for (const auto &g : gs) {
        if (!merged.isEmpty() && g.first <= merged.last().last + 1)
            merged.last().last = qMax(merged.last().last, g.last);
        else
            merged.append(g);
    }
    return merged;
}

// ------------------------------------------------------------ commands

void Editor::addCursorVertical(int dir)
{
    auto all = allCursors();
    QTextCursor ref = all[0];
    for (const auto &c : all)
        if (dir < 0 ? c.position() < ref.position() : c.position() > ref.position())
            ref = c;
    QTextCursor n(ref);
    n.clearSelection();
    n.movePosition(dir < 0 ? QTextCursor::Up : QTextCursor::Down);
    if (cursorRect(n).top() == cursorRect(ref).top())
        return; // already at first/last line
    all.append(n);
    setAllCursors(all, all.size() - 1);
}

void Editor::selectNextOccurrence()
{
    QTextCursor p = textCursor();
    auto all = allCursors();
    if (!p.hasSelection()) {
        p.select(QTextCursor::WordUnderCursor);
        if (p.hasSelection()) {
            all[0] = p;
            setAllCursors(all, 0);
        }
        return;
    }
    const QString needle = p.selectedText();
    const auto flags = QTextDocument::FindCaseSensitively;
    auto exists = [&](const QTextCursor &f) {
        for (const auto &c : all)
            if (c.selectionStart() == f.selectionStart() && c.selectionEnd() == f.selectionEnd())
                return true;
        return false;
    };
    int pos = p.selectionEnd();
    QTextCursor found;
    for (int i = 0; i <= all.size(); ++i) {
        QTextCursor f = document()->find(needle, pos, flags);
        if (f.isNull())
            f = document()->find(needle, 0, flags);
        if (f.isNull())
            return;
        if (!exists(f)) {
            found = f;
            break;
        }
        pos = f.selectionEnd();
    }
    if (found.isNull())
        return;
    all.append(found);
    setAllCursors(all, all.size() - 1);
}

void Editor::selectAllOccurrences(const QString &needle, QTextDocument::FindFlags flags)
{
    QString n = needle;
    if (n.isEmpty()) {
        QTextCursor p = textCursor();
        if (!p.hasSelection())
            p.select(QTextCursor::WordUnderCursor);
        n = p.selectedText();
    }
    if (n.isEmpty())
        return;
    QList<QTextCursor> found;
    QTextCursor f = document()->find(n, 0, flags);
    while (!f.isNull() && found.size() < 10000) {
        found << f;
        f = document()->find(n, f.selectionEnd(), flags);
    }
    if (found.isEmpty())
        return;
    const int here = textCursor().selectionStart();
    int primary = 0;
    for (int i = 0; i < found.size(); ++i)
        if (found[i].selectionEnd() >= here) {
            primary = i;
            break;
        }
    setAllCursors(found, primary);
}

void Editor::newline()
{
    edit([this](QTextCursor &c) {
        if (m_md->isEnabled() && markdownNewline(c))
            return;
        QString ws;
        if (m_autoIndent) {
            const QTextBlock b = document()->findBlock(c.selectionStart());
            ws = leadingWs(b, c.selectionStart() - b.position());
        }
        c.insertText("\n" + ws);
    });
}

void Editor::insertLine(bool above)
{
    edit([this, above](QTextCursor &c) {
        const QString ws = m_autoIndent ? leadingWs(c.block()) : QString();
        c.clearSelection();
        if (above) {
            c.movePosition(QTextCursor::StartOfBlock);
            c.insertText(ws + "\n");
            c.movePosition(QTextCursor::PreviousBlock);
            c.movePosition(QTextCursor::EndOfBlock);
        } else {
            c.movePosition(QTextCursor::EndOfBlock);
            c.insertText("\n" + ws);
        }
    });
}

void Editor::smartHome(bool shift)
{
    auto all = allCursors();
    for (auto &c : all) {
        const QTextBlock b = c.block();
        const int col = c.positionInBlock();
        int lineStart = 0;
        if (QTextLayout *lay = b.layout(); lay && lay->lineCount() > 0) {
            const QTextLine l = lay->lineForTextPosition(col);
            if (l.isValid())
                lineStart = l.textStart();
        }
        int target;
        if (lineStart > 0) {
            target = lineStart; // wrapped line: go to its visual start
        } else {
            const int first = leadingWs(b).size();
            target = col == first ? 0 : first;
        }
        c.setPosition(b.position() + target, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    }
    setAllCursors(all, 0);
}

void Editor::tabKey()
{
    for (const auto &c : allCursors()) {
        if (c.hasSelection() &&
            document()->findBlock(c.selectionStart()) != document()->findBlock(c.selectionEnd())) {
            indentLines(false);
            return;
        }
    }
    edit([this](QTextCursor &c) {
        const int col = c.selectionStart() - document()->findBlock(c.selectionStart()).position();
        c.insertText(indentUnit(col));
    });
}

void Editor::indentLines(bool outdent)
{
    const auto groups = blockGroups();
    auto all = allCursors();
    QList<bool> startAtCol0;
    for (const auto &c : all)
        startAtCol0 << (c.hasSelection() &&
                        c.selectionStart() == document()->findBlock(c.selectionStart()).position());

    QTextCursor ed(document());
    ed.beginEditBlock();
    for (const auto &g : groups) {
        for (int bn = g.first; bn <= g.last; ++bn) {
            const QTextBlock b = document()->findBlockByNumber(bn);
            QTextCursor t(b);
            if (!outdent) {
                if (b.text().isEmpty() && g.first != g.last)
                    continue;
                t.insertText(m_useSpaces ? QString(m_tabWidth, QLatin1Char(' ')) : QStringLiteral("\t"));
            } else {
                const QString txt = b.text();
                int n = 0;
                if (txt.startsWith('\t'))
                    n = 1;
                else
                    while (n < m_tabWidth && n < txt.size() && txt[n] == ' ')
                        ++n;
                if (n) {
                    t.setPosition(b.position() + n, QTextCursor::KeepAnchor);
                    t.removeSelectedText();
                }
            }
        }
    }
    ed.endEditBlock();

    // keep selections that started at column 0 anchored there
    for (int i = 0; i < all.size(); ++i) {
        if (!startAtCol0[i])
            continue;
        QTextCursor &c = all[i];
        const int bs = document()->findBlock(c.selectionStart()).position();
        if (c.anchor() <= c.position()) {
            const int p = c.position();
            c.setPosition(bs);
            c.setPosition(p, QTextCursor::KeepAnchor);
        } else {
            const int a = c.anchor();
            c.setPosition(a);
            c.setPosition(bs, QTextCursor::KeepAnchor);
        }
    }
    setAllCursors(all, 0);
}

namespace {
struct CursorPos { int ab, ac, pb, pc; };
}

void Editor::moveLines(int dir)
{
    const auto groups = blockGroups();
    const int last = document()->blockCount() - 1;
    for (const auto &g : groups)
        if ((dir < 0 && g.first == 0) || (dir > 0 && g.last == last))
            return;

    const auto all = allCursors();
    QList<CursorPos> ps;
    for (const auto &c : all) {
        const QTextBlock a = document()->findBlock(c.anchor());
        const QTextBlock p = document()->findBlock(c.position());
        ps.append({a.blockNumber(), c.anchor() - a.position(), p.blockNumber(), c.position() - p.position()});
    }

    QTextCursor ed(document());
    ed.beginEditBlock();
    if (dir < 0) {
        for (const auto &g : groups) {
            const QTextBlock prev = document()->findBlockByNumber(g.first - 1);
            const QString t = prev.text();
            QTextCursor r(document());
            r.setPosition(prev.position());
            r.setPosition(document()->findBlockByNumber(g.first).position(), QTextCursor::KeepAnchor);
            r.removeSelectedText();
            const QTextBlock endB = document()->findBlockByNumber(g.last - 1);
            QTextCursor ins(document());
            ins.setPosition(endB.position() + endB.length() - 1);
            ins.insertText("\n" + t);
        }
    } else {
        for (int i = groups.size() - 1; i >= 0; --i) {
            const auto &g = groups[i];
            const QTextBlock next = document()->findBlockByNumber(g.last + 1);
            const QString t = next.text();
            const QTextBlock endB = document()->findBlockByNumber(g.last);
            QTextCursor r(document());
            r.setPosition(endB.position() + endB.length() - 1);
            r.setPosition(next.position() + next.length() - 1, QTextCursor::KeepAnchor);
            r.removeSelectedText();
            QTextCursor ins(document());
            ins.setPosition(document()->findBlockByNumber(g.first).position());
            ins.insertText(t + "\n");
        }
    }
    ed.endEditBlock();

    QList<QTextCursor> out;
    for (const auto &p : ps) {
        QTextCursor c(document());
        c.setPosition(posAt(p.ab + dir, p.ac));
        c.setPosition(posAt(p.pb + dir, p.pc), QTextCursor::KeepAnchor);
        out << c;
    }
    setAllCursors(out, 0);
}

void Editor::duplicateLines(int dir)
{
    const auto groups = blockGroups();
    const auto all = allCursors();
    QList<CursorPos> ps;
    for (const auto &c : all) {
        const QTextBlock a = document()->findBlock(c.anchor());
        const QTextBlock p = document()->findBlock(c.position());
        ps.append({a.blockNumber(), c.anchor() - a.position(), p.blockNumber(), c.position() - p.position()});
    }

    QTextCursor ed(document());
    ed.beginEditBlock();
    for (int i = groups.size() - 1; i >= 0; --i) {
        const QTextBlock sb = document()->findBlockByNumber(groups[i].first);
        const QTextBlock eb = document()->findBlockByNumber(groups[i].last);
        QTextCursor r(document());
        r.setPosition(sb.position());
        r.setPosition(eb.position() + eb.length() - 1, QTextCursor::KeepAnchor);
        const QString t = r.selection().toPlainText();
        QTextCursor ins(document());
        ins.setPosition(eb.position() + eb.length() - 1);
        ins.insertText("\n" + t);
    }
    ed.endEditBlock();

    QList<QTextCursor> out;
    for (const auto &p : ps) {
        const int line = qMin(p.ab, p.pb);
        int offset = 0;
        for (const auto &g : groups) {
            const int size = g.last - g.first + 1;
            if (line > g.last)
                offset += size;
            else if (line >= g.first) {
                if (dir > 0)
                    offset += size;
                break;
            }
        }
        QTextCursor c(document());
        c.setPosition(posAt(p.ab + offset, p.ac));
        c.setPosition(posAt(p.pb + offset, p.pc), QTextCursor::KeepAnchor);
        out << c;
    }
    setAllCursors(out, 0);
}

void Editor::deleteLines()
{
    const auto groups = blockGroups();
    auto all = allCursors();
    QTextCursor ed(document());
    ed.beginEditBlock();
    for (int i = groups.size() - 1; i >= 0; --i) {
        const QTextBlock sb = document()->findBlockByNumber(groups[i].first);
        const QTextBlock eb = document()->findBlockByNumber(groups[i].last);
        QTextCursor r(document());
        if (eb.next().isValid()) {
            r.setPosition(sb.position());
            r.setPosition(eb.next().position(), QTextCursor::KeepAnchor);
        } else if (sb.previous().isValid()) {
            const QTextBlock pb = sb.previous();
            r.setPosition(pb.position() + pb.length() - 1);
            r.setPosition(eb.position() + eb.length() - 1, QTextCursor::KeepAnchor);
        } else {
            r.setPosition(sb.position());
            r.setPosition(eb.position() + eb.length() - 1, QTextCursor::KeepAnchor);
        }
        r.removeSelectedText();
    }
    ed.endEditBlock();
    for (auto &c : all)
        c.clearSelection();
    setAllCursors(all, 0);
}

void Editor::selectLines()
{
    auto all = allCursors();
    for (auto &c : all) {
        const int start = document()->findBlock(c.selectionStart()).position();
        // Select through the end of the line holding selectionEnd. If a previous Ctrl+L left the
        // selection ending at a line start, that line is the next one, so repeating extends by one.
        const QTextBlock endB = document()->findBlock(c.selectionEnd());
        const int end = endB.next().isValid() ? endB.next().position() : endB.position() + endB.length() - 1;
        c.setPosition(start);
        c.setPosition(end, QTextCursor::KeepAnchor);
    }
    setAllCursors(all, 0);
}

void Editor::copy(bool cut)
{
    auto all = allCursors();
    std::sort(all.begin(), all.end(),
              [](const QTextCursor &a, const QTextCursor &b) { return a.position() < b.position(); });
    const bool anySel = std::any_of(all.begin(), all.end(), [](const QTextCursor &c) { return c.hasSelection(); });

    auto *md = new QMimeData;
    if (!anySel) { // VSCode: copy/cut whole line without selection
        QStringList lines;
        for (const auto &c : all)
            lines << c.block().text();
        md->setText(lines.join('\n') + '\n');
        md->setData(kLineMime, "1");
        QGuiApplication::clipboard()->setMimeData(md);
        if (cut)
            deleteLines();
        return;
    }
    QStringList parts;
    for (const auto &c : all)
        parts << c.selection().toPlainText();
    md->setText(parts.join('\n'));
    QGuiApplication::clipboard()->setMimeData(md);
    if (cut)
        edit([](QTextCursor &c) { c.removeSelectedText(); });
}

void Editor::paste()
{
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md || !md->hasText())
        return;
    QString text = md->text();
    text.replace("\r\n", "\n");
    const bool lineMode = md->hasFormat(kLineMime);

    auto all = allCursors();
    const int primPos = all[0].position();
    std::sort(all.begin(), all.end(),
              [](const QTextCursor &a, const QTextCursor &b) { return a.position() < b.position(); });
    int primary = 0;
    for (int i = 0; i < all.size(); ++i)
        if (all[i].position() == primPos)
            primary = i;

    QStringList lines = text.split('\n');
    if (text.endsWith('\n'))
        lines.removeLast();
    const bool distribute = all.size() > 1 && lines.size() == all.size();

    QTextCursor ed(document());
    ed.beginEditBlock();
    for (int i = 0; i < all.size(); ++i) {
        QTextCursor &c = all[i];
        if (distribute)
            c.insertText(lines[i]);
        else if (lineMode && !c.hasSelection())
            QTextCursor(c.block()).insertText(text); // whole line goes above the current one
        else
            c.insertText(text);
    }
    ed.endEditBlock();
    setAllCursors(all, primary);
}

// ------------------------------------------------------------ events

void Editor::keyPressEvent(QKeyEvent *e)
{
    resetBlink();
    const auto mods = e->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool alt = mods & Qt::AltModifier;
    const int key = e->key();
    const bool multi = !m_extra.isEmpty();
    const bool vert = key == Qt::Key_Up || key == Qt::Key_Down;
    const int dir = key == Qt::Key_Up ? -1 : 1;

    if (key == Qt::Key_Escape) {
        if (multi) {
            clearExtraCursors();
            return;
        }
        if (textCursor().hasSelection()) {
            QTextCursor c = textCursor();
            c.clearSelection();
            setTextCursor(c);
            return;
        }
        e->ignore();
        return;
    }

    if (e->matches(QKeySequence::Undo)) { undo(); return; }
    if (e->matches(QKeySequence::Redo) || (ctrl && !shift && !alt && key == Qt::Key_Y)) { redo(); return; }

    if (vert && alt && shift && !ctrl) { addCursorVertical(dir); return; }
    if (vert && alt && shift && ctrl) { duplicateLines(dir); return; }
    if (vert && alt && !shift && !ctrl) { moveLines(dir); return; }

    if (e->matches(QKeySequence::Copy)) { copy(false); return; }
    if (e->matches(QKeySequence::Cut)) { copy(true); return; }
    if (e->matches(QKeySequence::Paste)) { paste(); return; }

    // markdown tables (single cursor, no selection)
    if (!alt && m_md->isEnabled() && m_extra.isEmpty() && !textCursor().hasSelection() &&
        isTableLine(textCursor().block())) {
        const bool enter = key == Qt::Key_Return || key == Qt::Key_Enter;
        if (!ctrl && (key == Qt::Key_Tab || key == Qt::Key_Backtab)) { // move between cells
            tableTab(key == Qt::Key_Backtab || shift);
            return;
        }
        if (!ctrl && enter) { // new row
            tableEnter();
            return;
        }
        if (ctrl && !shift && enter) { // leave the table
            tableLeave();
            return;
        }
        if (ctrl && !shift &&
            (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down)) {
            if (tableArrow(key)) // move between cells instead of words
                return;
        }
        if (ctrl && !shift && key == Qt::Key_Backspace) { // delete the row
            tableDeleteRow(true);
            return;
        }
        if (!ctrl && !shift && key == Qt::Key_Backspace) { // erasing the first | of an empty row deletes it
            const QTextCursor cur = textCursor();
            const QString text = cur.block().text();
            const QList<int> pipes = MarkdownHighlighter::pipePositions(text);
            const bool empty = MarkdownHighlighter::tableCells(text).join("").trimmed().isEmpty() &&
                               !MarkdownHighlighter::tableSepRe().match(text).hasMatch();
            if (empty && !pipes.isEmpty() && cur.positionInBlock() == pipes.first() + 1) {
                tableDeleteRow(false);
                return;
            }
        }
    }

    if (ctrl && !alt) {
        switch (key) {
        case Qt::Key_D:
            if (!shift) { selectNextOccurrence(); return; }
            break;
        case Qt::Key_L:
            if (shift) selectAllOccurrences(); else selectLines();
            return;
        case Qt::Key_K:
            if (shift) { deleteLines(); return; }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            insertLine(shift);
            return;
        case Qt::Key_B:
            if (!shift && m_md->isEnabled()) { wrapSelection("**"); return; }
            break;
        case Qt::Key_I:
            if (!shift && m_md->isEnabled()) { wrapSelection("*"); return; }
            break;
        case Qt::Key_BracketRight: indentLines(false); return;
        case Qt::Key_BracketLeft: indentLines(true); return;
        default: break;
        }
    }

    // markdown: "[] " or "[ ] " at the start of a line becomes a task
    if (!ctrl && !alt && key == Qt::Key_Space && m_md->isEnabled() && m_extra.isEmpty() &&
        !textCursor().hasSelection()) {
        static const QRegularExpression box(R"(^(\s*)\[ ?\]$)");
        QTextCursor c = textCursor();
        const auto m = box.match(c.block().text().left(c.positionInBlock()));
        if (m.hasMatch()) {
            c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            c.insertText(m.captured(1) + "- [ ] ");
            setTextCursor(c);
            return;
        }
    }

    if (!ctrl && !alt) {
        switch (key) {
        case Qt::Key_Tab:
            if (shift) indentLines(true); else tabKey();
            return;
        case Qt::Key_Backtab: indentLines(true); return;
        case Qt::Key_Return:
        case Qt::Key_Enter: newline(); return;
        case Qt::Key_Home: smartHome(shift); return;
        default: break;
        }
    }

    if (multi) {
        switch (key) {
        case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt:
        case Qt::Key_Meta: case Qt::Key_AltGr: case Qt::Key_Super_L: case Qt::Key_Super_R:
            return;
        case Qt::Key_Backspace:
            edit([ctrl](QTextCursor &c) {
                if (c.hasSelection())
                    c.removeSelectedText();
                else if (ctrl) {
                    c.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
                    c.removeSelectedText();
                } else
                    c.deletePreviousChar();
            });
            return;
        case Qt::Key_Delete:
            edit([ctrl](QTextCursor &c) {
                if (c.hasSelection())
                    c.removeSelectedText();
                else if (ctrl) {
                    c.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
                    c.removeSelectedText();
                } else
                    c.deleteChar();
            });
            return;
        case Qt::Key_Left:
            if (!alt) { move(ctrl ? QTextCursor::WordLeft : QTextCursor::Left, shift); return; }
            break;
        case Qt::Key_Right:
            if (!alt) { move(ctrl ? QTextCursor::WordRight : QTextCursor::Right, shift); return; }
            break;
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (!ctrl && !alt) { move(key == Qt::Key_Up ? QTextCursor::Up : QTextCursor::Down, shift); return; }
            break;
        case Qt::Key_End:
            if (!ctrl) { move(QTextCursor::EndOfLine, shift); return; }
            break;
        default: break;
        }
        const QString t = e->text();
        if (!t.isEmpty() && !ctrl && t[0].isPrint()) {
            edit([t](QTextCursor &c) { c.insertText(t); });
            return;
        }
        clearExtraCursors();
    }
    QPlainTextEdit::keyPressEvent(e);
}

void Editor::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier)) {
        const QTextCursor c = cursorForPosition(e->position().toPoint());
        auto all = allCursors();
        if (all.size() > 1) {
            for (int i = 0; i < all.size(); ++i) {
                if (!all[i].hasSelection() && all[i].position() == c.position()) {
                    all.removeAt(i); // alt+click on an existing cursor removes it
                    setAllCursors(all, 0);
                    return;
                }
            }
        }
        all.append(c);
        setAllCursors(all, all.size() - 1);
        return;
    }
    if (e->button() == Qt::LeftButton && m_md->isEnabled() && markdownClick(e))
        return;
    clearExtraCursors();
    QPlainTextEdit::mousePressEvent(e);
}

void Editor::paintEvent(QPaintEvent *e)
{
    if (m_md->isEnabled()) {
        QPainter under(viewport());
        paintMarkdownUnder(under);
    }
    QPlainTextEdit::paintEvent(e);
    if (m_md->isEnabled()) {
        QPainter over(viewport());
        paintMarkdownOver(over);
    }
    if (!hasFocus() || !m_blinkOn)
        return;
    QPainter p(viewport());
    for (const auto &c : allCursors()) {
        const QRect r = cursorRect(c);
        p.fillRect(QRect(r.left(), r.top(), 2, r.height()), m_cursorColor);
    }
}

void Editor::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const int d = e->angleDelta().y();
        if (d != 0)
            emit zoomRequested(d > 0 ? 1 : -1);
        e->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(e);
}

void Editor::focusInEvent(QFocusEvent *e)
{
    QPlainTextEdit::focusInEvent(e);
    resetBlink();
}

void Editor::focusOutEvent(QFocusEvent *e)
{
    QPlainTextEdit::focusOutEvent(e);
    m_blink.stop();
    viewport()->update();
}

void Editor::showEvent(QShowEvent *e)
{
    QPlainTextEdit::showEvent(e);
    if (pendingScroll >= 0) {
        const int v = pendingScroll;
        pendingScroll = -1;
        QTimer::singleShot(0, this, [this, v] { verticalScrollBar()->setValue(v); });
    }
}

// ------------------------------------------------------------ persistent undo history

static QString textHash(const QString &s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}

void Editor::loadContent(const QString &text)
{
    m_applying = true;
    setPlainText(text);
    m_applying = false;
    m_shadow = toPlainText();
    m_undo.clear();
    m_redo.clear();
    historyDirty = false;
}

void Editor::onContentsChange()
{
    if (m_formatOnly)
        return;
    const QString now = toPlainText();
    if (m_applying) {
        m_shadow = now;
        return;
    }
    // minimal diff between the previous and the new text (Qt's own counts are off at document end)
    const QString &old = m_shadow;
    const int ol = old.size(), nl = now.size();
    int p = 0;
    while (p < ol && p < nl && old[p] == now[p])
        ++p;
    int s = 0;
    while (s < ol - p && s < nl - p && old[ol - 1 - s] == now[nl - 1 - s])
        ++s;
    if (p == ol && p == nl)
        return; // formatting only
    Op op{p, old.mid(p, ol - p - s), now.mid(p, nl - p - s)};
    m_shadow = now;
    m_redo.clear();
    historyDirty = true;
    emit edited();

    const qint64 t = QDateTime::currentMSecsSinceEpoch();

    // consecutive typing / backspace / delete become a single undo step, like in VSCode
    auto tryMerge = [&op, t](Step &st) {
        if (st.ops.size() != 1 || t - st.time > 1500)
            return false;
        Op &l = st.ops.last();
        const bool typing = op.removed.isEmpty() && op.added.size() == 1 && op.added[0] != '\n' &&
                            l.removed.isEmpty() && !l.added.isEmpty() && !l.added.contains('\n') &&
                            op.pos == l.pos + l.added.size() &&
                            !(op.added[0].isSpace() && !l.added.back().isSpace()); // a new word starts a new step
        if (typing) {
            l.added += op.added;
        } else if (op.added.isEmpty() && op.removed.size() == 1 && l.added.isEmpty() && !l.removed.isEmpty() &&
                   op.pos + 1 == l.pos) { // backspace
            l.pos = op.pos;
            l.removed.prepend(op.removed);
        } else if (op.added.isEmpty() && op.removed.size() == 1 && l.added.isEmpty() && !l.removed.isEmpty() &&
                   op.pos == l.pos) { // delete
            l.removed += op.removed;
        } else {
            return false;
        }
        st.time = t;
        return true;
    };

    if (m_turnOpen && !m_undo.isEmpty()) { // same event-loop turn: same step
        if (!tryMerge(m_undo.last()))
            m_undo.last().ops << op;
        return;
    }
    m_turnOpen = true;
    QTimer::singleShot(0, this, [this] { m_turnOpen = false; });

    const bool mayMerge = !m_breakMerge && !m_undo.isEmpty();
    m_breakMerge = false;
    if (mayMerge && tryMerge(m_undo.last()))
        return;
    m_undo.append(Step{{op}, t});
}

void Editor::applyStep(const Step &st, bool undo)
{
    m_applying = true;
    QTextCursor c(document());
    c.beginEditBlock();
    if (undo) {
        for (int i = st.ops.size() - 1; i >= 0; --i) {
            const Op &o = st.ops[i];
            c.setPosition(o.pos);
            c.setPosition(o.pos + o.added.size(), QTextCursor::KeepAnchor);
            c.insertText(o.removed);
        }
    } else {
        for (const Op &o : st.ops) {
            c.setPosition(o.pos);
            c.setPosition(o.pos + o.removed.size(), QTextCursor::KeepAnchor);
            c.insertText(o.added);
        }
    }
    c.endEditBlock();
    m_applying = false;
    m_shadow = toPlainText();
    m_breakMerge = true;
    historyDirty = true;
    emit edited();

    // a cursor at the end of each restored change
    const int max = document()->characterCount() - 1;
    QList<QTextCursor> cs;
    for (const Op &o : st.ops) {
        QTextCursor k(document());
        k.setPosition(qBound(0, o.pos + int(undo ? o.removed.size() : o.added.size()), max));
        cs << k;
    }
    setAllCursors(cs, cs.size() - 1);
}

void Editor::undo()
{
    if (m_undo.isEmpty())
        return;
    const Step st = m_undo.takeLast();
    applyStep(st, true);
    m_redo.append(st);
}

void Editor::redo()
{
    if (m_redo.isEmpty())
        return;
    const Step st = m_redo.takeLast();
    applyStep(st, false);
    m_undo.append(st);
}

QJsonObject Editor::historyJson() const
{
    auto enc = [](const QList<Step> &steps) {
        QJsonArray a;
        for (const Step &st : steps) {
            QJsonArray ops;
            for (const Op &o : st.ops)
                ops.append(QJsonArray{o.pos, o.removed, o.added});
            a.append(QJsonObject{{"t", st.time}, {"ops", ops}});
        }
        return a;
    };
    return QJsonObject{{"version", 1}, {"hash", textHash(m_shadow)}, {"undo", enc(m_undo)}, {"redo", enc(m_redo)}};
}

bool Editor::restoreHistory(const QJsonObject &o)
{
    if (o.value("version").toInt() != 1 || o.value("hash").toString() != textHash(m_shadow))
        return false; // edited outside the app (or no history): positions would be meaningless
    auto dec = [](const QJsonArray &a, QList<Step> &out) {
        for (const auto &v : a) {
            Step st;
            st.time = v.toObject().value("t").toInteger();
            for (const auto &ov : v.toObject().value("ops").toArray()) {
                const QJsonArray x = ov.toArray();
                if (x.size() != 3)
                    return false;
                st.ops.append(Op{x[0].toInt(), x[1].toString(), x[2].toString()});
            }
            if (!st.ops.isEmpty())
                out.append(st);
        }
        return true;
    };
    QList<Step> u, r;
    if (!dec(o.value("undo").toArray(), u) || !dec(o.value("redo").toArray(), r))
        return false;
    m_undo = u;
    m_redo = r;
    m_breakMerge = true;
    historyDirty = false;
    return true;
}

void Editor::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *m = createStandardContextMenu();
    for (QAction *a : m->actions()) {
        if (a->objectName() == "edit-undo") {
            a->disconnect();
            a->setEnabled(!m_undo.isEmpty());
            connect(a, &QAction::triggered, this, &Editor::undo);
        } else if (a->objectName() == "edit-redo") {
            a->disconnect();
            a->setEnabled(!m_redo.isEmpty());
            connect(a, &QAction::triggered, this, &Editor::redo);
        }
    }
    m->exec(e->globalPos());
    delete m;
}

// ------------------------------------------------------------ markdown editing

// Enter inside a list item / quote continues it; Enter on an empty item ends the list.
bool Editor::markdownNewline(QTextCursor &c)
{
    const QTextBlock b = document()->findBlock(c.selectionStart());
    const QString t = b.text();
    const int col = c.selectionStart() - b.position();

    if (inCodeBlock(b))
        return false; // plain newline (with auto-indent) inside code

    // ```lang + Enter: close the block and put the cursor inside it
    if (const auto f = MarkdownHighlighter::fenceRe().match(t);
        f.hasMatch() && !c.hasSelection() && col == t.size()) {
        int fencesAfter = 0;
        for (QTextBlock n = b.next(); n.isValid(); n = n.next())
            if (MarkdownHighlighter::fenceRe().match(n.text()).hasMatch())
                ++fencesAfter;
        if (fencesAfter % 2 == 0) { // otherwise this ``` already closes/opens an existing block
            const QString ws = f.captured(1);
            c.insertText("\n" + ws + "\n" + ws + f.captured(2));
            c.movePosition(QTextCursor::PreviousBlock);
            c.movePosition(QTextCursor::EndOfBlock);
            return true;
        }
        return false;
    }

    if (const auto m = MarkdownHighlighter::listRe().match(t); m.hasMatch() && col >= m.capturedEnd(0)) {
        if (!c.hasSelection() && t.mid(m.capturedEnd(0)).trimmed().isEmpty()) {
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.insertText(m.captured(1)); // keep the indentation, drop the marker
            return true;
        }
        QString marker = m.captured(2);
        bool numbered = false;
        const int n = marker.left(marker.size() - 1).toInt(&numbered);
        if (numbered)
            marker = QString::number(n + 1) + marker.back();
        c.insertText("\n" + m.captured(1) + marker + m.captured(3) + (m.capturedLength(4) ? "[ ] " : ""));
        return true;
    }

    static const QRegularExpression quote(R"(^(\s*>+\s?))");
    if (const auto q = quote.match(t); q.hasMatch() && col >= q.capturedEnd(1)) {
        if (!c.hasSelection() && t.mid(q.capturedEnd(1)).trimmed().isEmpty()) {
            c.movePosition(QTextCursor::StartOfBlock);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.insertText(QString());
            return true;
        }
        c.insertText("\n" + q.captured(1));
        return true;
    }
    return false;
}

// Click on "[ ]" toggles a task; Ctrl+click opens a link.
bool Editor::markdownClick(QMouseEvent *e)
{
    const QTextCursor at = cursorForPosition(e->position().toPoint());
    const QTextBlock b = at.block();
    const QString t = b.text();
    const int col = at.positionInBlock();

    if (e->modifiers() & Qt::ControlModifier) {
        for (auto it = MarkdownHighlighter::linkRe().globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            if (col >= m.capturedStart(0) && col <= m.capturedEnd(0))
                return QDesktopServices::openUrl(QUrl::fromUserInput(m.captured(2)));
        }
        for (auto it = MarkdownHighlighter::urlRe().globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            if (col >= m.capturedStart(0) && col <= m.capturedEnd(0))
                return QDesktopServices::openUrl(QUrl(m.captured(0)));
        }
        return false;
    }

    if (e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))
        return false;
    if (const auto m = MarkdownHighlighter::taskRe().match(t); m.hasMatch()) {
        const int box = m.capturedLength(1); // column of "["
        if (col >= box && col <= box + 3) {
            const bool done = m.captured(2) != " ";
            QTextCursor c(document());
            c.setPosition(b.position() + box + 1);
            c.setPosition(b.position() + box + 2, QTextCursor::KeepAnchor);
            c.insertText(done ? " " : "x");
            return true; // the click only toggles, the cursor stays where it was
        }
    }
    return false;
}

// Ctrl+B / Ctrl+I: wrap the selection in ** / *, or unwrap it if it's already wrapped.
void Editor::wrapSelection(const QString &marker)
{
    const int ms = marker.size();
    edit([this, &marker, ms](QTextCursor &c) {
        const QString sel = c.selection().toPlainText();
        const int s = c.selectionStart(), e = c.selectionEnd();
        auto select = [&c](int a, int b) {
            c.setPosition(a);
            c.setPosition(b, QTextCursor::KeepAnchor);
        };
        if (sel.size() >= 2 * ms && sel.startsWith(marker) && sel.endsWith(marker)) {
            const QString inner = sel.mid(ms, sel.size() - 2 * ms);
            c.insertText(inner);
            select(s, s + inner.size());
            return;
        }
        if (s >= ms) {
            QTextCursor around(document());
            around.setPosition(s - ms);
            around.setPosition(qMin(e + ms, document()->characterCount() - 1), QTextCursor::KeepAnchor);
            if (around.selection().toPlainText() == marker + sel + marker) {
                around.insertText(sel);
                select(s - ms, s - ms + sel.size());
                return;
            }
        }
        c.insertText(marker + sel + marker);
        select(s + ms, s + ms + sel.size());
    });
}

// ------------------------------------------------------------ markdown: code blocks & tables

bool Editor::inCodeBlock(const QTextBlock &b) const
{
    // content and closing-fence lines come after a block whose state is "inside a fence"
    return b.previous().isValid() && MarkdownHighlighter::inFence(b.previous().userState());
}

bool Editor::isTableLine(const QTextBlock &b) const
{
    return b.isValid() && MarkdownHighlighter::tableRowRe().match(b.text()).hasMatch() && !inCodeBlock(b);
}

int Editor::cellAt(const QString &row, int col) const
{
    int n = -1;
    for (int p : MarkdownHighlighter::pipePositions(row))
        if (p < col)
            ++n;
    return qMax(0, n);
}

// Rewrites the table around `inside` with aligned columns (one undo step). Same number of lines.
Editor::TableInfo Editor::formatTable(const QTextBlock &inside)
{
    TableInfo t;
    QTextBlock first = inside, last = inside;
    while (isTableLine(first.previous()))
        first = first.previous();
    while (isTableLine(last.next()))
        last = last.next();
    t.first = first.blockNumber();
    t.last = last.blockNumber();
    t.indent = leadingWs(first);

    QList<QStringList> rows;
    QList<int> align; // 0 left, 1 center, 2 right
    for (QTextBlock b = first;; b = b.next()) {
        const QString text = b.text();
        const QStringList cells = MarkdownHighlighter::tableCells(text);
        if (MarkdownHighlighter::tableSepRe().match(text).hasMatch()) {
            t.sepRow = rows.size();
            align.clear();
            for (const QString &c : cells) {
                const bool l = c.startsWith(':'), r = c.endsWith(':');
                align << (l && r ? 1 : r ? 2 : 0);
            }
        }
        rows << cells;
        if (b == last)
            break;
    }
    int cols = 0;
    for (const auto &r : rows)
        cols = qMax(cols, int(r.size()));
    t.widths = QList<int>(cols, 3);
    for (int i = 0; i < rows.size(); ++i)
        if (i != t.sepRow)
            for (int c = 0; c < rows[i].size(); ++c)
                t.widths[c] = qMax(t.widths[c], int(rows[i][c].size()));

    QStringList lines;
    for (int i = 0; i < rows.size(); ++i) {
        QString line = t.indent + "|";
        for (int c = 0; c < cols; ++c) {
            const int w = t.widths[c];
            const int a = align.value(c, 0);
            if (i == t.sepRow) {
                QString dash(w + 2, '-');
                if (a == 1 || (a == 0 && rows[i].value(c).startsWith(':')))
                    dash[0] = ':';
                if (a >= 1)
                    dash[w + 1] = ':';
                line += dash + "|";
                continue;
            }
            const QString cell = rows[i].value(c);
            const int pad = w - cell.size();
            const QString body = a == 2 ? QString(pad, ' ') + cell
                                 : a == 1 ? QString(pad / 2, ' ') + cell + QString(pad - pad / 2, ' ')
                                          : cell + QString(pad, ' ');
            line += " " + body + " |";
        }
        lines << line;
    }
    const QString formatted = lines.join('\n');

    QTextCursor c(document());
    c.setPosition(first.position());
    c.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    if (c.selection().toPlainText() != formatted)
        c.insertText(formatted);
    return t;
}

QString Editor::emptyRow(const TableInfo &t) const
{
    QString line = t.indent + "|";
    for (int w : t.widths)
        line += QString(w + 2, ' ') + "|";
    return line;
}

// cursor at the end of a cell's content (formatted table)
QTextCursor Editor::cellCursor(const TableInfo &t, int row, int cell) const
{
    const QTextBlock b = document()->findBlockByNumber(t.first + row);
    int start = t.indent.size() + 2;
    for (int c = 0; c < cell && c < t.widths.size(); ++c)
        start += t.widths[c] + 3;
    const QString region = b.text().mid(start, t.widths.value(cell, 0));
    int end = region.size();
    while (end > 0 && region[end - 1] == ' ')
        --end;
    QTextCursor c(document());
    c.setPosition(b.position() + start + end); // empty cell: its first column
    return c;
}

void Editor::tableTab(bool back)
{
    const QTextCursor cur = textCursor();
    const QTextBlock b = cur.block();
    int cell = cellAt(b.text(), cur.positionInBlock());
    const int line = b.blockNumber(); // b is invalidated by formatTable (it rewrites the table text)
    QTextCursor block(document());
    block.beginEditBlock();
    const TableInfo t = formatTable(b);
    int row = line - t.first;
    const int cols = t.widths.size();
    int rows = t.last - t.first + 1;

    auto step = [&] {
        cell += back ? -1 : 1;
        if (cell >= cols) {
            cell = 0;
            ++row;
        } else if (cell < 0) {
            cell = cols - 1;
            --row;
        }
        if (row == t.sepRow)
            row += back ? -1 : 1;
    };
    step();
    if (row < 0) { // before the first cell: stay
        row = 0;
        cell = 0;
    } else if (row >= rows) { // past the last cell: new row
        QTextCursor end(document());
        end.setPosition(document()->findBlockByNumber(t.last).position());
        end.movePosition(QTextCursor::EndOfBlock);
        end.insertText("\n" + emptyRow(t));
        rows += 1;
        row = rows - 1;
        cell = 0;
    }
    block.endEditBlock();
    setAllCursors({cellCursor(t, row, cell)});
}

void Editor::tableEnter()
{
    const QTextBlock b = textCursor().block();
    const bool emptyLine = MarkdownHighlighter::tableCells(b.text()).join("").trimmed().isEmpty();
    QTextCursor block(document());
    block.beginEditBlock();
    if (emptyLine && !MarkdownHighlighter::tableSepRe().match(b.text()).hasMatch()) {
        // Enter on an empty row leaves the table: the row becomes an empty line
        QTextCursor c(b);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.insertText(QString());
        block.endEditBlock();
        setAllCursors({c});
        return;
    }
    const int line = b.blockNumber(); // b is invalidated by formatTable
    const TableInfo t = formatTable(b);
    const int row = line - t.first;
    QTextCursor at(document());
    at.setPosition(document()->findBlockByNumber(t.first + row).position());
    at.movePosition(QTextCursor::EndOfBlock);
    int newRow = row + 1;
    if (row == 0 && t.sepRow < 0) { // header without separator: add it
        QString sep = t.indent + "|";
        for (int w : t.widths)
            sep += QString(w + 2, '-') + "|";
        at.insertText("\n" + sep);
        newRow = 2;
    } else if (row == t.sepRow) {
        newRow = row + 1;
    }
    at.insertText("\n" + emptyRow(t));
    block.endEditBlock();
    setAllCursors({cellCursor(t, newRow, 0)});
}

template <class F>
static void forVisibleBlocks(QTextBlock b, QPointF off, int height,
                             const std::function<QRectF(const QTextBlock &)> &geom, F f)
{
    while (b.isValid()) {
        const QRectF g = geom(b).translated(off);
        if (g.top() > height)
            break;
        if (b.isVisible())
            f(b, g);
        b = b.next();
    }
}

void Editor::paintMarkdownUnder(QPainter &p)
{
    // rounded background for fenced code blocks
    const qreal m = document()->documentMargin();
    const qreal left = qMax<qreal>(2, m - 10), right = viewport()->width() - left;
    QRectF group;
    auto flush = [&] {
        if (group.isNull())
            return;
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(m_codeBg);
        p.drawRoundedRect(QRectF(left, group.top() - 2, right - left, group.height() + 4), 6, 6);
        group = QRectF();
    };
    const auto geom = [this](const QTextBlock &b) { return blockBoundingGeometry(b); };
    forVisibleBlocks(firstVisibleBlock(), contentOffset(), viewport()->height(), geom,
                     [&](const QTextBlock &b, const QRectF &g) {
                         if (MarkdownHighlighter::isCode(b.userState()))
                             group = group.isNull() ? g : group.united(g);
                         else
                             flush();
                     });
    flush();
}

void Editor::paintMarkdownOver(QPainter &p)
{
    const qreal m = document()->documentMargin();
    const qreal x0 = m, x1 = viewport()->width() - m;
    p.setRenderHint(QPainter::Antialiasing, false);
    const auto geom = [this](const QTextBlock &b) { return blockBoundingGeometry(b); };
    forVisibleBlocks(firstVisibleBlock(), contentOffset(), viewport()->height(), geom,
                     [&](const QTextBlock &b, const QRectF &g) {
        const int st = b.userState();
        if (st == MarkdownHighlighter::Rule) { // the divider takes the whole line
            p.setPen(QPen(m_ruleColor, 2));
            p.drawLine(QPointF(x0, g.center().y()), QPointF(x1, g.center().y()));
            return;
        }
    });
}

// cursor at the end of a cell's content, for rows that may not be aligned
QTextCursor Editor::cursorInCell(const QTextBlock &b, int cell) const
{
    const QString text = b.text();
    const QList<int> pipes = MarkdownHighlighter::pipePositions(text);
    QTextCursor c(document());
    if (pipes.isEmpty()) {
        c.setPosition(b.position() + text.size());
        return c;
    }
    cell = qBound(0, cell, qMax(0, int(pipes.size()) - 2));
    const int start = pipes[cell] + 1;
    const int end = cell + 1 < pipes.size() ? pipes[cell + 1] : int(text.size());
    int e = end;
    while (e > start && text[e - 1] == ' ')
        --e;
    // end of the content; an empty cell gets the column after its leading space
    c.setPosition(b.position() + (e > start ? e : qMin(start + 1, end)));
    return c;
}

// Ctrl+Enter: align the table and continue on the line after it
void Editor::tableLeave()
{
    const QTextBlock b = textCursor().block();
    QTextCursor edit(document());
    edit.beginEditBlock();
    const TableInfo t = formatTable(b);
    const QTextBlock last = document()->findBlockByNumber(t.last);
    QTextCursor c(document());
    if (last.next().isValid() && last.next().text().trimmed().isEmpty()) {
        c.setPosition(last.next().position());
        c.movePosition(QTextCursor::EndOfBlock);
    } else {
        c.setPosition(last.position() + last.length() - 1);
        c.insertText("\n");
    }
    edit.endEditBlock();
    setAllCursors({c});
}

// Ctrl+Backspace (keepColumn) or Backspace on the first | of an empty row: remove the row
void Editor::tableDeleteRow(bool keepColumn)
{
    const QTextCursor cur = textCursor();
    const QTextBlock b = cur.block();
    const int cell = cellAt(b.text(), cur.positionInBlock());
    const int line = b.blockNumber();
    const bool hasPrev = b.previous().isValid();

    QTextCursor r(document());
    if (b.next().isValid()) {
        r.setPosition(b.position());
        r.setPosition(b.next().position(), QTextCursor::KeepAnchor);
    } else if (hasPrev) {
        r.setPosition(b.previous().position() + b.previous().length() - 1);
        r.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
    } else {
        r.setPosition(b.position());
        r.setPosition(b.position() + b.length() - 1, QTextCursor::KeepAnchor);
    }
    r.removeSelectedText();

    // where the cursor goes: the row above (skipping the separator), else what moved into this line
    QTextBlock target = hasPrev ? document()->findBlockByNumber(line - 1) : document()->findBlockByNumber(line);
    if (!keepColumn) { // plain Backspace: end of the previous line
        QTextCursor c(document());
        c.setPosition(target.position() + target.length() - 1);
        setAllCursors({c});
        return;
    }
    if (MarkdownHighlighter::tableSepRe().match(target.text()).hasMatch()) {
        // above is the separator: prefer the row that moved up into this line, else the header
        const QTextBlock below = document()->findBlockByNumber(line);
        target = isTableLine(below) ? below : target.previous();
    }
    if (isTableLine(target)) {
        setAllCursors({cursorInCell(target, cell)});
    } else {
        QTextCursor c(document());
        c.setPosition(target.position() + target.length() - 1);
        setAllCursors({c});
    }
}

// Ctrl+←/→: previous/next cell (wrapping across rows); Ctrl+↑/↓: same column one row up/down.
// Only moves the cursor (no realignment). Returns false at the table's edge for ↑/↓.
bool Editor::tableArrow(int key)
{
    const QTextCursor cur = textCursor();
    QTextBlock b = cur.block();
    const auto isRow = [this](const QTextBlock &x) {
        return isTableLine(x) && !MarkdownHighlighter::tableSepRe().match(x.text()).hasMatch();
    };
    const auto cellCount = [](const QTextBlock &x) {
        return qMax(1, int(MarkdownHighlighter::pipePositions(x.text()).size()) - 1);
    };
    const auto step = [&](const QTextBlock &x, bool down) { // next data row, skipping the separator
        QTextBlock n = down ? x.next() : x.previous();
        while (isTableLine(n) && !isRow(n))
            n = down ? n.next() : n.previous();
        return isRow(n) ? n : QTextBlock();
    };
    int cell = cellAt(b.text(), cur.positionInBlock());

    if (key == Qt::Key_Up || key == Qt::Key_Down) {
        const QTextBlock n = step(b, key == Qt::Key_Down);
        if (!n.isValid())
            return false;
        setAllCursors({cursorInCell(n, cell)});
        return true;
    }
    if (key == Qt::Key_Right) {
        if (cell + 1 < cellCount(b)) {
            ++cell;
        } else if (const QTextBlock n = step(b, true); n.isValid()) {
            b = n;
            cell = 0;
        } // else: last cell of the table, stay in it
    } else {
        if (cell > 0) {
            --cell;
        } else if (const QTextBlock n = step(b, false); n.isValid()) {
            b = n;
            cell = cellCount(n) - 1;
        }
    }
    setAllCursors({cursorInCell(b, cell)});
    return true;
}
