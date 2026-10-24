#include "MarkdownIO.h"
#include "NoteDocument.h"
#include "NoteEditor.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>

using namespace zn;

class TestEditor : public QObject {
    Q_OBJECT
    NoteEditor* editor = nullptr;
    NoteDocument* doc = nullptr;

    QString markdown() const { return exportMarkdown(*doc); }

private slots:
    void init() {
        editor = new NoteEditor;
        doc = new NoteDocument(editor);
        editor->setNoteDocument(doc);
        editor->resize(700, 500);
        editor->show();
        QVERIFY(QTest::qWaitForWindowExposed(editor));
        editor->setFocus();
    }

    void cleanup() {
        delete editor;
        editor = nullptr;
    }

    void markdownShortcutsWhileTyping() {
        QTest::keyClicks(editor, "# Trip");
        QCOMPARE(doc->paragraphStyle(doc->begin()), ParagraphStyle::Title);
        QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(doc->paragraphStyle(editor->textCursor().block()), ParagraphStyle::Body);  // body after a title
        QTest::keyClicks(editor, "- passport");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClicks(editor, "tickets");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClick(editor, Qt::Key_Return);  // empty item ends the list
        QTest::keyClicks(editor, "[] book hotel");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClicks(editor, "pack");
        QCOMPARE(markdown(), QStringLiteral("# Trip\n\n- passport\n- tickets\n- [ ] book hotel\n- [ ] pack\n"));
    }

    void dashedNumberedQuote() {
        QTest::keyClicks(editor, "* one");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClicks(editor, "1. first");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClicks(editor, "second");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClicks(editor, "> wise words");
        QCOMPARE(markdown(), QStringLiteral("* one\n1. first\n2. second\n\n> wise words\n"));
    }

    void tabIndentsListItems() {
        QTest::keyClicks(editor, "- parent");
        QTest::keyClick(editor, Qt::Key_Return);
        QTest::keyClicks(editor, "child");
        QTest::keyClick(editor, Qt::Key_Tab);
        QCOMPARE(markdown(), QStringLiteral("- parent\n  - child\n"));
        QTest::keyClick(editor, Qt::Key_Backtab);
        QCOMPARE(markdown(), QStringLiteral("- parent\n- child\n"));
    }

