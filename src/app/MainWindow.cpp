#include "MainWindow.h"

#include "DiagramEditorDialog.h"
#include "DiagramService.h"
#include "FindBar.h"
#include "Icons.h"
#include "MarkdownIO.h"
#include "NoteDocument.h"
#include "NoteEditor.h"
#include "NoteListView.h"
#include "SearchField.h"
#include "TextNormalizer.h"
#include "Theme.h"
#include "VaultController.h"
#include "WelcomeWidget.h"

#ifdef ZN_WITH_WEBENGINE
#include "ExcalidrawDialog.h"
#endif

#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace zn {

namespace {

constexpr int kFlushDelayMs = 700;
constexpr int kDefaultAutoLockMinutes = 10;

class Pane : public QWidget {
public:
    Pane(std::function<QColor()> color, QWidget* parent) : QWidget(parent), color_(std::move(color)) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), color_());
    }

private:
    std::function<QColor()> color_;
};

QToolButton* toolButton(const QString& icon, const QString& tip, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setIcon(icons::icon(icon));
    b->setIconSize(QSize(20, 20));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setFixedSize(34, 30);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QString longDate(const QDateTime& utc) {
    const QDateTime l = utc.toLocalTime();
    return QObject::tr("%1 at %2").arg(QLocale().toString(l.date(), QLocale::LongFormat),
                                       QLocale().toString(l.time(), QLocale::ShortFormat));
}

// Excalidraw previews are exported light (dark strokes on a transparent background), which disappears on a
// dark editor. Invert the lightness and keep the hue, the way Excalidraw's own dark mode does.
QImage darkDrawing(QImage img) {
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = line[x];
            const int a = qAlpha(px);
            if (a == 0)
                continue;
            QColor c = QColor::fromRgb(qRed(px), qGreen(px), qBlue(px));
            float h = 0, s = 0, l = 0;
            c.getHslF(&h, &s, &l);
            c.setHslF(h, s, std::clamp(0.93f - l * 0.86f, 0.0f, 1.0f));
            line[x] = qRgba(c.red(), c.green(), c.blue(), a);
        }
    }
    return img;
}

QImage pngImage(const QByteArray& png) {
    QImage img = QImage::fromData(png);
    if (!img.isNull())
        img.setDevicePixelRatio(2.0);
    return img;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    vc_ = new VaultController(this);
    diagrams_ = new DiagramService(*vc_, this);
    buildUi();
    buildMenus();

    flushTimer_.setSingleShot(true);
    connect(&flushTimer_, &QTimer::timeout, this, &MainWindow::flushEditor);
    toastTimer_.setSingleShot(true);
    connect(&toastTimer_, &QTimer::timeout, toast_, &QLabel::hide);
    idleTimer_.setSingleShot(true);
    connect(&idleTimer_, &QTimer::timeout, this, [this] {
        if (vc_->isOpen())
            lockNow();
    });
    qApp->installEventFilter(this);

    connect(vc_, &VaultController::opened, this, &MainWindow::onOpened);
    connect(vc_, &VaultController::lockedVault, this, &MainWindow::onLocked);
    connect(vc_, &VaultController::openFailed, this, [this](const QString& err) {
        welcome_->showUnlock(welcome_->currentPath(), err);
    });
    connect(vc_, &VaultController::busyChanged, this, [this](bool busy, const QString& msg) {
        welcome_->setBusy(busy, msg);
        mainView_->setEnabled(!busy);
        if (busy && vc_->isOpen())
            toast(msg);
    });
    connect(vc_, &VaultController::saveFailed, this, [this](const QString& err) { toast(tr("Could not save: %1").arg(err), true); });
    connect(vc_, &VaultController::integrityWarning, this, [this](const QString& msg) {
        QMessageBox::warning(this, tr("Vault integrity"),
                             tr("Some entries could not be read and are shown as damaged notes:\n\n%1").arg(msg.left(2000)));
    });
    connect(vc_, &VaultController::passwordChanged, this, [this](bool ok, const QString& err) {
        if (ok)
            toast(tr("Password changed"));
        else
            QMessageBox::warning(this, tr("Change Password"), err);
    });
    connect(vc_, &VaultController::notesChanged, this, [this] {
        sidebar_->rebuild();
        if (searching_)
            onQueryChanged();
        else
            refreshList();
    });
    connect(vc_, &VaultController::foldersChanged, this, [this] {
        sidebar_->rebuild();
        refreshList();
    });
    connect(vc_, &VaultController::noteContentChanged, this, [this](const QString& id) {
        if (searching_) {
            listModel_->refreshNote(*vc_, id);
            return;
        }
        NoteListModel probe;
        const SidebarSelection s = sidebar_->selection();
        probe.showNotes(*vc_, notesForSelection(true), sort_, s.kind != SidebarSelection::Folder, s.kind == SidebarSelection::Trash);
        if (probe.layoutSignature() == listModel_->layoutSignature())
            listModel_->refreshNote(*vc_, id);
        else
            refreshList();
        sidebar_->rebuild();
    });
    connect(vc_, &VaultController::searchFinished, this, &MainWindow::onSearchFinished);
    connect(diagrams_, &DiagramService::rendered, this, [this](const QString& key) { refreshDiagramsInDocument(key); });
    connect(&Theme::instance(), &Theme::changed, this, [this] {
        if (doc_) {
            NoteDocument* d = doc_;
            const bool wasModified = d->isModified();
            d->setStyles(Theme::instance().textStyles());
            d->setModified(wasModified);
            refreshDiagramsInDocument();
        }
        update();
    });

    setWindowTitle(QStringLiteral("ZipNotes"));
    resize(1240, 780);
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
    splitter_->restoreState(s.value(QStringLiteral("window/splitter")).toByteArray());
    sort_ = NoteSort(s.value(QStringLiteral("list/sort"), int(NoteSort::DateEdited)).toInt());
    syncToolbarToSplitter();
}

MainWindow::~MainWindow() {
    // Normally closeEvent() saved and locked already. If the window is destroyed directly, do it here,
    // before the members go: the controller must not signal into a half-destroyed window.
    qApp->removeEventFilter(this);
    disconnect(vc_, nullptr, this, nullptr);
    disconnect(diagrams_, nullptr, this, nullptr);
    flushEditor();
    if (vc_->isOpen())
        vc_->lock();
}

QAction* MainWindow::action(const QString& text, const QKeySequence& key, std::function<void()> fn, const QString& icon) {
    auto* a = new QAction(text, this);
    if (!key.isEmpty())
        a->setShortcut(key);
    if (!icon.isEmpty())
        a->setIcon(icons::icon(icon));
    connect(a, &QAction::triggered, this, [fn = std::move(fn)] { fn(); });
    addAction(a);
    return a;
}

