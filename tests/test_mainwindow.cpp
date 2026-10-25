// End-to-end checks of the main window on a fresh sample vault.
#include "MainWindow.h"
#include "NoteEditor.h"
#include "NoteListModel.h"
#include "NoteListView.h"
#include "SearchField.h"
#include "SidebarView.h"
#include "VaultController.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QPointer>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace zn;

class TestMainWindow : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    MainWindow* w = nullptr;
    VaultController* vc = nullptr;
    NoteListView* list = nullptr;
    NoteEditor* editor = nullptr;
    SidebarView* sidebar = nullptr;
    QLineEdit* searchEdit = nullptr;

    const NoteListModel* model() const { return static_cast<const NoteListModel*>(list->model()); }

    QStringList listedNotes() const {
        QStringList out;
        for (int r = 0; r < model()->rowCount(); ++r)
            if (model()->row(r).kind == ListRow::Note)
                out.append(model()->row(r).noteId);
        return out;
    }

    QString folderId(const QString& name) const {
        for (const auto& f : vc->folders())
            if (f.name == name)
                return f.id;
        return {};
    }

    void trigger(const QString& text) {
        for (QAction* a : w->findChildren<QAction*>())
            if (a->text() == text) {
                a->trigger();
                return;
            }
        QFAIL(qPrintable(QStringLiteral("no action ") + text));
    }

    void clickSidebarRow(const QString& text) {
        auto* tree = sidebar->findChild<FolderTree*>();
        auto* m = static_cast<QStandardItemModel*>(tree->model());
        const auto hits = m->findItems(text, Qt::MatchExactly | Qt::MatchRecursive);
        QVERIFY2(!hits.isEmpty(), qPrintable(text));
        const QRect r = tree->visualRect(hits.first()->index());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, r.center());
    }

    // flushes typing to the vault (the editor saves 700 ms after the last change)
    void waitForSave() { QTest::qWait(900); }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("ZipNotesTest"));
        w = new MainWindow;
        w->resize(1200, 800);
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        w->activateWindow();
        vc = w->findChild<VaultController*>();
        list = w->findChild<NoteListView*>();
        editor = w->findChild<NoteEditor*>();
        sidebar = w->findChild<SidebarView*>();
        searchEdit = w->findChild<SearchField*>()->findChild<QLineEdit*>();
        QVERIFY(vc && list && editor && sidebar && searchEdit);
        w->createWith(dir.filePath(QStringLiteral("test.zip")), QStringLiteral("pw"));
        QTRY_VERIFY_WITH_TIMEOUT(vc->isOpen(), 60000);
        QTRY_VERIFY(listedNotes().size() == 7);
    }

    void cleanupTestCase() { delete w; }

    void searchKeepsFocusInTheField() {
        searchEdit->setFocus();
        QTest::keyClicks(searchEdit, QStringLiteral("hu"));
        QTest::qWait(600);  // results arrive and the first one is opened
        QCOMPARE(QApplication::focusWidget(), searchEdit);
        QTest::keyClicks(searchEdit, QStringLiteral("b"));
        QTest::qWait(600);
        QCOMPARE(searchEdit->text(), QStringLiteral("hub"));
        QCOMPARE(QApplication::focusWidget(), searchEdit);
        QVERIFY(model()->isSearch());
        QTest::keyClick(searchEdit, Qt::Key_Escape);  // closes the suggestions
        QTest::keyClick(searchEdit, Qt::Key_Escape);  // clears the search
        QTRY_VERIFY(!model()->isSearch());
    }

    void foldersAfterATag() {
        sidebar->select({SidebarSelection::AllNotes, {}});
        sidebar->select({SidebarSelection::Tag, QStringLiteral("home")});
        QCOMPARE(sidebar->selection().kind, SidebarSelection::Tag);
        clickSidebarRow(QStringLiteral("All Notes"));  // the row that was current before the tag
        QCOMPARE(sidebar->selection().kind, SidebarSelection::AllNotes);
        QCOMPARE(listedNotes().size(), 7);
        sidebar->select({SidebarSelection::Tag, QStringLiteral("home")});
        clickSidebarRow(QStringLiteral("Projects"));
        QCOMPARE(sidebar->selection().kind, SidebarSelection::Folder);
        QCOMPARE(sidebar->selection().id, folderId(QStringLiteral("Projects")));
    }

    void selectedFolderIsHighlighted() {
        sidebar->select({SidebarSelection::Folder, folderId(QStringLiteral("Personal"))});
        editor->setFocus();  // the highlight stays while the focus is elsewhere
        auto* tree = sidebar->findChild<FolderTree*>();
        auto* m = static_cast<QStandardItemModel*>(tree->model());
        const QImage shot = tree->viewport()->grab().toImage();
        const auto rowColor = [&](const QString& text) {
            const QRect r = tree->visualRect(m->findItems(text, Qt::MatchExactly | Qt::MatchRecursive).first()->index());
            return shot.pixelColor(QPoint(r.left() + 12, r.center().y()) * shot.devicePixelRatio());
        };
        QVERIFY(rowColor(QStringLiteral("Personal")) != rowColor(QStringLiteral("Projects")));
    }

    void nameFolder(const QString& text) {
        auto* tree = sidebar->findChild<FolderTree*>();
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        const QPointer<QLineEdit> name = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        QVERIFY(tree->isAncestorOf(name));
        QTest::qWait(50);  // pending zero timers, e.g. discarding the empty note that was left
        QVERIFY2(name && QApplication::focusWidget() == name, "the name editor was closed");
        name->selectAll();
        QTest::keyClicks(name, text);
        QTest::keyClick(name, Qt::Key_Return);
        QTRY_VERIFY(!folderId(text).isEmpty());
        QTest::qWait(50);  // the deferred editor deletion and repaints
        QCOMPARE(sidebar->selection().id, folderId(text));
    }

    void newFolderIsNamedInPlace() {
        // the rename rebuilds the sidebar model: it used to crash in the tree's dataChanged
        trigger(QStringLiteral("New Folder"));
        nameFolder(QStringLiteral("Work"));
        emit sidebar->newFolderRequested(folderId(QStringLiteral("Work")));
        nameFolder(QStringLiteral("Reports"));
        QCOMPARE(vc->folders().last().parentId, folderId(QStringLiteral("Work")));
    }

    void newFolderFromAnEmptyNote() {
        // leaving the empty note discards it, which used to rebuild the sidebar and close the name editor
        sidebar->select({SidebarSelection::AllNotes, {}});
        trigger(QStringLiteral("New Note"));
        trigger(QStringLiteral("New Folder"));
        nameFolder(QStringLiteral("Archive"));
    }

    void rebuildWaitsForTheNameEditor() {
        trigger(QStringLiteral("New Folder"));
        QTRY_VERIFY(qobject_cast<QLineEdit*>(QApplication::focusWidget()));
        auto* name = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        auto* tree = sidebar->findChild<FolderTree*>();
        auto* m = static_cast<QStandardItemModel*>(tree->model());
        const auto countOf = [&](const QString& text) {
            return m->findItems(text, Qt::MatchExactly | Qt::MatchRecursive).first()->index().data(Qt::UserRole + 3).toInt();
        };
        const int before = countOf(QStringLiteral("Personal"));
        vc->createNote(folderId(QStringLiteral("Personal")), QStringLiteral("Later"));  // counts change meanwhile
        QTest::qWait(50);
        QCOMPARE(QApplication::focusWidget(), name);
        QCOMPARE(countOf(QStringLiteral("Personal")), before);
        // the name is kept, so no rename rebuilds the sidebar; moving the current row closes the editor
        QTest::keyClick(name, Qt::Key_Up);
        QTRY_COMPARE(countOf(QStringLiteral("Personal")), before + 1);
    }

    void newNoteSurvivesClearingItsTitle() {
        sidebar->select({SidebarSelection::Folder, folderId(QStringLiteral("Personal"))});
        trigger(QStringLiteral("New Note"));
        QTest::keyClicks(editor, QStringLiteral("Plan"));
        waitForSave();
        const QString id = list->currentNoteId();
        QVERIFY(listedNotes().contains(id));
        QCOMPARE(vc->record(id)->digest.title, QStringLiteral("Plan"));

        editor->selectAll();
        QTest::keyClick(editor, Qt::Key_Backspace);
        waitForSave();
        QVERIFY(listedNotes().contains(id));
        QTest::keyClicks(editor, QStringLiteral("Plan B"));
        waitForSave();
        QVERIFY(listedNotes().contains(id));
        QCOMPARE(vc->record(id)->digest.title, QStringLiteral("Plan B"));
    }

    void newNoteInATagStaysWhileOpen() {
        sidebar->select({SidebarSelection::Tag, QStringLiteral("home")});
        trigger(QStringLiteral("New Note"));
        QTest::keyClicks(editor, QStringLiteral("Lights"));
        waitForSave();
        const QString id = list->currentNoteId();
        QVERIFY(vc->record(id)->digest.tags.contains(QStringLiteral("home")));
        QVERIFY(listedNotes().contains(id));

        editor->selectAll();  // removes the #home line as well
        QTest::keyClick(editor, Qt::Key_Backspace);
        QTest::keyClicks(editor, QStringLiteral("Lights again"));
        waitForSave();
        QVERIFY(listedNotes().contains(id));  // still listed while it is open
        QCOMPARE(list->currentNoteId(), id);
    }

    void emptyNoteIsDiscardedWhenLeft() {
        sidebar->select({SidebarSelection::AllNotes, {}});
        const int before = static_cast<int>(vc->records().size());
        trigger(QStringLiteral("New Note"));
        QCOMPARE(static_cast<int>(vc->records().size()), before + 1);
        list->selectFirst();
        QTRY_COMPARE(static_cast<int>(vc->records().size()), before);
    }
};

int main(int argc, char** argv) {
    // same process setup as the app: WebEngine renders Mermaid when a search result is opened
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
    QApplication app(argc, argv);
    TestMainWindow t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_mainwindow.moc"