    void checklistClickToggles() {
        importMarkdown(*doc, QStringLiteral("- [ ] water plants\n"));
        editor->setNoteDocument(doc);
        QApplication::processEvents();
        const QTextBlock b = doc->begin();
        const QRectF br = doc->documentLayout()->blockBoundingRect(b);
        const QFontMetrics fm(b.charFormat().font());
        const qreal indent = b.blockFormat().leftMargin() + b.textList()->format().indent() * 40;
        const QPoint click(int(br.left() + indent - fm.height() / 2.0) - editor->horizontalScrollBar()->value(),
                           int(br.top() + fm.height() / 2.0) - editor->verticalScrollBar()->value());
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, click);
        QCOMPARE(markdown(), QStringLiteral("- [x] water plants\n"));
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, click);
        QCOMPARE(markdown(), QStringLiteral("- [ ] water plants\n"));
    }

    void checklistClickWithCaretInItemAfterHover() {
        // Qt also toggles checkboxes on release, in the block holding the caret; with the caret in the item
        // that cancelled our toggle. Hover first, as a real mouse does.
        importMarkdown(*doc, QStringLiteral("Todo\n\n- [ ] first\n- [ ] second\n"));
        editor->setNoteDocument(doc);
        QApplication::processEvents();
        QTextBlock second = doc->begin();
        while (second.isValid() && second.text() != u"second")
            second = second.next();
        QVERIFY(second.isValid());
        QTextCursor caret(second);
        caret.movePosition(QTextCursor::EndOfBlock);
        editor->setTextCursor(caret);
        const QRectF br = doc->documentLayout()->blockBoundingRect(second);
        const QFontMetrics fm(second.charFormat().font());
        const qreal indent = second.blockFormat().leftMargin() + second.textList()->format().indent() * 40;
        const QPoint click(int(br.left() + indent - fm.height() / 2.0) - editor->horizontalScrollBar()->value(),
                           int(br.top() + fm.height() / 2.0) - editor->verticalScrollBar()->value());
        QTest::mouseMove(editor->viewport(), click);
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, click);
        QCOMPARE(markdown(), QStringLiteral("Todo\n\n- [ ] first\n- [x] second\n"));
        // the caret moves to another item: still only the clicked one changes
        editor->setTextCursor(QTextCursor(doc->begin()));
        QTest::mouseMove(editor->viewport(), click + QPoint(1, 0));
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, {}, click);
        QCOMPARE(markdown(), QStringLiteral("Todo\n\n- [ ] first\n- [ ] second\n"));
    }

    void switchingDocumentsAfterHoveringAChecklist() {
        // QTextEdit remembers the checklist block under the mouse; it must not outlive its document.
        // Reproduces deterministically with GLIBC_TUNABLES=glibc.malloc.tcache_count=0 MALLOC_PERTURB_=165.
        auto* first = new NoteDocument;
        importMarkdown(*first, QStringLiteral("- [ ] item\n"));
        editor->setNoteDocument(first);
        QApplication::processEvents();
        const QTextBlock b = first->begin();
        const QRectF br = first->documentLayout()->blockBoundingRect(b);
        const QFontMetrics fm(b.charFormat().font());
        const qreal indent = b.blockFormat().leftMargin() + b.textList()->format().indent() * 40;
        const QPoint marker(int(br.left() + indent - fm.height() / 2.0), int(br.top() + fm.height() / 2.0));
        QVERIFY(first->documentLayout()->blockWithMarkerAt(QPointF(marker)).isValid());
        QTest::mouseMove(editor->viewport(), marker);
        QTest::mouseMove(editor->viewport(), marker + QPoint(1, 1));
        editor->setNoteDocument(doc);
        delete first;
        QTest::mouseMove(editor->viewport(), marker + QPoint(2, 2));
        QTest::mouseMove(editor->viewport(), QPoint(50, 50));
    }

    void selectedTableIsDeleted() {
        QTest::keyClicks(editor, "before");
        editor->insertTable(2, 2);
        QTest::keyClicks(editor, "cell");
        QCOMPARE(doc->rootFrame()->childFrames().size(), 1);
        auto* table = qobject_cast<QTextTable*>(doc->rootFrame()->childFrames().first());
        QTextCursor sel(doc);
        sel.setPosition(table->firstPosition());
        sel.setPosition(table->lastPosition(), QTextCursor::KeepAnchor);
        editor->setTextCursor(sel);
        QTest::keyClick(editor, Qt::Key_Backspace);
        QVERIFY(doc->rootFrame()->childFrames().isEmpty());
        QVERIFY(!markdown().contains(u'|'));
    }

    void backspaceAfterTableSelectsThenDeletes() {
        QTest::keyClicks(editor, "before");
        editor->insertTable(2, 2);
        auto* table = qobject_cast<QTextTable*>(doc->rootFrame()->childFrames().first());
        QTextCursor after(doc);
        after.setPosition(table->lastPosition() + 1);  // the block right after the table
        editor->setTextCursor(after);
        QTest::keyClick(editor, Qt::Key_Backspace);
        QVERIFY(editor->textCursor().hasComplexSelection());
        QTest::keyClick(editor, Qt::Key_Backspace);
        QVERIFY(doc->rootFrame()->childFrames().isEmpty());
    }

    void inlineToggles() {
        QTest::keyClicks(editor, "make this bold");
        QTextCursor c = editor->textCursor();
        c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, 4);
        editor->setTextCursor(c);
        editor->toggleBold();
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
        editor->setTextCursor(c);
        editor->toggleHighlight();
        QCOMPARE(markdown(), QStringLiteral("==make== this **bold**\n"));
    }

    void insertsDiagramAndTable() {
        QTest::keyClicks(editor, "Arch");
        editor->insertDiagram(DiagramKind::Mermaid, QStringLiteral("graph TD; A-->B;"));
        QTest::keyClicks(editor, "after");
        QCOMPARE(markdown(), QStringLiteral("Arch\n\n```mermaid\ngraph TD; A-->B;\n```\n\nafter\n"));
        QCOMPARE(editor->diagramAtCursor(), QString());
    }

    void pasteMarkdown() {
        auto* mime = new QMimeData;
        mime->setText(QStringLiteral("## Pasted\n\n- a\n- b\n"));
        QApplication::clipboard()->setMimeData(mime);
        editor->paste();
        QVERIFY2(markdown().startsWith(QStringLiteral("## Pasted\n\n- a\n- b")), qPrintable(markdown()));
    }

    void pasteHtmlKeepsStructureOnly() {
        auto* mime = new QMimeData;
        mime->setHtml(QStringLiteral("<h1 style='color:red;font-family:Comic Sans MS'>Big</h1><p>plain <b>bold</b></p>"));
        QApplication::clipboard()->setMimeData(mime);
        editor->paste();
        const QString md = markdown();
        QVERIFY2(md.contains(QStringLiteral("# Big")) && md.contains(QStringLiteral("plain **bold**")), qPrintable(md));
        QVERIFY(!md.contains(u"Comic"));
    }

    void searchHighlights() {
        importMarkdown(*doc, QStringLiteral("# Café menu\n\nCafé au lait, cafeteria\n"));
        editor->setNoteDocument(doc);
        MatchSet m;
        m.prefixes = {QStringLiteral("cafe")};
        editor->setSearchMatches(m, true);
        QCOMPARE(editor->extraSelections().size(), 3);
        QCOMPARE(editor->extraSelections().first().cursor.selectedText(), QStringLiteral("Café"));
    }
};

QTEST_MAIN(TestEditor)
#include "test_editor.moc"
