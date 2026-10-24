#pragma once

#include "NoteListModel.h"
#include "NoteText.h"
#include "SearchIndex.h"
#include "SidebarView.h"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <functional>

class QAction;
class QLabel;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace zn {

class DiagramService;
class FindBar;
class NoteDocument;
class NoteEditor;
class NoteListView;
class SearchField;
class VaultController;
class WelcomeWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void showVault(const QString& path);  // lock screen for this vault
    void unlockWith(const QString& path, const QString& password);  // automation and tests
    void createWith(const QString& path, const QString& password);  // new vault with sample notes
    void runAutomation(const QStringList& steps, const QString& screenshotPath);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    // setup
    void buildUi();
    void buildToolbar(QWidget* bar);
    void buildMenus();
    QAction* action(const QString& text, const QKeySequence& key, std::function<void()> fn, const QString& icon = {});

    // vault lifecycle
    void onOpened();
    void onLocked();
    void lockNow();
    void changePassword();
    void showSettings();

    // list & selection
    void refreshList(bool keepSelection = true);
    QStringList notesForSelection(bool keepOpenNote) const;
    QStringList notesMatchingSelection() const;
    void onSidebarSelection(const SidebarSelection& s);
    void onNoteSelected(const QString& noteId, const QString& attachmentKey);
    void openNote(const QString& noteId, const QString& attachmentKey = {}, bool fromSearch = false);
    void closeNote();
    void discardIfEmpty(const QString& noteId);
    void showNoteMenu(const QString& noteId, const QPoint& globalPos);

    // editing
    void flushEditor();
    void scheduleFlush();
    void newNote();
    void deleteCurrentNote();
    void togglePinCurrent();
    void updateFormatActions();
    void editDiagram(const QString& diagramId);
    void insertDiagram(DiagramKind kind);
    void insertDrawing();
    void editDrawing(const QString& fileId);
    void insertLinkDialog();
    void insertImageFromFile();
    QVariant loadResource(const QUrl& url, const NoteDocument* doc);
    void refreshDiagramsInDocument(const QString& key = {});

    // search
    void onQueryChanged();
    void onSearchFinished(quint64 generation, const SearchResults& results);
    void exitSearch();

    void toast(const QString& message, bool error = false);
    void syncToolbarToSplitter();
    void resetIdleTimer();
    void startAutomation(const QStringList& steps, const QString& screenshotPath);
    void finishAutomation(const QString& screenshotPath, int diagramsTimeoutMs);

    VaultController* vc_;
    DiagramService* diagrams_;
    QStackedWidget* stack_;
    WelcomeWidget* welcome_;
    QWidget* mainView_;
    QWidget* toolbar_;
    QWidget* toolbarSidebarPart_;
    QWidget* toolbarListPart_;
    QSplitter* splitter_;
    SidebarView* sidebar_;
    NoteListModel* listModel_;
    NoteListView* list_;
    QWidget* editorPane_;
    QStackedWidget* editorStack_;
    NoteEditor* editor_;
    FindBar* findBar_;
    QLabel* dateLabel_;
    QLabel* emptyEditor_;
    SearchField* search_;
    QLabel* toast_;
    QTimer toastTimer_;
    QTimer flushTimer_;
    QTimer idleTimer_;

    QPointer<NoteDocument> doc_;
    QString currentNote_;
    NoteSort sort_ = NoteSort::DateEdited;
    bool searching_ = false;
    quint64 searchGeneration_ = 0;
    SearchResults lastResults_;
    bool sidebarVisible_ = true;

    QHash<QString, QAction*> actions_;
    QToolButton* formatButton_ = nullptr;
};

}  // namespace zn
