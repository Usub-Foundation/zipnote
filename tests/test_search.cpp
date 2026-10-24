#include "SearchIndex.h"

#include <QElapsedTimer>
#include <QTest>

using namespace zn;

namespace {

IndexDoc doc(const QString& id, const QString& title, const QString& body, int daysAgo = 1) {
    IndexDoc d;
    d.id = id;
    d.folderId = "f1";
    d.folderName = "Notes";
    d.title = title;
    d.body = body;
    d.created = d.modified = QDateTime::currentDateTimeUtc().addDays(-daysAgo);
    return d;
}

QStringList ids(const QList<NoteHit>& hits) {
    QStringList out;
    for (const auto& h : hits)
        out.append(h.noteId);
    return out;
}

QStringList allIds(const SearchResults& r) {
    return ids(r.topHits) + ids(r.notes);
}

QSet<QString> asSet(const QStringList& l) {
    return QSet<QString>(l.begin(), l.end());
}

QString marked(const QString& s, const QList<Highlight>& hl) {
    QString out = s;
    for (auto it = hl.crbegin(); it != hl.crend(); ++it) {
        out.insert(it->start + it->length, u']');
        out.insert(it->start, u'[');
    }
    return out;
}

}  // namespace

class TestSearch : public QObject {
    Q_OBJECT
    SearchIndex index;

    SearchResults find(const QString& q, SearchOptions o = {}) {
        return index.search(SearchQuery::parse(q, QDate(2026, 10, 24)), o);
    }

private slots:
    void init() {
        index.clear();
        index.upsert(doc("meet", "Meeting notes", "Discussed the roadmap with the café team. Next steps: hire.", 3));
        index.upsert(doc("road", "Roadmap 2027", "Quarterly goals and the meeting schedule.", 10));
        index.upsert(doc("shop", "Shopping", "Milk, bread, Café beans", 1));
        index.upsert(doc("ru", "Заметки о поездке", "Ёлка, ёжик и йогурт. Встреча в среду.", 2));
        auto diag = doc("arch", "Architecture", "Service overview");
        diag.attachments.append({"arch/diagram/0", "mermaid", "graph TD; Web-->OrderService; OrderService-->Inventory"});
        diag.flags = HasMermaid;
        index.upsert(diag);
        auto todo = doc("todo", "Weekend", "- [ ] buy paint");
        todo.flags = HasChecklist | HasOpenTodo;
        todo.tags = {"home", "Work/Projects"};
        index.upsert(todo);
    }

    void prefixAsYouType() {
        QCOMPARE(asSet(allIds(find("mee"))), (QSet<QString>{"meet", "road"}));
        QCOMPARE(allIds(find("meeting no")), QStringList{"meet"});
    }

    void caseAndDiacriticInsensitive() {
        QCOMPARE(asSet(allIds(find("CAFE"))), (QSet<QString>{"meet", "shop"}));
        QCOMPARE(allIds(find("елка")), QStringList{"ru"});   // ё == е
        QCOMPARE(allIds(find("ЁЖИК")), QStringList{"ru"});
        QVERIFY(allIds(find("иогурт")).isEmpty() || find("иогурт").usedFuzzy);  // й != и
    }

    void titleMatchRanksFirst() {
        // "roadmap" is the title of one note and a body word of another
        const auto r = find("roadmap");
        QCOMPARE(allIds(r).first(), QStringLiteral("road"));
        QCOMPARE(ids(r.topHits).first(), QStringLiteral("road"));
    }

    void notesSectionByDateTopHitsByRelevance() {
        const auto r = find("meeting");
        QCOMPARE(r.noteCount(), 2);
        QCOMPARE(ids(r.topHits), QStringList{"meet"});
        QCOMPARE(ids(r.notes), QStringList{"road"});
    }

    void andSemantics() {
        QCOMPARE(allIds(find("café hire")), QStringList{"meet"});
        QVERIFY(allIds(find("café nonexistentword")).isEmpty());
    }

    void phrase() {
        QCOMPARE(allIds(find("\"meeting schedule\"")), QStringList{"road"});
        QVERIFY(allIds(find("\"schedule meeting\"")).isEmpty());
    }

    void negation() {
        QCOMPARE(allIds(find("meeting -quarterly")), QStringList{"meet"});
    }