void MainWindow::buildUi() {
    stack_ = new QStackedWidget(this);
    setCentralWidget(stack_);
    welcome_ = new WelcomeWidget(stack_);
    stack_->addWidget(welcome_);
    connect(welcome_, &WelcomeWidget::unlockRequested, this, [this](const QString& path, const QString& pw) { vc_->openVault(path, pw); });
    connect(welcome_, &WelcomeWidget::createRequested, this, [this](const QString& path, const QString& pw, bool samples) {
        welcome_->showUnlock(path);
        welcome_->setBusy(true, tr("Creating vault…"));
        vc_->createVault(path, pw, samples);
    });

    mainView_ = new QWidget(stack_);
    auto* mv = new QVBoxLayout(mainView_);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);
    toolbar_ = new Pane([] { return Theme::palette().window; }, mainView_);
    toolbar_->setFixedHeight(46);
    mv->addWidget(toolbar_);
    auto* line = new Pane([] { return Theme::palette().separator; }, mainView_);
    line->setFixedHeight(1);
    mv->addWidget(line);

    splitter_ = new QSplitter(mainView_);
    splitter_->setHandleWidth(1);
    splitter_->setChildrenCollapsible(false);
    sidebar_ = new SidebarView(*vc_, splitter_);
    sidebar_->setMinimumWidth(170);
    listModel_ = new NoteListModel(this);
    list_ = new NoteListView(listModel_, splitter_);
    list_->setMinimumWidth(240);

    editorPane_ = new Pane([] { return Theme::palette().editor; }, splitter_);
    auto* ep = new QVBoxLayout(editorPane_);
    ep->setContentsMargins(0, 0, 0, 0);
    ep->setSpacing(0);
    editor_ = new NoteEditor(editorPane_);
    findBar_ = new FindBar(editor_, editorPane_);
    ep->addWidget(findBar_);
    editorStack_ = new QStackedWidget(editorPane_);
    auto* noteView = new QWidget(editorStack_);
    auto* nv = new QVBoxLayout(noteView);
    nv->setContentsMargins(0, 10, 0, 0);
    nv->setSpacing(0);
    dateLabel_ = new QLabel(noteView);
    dateLabel_->setAlignment(Qt::AlignCenter);
    dateLabel_->setFont(Theme::instance().uiFont(QFont().pointSizeF() - 0.5));
    nv->addWidget(dateLabel_);
    nv->addWidget(editor_, 1);
    editorStack_->addWidget(noteView);
    emptyEditor_ = new QLabel(tr("No note selected"), editorStack_);
    emptyEditor_->setAlignment(Qt::AlignCenter);
    emptyEditor_->setFont(Theme::instance().uiFont(QFont().pointSizeF() + 3, QFont::DemiBold));
    editorStack_->addWidget(emptyEditor_);
    editorStack_->setCurrentWidget(emptyEditor_);
    ep->addWidget(editorStack_, 1);

    splitter_->addWidget(sidebar_);
    splitter_->addWidget(list_);
    splitter_->addWidget(editorPane_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 0);
    splitter_->setStretchFactor(2, 1);
    splitter_->setSizes({210, 320, 710});
    mv->addWidget(splitter_, 1);
    stack_->addWidget(mainView_);

    buildToolbar(toolbar_);

    toast_ = new QLabel(this);
    toast_->setAlignment(Qt::AlignCenter);
    toast_->hide();

    connect(splitter_, &QSplitter::splitterMoved, this, &MainWindow::syncToolbarToSplitter);
    connect(sidebar_, &SidebarView::selectionChanged, this, &MainWindow::onSidebarSelection);
    connect(sidebar_, &SidebarView::newFolderRequested, this, [this](const QString& parent) {
        const QString id = vc_->createFolder(tr("New Folder"), parent);
        sidebar_->select({SidebarSelection::Folder, id});
        sidebar_->editFolderName(id);
    });
    connect(sidebar_, &SidebarView::renameFolderRequested, vc_, &VaultController::renameFolder);
    connect(sidebar_, &SidebarView::deleteFolderRequested, this, [this](const QString& id) {
        const int n = vc_->noteCount(id);
        if (QMessageBox::question(this, tr("Delete Folder"),
                                  n > 0 ? tr("Delete “%1”? Its %n note(s) will be moved to Recently Deleted.", nullptr, n).arg(vc_->folderPath(id))
                                        : tr("Delete “%1”?").arg(vc_->folderPath(id))) == QMessageBox::Yes)
            vc_->deleteFolder(id);
    });
    connect(sidebar_, &SidebarView::notesDroppedOnFolder, this, [this](const QStringList& ids, const QString& folder) {
        flushEditor();
        for (const auto& id : ids)
            vc_->moveNote(id, folder);
    });
    connect(sidebar_, &SidebarView::emptyTrashRequested, this, [this] {
        if (QMessageBox::question(this, tr("Recently Deleted"), tr("Delete all notes in Recently Deleted permanently?")) == QMessageBox::Yes) {
            if (const NoteMeta* m = vc_->meta(currentNote_); m && m->trashed.isValid())
                closeNote();
            vc_->emptyTrash();
        }
    });
    connect(list_, &NoteListView::noteSelected, this, &MainWindow::onNoteSelected);
    connect(list_, &NoteListView::contextMenuForNote, this, &MainWindow::showNoteMenu);
    connect(list_, &NoteListView::deleteRequested, this, &MainWindow::deleteCurrentNote);
    connect(editor_, &NoteEditor::formatChanged, this, &MainWindow::updateFormatActions);
    connect(editor_, &NoteEditor::diagramActivated, this, &MainWindow::editDiagram);
    connect(editor_, &NoteEditor::fileActivated, this, &MainWindow::editDrawing);
    connect(editor_, &QTextEdit::textChanged, this, [this] {
        if (doc_ && doc_->isModified())
            scheduleFlush();
    });
    editor_->storePastedImage = [this](const QImage& img) -> QString {
        if (currentNote_.isEmpty())
            return {};
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        return vc_->addFile(currentNote_, QStringLiteral("image"), QStringLiteral("image/png"), png);
    };
}

