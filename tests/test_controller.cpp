#include "VaultController.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace zn;

namespace {

constexpr int kUnlockTimeoutMs = 60000;  // Argon2id with production parameters

QStringList hitIds(const SearchResults& r) {
    QStringList out;
    for (const auto& h : r.topHits)
        out.append(h.noteId);
    for (const auto& h : r.notes)
        out.append(h.noteId);
    return out;
}

}  // namespace

class TestController : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString path;
    QString noteId;

    SearchResults search(VaultController& vc, const QString& q) {
        QSignalSpy spy(&vc, &VaultController::searchFinished);
        vc.search(SearchQuery::parse(q));
        if (!spy.wait(5000))
            return {};
        return spy.last().at(1).value<SearchResults>();
    }

    void open(VaultController& vc, const QString& password) {
        QSignalSpy opened(&vc, &VaultController::opened);
        vc.openVault(path, password);
        QVERIFY(opened.wait(kUnlockTimeoutMs));
    }

private slots:
    void initTestCase() {
        QVERIFY(crypto::init());
        path = dir.filePath("vault.zip");
    }

    void createWithSamples() {
        VaultController vc;
        QSignalSpy opened(&vc, &VaultController::opened);
        vc.createVault(path, "correct horse battery", true);
        QVERIFY(opened.wait(kUnlockTimeoutMs));
        QCOMPARE(vc.records().size(), 7);
        QCOMPARE(vc.folders().size(), 3);
        QVERIFY(vc.tags().size() >= 5);

        noteId = vc.createNote(vc.defaultFolderId(), "# Shopping\n\nOat milk and blueberries\n");
        vc.updateNoteContent(noteId, "# Shopping\n\nOat milk, blueberries and #errands\n");
        QVERIFY(vc.saveNow());
        vc.lock();
        QVERIFY(!vc.isOpen());
    }

    void wrongPassword() {
        VaultController vc;
        QSignalSpy failed(&vc, &VaultController::openFailed);
        vc.openVault(path, "nope");
        QVERIFY(failed.wait(kUnlockTimeoutMs));
        QCOMPARE(failed.first().first().toString(), QStringLiteral("wrong password"));
    }

    void reopenKeepsEditsAndSearches() {
        VaultController vc;
        open(vc, "correct horse battery");
        QCOMPARE(vc.records().size(), 8);
        QVERIFY(vc.record(noteId)->markdown.contains("#errands"));
        QCOMPARE(hitIds(search(vc, "bluebe")), QStringList{noteId});
        QCOMPARE(hitIds(search(vc, "#errands")), QStringList{noteId});

        const SearchResults hub = search(vc, "hub");  // only inside the Mermaid diagram
        QCOMPARE(hub.noteCount(), 1);
        QCOMPARE(hub.attachments.size(), 1);
        QCOMPARE(hub.attachments.first().kind, QStringLiteral("mermaid"));
        const SearchResults passport = search(vc, "passport");  // only inside the PlantUML diagram
        QCOMPARE(passport.noteCount(), 1);
        QCOMPARE(passport.attachments.size(), 1);
        QCOMPARE(passport.attachments.first().kind, QStringLiteral("plantuml"));

        // ё and е are the same letter for search: "елки" finds "ёлки" exactly; "елка" elsewhere only as a typo match
        const SearchResults yolki = search(vc, "елки");
        const QString first = hitIds(yolki).value(0);
        QVERIFY(vc.record(first)->digest.title.startsWith(u"Идеи"));
        QVERIFY(yolki.usedFuzzy);
    }

    void trashAndRestore() {
        VaultController vc;
        open(vc, "correct horse battery");
        vc.trashNote(noteId);
        QCOMPARE(vc.trashCount(), 1);
        QVERIFY(hitIds(search(vc, "blueberries")).isEmpty());
        QCOMPARE(hitIds(search(vc, "blueberries in:trash")), QStringList{noteId});
        vc.restoreNote(noteId);
        QCOMPARE(vc.trashCount(), 0);
        QCOMPARE(hitIds(search(vc, "blueberries")), QStringList{noteId});
    }

    void drawingTextIsSearchable() {
        VaultController vc;
        open(vc, "correct horse battery");
        const QString fileId = vc.addFile(noteId, "excalidraw", "application/json", "{\"elements\":[]}", "Whiteboard roadmap sketch");
        vc.updateNoteContent(noteId, vc.record(noteId)->markdown + QStringLiteral("\n![Drawing](zn-file://%1)\n").arg(fileId));
        const SearchResults r = search(vc, "whiteboard");
        QCOMPARE(hitIds(r), QStringList{noteId});
        QCOMPARE(r.attachments.size(), 1);
        QCOMPARE(r.attachments.first().kind, QStringLiteral("excalidraw"));
        QCOMPARE(search(vc, "has:drawing").noteCount(), 1);
        QVERIFY(vc.saveNow());
    }

    void unreferencedFilesAreCollected() {
        QString orphan;
        {
            VaultController vc;
            open(vc, "correct horse battery");
            vc.storeRender("deadbeef", "not really a png");
            orphan = vc.addFile(noteId, "image", "image/png", "x");
            QVERIFY(vc.fileMeta("r-deadbeef"));
            vc.lock();  // collects garbage and saves
        }
        VaultController vc;
        open(vc, "correct horse battery");
        QVERIFY(!vc.fileMeta("r-deadbeef"));
        QVERIFY(!vc.fileMeta(orphan));
        QCOMPARE(search(vc, "whiteboard").attachments.size(), 1);  // the referenced drawing survives
    }

    void deleteFolderMovesNotesToTrash() {
        VaultController vc;
        open(vc, "correct horse battery");
        QString projects;
        for (const auto& f : vc.folders())
            if (f.name == u"Projects")
                projects = f.id;
        QVERIFY(!projects.isEmpty());
        QCOMPARE(vc.noteCount(projects), 2);
        vc.deleteFolder(projects);
        QCOMPARE(vc.trashCount(), 2);
        QCOMPARE(vc.folders().size(), 2);
        QVERIFY(search(vc, "hub").noteCount() == 0);
    }

    void changePassword() {
        VaultController vc;
        open(vc, "correct horse battery");
        QSignalSpy changed(&vc, &VaultController::passwordChanged);
        vc.changePassword("wrong current", "new secret");
        QVERIFY(changed.wait(kUnlockTimeoutMs));
        QCOMPARE(changed.last().first().toBool(), false);
        vc.changePassword("correct horse battery", "new secret");
        QVERIFY(changed.wait(kUnlockTimeoutMs));
        QCOMPARE(changed.last().first().toBool(), true);
        vc.lock();
        VaultController again;
        open(again, "new secret");
        QVERIFY(again.isOpen());
    }
};

QTEST_MAIN(TestController)
#include "test_controller.moc"
