#include "MarkdownIO.h"
#include "NoteDocument.h"

#include <QGuiApplication>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextList>

using namespace zn;

namespace {

QString roundTrip(const QString& md) {
    NoteDocument doc;
    importMarkdown(doc, md);
    return exportMarkdown(doc);
}

}  // namespace

class TestMarkdown : public QObject {
    Q_OBJECT
private slots:
    void canonicalRoundTripIsStable() {
        const QString md = QStringLiteral(
            "# Title\n"
            "\n"
            "## Heading\n"
            "\n"
            "### Subheading\n"
            "\n"
            "Body with **bold**, *italic*, ~~strike~~, <u>under</u>, ==mark==, `code` and a [link](https://example.com).\n"
            "\n"
            "- bullet one\n"
            "- bullet two\n"
            "  - nested\n"
            "* dashed\n"
            "1. first\n"
            "2. second\n"
            "\n"
            "> quote line\n"
            ">\n"
            "> second para\n"
            "\n"
            "```cpp\n"
            "int main() {\n"
            "\n"
            "    return 0;\n"
            "}\n"
            "```\n"
            "\n"
            "```mermaid\n"
            "graph TD; A-->B;\n"
            "```\n"
            "\n"
            "| a | b |\n"
            "| --- | ---: |\n"
            "| 1 | **2** |\n"
            "\n"
            "&nbsp;\n"
            "\n"
            "Last paragraph with snake_case and 2\\*3.\n");
        const QString out = roundTrip(md);
        if (out != md)
            qInfo().noquote() << "---- actual\n" << out << "---- end";
        QCOMPARE(out, md);
        QCOMPARE(roundTrip(out), md);
    }

    void checklistMarkers() {
        const QString md = QStringLiteral("Groceries\n\n- [ ] milk\n- [x] bread\n\nafter\n");
        NoteDocument doc;
        importMarkdown(doc, md);
        int checked = 0, unchecked = 0;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (b.blockFormat().marker() == QTextBlockFormat::MarkerType::Checked)
                ++checked;
            if (b.blockFormat().marker() == QTextBlockFormat::MarkerType::Unchecked)
                ++unchecked;
        }
        QCOMPARE(checked, 1);
        QCOMPARE(unchecked, 1);  // Qt leaks the marker into "after"; import must clear it
        QCOMPARE(exportMarkdown(doc), md);
    }

    void diagramsBecomeObjects() {
        NoteDocument doc;
        importMarkdown(doc, QStringLiteral("Arch\n\n```mermaid\nsequenceDiagram\n  A->>B: hi\n```\n\n~~~plantuml\n@startuml\nA -> B\n@enduml\n~~~\n"));
        QCOMPARE(doc.diagramIds().size(), 2);
        int objects = 0;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            objects += static_cast<int>(b.text().count(QChar(0xFFFC)));
        QCOMPARE(objects, 2);
        QCOMPARE(exportMarkdown(doc), QStringLiteral("Arch\n\n```mermaid\nsequenceDiagram\n  A->>B: hi\n```\n\n"
                                                     "```plantuml\n@startuml\nA -> B\n@enduml\n```\n"));
    }

    void editedDiagramSourceIsExported() {
        NoteDocument doc;
        importMarkdown(doc, QStringLiteral("```mermaid\ngraph TD; A-->B;\n```\n"));
        doc.setDiagramSource(doc.diagramIds().first(), QStringLiteral("graph LR; X-->Y;"));
        QCOMPARE(exportMarkdown(doc), QStringLiteral("```mermaid\ngraph LR; X-->Y;\n```\n"));
    }

    void escapesBlockSyntaxInText() {
        NoteDocument doc;
        QTextCursor c(&doc);
        c.insertText(QStringLiteral("# not a heading"));
        c.insertBlock();
        c.insertText(QStringLiteral("1. not a list [x] <b> a*b ~~ == snake_case _lead"));
        c.insertBlock();
        c.insertText(QStringLiteral("- not a bullet"));
        const QString md = exportMarkdown(doc);
        QCOMPARE(md, QStringLiteral("\\# not a heading\n\n1\\. not a list \\[x\\] \\<b> a\\*b \\~\\~ \\=\\= snake_case \\_lead\n\n"
                                    "\\- not a bullet\n"));
        NoteDocument back;
        importMarkdown(back, md);
        QCOMPARE(back.begin().text(), QStringLiteral("# not a heading"));
        QCOMPARE(back.begin().next().text(), QStringLiteral("1. not a list [x] <b> a*b ~~ == snake_case _lead"));
    }

    void markersHugText() {
        NoteDocument doc;
        QTextCursor c(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        c.insertText(QStringLiteral("say "), QTextCharFormat());
        c.insertText(QStringLiteral("bold "), bold);
        c.insertText(QStringLiteral("text"), QTextCharFormat());
        QCOMPARE(exportMarkdown(doc), QStringLiteral("say **bold** text\n"));
    }

    void nestedEmphasis() {
        NoteDocument doc;
        QTextCursor c(&doc);
        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);
        QTextCharFormat both = bold;
        both.setFontItalic(true);
        c.insertText(QStringLiteral("a"), bold);
        c.insertText(QStringLiteral("b"), both);
        c.insertText(QStringLiteral(" c"), bold);
        QCOMPARE(exportMarkdown(doc), QStringLiteral("**a*b* c**\n"));
    }

    void emptyParagraphsSurvive() {
        const QString md = QStringLiteral("one\n\n&nbsp;\n\n&nbsp;\n\ntwo\n");
        NoteDocument doc;
        importMarkdown(doc, md);
        QCOMPARE(doc.blockCount(), 4);
        QCOMPARE(exportMarkdown(doc), md);
    }

    void dashedListUsesUndefinedStyle() {
        NoteDocument doc;
        importMarkdown(doc, QStringLiteral("* a\n* b\n"));
        QTextList* list = doc.begin().textList();
        QVERIFY(list);
        QVERIFY(list->format().boolProperty(kDashedListProperty));
        QCOMPARE(doc.paragraphStyle(doc.begin()), ParagraphStyle::Dashed);
    }

    void excalidrawFileLink() {
        const QString md = QStringLiteral("Sketch\n\n![Drawing](zn-file://0b1c-2d)\n");
        NoteDocument doc;
        importMarkdown(doc, md);
        QCOMPARE(exportMarkdown(doc), md);
    }

    void hardLineBreakInList() {
        const QString md = QStringLiteral("- first line\\\n  second line\n- next\n");
        QCOMPARE(roundTrip(md), md);
    }
};

QTEST_MAIN(TestMarkdown)
#include "test_markdown.moc"