void MainWindow::buildToolbar(QWidget* bar) {
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(8, 0, 10, 0);
    lay->setSpacing(2);

    toolbarSidebarPart_ = new QWidget(bar);
    auto* sp = new QHBoxLayout(toolbarSidebarPart_);
    sp->setContentsMargins(0, 0, 0, 0);
    auto* sidebarBtn = toolButton("sidebar", tr("Show or Hide Sidebar"), toolbarSidebarPart_);
    sp->addWidget(sidebarBtn);
    sp->addStretch();
    lay->addWidget(toolbarSidebarPart_);

    toolbarListPart_ = new QWidget(bar);
    auto* lp = new QHBoxLayout(toolbarListPart_);
    lp->setContentsMargins(10, 0, 0, 0);
    lp->addStretch();
    auto* deleteBtn = toolButton("trash", tr("Delete Note"), toolbarListPart_);
    lp->addWidget(deleteBtn);
    lay->addWidget(toolbarListPart_);

    auto* compose = toolButton("compose", tr("New Note (Ctrl+N)"), bar);
    lay->addWidget(compose);
    lay->addSpacing(14);

    formatButton_ = toolButton("format", tr("Text Styles"), bar);
    formatButton_->setPopupMode(QToolButton::InstantPopup);
    formatButton_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));
    lay->addWidget(formatButton_);
    auto* checklist = toolButton("checklist", tr("Checklist (Ctrl+Shift+L)"), bar);
    lay->addWidget(checklist);
    auto* table = toolButton("table", tr("Table"), bar);
    lay->addWidget(table);
    auto* diagram = toolButton("diagram", tr("Insert Diagram"), bar);
    diagram->setPopupMode(QToolButton::InstantPopup);
    diagram->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));
    auto* diagramMenu = new QMenu(diagram);
    diagramMenu->addAction(icons::icon("mermaid"), tr("Mermaid Diagram"), this, [this] { insertDiagram(DiagramKind::Mermaid); });
    diagramMenu->addAction(icons::icon("plantuml"), tr("PlantUML Diagram"), this, [this] { insertDiagram(DiagramKind::PlantUml); });
    diagramMenu->addAction(icons::icon("drawing"), tr("Drawing (Excalidraw)"), this, &MainWindow::insertDrawing);
    diagram->setMenu(diagramMenu);
    lay->addWidget(diagram);
    auto* link = toolButton("link", tr("Link (Ctrl+K)"), bar);
    lay->addWidget(link);
    lay->addStretch(1);
    auto* lockBtn = toolButton("lock", tr("Lock Vault (Ctrl+L)"), bar);
    lay->addWidget(lockBtn);
    lay->addSpacing(6);
    search_ = new SearchField(*vc_, bar);
    search_->setFixedWidth(260);
    search_->setPopupHost(this);
    lay->addWidget(search_);

    connect(sidebarBtn, &QToolButton::clicked, this, [this] {
        sidebarVisible_ = !sidebarVisible_;
        sidebar_->setVisible(sidebarVisible_);
        syncToolbarToSplitter();
    });
    connect(deleteBtn, &QToolButton::clicked, this, &MainWindow::deleteCurrentNote);
    connect(compose, &QToolButton::clicked, this, &MainWindow::newNote);
    connect(checklist, &QToolButton::clicked, this, [this] { editor_->applyParagraphStyle(ParagraphStyle::Checklist); });
    connect(table, &QToolButton::clicked, this, [this] { editor_->insertTable(3, 3); });
    connect(link, &QToolButton::clicked, this, &MainWindow::insertLinkDialog);
    connect(lockBtn, &QToolButton::clicked, this, &MainWindow::lockNow);
    connect(search_, &SearchField::queryChanged, this, &MainWindow::onQueryChanged);
    connect(search_, &SearchField::cancelled, this, &MainWindow::exitSearch);
    connect(search_, &SearchField::moveToResults, this, [this] {
        list_->setFocus();
        if (list_->currentNoteId().isEmpty())
            list_->selectFirst();
    });
}

