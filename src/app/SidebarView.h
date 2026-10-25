#pragma once

#include <QStandardItemModel>

class QScrollArea;
class QToolButton;
#include <QTreeView>
#include <QWidget>

namespace zn {

class VaultController;

struct SidebarSelection {
    enum Kind { AllNotes, Folder, Trash, Tag };
    Kind kind = AllNotes;
    QString id;  // folder id or tag
    bool operator==(const SidebarSelection&) const = default;
};

// Tags as chips under the folder list.
class TagCloud : public QWidget {
    Q_OBJECT
public:
    explicit TagCloud(QWidget* parent = nullptr);
    void setTags(const QStringList& tags);
    void setSelectedTag(const QString& tag);
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

signals:
    void tagClicked(const QString& tag);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QList<QRect> layoutChips(int width) const;
    QStringList tags_;
    QString selected_;
    int hover_ = -1;
};

class FolderTree : public QTreeView {
    Q_OBJECT
public:
    using QTreeView::QTreeView;
    bool isEditing() const { return state() == EditingState; }

signals:
    void notesDropped(const QStringList& noteIds, const QModelIndex& target);
    void editorClosed();  // a folder name editor closed, committed or not

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void drawRow(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
    void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}
    // the delegate's closeEditor signal and the view itself (moving the current row) both end up here
    void closeEditor(QWidget* editor, QAbstractItemDelegate::EndEditHint hint) override;
};

class SidebarView : public QWidget {
    Q_OBJECT
public:
    explicit SidebarView(VaultController& vc, QWidget* parent = nullptr);

    void rebuild();  // folders, counts or tags changed
    SidebarSelection selection() const { return selection_; }
    void select(const SidebarSelection& s);
    void editFolderName(const QString& folderId);

signals:
    void selectionChanged(const SidebarSelection& s);
    void newFolderRequested(const QString& parentId);
    void renameFolderRequested(const QString& folderId, const QString& name);
    void deleteFolderRequested(const QString& folderId);
    void notesDroppedOnFolder(const QStringList& noteIds, const QString& folderId);
    void emptyTrashRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void updateTagsArea();
    QStandardItem* itemFor(const SidebarSelection& s) const;
    void onCurrentChanged(const QModelIndex& index);
    void showContextMenu(const QPoint& pos);

    VaultController& vc_;
    QStandardItemModel model_;
    FolderTree* tree_;
    TagCloud* tags_;
    QToolButton* tagsHeader_;
    QScrollArea* tagsScroll_;
    int tagCount_ = 0;
    bool tagsCollapsed_ = false;
    SidebarSelection selection_;
    bool rebuilding_ = false;
    bool rebuildPending_ = false;  // a rebuild came in while a folder name was being edited
    QString signature_;
};

}  // namespace zn
