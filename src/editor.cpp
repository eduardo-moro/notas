#include "editor.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
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
}

void Editor::applyTheme(const Theme &t)
{
    m_cursorColor = t.cursor;
    m_extraSel = t.selection;
    m_searchColor = t.search;
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
        case Qt::Key_BracketRight: indentLines(false); return;
        case Qt::Key_BracketLeft: indentLines(true); return;
        default: break;
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
    clearExtraCursors();
    QPlainTextEdit::mousePressEvent(e);
}

void Editor::paintEvent(QPaintEvent *e)
{
    QPlainTextEdit::paintEvent(e);
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