void MainWindow::buildMenus() {
    auto& a = actions_;
    QMenu* file = menuBar()->addMenu(tr("&File"));
    a["newNote"] = action(tr("New Note"), QKeySequence::New, [this] { newNote(); }, "compose");
    a["newFolder"] = action(tr("New Folder"), QKeySequence(tr("Ctrl+Shift+N")), [this] { emit sidebar_->newFolderRequested({}); }, "folderplus");
    file->addAction(a["newNote"]);
    file->addAction(a["newFolder"]);
    file->addSeparator();
    file->addAction(tr("New Vault…"), this, [this] {
        if (vc_->isOpen())
            lockNow();
        welcome_->showStart();
        stack_->setCurrentWidget(welcome_);
    });
    file->addAction(tr("Open Vault…"), this, [this] {
        if (vc_->isOpen())
            lockNow();
        welcome_->showStart();
        stack_->setCurrentWidget(welcome_);
    });
    a["lock"] = action(tr("Lock Vault"), QKeySequence(tr("Ctrl+L")), [this] { lockNow(); }, "lock");
    file->addAction(a["lock"]);
    a["password"] = action(tr("Change Password…"), {}, [this] { changePassword(); }, "key");
    file->addAction(a["password"]);
    file->addSeparator();
    file->addAction(tr("Settings…"), this, &MainWindow::showSettings);
    file->addSeparator();
    file->addAction(tr("Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("Undo"), QKeySequence::Undo, editor_, &QTextEdit::undo);
    edit->addAction(tr("Redo"), QKeySequence::Redo, editor_, &QTextEdit::redo);
    edit->addSeparator();
    edit->addAction(tr("Cut"), QKeySequence::Cut, editor_, &QTextEdit::cut);
    edit->addAction(tr("Copy"), QKeySequence::Copy, editor_, &QTextEdit::copy);
    edit->addAction(tr("Paste"), QKeySequence::Paste, editor_, &QTextEdit::paste);
    edit->addAction(tr("Select All"), QKeySequence::SelectAll, editor_, &QTextEdit::selectAll);
    edit->addSeparator();
    a["find"] = action(tr("Find in Note"), QKeySequence::Find, [this] {
        if (!currentNote_.isEmpty())
            findBar_->activate(editor_->textCursor().selectedText());
    }, "search");
    edit->addAction(a["find"]);
    a["search"] = action(tr("Search All Notes"), QKeySequence(tr("Ctrl+Alt+F")), [this] { search_->focusField(); }, "search");
    edit->addAction(a["search"]);

    QMenu* format = menuBar()->addMenu(tr("F&ormat"));
    struct StyleItem {
        const char* key;
        QString text;
        QKeySequence seq;
        ParagraphStyle style;
    };
    const StyleItem styles[] = {
        {"title", tr("Title"), QKeySequence(tr("Ctrl+Shift+T")), ParagraphStyle::Title},
        {"heading", tr("Heading"), QKeySequence(tr("Ctrl+Shift+H")), ParagraphStyle::Heading},
        {"subheading", tr("Subheading"), QKeySequence(tr("Ctrl+Shift+J")), ParagraphStyle::Subheading},
        {"body", tr("Body"), QKeySequence(tr("Ctrl+Shift+B")), ParagraphStyle::Body},
        {"mono", tr("Monostyled"), QKeySequence(tr("Ctrl+Shift+M")), ParagraphStyle::Monostyled},
        {"bulleted", tr("• Bulleted List"), QKeySequence(tr("Ctrl+Shift+7")), ParagraphStyle::Bulleted},
        {"dashed", tr("– Dashed List"), QKeySequence(tr("Ctrl+Shift+8")), ParagraphStyle::Dashed},
        {"numbered", tr("1. Numbered List"), QKeySequence(tr("Ctrl+Shift+9")), ParagraphStyle::Numbered},
        {"checklist", tr("Checklist"), QKeySequence(tr("Ctrl+Shift+L")), ParagraphStyle::Checklist},
    };
    auto* styleMenu = new QMenu(this);
    for (const auto& s : styles) {
        QAction* act = action(s.text, s.seq, [this, st = s.style] { editor_->applyParagraphStyle(st); });
        act->setCheckable(true);
        a[QString::fromLatin1(s.key)] = act;
        format->addAction(act);
        styleMenu->addAction(act);
        if (s.style == ParagraphStyle::Monostyled) {
            format->addSeparator();
            styleMenu->addSeparator();
        }
    }
    format->addSeparator();
    styleMenu->addSeparator();
    struct Toggle {
        const char* key;
        QString text;
        QKeySequence seq;
        void (NoteEditor::*fn)();
    };
    const Toggle toggles[] = {
        {"bold", tr("Bold"), QKeySequence::Bold, &NoteEditor::toggleBold},
        {"italic", tr("Italic"), QKeySequence::Italic, &NoteEditor::toggleItalic},
        {"underline", tr("Underline"), QKeySequence::Underline, &NoteEditor::toggleUnderline},
        {"strike", tr("Strikethrough"), QKeySequence(tr("Ctrl+Shift+X")), &NoteEditor::toggleStrikethrough},
        {"highlight", tr("Highlight"), QKeySequence(tr("Ctrl+Shift+E")), &NoteEditor::toggleHighlight},
        {"code", tr("Inline Code"), QKeySequence(tr("Ctrl+E")), &NoteEditor::toggleInlineCode},
        {"quote", tr("Block Quote"), QKeySequence(tr("Ctrl+'")), &NoteEditor::toggleBlockQuote},
    };
    for (const auto& t : toggles) {
        QAction* act = action(t.text, t.seq, [this, fn = t.fn] { (editor_->*fn)(); });
        act->setCheckable(true);
        a[QString::fromLatin1(t.key)] = act;
        format->addAction(act);
        styleMenu->addAction(act);
    }
    format->addSeparator();
    format->addAction(action(tr("Increase Indent"), QKeySequence(tr("Ctrl+]")), [this] { editor_->changeIndent(1); }));
    format->addAction(action(tr("Decrease Indent"), QKeySequence(tr("Ctrl+[")), [this] { editor_->changeIndent(-1); }));
    formatButton_->setMenu(styleMenu);

    QMenu* insert = menuBar()->addMenu(tr("&Insert"));
    insert->addAction(action(tr("Table"), QKeySequence(tr("Ctrl+Alt+T")), [this] { editor_->insertTable(3, 3); }, "table"));
    insert->addAction(action(tr("Link…"), QKeySequence(tr("Ctrl+K")), [this] { insertLinkDialog(); }, "link"));
    insert->addAction(action(tr("Horizontal Line"), {}, [this] { editor_->insertHorizontalRule(); }));
    insert->addAction(action(tr("Image…"), {}, [this] { insertImageFromFile(); }, "image"));
    insert->addSeparator();
    insert->addAction(action(tr("Mermaid Diagram"), QKeySequence(tr("Ctrl+Alt+M")), [this] { insertDiagram(DiagramKind::Mermaid); }, "mermaid"));
    insert->addAction(action(tr("PlantUML Diagram"), QKeySequence(tr("Ctrl+Alt+U")), [this] { insertDiagram(DiagramKind::PlantUml); }, "plantuml"));
    insert->addAction(action(tr("Drawing (Excalidraw)"), QKeySequence(tr("Ctrl+Alt+D")), [this] { insertDrawing(); }, "drawing"));

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(action(tr("Show or Hide Sidebar"), QKeySequence(tr("Ctrl+Alt+S")), [this] {
        sidebarVisible_ = !sidebarVisible_;
        sidebar_->setVisible(sidebarVisible_);
        syncToolbarToSplitter();
    }, "sidebar"));
    QMenu* sortMenu = view->addMenu(tr("Sort Notes By"));
    auto* sortGroup = new QActionGroup(this);
    const std::pair<QString, NoteSort> sorts[] = {{tr("Date Edited"), NoteSort::DateEdited},
                                                  {tr("Date Created"), NoteSort::DateCreated},
                                                  {tr("Title"), NoteSort::Title}};
    for (const auto& [text, s] : sorts) {
        QAction* act = sortMenu->addAction(text, this, [this, s] {
            sort_ = s;
            QSettings().setValue(QStringLiteral("list/sort"), int(s));
            refreshList();
        });
        act->setCheckable(true);
        act->setChecked(s == sort_);
        sortGroup->addAction(act);
    }

    QMenu* note = menuBar()->addMenu(tr("&Note"));
    a["pin"] = action(tr("Pin Note"), QKeySequence(tr("Ctrl+Shift+P")), [this] { togglePinCurrent(); }, "pin");
    note->addAction(a["pin"]);
    a["delete"] = action(tr("Delete Note"), QKeySequence(tr("Ctrl+Backspace")), [this] { deleteCurrentNote(); }, "trash");
    note->addAction(a["delete"]);

    for (QAction* act : actions_)
        act->setEnabled(false);
}

// ------------------------------------------------------------------------------------------------ lifecycle

void MainWindow::showVault(const QString& path) {
    welcome_->showUnlock(path);
    stack_->setCurrentWidget(welcome_);
}

void MainWindow::unlockWith(const QString& path, const QString& password) {
    welcome_->showUnlock(path);
    stack_->setCurrentWidget(welcome_);
    vc_->openVault(path, password);
}

void MainWindow::createWith(const QString& path, const QString& password) {
    welcome_->showUnlock(path);
    welcome_->setBusy(true, tr("Creating vault…"));
    stack_->setCurrentWidget(welcome_);
    vc_->createVault(path, password, true);
}

void MainWindow::onOpened() {
    WelcomeWidget::rememberVault(vc_->vaultPath());
    setWindowTitle(QStringLiteral("%1 — ZipNotes").arg(vc_->vaultName()));
    for (QAction* act : actions_)
        act->setEnabled(true);
    stack_->setCurrentWidget(mainView_);
    sidebar_->rebuild();
    sidebar_->select({SidebarSelection::AllNotes, {}});
    refreshList(false);
    list_->selectFirst();
    resetIdleTimer();
    syncToolbarToSplitter();
}

void MainWindow::lockNow() {
    if (!vc_->isOpen() || vc_->isBusy())
        return;
    flushEditor();
    findBar_->deactivate();
    search_->clear();
    closeNote();
    const QString path = vc_->vaultPath();
    vc_->lock();
    welcome_->showUnlock(path);
}

void MainWindow::onLocked() {
    idleTimer_.stop();
    closeNote();
    listModel_->showNotes(*vc_, {}, sort_, false, false);
    diagrams_->clearMemory();
    for (QAction* act : actions_)
        act->setEnabled(false);
    setWindowTitle(QStringLiteral("ZipNotes"));
    stack_->setCurrentWidget(welcome_);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    flushEditor();
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/splitter"), splitter_->saveState());
    if (vc_->isOpen()) {
        closeNote();
        vc_->lock();
    }
    e->accept();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    switch (e->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
        resetIdleTimer();
        break;
    case QEvent::Resize:
        if (o == this && toast_->isVisible())
            toast_->move((width() - toast_->width()) / 2, height() - toast_->height() - 28);
        break;
    default:
        break;
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::resetIdleTimer() {
    if (!vc_ || !vc_->isOpen())
        return;
    const int minutes = QSettings().value(QStringLiteral("security/autoLockMinutes"), kDefaultAutoLockMinutes).toInt();
    if (minutes > 0)
        idleTimer_.start(minutes * 60 * 1000);
    else
        idleTimer_.stop();
}

void MainWindow::changePassword() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Change Password"));
    auto* form = new QFormLayout(&dlg);
    auto* current = new QLineEdit(&dlg);
    auto* next = new QLineEdit(&dlg);
    auto* confirm = new QLineEdit(&dlg);
    for (auto* f : {current, next, confirm})
        f->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Current password:"), current);
    form->addRow(tr("New password:"), next);
    form->addRow(tr("Confirm:"), confirm);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto validate = [&] { buttons->button(QDialogButtonBox::Ok)->setEnabled(!next->text().isEmpty() && next->text() == confirm->text()); };
    connect(next, &QLineEdit::textChanged, &dlg, validate);
    connect(confirm, &QLineEdit::textChanged, &dlg, validate);
    validate();
    if (dlg.exec() != QDialog::Accepted)
        return;
    flushEditor();
    vc_->changePassword(current->text(), next->text());
}

void MainWindow::showSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Settings"));
    auto* form = new QFormLayout(&dlg);
    QSettings s;
    auto* lockMinutes = new QSpinBox(&dlg);
    lockMinutes->setRange(0, 240);
    lockMinutes->setSuffix(tr(" min"));
    lockMinutes->setSpecialValueText(tr("Never"));
    lockMinutes->setValue(s.value(QStringLiteral("security/autoLockMinutes"), kDefaultAutoLockMinutes).toInt());
    form->addRow(tr("Lock after inactivity:"), lockMinutes);
    auto* plantuml = new QLineEdit(s.value(QStringLiteral("plantuml/command")).toString(), &dlg);
    QString placeholder = tr("plantuml, or /path/to/plantuml.jar");
    if (diagrams_->builtInPlantUml())
        placeholder = tr("Built-in; or plantuml, /path/to/plantuml.jar");
    else if (!diagrams_->plantUmlCommand().isEmpty())
        placeholder = diagrams_->plantUmlCommand();
    plantuml->setPlaceholderText(placeholder);
    form->addRow(tr("PlantUML command:"), plantuml);
    auto* note = new QLabel(tr("Leave it empty to use the PlantUML engine built into ZipNotes. A command runs locally with the "
                               "SANDBOX security profile. No diagram is ever sent to a server."), &dlg);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1").arg(Theme::palette().tertiaryText.name()));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(520, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted)
        return;
    s.setValue(QStringLiteral("security/autoLockMinutes"), lockMinutes->value());
    s.setValue(QStringLiteral("plantuml/command"), plantuml->text().trimmed());
    diagrams_->clearMemory();
    refreshDiagramsInDocument();
    resetIdleTimer();
}

// ------------------------------------------------------------------------------------------------ list

QStringList MainWindow::notesForSelection(bool keepOpenNote) const {
    QStringList out = notesMatchingSelection();
    if (!keepOpenNote)
        return out;
    // The open note stays listed while it is open, even after an edit removed the tag being viewed;
    // otherwise it vanishes under the cursor and typing the tag back is the only way to find it.
    const SidebarSelection s = sidebar_->selection();
    if (s.kind == SidebarSelection::Tag && !currentNote_.isEmpty() && !out.contains(currentNote_))
        if (const NoteMeta* m = vc_->meta(currentNote_); m && !m->trashed.isValid())
            out.append(currentNote_);
    return out;
}

QStringList MainWindow::notesMatchingSelection() const {
    const SidebarSelection s = sidebar_->selection();
    QStringList out;
    QStringList folders;
    if (s.kind == SidebarSelection::Folder)
        folders = vc_->folderWithDescendants(s.id);
    const QString tag = text::fold(s.id);
    for (const NoteRecord* r : vc_->records()) {
        const NoteMeta* m = vc_->meta(r->id);
        if (!m)
            continue;
        const bool trashed = m->trashed.isValid();
        switch (s.kind) {
        case SidebarSelection::Trash:
            if (trashed)
                out.append(r->id);
            break;
        case SidebarSelection::AllNotes:
            if (!trashed)
                out.append(r->id);
            break;
        case SidebarSelection::Folder:
            if (!trashed && folders.contains(m->folderId))
                out.append(r->id);
            break;
        case SidebarSelection::Tag:
            if (!trashed && std::any_of(r->digest.tags.begin(), r->digest.tags.end(),
                                        [&](const QString& t) { return text::fold(t) == tag || text::fold(t).startsWith(tag + u'/'); }))
                out.append(r->id);
            break;
        }
    }
    return out;
}

void MainWindow::refreshList(bool keepSelection) {
    if (!vc_->isOpen() || searching_)
        return;
    const SidebarSelection s = sidebar_->selection();
    const bool showFolder = s.kind != SidebarSelection::Folder;
    const int scroll = list_->verticalScrollBar()->value();
    listModel_->showNotes(*vc_, notesForSelection(keepSelection), sort_, showFolder, s.kind == SidebarSelection::Trash);
    if (keepSelection && !currentNote_.isEmpty()) {
        list_->selectNote(currentNote_);
        list_->verticalScrollBar()->setValue(scroll);
        list_->scrollTo(list_->currentIndex());
    }
}

void MainWindow::onSidebarSelection(const SidebarSelection& s) {
    if (searching_) {
        // a folder picked while searching becomes a search scope token
        if (s.kind == SidebarSelection::Folder) {
            search_->addToken({SearchToken::Folder, s.id, vc_->folderPath(s.id), QStringLiteral("folder")});
            return;
        }
        search_->clear();
        searching_ = false;
    }
    flushEditor();
    refreshList(false);
    const QString keep = currentNote_;
    if (listModel_->rowOfNote(keep) >= 0)
        list_->selectNote(keep);
    else {
        closeNote();
        list_->selectFirst();
    }
}

void MainWindow::onNoteSelected(const QString& noteId, const QString& attachmentKey) {
    openNote(noteId, attachmentKey, searching_);
}

void MainWindow::showNoteMenu(const QString& noteId, const QPoint& globalPos) {
    const NoteMeta* m = vc_->meta(noteId);
    if (!m)
        return;
    QMenu menu(this);
    if (m->trashed.isValid()) {
        menu.addAction(tr("Recover"), this, [this, noteId] { vc_->restoreNote(noteId); });
        menu.addAction(icons::icon("trash"), tr("Delete Permanently"), this, [this, noteId] {
            if (noteId == currentNote_)
                closeNote();
            vc_->deleteNoteForever(noteId);
        });
    } else {
        menu.addAction(icons::icon(m->pinned ? "pinfill" : "pin"), m->pinned ? tr("Unpin Note") : tr("Pin Note"), this,
                       [this, noteId, pinned = m->pinned] { vc_->setPinned(noteId, !pinned); });
        QMenu* move = menu.addMenu(icons::icon("folder"), tr("Move to"));
        for (const auto& f : vc_->folders()) {
            QAction* act = move->addAction(vc_->folderPath(f.id), this, [this, noteId, id = f.id] {
                flushEditor();
                vc_->moveNote(noteId, id);
            });
            act->setEnabled(f.id != m->folderId);
        }
        menu.addSeparator();
        menu.addAction(icons::icon("trash"), tr("Delete"), this, [this, noteId] {
            if (noteId == currentNote_)
                closeNote();
            vc_->trashNote(noteId);
        });
    }
    menu.exec(globalPos);
}

// ------------------------------------------------------------------------------------------------ editor

QVariant MainWindow::loadResource(const QUrl& url, const NoteDocument* doc) {
    const QString name = url.toString();
    if (const QString id = NoteDocument::diagramIdFromUrl(name); !id.isEmpty()) {
        if (const DiagramRef* d = doc->diagram(id))
            return diagrams_->image(d->kind, d->source);
        return {};
    }
    const QString fileId = NoteDocument::fileIdFromUrl(name);
    const FileMeta* meta = vc_->fileMeta(fileId);
    if (!meta)
        return {};
    if (meta->kind == u"excalidraw") {
        if (Theme::isDark() && !meta->darkPreviewId.isEmpty())
            if (auto png = vc_->readFile(meta->darkPreviewId))
                return pngImage(*png);
        // light preview; drawings saved before dark previews existed get an approximated dark version
        if (!meta->previewId.isEmpty())
            if (auto png = vc_->readFile(meta->previewId))
                return Theme::isDark() ? darkDrawing(pngImage(*png)) : pngImage(*png);
        QImage empty(QSize(460, 120) * 2, QImage::Format_ARGB32_Premultiplied);
        empty.setDevicePixelRatio(2);
        empty.fill(Qt::transparent);
        QPainter p(&empty);
        p.setPen(QPen(Theme::palette().separator, 1.2, Qt::DashLine));
        p.drawRoundedRect(QRectF(1, 1, 458, 118), 10, 10);
        p.setPen(Theme::palette().secondaryText);
        p.drawText(QRectF(0, 0, 460, 120), Qt::AlignCenter, tr("Empty drawing — double-click to draw"));
        return empty;
    }
    if (auto bytes = vc_->readFile(fileId)) {
        QImage img = QImage::fromData(*bytes);
        if (!img.isNull() && img.width() > 1600)
            img.setDevicePixelRatio(2);
        return img;
    }
    return {};
}

void MainWindow::refreshDiagramsInDocument(const QString& key) {
    if (!doc_)
        return;
    if (key.isEmpty()) {  // theme change or settings: drawings are recoloured as well
        for (QTextBlock b = doc_->begin(); b.isValid(); b = b.next())
            for (auto it = b.begin(); !it.atEnd(); ++it)
                if (const QTextCharFormat f = it.fragment().charFormat(); f.isImageFormat())
                    if (const QString name = f.toImageFormat().name(); name.startsWith(u"zn-file://"))
                        doc_->refreshResource(name);
    }
    for (const QString& id : doc_->diagramIds()) {
        const DiagramRef* d = doc_->diagram(id);
        if (d && (key.isEmpty() || diagrams_->key(d->kind, d->source) == key))
            doc_->refreshResource(NoteDocument::diagramUrl(id));
    }
}

void MainWindow::discardIfEmpty(const QString& noteId) {
    const NoteRecord* r = vc_->record(noteId);
    const NoteMeta* m = vc_->meta(noteId);
    if (!r || !m || m->trashed.isValid() || !r->loadError.isEmpty())
        return;
    // "empty" also covers an untouched note from a tag view (only the tag) and an empty title line
    static const QRegularExpression filler(QStringLiteral(R"(&nbsp;|(?<![\p{L}\p{N}_/&#])#[\p{L}\p{N}_][\p{L}\p{N}_\-/]*|(?m)^#{1,6}[ \t]*$)"));
    QString rest = r->markdown;
    rest.remove(filler);
    if (!rest.trimmed().isEmpty())
        return;
    vc_->deleteNoteForever(noteId);
}

void MainWindow::openNote(const QString& noteId, const QString& attachmentKey, bool fromSearch) {
    if (noteId == currentNote_ && doc_) {
        if (fromSearch)
            editor_->setSearchMatches(lastResults_.matches, true);
        return;
    }
    flushEditor();
    const QString previous = currentNote_;
    const NoteRecord* rec = vc_->record(noteId);
    const NoteMeta* meta = vc_->meta(noteId);
    if (!rec || !meta) {
        closeNote();
        return;
    }
    findBar_->deactivate();
    auto* doc = new NoteDocument(this);
    doc->setStyles(Theme::instance().textStyles());
    doc->setResourceLoader([this](const QUrl& url, const NoteDocument* d) { return loadResource(url, d); });
    importMarkdown(*doc, rec->markdown);
    NoteDocument* old = doc_;
    doc_ = doc;
    currentNote_ = noteId;
    editor_->setNoteDocument(doc);
    delete old;
    editor_->setReadOnly(meta->trashed.isValid() || !rec->loadError.isEmpty());
    dateLabel_->setText(rec->loadError.isEmpty() ? longDate(meta->modified) : rec->loadError);
    dateLabel_->setStyleSheet(QStringLiteral("color: %1").arg(rec->loadError.isEmpty() ? Theme::palette().tertiaryText.name() : QStringLiteral("#d73a3a")));
    editorStack_->setCurrentIndex(0);
    if (fromSearch || searching_)
        editor_->setSearchMatches(lastResults_.matches, true);
    if (!attachmentKey.isEmpty()) {
        // scroll to the diagram or drawing the attachment result points at
        const int slash = static_cast<int>(attachmentKey.lastIndexOf(u'/'));
        const int diagramIndex = attachmentKey.contains(QLatin1String("/diagram/")) ? attachmentKey.mid(slash + 1).toInt() : -1;
        int seen = 0;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat f = it.fragment().charFormat();
                if (!f.isImageFormat())
                    continue;
                const QString name = f.toImageFormat().name();
                const bool hit = diagramIndex >= 0 ? (!NoteDocument::diagramIdFromUrl(name).isEmpty() && seen++ == diagramIndex)
                                                   : NoteDocument::fileIdFromUrl(name) == attachmentKey;
                if (hit) {
                    QTextCursor c(doc);
                    c.setPosition(it.fragment().position());
                    editor_->setTextCursor(c);
                    editor_->ensureCursorVisible();
                }
            }
        }
    }
    if (QAction* pin = actions_.value(QStringLiteral("pin")))
        pin->setText(meta->pinned ? tr("Unpin Note") : tr("Pin Note"));
    updateFormatActions();
    if (!previous.isEmpty() && previous != noteId)
        QTimer::singleShot(0, this, [this, previous] { discardIfEmpty(previous); });
}

