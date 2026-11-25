// Run: build/tests/tst_notas -platform offscreen
#include "editor.h"
#include "markdown.h"
#include "mainwindow.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTabBar>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

static void type(QWidget *w, const QString &text)
{
    for (const QChar c : text) {
        if (c == '\n')
            QTest::keyClick(w, Qt::Key_Return);
        else if (c.unicode() < 128)
            QTest::keyClicks(w, QString(c));
        else // QTest only maps ASCII to keys: send the text directly (ã, ç, é...)
            QTest::sendKeyEvent(QTest::Click, w, Qt::Key_unknown, QString(c), Qt::NoModifier);
        QTest::qWait(1); // let the event loop turn, like real typing
    }
}

static void key(QWidget *w, Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier)
{
    QTest::keyClick(w, k, m);
    QTest::qWait(1);
}

class TestNotas : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;

    AppSettings settings()
    {
        AppSettings s;
        s.load();
        s.notesDir = m_tmp.path() + "/Notas";
        s.autosaveMs = 50;
        return s;
    }

    Editor *currentEditor(MainWindow &w)
    {
        auto *tabs = w.findChild<QTabBar *>();
        for (Editor *e : w.findChildren<Editor *>())
            if (e->isVisible() && tabs && e->parentWidget())
                return e;
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", (m_tmp.path() + "/config").toUtf8());
        qputenv("XDG_STATE_HOME", (m_tmp.path() + "/state").toUtf8());
        qputenv("XDG_RUNTIME_DIR", (m_tmp.path() + "/run").toUtf8());
    }

    void typingIsGroupedByWord()
    {
        Editor e;
        e.show();
        type(&e, "abc def");
        QCOMPARE(e.toPlainText(), QString("abc def"));
        e.undo();
        QCOMPARE(e.toPlainText(), QString("abc"));
        e.undo();
        QCOMPARE(e.toPlainText(), QString(""));
        e.redo();
        e.redo();
        QCOMPARE(e.toPlainText(), QString("abc def"));
    }

    void backspaceIsOneStep()
    {
        Editor e;
        e.show();
        type(&e, "hello");
        for (int i = 0; i < 3; ++i)
            key(&e, Qt::Key_Backspace);
        QCOMPARE(e.toPlainText(), QString("he"));
        e.undo();
        QCOMPARE(e.toPlainText(), QString("hello"));
    }

    void multiLineCommandsUndoAtOnce()
    {
        Editor e;
        e.show();
        e.loadContent("um\ndois\ntres");
        QTextCursor c(e.document());
        c.setPosition(0);
        e.setTextCursor(c);
        key(&e, Qt::Key_Down, Qt::AltModifier); // move line down
        QCOMPARE(e.toPlainText(), QString("dois\num\ntres"));
        key(&e, Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier); // delete line
        QCOMPARE(e.toPlainText(), QString("dois\ntres"));
        key(&e, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(e.toPlainText(), QString("dois\num\ntres"));
        key(&e, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(e.toPlainText(), QString("um\ndois\ntres"));
        key(&e, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); // redo
        QCOMPARE(e.toPlainText(), QString("dois\num\ntres"));
    }

    void historyRoundTrip()
    {
        QJsonObject saved;
        QString text;
        {
            Editor e;
            e.show();
            e.loadContent("base\n");
            QTextCursor c(e.document());
            c.movePosition(QTextCursor::End);
            e.setTextCursor(c);
            type(&e, "primeira linha\nsegunda");
            text = e.toPlainText();
            saved = QJsonDocument::fromJson(QJsonDocument(e.historyJson()).toJson()).object();
        }
        Editor e2;
        e2.show();
        e2.loadContent(text);
        QVERIFY(e2.restoreHistory(saved));
        e2.undo();
        QCOMPARE(e2.toPlainText(), QString("base\nprimeira linha\n"));
        while (e2.toPlainText() != "base\n") {
            const QString before = e2.toPlainText();
            e2.undo();
            QVERIFY2(e2.toPlainText() != before, "undo stalled before reaching the base text");
        }
        e2.redo();
        QCOMPARE(e2.toPlainText(), QString("base\nprimeira"));
    }

    void historyOfOtherTextIsRejected()
    {
        Editor e;
        e.show();
        type(&e, "abc");
        const QJsonObject h = e.historyJson();
        Editor e2;
        e2.loadContent("abd"); // file changed outside the app
        QVERIFY(!e2.restoreHistory(h));
        e2.undo(); // nothing to undo, nothing breaks
        QCOMPARE(e2.toPlainText(), QString("abd"));
    }

    // ---------------------------------------------------------------- markdown

    Editor *mdEditor()
    {
        auto *e = new Editor;
        AppSettings st = settings();
        st.markdown = true;
        e->applySettings(st);
        e->resize(600, 400);
        e->show();
        return e;
    }

    void markdownListsContinue()
    {
        std::unique_ptr<Editor> e(mdEditor());
        type(e.get(), "- leite\npão\n\n");
        QCOMPARE(e->toPlainText(), QString("- leite\n- pão\n")); // Enter on an empty item drops its marker
        e->loadContent("");
        type(e.get(), "1. um\ndois\n");
        QCOMPARE(e->toPlainText(), QString("1. um\n2. dois\n3. "));
        e->loadContent("");
        type(e.get(), "[] comprar\nlavar");
        QCOMPARE(e->toPlainText(), QString("- [ ] comprar\n- [ ] lavar"));
        e->loadContent("");
        type(e.get(), "> citação\ncontinua");
        QCOMPARE(e->toPlainText(), QString("> citação\n> continua"));
    }

    void markdownBoldToggles()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("isso importa");
        QTextCursor c(e->document());
        c.setPosition(5);
        c.setPosition(12, QTextCursor::KeepAnchor);
        e->setTextCursor(c);
        key(e.get(), Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(e->toPlainText(), QString("isso **importa**"));
        QCOMPARE(e->textCursor().selectedText(), QString("importa"));
        key(e.get(), Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(e->toPlainText(), QString("isso importa"));
    }

    void markdownTaskClickToggles()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("título\n- [ ] tarefa");
        QTextCursor home(e->document());
        e->setTextCursor(home); // cursor on line 1, the task line is inactive
        QTextCursor box(e->document());
        box.setPosition(e->document()->findBlockByNumber(1).position() + 3); // the space inside [ ]
        const QPoint p = e->cursorRect(box).center() + QPoint(1, 0);
        QTest::mouseClick(e->viewport(), Qt::LeftButton, {}, p);
        QCOMPARE(e->toPlainText(), QString("título\n- [x] tarefa"));
        QCOMPARE(e->textCursor().position(), 0); // the click didn't move the cursor
        e->undo();
        QCOMPARE(e->toPlainText(), QString("título\n- [ ] tarefa"));
    }

    void markdownHidesMarkersOffTheCursorLine()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("# Título\ntexto");
        auto markerHidden = [&]() {
            const QTextBlock b = e->document()->firstBlock();
            for (const auto &r : b.layout()->formats())
                if (r.start == 0 && r.format.foreground().color().alpha() == 0)
                    return true;
            return false;
        };
        QTextCursor c(e->document());
        c.setPosition(e->document()->findBlockByNumber(1).position());
        e->setTextCursor(c);
        QVERIFY(markerHidden());
        c.setPosition(3);
        e->setTextCursor(c);
        QVERIFY(!markerHidden());
    }

    void tableTabAlignsAndMoves()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("| a | bb |\n|-|-|\n| ccc | d |");
        QTextCursor c(e->document());
        c.setPosition(3); // in "a"
        e->setTextCursor(c);
        key(e.get(), Qt::Key_Tab);
        QCOMPARE(e->toPlainText(), QString("| a   | bb  |\n|-----|-----|\n| ccc | d   |"));
        QCOMPARE(e->textCursor().block().blockNumber(), 0);
        QCOMPARE(e->textCursor().positionInBlock(), 10); // end of "bb"
        key(e.get(), Qt::Key_Tab);                       // skips the separator row
        QCOMPARE(e->textCursor().block().blockNumber(), 2);
        key(e.get(), Qt::Key_Tab);
        key(e.get(), Qt::Key_Tab); // past the last cell: new row
        QCOMPARE(e->document()->blockCount(), 4);
        QCOMPARE(e->textCursor().block().text(), QString("|     |     |"));
        type(e.get(), "x");
        key(e.get(), Qt::Key_Backtab);
        QCOMPARE(e->textCursor().block().blockNumber(), 2);
        e->undo(); // first the realignment done by Shift+Tab...
        QVERIFY(e->toPlainText().contains('x'));
        e->undo(); // ...then the typed "x"
        QVERIFY(!e->toPlainText().contains('x'));
    }

    void tableHeaderEnterAddsSeparator()
    {
        std::unique_ptr<Editor> e(mdEditor());
        type(e.get(), "| nome | idade |\n");
        QCOMPARE(e->toPlainText(), QString("| nome | idade |\n|------|-------|\n|      |       |"));
        QCOMPARE(e->textCursor().block().blockNumber(), 2);
        type(e.get(), "ana\t30\n");
        QCOMPARE(e->toPlainText(),
                 QString("| nome | idade |\n|------|-------|\n| ana  | 30    |\n|      |       |"));
        key(e.get(), Qt::Key_Return); // Enter on an empty row leaves the table
        QCOMPARE(e->toPlainText(), QString("| nome | idade |\n|------|-------|\n| ana  | 30    |\n"));
    }

    void tableDeletingAndLeaving()
    {
        std::unique_ptr<Editor> e(mdEditor());
        const QString table = "| a   | b   |\n|-----|-----|\n| um  | 1   |\n| dois| 2   |";
        auto put = [&](int line, int col) {
            QTextCursor c(e->document());
            c.setPosition(e->document()->findBlockByNumber(line).position() + col);
            e->setTextCursor(c);
        };
        // Ctrl+Backspace deletes the row, cursor stays in the same column one row up
        e->loadContent(table);
        put(3, 9);
        key(e.get(), Qt::Key_Backspace, Qt::ControlModifier);
        QCOMPARE(e->toPlainText(), QString("| a   | b   |\n|-----|-----|\n| um  | 1   |"));
        QCOMPARE(e->textCursor().block().blockNumber(), 2);
        QCOMPARE(e->textCursor().positionInBlock(), 9); // end of "1"
        // first data row: the separator is skipped, the cursor goes to the row that moved up
        e->loadContent(table);
        put(2, 3);
        key(e.get(), Qt::Key_Backspace, Qt::ControlModifier);
        QCOMPARE(e->textCursor().block().text(), QString("| dois| 2   |"));
        e->undo();
        QCOMPARE(e->toPlainText(), table);

        // Backspace right after the first | of an empty row removes the row
        e->loadContent("| a | b |\n|---|---|\n|   |   |");
        put(2, 1);
        key(e.get(), Qt::Key_Backspace);
        QCOMPARE(e->toPlainText(), QString("| a | b |\n|---|---|"));
        QCOMPARE(e->textCursor().position(), e->document()->characterCount() - 1);
        // ...but not when the row has content
        e->loadContent("| a | b |\n|---|---|\n| x |   |");
        put(2, 1);
        key(e.get(), Qt::Key_Backspace);
        QCOMPARE(e->toPlainText(), QString("| a | b |\n|---|---|\n x |   |"));

        // Ctrl+Enter aligns the table and continues on the line after it
        e->loadContent("| a | bb |\n|-|-|\n| c | d |\ndepois");
        put(0, 2);
        key(e.get(), Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(e->toPlainText(), QString("| a   | bb  |\n|-----|-----|\n| c   | d   |\n\ndepois"));
        QCOMPARE(e->textCursor().block().blockNumber(), 3);
        e->loadContent("| a | b |\n|---|---|\n\nfim");
        put(0, 2);
        key(e.get(), Qt::Key_Return, Qt::ControlModifier); // an empty line already follows: reuse it
        QCOMPARE(e->toPlainText(), QString("| a   | b   |\n|-----|-----|\n\nfim"));
        QCOMPARE(e->textCursor().block().blockNumber(), 2);
    }

    void tableCtrlArrowsMoveByCell()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("| a   | bb  |\n|-----|-----|\n| um  | 2   |\n| tres| 4   |\nfora");
        QTextCursor c(e->document());
        c.setPosition(2); // in "a"
        e->setTextCursor(c);
        auto at = [&] { return qMakePair(e->textCursor().block().blockNumber(), e->textCursor().positionInBlock()); };
        key(e.get(), Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(at(), qMakePair(0, 10)); // end of "bb"
        key(e.get(), Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(at(), qMakePair(2, 4)); // next row (separator skipped), end of "um"
        key(e.get(), Qt::Key_Left, Qt::ControlModifier);
        QCOMPARE(at(), qMakePair(0, 10)); // back to the last cell of the header
        key(e.get(), Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(at(), qMakePair(2, 9)); // same column, end of "2"
        key(e.get(), Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(at(), qMakePair(3, 9));
        key(e.get(), Qt::Key_Right, Qt::ControlModifier); // last cell of the table: stays
        QCOMPARE(at(), qMakePair(3, 9));
        QCOMPARE(e->toPlainText(), QString("| a   | bb  |\n|-----|-----|\n| um  | 2   |\n| tres| 4   |\nfora"));
    }

    void codeFenceClosesAndHighlights()
    {
        std::unique_ptr<Editor> e(mdEditor());
        type(e.get(), "```python\n");
        QCOMPARE(e->toPlainText(), QString("```python\n\n```"));
        QCOMPARE(e->textCursor().block().blockNumber(), 1);
        type(e.get(), "- def f():\n");
        // inside code: no list continuation, just the line break
        QCOMPARE(e->toPlainText(), QString("```python\n- def f():\n\n```"));
        const QTextBlock code = e->document()->findBlockByNumber(1);
        QVERIFY(MarkdownHighlighter::inFence(code.previous().userState()));
        QCOMPARE(code.next().next().userState(), int(MarkdownHighlighter::FenceClose));
        bool keywordBold = false;
        for (const auto &r : code.layout()->formats())
            if (r.start == 2 && r.length == 3 && r.format.fontWeight() == QFont::Bold)
                keywordBold = true; // "def"
        QVERIFY(keywordBold);
        // a second ``` that closes an existing block doesn't get auto-closed
        e->loadContent("```\ncodigo");
        QTextCursor c(e->document());
        c.movePosition(QTextCursor::End);
        e->setTextCursor(c);
        type(e.get(), "\n```\n");
        QCOMPARE(e->toPlainText(), QString("```\ncodigo\n```\n"));
    }

    void rulesAndHeadingsAreClassified()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("# um\n## dois\n### tres\n---\ntexto");
        auto st = [&](int n) { return e->document()->findBlockByNumber(n).userState(); };
        QCOMPARE(MarkdownHighlighter::headingLevel(st(0)), 1);
        QCOMPARE(MarkdownHighlighter::headingLevel(st(1)), 2);
        QCOMPARE(MarkdownHighlighter::headingLevel(st(2)), 3);
        QCOMPARE(st(3), int(MarkdownHighlighter::Rule));
        QCOMPARE(st(4), int(MarkdownHighlighter::Normal));
    }

    void tabTitles()
    {
        AppSettings s = settings();
        s.notesDir = m_tmp.path() + "/titulos";
        s.markdown = true;
        MainWindow w(s);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *tabs = w.findChild<QTabBar *>();
        auto titleOf = [&](const QString &text) {
            w.handleCommand(QJsonObject{{"texts", QJsonArray{text}}});
            return tabs->tabText(tabs->currentIndex());
        };
        QCOMPARE(titleOf("| **item** | qtd |\n|---|---|\n| sal | 1 |"), QString("tabela - item"));
        QCOMPARE(titleOf("|  | qtd |\n|---|---|"), QString("tabela"));
        QCOMPARE(titleOf("## Compras *da semana*"), QString("Compras da semana"));
        QCOMPARE(titleOf("- [ ] ligar pro banco"), QString("ligar pro banco"));
    }

    void formattingIsNotAnEdit()
    {
        std::unique_ptr<Editor> e(mdEditor());
        e->loadContent("# a\n**b**\n- c");
        QSignalSpy spy(e.get(), &Editor::edited);
        for (int i = 0; i < 3; ++i)
            key(e.get(), Qt::Key_Down);
        key(e.get(), Qt::Key_Up);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!e->historyDirty);
        type(e.get(), "x");
        QCOMPARE(spy.count(), 1);
    }

    void undoSurvivesRestartAndClosedTabsReopen()
    {
        const AppSettings s = settings();
        {
            MainWindow w(s);
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            Editor *ed = w.findChildren<Editor *>().value(0);
            QVERIFY(ed);
            ed->setFocus();
            type(ed, "lista\ncafe");
            w.saveEverything();
        }
        {
            MainWindow w(s); // "restart"
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            Editor *ed = w.findChildren<Editor *>().value(0);
            QVERIFY(ed);
            QCOMPARE(ed->toPlainText(), QString("lista\ncafe"));
            ed->setFocus();
            key(ed, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(ed->toPlainText(), QString("lista\n"));
            key(ed, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            QCOMPARE(ed->toPlainText(), QString("lista\ncafe"));
            w.saveEverything();
        }

        // close many tabs, then reopen all of them with Ctrl+Shift+T (no limit), history included
        const int n = 120;
        {
            MainWindow w(s);
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            w.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&w));
            for (int i = 0; i < n; ++i) {
                QJsonObject cmd{{"texts", QJsonArray{QString("nota %1").arg(i)}}};
                w.handleCommand(cmd);
            }
            auto *tabs = w.findChild<QTabBar *>();
            QCOMPARE(tabs->count(), n + 1);
            for (int i = 0; i < n + 1; ++i) {
                QTest::keyClick(&w, Qt::Key_W, Qt::ControlModifier);
                QTest::qWait(1);
            }
            QCOMPARE(tabs->count(), 1); // a fresh empty tab replaces the last one
            for (int i = 0; i < n + 1; ++i) {
                QTest::keyClick(&w, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
                QTest::qWait(1);
            }
            QCOMPARE(tabs->count(), n + 2);
            w.saveEverything();

            // the very first note came back with its undo history
            Editor *lista = nullptr;
            for (Editor *e : w.findChildren<Editor *>())
                if (e->toPlainText() == "lista\ncafe")
                    lista = e;
            QVERIFY(lista);
            lista->undo();
            QCOMPARE(lista->toPlainText(), QString("lista\n"));
        }
    }
};

QTEST_MAIN(TestNotas)
#include "tst_notas.moc"
