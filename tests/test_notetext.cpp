#include "NoteText.h"
#include "SearchQuery.h"
#include "TextNormalizer.h"

#include <QTest>

using namespace zn;

class TestNoteText : public QObject {
    Q_OBJECT
private slots:
    void foldCaseAndDiacritics() {
        QCOMPARE(text::fold(u"Café ÉTÉ"), QStringLiteral("cafe ete"));
        QCOMPARE(text::fold(u"Ёлка ЗАМЕТКА"), QStringLiteral("елка заметка"));
        QCOMPARE(text::fold(u"Йогурт"), QStringLiteral("йогурт"));  // й stays a letter of its own
        QCOMPARE(text::fold(u"Straße ﬁle"), QStringLiteral("strasse file"));
    }

    void normalizedMapsBackToSource() {
        const QString src = QStringLiteral("Über ﬁx");
        const auto n = text::normalize(src);
        QCOMPARE(n.text, QStringLiteral("uber fix"));
        // "fix" in normalized text starts at 5 and comes from "ﬁx" at 5..7 in the source
        const auto [s, e] = n.sourceRange(5, 8, static_cast<int>(src.size()));
        QCOMPARE(src.mid(s, e - s), QStringLiteral("ﬁx"));
        // half of the ligature still selects the whole source character
        const auto [s2, e2] = n.sourceRange(5, 6, static_cast<int>(src.size()));
        QCOMPARE(src.mid(s2, e2 - s2), QStringLiteral("ﬁ"));
    }

    void decomposedInputHighlightsWholeCharacter() {
        const QString src = QStringLiteral("café noir");  // e + combining acute
        const auto n = text::normalize(src);
        QCOMPARE(n.text, QStringLiteral("cafe noir"));
        const auto [s, e] = n.sourceRange(0, 4, static_cast<int>(src.size()));
        QCOMPARE(src.mid(s, e - s), QStringLiteral("café"));
    }

    void tokenizeWordsAndCjk() {
        QCOMPARE(text::words(u"Hello, мир! e-mail 2026"), (QStringList{"hello", "мир", "e", "mail", "2026"}));
        QCOMPARE(text::words(u"東京タワー"), (QStringList{"東", "京", "タ", "ワ", "ー"}));
    }

    void digestTitleSnippetBody() {
        const auto d = digestMarkdown(QStringLiteral("# Trip to **Kyoto**\n\nBook the *ryokan* and [train](https://jr.jp).\n\n"
                                                     "## Packing\n- [ ] passport\n- [x] charger\n"));
        QCOMPARE(d.title, QStringLiteral("Trip to Kyoto"));
        QCOMPARE(d.snippet, QStringLiteral("Book the ryokan and train. Packing passport charger"));
        QCOMPARE(d.headings, QStringLiteral("Packing"));
        QCOMPARE(d.checklistOpen, 1);
        QCOMPARE(d.checklistDone, 1);
        QVERIFY(d.hasLink);
        QVERIFY(!d.body.contains(u"Kyoto"));  // title is a field of its own
    }

    void digestDiagramsAndCode() {
        const auto d = digestMarkdown(QStringLiteral("Arch\n\n```mermaid\ngraph TD; Api-->Db;\n```\n\n```cpp\nint main();\n```\n"
                                                     "~~~plantuml\nAlice -> Bob: hi\n~~~\n"));
        QCOMPARE(d.diagrams.size(), 2);
        QCOMPARE(d.diagrams[0].kind, DiagramKind::Mermaid);
        QCOMPARE(d.diagrams[0].source, QStringLiteral("graph TD; Api-->Db;"));
        QCOMPARE(d.diagrams[1].kind, DiagramKind::PlantUml);
        QCOMPARE(d.code, QStringLiteral("int main();"));
        QVERIFY(d.hasCode);
        QVERIFY(d.body.isEmpty());
    }

    void digestTags() {
        const auto d = digestMarkdown(QStringLiteral("Note\n#work and #Работа/план, not #123, `#code`, http://x.y/#frag, a#b #work\n"));
        QCOMPARE(d.tags, (QStringList{"work", "Работа/план"}));
    }

    void digestTable() {
        const auto d = digestMarkdown(QStringLiteral("T\n\n| Name | Qty |\n|---|---:|\n| apple | 3 |\n"));
        QVERIFY(d.hasTable);
        QVERIFY(d.body.contains(u"apple"));
        QVERIFY(!d.body.contains(u"---"));
    }

    void digestFileRefs() {
        const auto d = digestMarkdown(QStringLiteral("x\n\n![Drawing](zn-file://1f0e-ab)\n"));
        QCOMPARE(d.fileRefs, QStringList{"1f0e-ab"});
        QVERIFY(!d.hasLink);
    }

    void stripInlineKeepsSnakeCase() {
        QCOMPARE(stripInline(u"use my_var in **bold** and _it_ ==hl== <u>u</u> \\*lit\\*"),
                 QStringLiteral("use my_var in bold and it hl u *lit*"));
    }

    void parseQuery() {
        const QDate today(2026, 10, 24);
        auto q = SearchQuery::parse(QStringLiteral(R"(Meet "road map" -draft #Work folder:"Big Plans" has:todo is:pinned after:week)"), today);
        QCOMPARE(q.terms.size(), 3);
        QCOMPARE(q.terms[0].words, QStringList{"meet"});
        QVERIFY(q.terms[1].phrase);
        QCOMPARE(q.terms[1].words, (QStringList{"road", "map"}));
        QVERIFY(q.terms[2].negated);
        QCOMPARE(q.tags, QStringList{"work"});
        QCOMPARE(q.folderNames, QStringList{"big plans"});
        QVERIFY(q.has.contains("todo"));
        QVERIFY(q.pinnedOnly);
        QCOMPARE(q.modifiedAfter, QDate(2026, 10, 17));
    }

    void parseQueryPlainColonIsText() {
        auto q = SearchQuery::parse(QStringLiteral("ratio 16:9 http://x"));
        QVERIFY(q.hasPositiveTerms());
        QCOMPARE(q.terms.size(), 3);
        QVERIFY(!q.hasFilters());
    }
};

QTEST_GUILESS_MAIN(TestNoteText)
#include "test_notetext.moc"