void MainWindow::closeNote() {
    flushEditor();
    const QString previous = currentNote_;
    NoteDocument* old = doc_;
    doc_ = nullptr;
    currentNote_.clear();
    editor_->setNoteDocument(nullptr);
    delete old;
    editorStack_->setCurrentWidget(emptyEditor_);
    if (!previous.isEmpty() && vc_->isOpen())
        QTimer::singleShot(0, this, [this, previous] { discardIfEmpty(previous); });
}

void MainWindow::scheduleFlush() {
    flushTimer_.start(kFlushDelayMs);
}

void MainWindow::flushEditor() {
    flushTimer_.stop();
    if (!doc_ || currentNote_.isEmpty() || !doc_->isModified() || editor_->isReadOnly())
        return;
    const QString md = exportMarkdown(*doc_);
    doc_->setModified(false);
    vc_->updateNoteContent(currentNote_, md);
    if (const NoteMeta* m = vc_->meta(currentNote_))
        dateLabel_->setText(longDate(m->modified));
}

void MainWindow::newNote() {
    if (!vc_->isOpen())
        return;
    if (searching_) {
        search_->clear();
        searching_ = false;
    }
    const SidebarSelection s = sidebar_->selection();
    if (s.kind == SidebarSelection::Trash)
        sidebar_->select({SidebarSelection::AllNotes, {}});
    const QString folder = s.kind == SidebarSelection::Folder ? s.id : vc_->defaultFolderId();
    // in a tag view the new note carries the tag on its own line under an empty title
    const QString markdown = s.kind == SidebarSelection::Tag ? QStringLiteral("&nbsp;\n\n#%1\n").arg(s.id) : QString();
    const QString id = vc_->createNote(folder, markdown);
    refreshList(false);
    list_->selectNote(id);
    openNote(id);
    // new notes start with the Title style
    QTextCursor c(doc_);
    c.movePosition(QTextCursor::Start);
    editor_->setTextCursor(c);
    editor_->applyParagraphStyle(ParagraphStyle::Title);
    doc_->setModified(false);
    editor_->setFocus();
}

