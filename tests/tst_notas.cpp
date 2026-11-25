// Run: build/tests/tst_notas -platform offscreen
#include "editor.h"
#include "mainwindow.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTabBar>
#include <QTemporaryDir>
#include <QtTest>

static void type(QWidget *w, const QString &text)
{
    for (const QChar c : text) {
        if (c == '\n')
            QTest::keyClick(w, Qt::Key_Return);
        else
            QTest::keyClicks(w, QString(c));
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