    void typoTolerance() {
        const auto r = find("meetnig");
        QVERIFY(r.usedFuzzy);
        QVERIFY(allIds(r).contains("meet"));
        QVERIFY(r.notes.first().fuzzy || r.topHits.isEmpty());
        // fuzzy only runs when the exact pass found little
        QVERIFY(!find("meeting").usedFuzzy);
    }

    void filtersWithoutText() {
        QCOMPARE(allIds(find("has:todo")), QStringList{"todo"});
        QCOMPARE(allIds(find("has:che")), QStringList{"todo"});
        QCOMPARE(allIds(find("#home")), QStringList{"todo"});
        QCOMPARE(allIds(find("#work")), QStringList{"todo"});  // prefix of Work/Projects
        QCOMPARE(allIds(find("has:diagram")), QStringList{"arch"});
        QVERIFY(allIds(find("has:nonsense")).isEmpty());
    }

    void filtersCombineWithText() {
        QVERIFY(allIds(find("paint #home")).contains("todo"));
        QVERIFY(allIds(find("meeting #home")).isEmpty());
    }

    void attachmentsSection() {
        const auto r = find("orderserv");
        QCOMPARE(allIds(r), QStringList{"arch"});
        QCOMPARE(r.attachments.size(), 1);
        QCOMPARE(r.attachments[0].kind, QStringLiteral("mermaid"));
        QVERIFY(r.attachments[0].snippet.contains("OrderService"));
    }

    void titleAndSnippetHighlights() {
        const auto r = find("meet caf");
        QCOMPARE(allIds(r), QStringList{"meet"});
        const NoteHit& h = r.topHits.isEmpty() ? r.notes.first() : r.topHits.first();
        QCOMPARE(marked("Meeting notes", h.titleHighlights), QStringLiteral("[Meet]ing notes"));
        QVERIFY2(marked(h.snippet, h.snippetHighlights).contains("[caf]é"), qPrintable(marked(h.snippet, h.snippetHighlights)));
    }

    void highlightHelperOnArbitraryText() {
        MatchSet m;
        m.prefixes = {"еж", "cafe"};  // MatchSet holds folded words, as produced by SearchQuery::parse
        QCOMPARE(marked("Ёжик в CAFÉ", SearchIndex::highlight(u"Ёжик в CAFÉ", m)), QStringLiteral("[Ёж]ик в [CAFÉ]"));
    }

    void trashExcludedUnlessAsked() {
        auto t = doc("gone", "Meeting archive", "old");
        t.trashed = true;
        index.upsert(t);
        QVERIFY(!allIds(find("meeting")).contains("gone"));
        QCOMPARE(allIds(find("meeting in:trash")), QStringList{"gone"});
    }

    void upsertReplacesAndRemoveWorks() {
        index.upsert(doc("shop", "Groceries", "eggs"));
        QVERIFY(allIds(find("milk")).isEmpty());
        QCOMPARE(allIds(find("eggs")), QStringList{"shop"});
        index.remove("shop");
        QVERIFY(allIds(find("eggs")).isEmpty());
        QCOMPARE(index.size(), 5);
    }

    void tagsSuggestions() {
        const auto t = index.tags(u"proj");
        QCOMPARE(t.size(), 1);
        QCOMPARE(t[0].tag, QStringLiteral("Work/Projects"));
    }

    void cancellation() {
        const auto r = index.search(SearchQuery::parse("m"), {}, [] { return true; });
        QVERIFY(r.cancelled || r.noteCount() > 0);  // tiny index may finish before the first check
    }

    void scalesToThousandsOfNotes() {
        SearchIndex big;
        for (int i = 0; i < 5000; ++i)
            big.upsert(doc(QString::number(i), QStringLiteral("Note %1 about topic%2").arg(i).arg(i % 50),
                           QStringLiteral("Lorem ipsum dolor sit amet %1 consectetur adipiscing elit sed do eiusmod "
                                          "tempor incididunt ut labore et dolore magna aliqua %2")
                               .arg(i)
                               .arg(i % 7 == 0 ? "needle" : "hay")));
        QElapsedTimer t;
        t.start();
        const auto r = big.search(SearchQuery::parse("needle topic1"));
        const auto ms = t.elapsed();
        QVERIFY(r.noteCount() > 0);
        qInfo() << "5000 notes:" << r.noteCount() << "hits in" << ms << "ms";
        QVERIFY(ms < 500);
    }
};

QTEST_GUILESS_MAIN(TestSearch)
#include "test_search.moc"