void MainWindow::deleteCurrentNote() {
    if (currentNote_.isEmpty())
        return;
    const QString id = currentNote_;
    const NoteMeta* m = vc_->meta(id);
    if (m && m->trashed.isValid() &&
        QMessageBox::question(this, tr("Delete Permanently"), tr("This note will be deleted permanently. This cannot be undone.")) != QMessageBox::Yes)
        return;
    // select the neighbour before the row disappears
    const int row = listModel_->rowOfNote(id);
    QString next;
    for (int r = row + 1; r < listModel_->rowCount() && next.isEmpty(); ++r)
        if (listModel_->row(r).kind != ListRow::Header)
            next = listModel_->row(r).noteId;
    for (int r = row - 1; r >= 0 && next.isEmpty(); --r)
        if (listModel_->row(r).kind != ListRow::Header)
            next = listModel_->row(r).noteId;
    closeNote();
    vc_->trashNote(id);
    if (!next.isEmpty()) {
        list_->selectNote(next);
        openNote(next);
    }
}

void MainWindow::togglePinCurrent() {
    if (const NoteMeta* m = vc_->meta(currentNote_); m && !m->trashed.isValid()) {
        vc_->setPinned(currentNote_, !m->pinned);
        if (QAction* pin = actions_.value(QStringLiteral("pin")))
            pin->setText(!m->pinned ? tr("Pin Note") : tr("Unpin Note"));
    }
}

void MainWindow::updateFormatActions() {
    if (actions_.isEmpty())
        return;
    const ParagraphStyle st = editor_->currentParagraphStyle();
    const std::pair<const char*, ParagraphStyle> map[] = {
        {"title", ParagraphStyle::Title},       {"heading", ParagraphStyle::Heading},     {"subheading", ParagraphStyle::Subheading},
        {"body", ParagraphStyle::Body},         {"mono", ParagraphStyle::Monostyled},     {"bulleted", ParagraphStyle::Bulleted},
        {"dashed", ParagraphStyle::Dashed},     {"numbered", ParagraphStyle::Numbered},   {"checklist", ParagraphStyle::Checklist},
    };
    for (const auto& [key, style] : map)
        if (QAction* a = actions_.value(QString::fromLatin1(key)))
            a->setChecked(st == style);
    auto set = [this](const char* key, bool on) {
        if (QAction* a = actions_.value(QString::fromLatin1(key)))
            a->setChecked(on);
    };
    set("bold", editor_->isBold());
    set("italic", editor_->isItalic());
    set("underline", editor_->isUnderline());
    set("strike", editor_->isStrikethrough());
    set("highlight", editor_->isHighlight());
    set("code", editor_->isInlineCode());
    set("quote", editor_->isBlockQuote());
}

void MainWindow::insertLinkDialog() {
    if (!doc_ || editor_->isReadOnly())
        return;
    bool ok = false;
    const QString url = QInputDialog::getText(this, tr("Add Link"), tr("URL:"), QLineEdit::Normal, QStringLiteral("https://"), &ok);
    if (!ok || url.trimmed().isEmpty() || url.trimmed() == u"https://")
        return;
    editor_->insertLink(url.trimmed(), editor_->textCursor().hasSelection() ? QString() : url.trimmed());
}

void MainWindow::insertImageFromFile() {
    if (!doc_ || editor_->isReadOnly())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Insert Image"), {}, tr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
    if (path.isEmpty())
        return;
    QImageReader reader(path);
    const QImage img = reader.read();
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Insert Image"), reader.errorString());
        return;
    }
    if (const QString id = editor_->storePastedImage(img); !id.isEmpty())
        editor_->insertFile(id, QFileInfo(path).completeBaseName());
}

void MainWindow::insertDiagram(DiagramKind kind) {
    if (!doc_ || editor_->isReadOnly())
        return;
    const QString note = currentNote_;
    DiagramEditorDialog dlg(*diagrams_, kind, DiagramEditorDialog::defaultSource(kind), this);
    if (dlg.exec() != QDialog::Accepted || !doc_ || currentNote_ != note)
        return;
    editor_->insertDiagram(kind, dlg.source());
    flushEditor();
}

void MainWindow::editDiagram(const QString& diagramId) {
    if (!doc_)
        return;
    const DiagramRef* d = doc_->diagram(diagramId);
    if (!d)
        return;
    const QString note = currentNote_;
    DiagramEditorDialog dlg(*diagrams_, d->kind, d->source, this);
    if (editor_->isReadOnly())
        dlg.setWindowTitle(dlg.windowTitle() + tr(" (read-only)"));
    if (dlg.exec() != QDialog::Accepted || editor_->isReadOnly() || !doc_ || currentNote_ != note)
        return;
    doc_->setDiagramSource(diagramId, dlg.source());
    flushEditor();
}

void MainWindow::insertDrawing() {
    if (!doc_ || editor_->isReadOnly())
        return;
#ifdef ZN_WITH_WEBENGINE
    const QString note = currentNote_;
    ExcalidrawDialog dlg({}, this);
    if (dlg.exec() != QDialog::Accepted || !doc_ || currentNote_ != note || !vc_->isOpen())
        return;
    const QString id = vc_->addFile(currentNote_, QStringLiteral("excalidraw"), QStringLiteral("application/vnd.excalidraw+json"),
                                    dlg.sceneJson(), dlg.searchText());
    if (!dlg.previewPng().isEmpty())
        vc_->setFilePreview(id, dlg.previewPng());
    if (!dlg.darkPreviewPng().isEmpty())
        vc_->setFilePreview(id, dlg.darkPreviewPng(), true);
    editor_->insertFile(id, tr("Drawing"));
    flushEditor();
#else
    QMessageBox::information(this, tr("Drawing"), tr("This build has no Qt WebEngine, so the Excalidraw editor is not available."));
#endif
}

void MainWindow::editDrawing(const QString& fileId) {
    const FileMeta* meta = vc_->fileMeta(fileId);
    if (!meta || meta->kind != u"excalidraw")
        return;
#ifdef ZN_WITH_WEBENGINE
    const auto scene = vc_->readFile(fileId);
    const QString note = currentNote_;
    ExcalidrawDialog dlg(scene.value_or(QByteArray()), this);
    if (dlg.exec() != QDialog::Accepted || editor_->isReadOnly() || !vc_->isOpen() || currentNote_ != note)
        return;
    vc_->updateFile(fileId, dlg.sceneJson(), dlg.searchText());
    if (!dlg.previewPng().isEmpty())
        vc_->setFilePreview(fileId, dlg.previewPng());
    if (!dlg.darkPreviewPng().isEmpty())
        vc_->setFilePreview(fileId, dlg.darkPreviewPng(), true);
    if (doc_)
        doc_->refreshResource(NoteDocument::fileUrl(fileId));
#else
    QMessageBox::information(this, tr("Drawing"), tr("This build has no Qt WebEngine, so the Excalidraw editor is not available."));
#endif
}

// ------------------------------------------------------------------------------------------------ search

void MainWindow::onQueryChanged() {
    if (!vc_->isOpen())
        return;
    if (search_->isEmpty()) {
        exitSearch();
        return;
    }
    if (!searching_) {
        flushEditor();
        searching_ = true;
    }
    searchGeneration_ = vc_->search(search_->query());
}

void MainWindow::onSearchFinished(quint64 generation, const SearchResults& results) {
    if (!searching_ || generation != searchGeneration_)
        return;
    lastResults_ = results;
    const QString keep = currentNote_;
    listModel_->showSearch(*vc_, results);
    if (!keep.isEmpty() && listModel_->rowOfNote(keep) >= 0) {
        list_->selectNote(keep);
        editor_->setSearchMatches(results.matches, false);
    } else if (results.noteCount() + results.attachments.size() > 0) {
        list_->selectFirst();
    } else {
        closeNote();
    }
}

void MainWindow::exitSearch() {
    if (!searching_)
        return;
    searching_ = false;
    lastResults_ = {};
    editor_->clearSearchMatches();
    refreshList(false);
    if (!currentNote_.isEmpty() && listModel_->rowOfNote(currentNote_) >= 0)
        list_->selectNote(currentNote_);
    else {
        closeNote();
        list_->selectFirst();
    }
}

// ------------------------------------------------------------------------------------------------ misc

void MainWindow::toast(const QString& message, bool error) {
    const ThemePalette& pal = Theme::palette();
    toast_->setText(message);
    toast_->setStyleSheet(QStringLiteral("QLabel { background: %1; color: %2; border-radius: 8px; padding: 8px 14px; }")
                              .arg(error ? QStringLiteral("#d73a3a") : (pal.dark ? QStringLiteral("#3a3a3c") : QStringLiteral("#1d1d1f")),
                                   QStringLiteral("#ffffff")));
    toast_->adjustSize();
    toast_->move((width() - toast_->width()) / 2, height() - toast_->height() - 28);
    toast_->show();
    toast_->raise();
    toastTimer_.start(error ? 6000 : 2200);
}

void MainWindow::syncToolbarToSplitter() {
    if (!splitter_ || !toolbarSidebarPart_)
        return;
    const QList<int> sizes = splitter_->sizes();
    toolbarSidebarPart_->setFixedWidth(sidebarVisible_ ? std::max(0, sizes.value(0) - 8) : 40);
    toolbarListPart_->setFixedWidth(std::max(0, sizes.value(1) - (sidebarVisible_ ? 0 : 40) - 34));
}

void MainWindow::runAutomation(const QStringList& steps, const QString& screenshotPath) {
    // an unlock may still be deriving the key: start the timeline once the vault is open
    if (vc_->isBusy()) {
        connect(vc_, &VaultController::opened, this, [this, steps, screenshotPath] { startAutomation(steps, screenshotPath); },
                Qt::SingleShotConnection);
        connect(vc_, &VaultController::openFailed, this, [this, steps, screenshotPath] { startAutomation(steps, screenshotPath); },
                Qt::SingleShotConnection);
        return;
    }
    startAutomation(steps, screenshotPath);
}

void MainWindow::startAutomation(const QStringList& steps, const QString& screenshotPath) {
    // Test hook: "search:<text>", "open:<n-th row>", "find:<text>", "folder:<name>", "tag:<tag>", "wait:<ms>",
    // "diagrams:<ms>" (before quitting, wait that long for diagrams to render; exit code 3 if one did not).
    int delay = 300;
    int diagramsTimeoutMs = -1;
    for (const QString& step : steps) {
        if (step.startsWith(QLatin1String("diagrams:"))) {
            diagramsTimeoutMs = step.mid(9).toInt();
            continue;
        }
        QTimer::singleShot(delay, this, [this, step] {
            if (step.startsWith(QLatin1String("search:"))) {
                search_->focusField();
                // straight to the line edit: without a window manager (Xvfb) the window is never activated,
                // so there is no focus widget to type into
                auto* edit = search_->findChild<QLineEdit*>(QString(), Qt::FindDirectChildrenOnly);
                for (QChar c : step.mid(7)) {
                    QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                    QApplication::sendEvent(edit, &press);
                }
            } else if (step.startsWith(QLatin1String("token:"))) {
                search_->addToken({SearchToken::Has, step.mid(6), step.mid(6), QStringLiteral("checklist")});
            } else if (step.startsWith(QLatin1String("open:"))) {
                int n = step.mid(5).toInt();
                for (int r = 0; r < listModel_->rowCount(); ++r)
                    if (listModel_->row(r).kind != ListRow::Header && n-- == 0) {
                        list_->setCurrentIndex(listModel_->index(r));
                        break;
                    }
            } else if (step.startsWith(QLatin1String("find:"))) {
                findBar_->activate(step.mid(5));
            } else if (step == u"escape") {
                search_->clear();
                exitSearch();
            } else if (step.startsWith(QLatin1String("folder:"))) {
                for (const auto& f : vc_->folders())
                    if (f.name == step.mid(7))
                        sidebar_->select({SidebarSelection::Folder, f.id});
            } else if (step.startsWith(QLatin1String("note:"))) {
                QString md = step.mid(5);
                md.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
                vc_->createNote(vc_->defaultFolderId(), md);
            } else if (step.startsWith(QLatin1String("tag:"))) {
                sidebar_->select({SidebarSelection::Tag, step.mid(4)});
            } else if (step.startsWith(QLatin1String("style:"))) {
                editor_->setFocus();
                QTextCursor c = editor_->textCursor();
                c.movePosition(QTextCursor::End);
                editor_->setTextCursor(c);
            }
        });
        delay += step.startsWith(QLatin1String("wait:")) ? step.mid(5).toInt() : 400;
    }
    if (!screenshotPath.isEmpty())
        QTimer::singleShot(delay + 600, this, [this, screenshotPath, diagramsTimeoutMs] {
            finishAutomation(screenshotPath, diagramsTimeoutMs);
        });
}

void MainWindow::finishAutomation(const QString& screenshotPath, int diagramsTimeoutMs) {
    constexpr int kPoll = 250;
    if (diagramsTimeoutMs > 0 && diagrams_->busy()) {
        QTimer::singleShot(kPoll, this, [this, screenshotPath, diagramsTimeoutMs] {
            finishAutomation(screenshotPath, std::max(0, diagramsTimeoutMs - kPoll));
        });
        return;
    }
    int code = 0;
    if (diagramsTimeoutMs >= 0) {
        if (diagrams_->busy()) {
            qWarning("diagrams are still rendering");
            code = 3;
        }
        for (const QString& error : diagrams_->errors()) {
            qWarning().noquote() << "diagram failed:" << error;
            code = 3;
        }
    }
    // let the editor repaint the finished renders before grabbing
    QTimer::singleShot(kPoll, this, [this, screenshotPath, code] {
        grab().save(screenshotPath);
        qInfo().noquote() << "screenshot:" << screenshotPath;
        QApplication::exit(code);
    });
}

}  // namespace zn
