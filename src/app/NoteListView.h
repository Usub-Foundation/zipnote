#pragma once

#include <QListView>
#include <QStyledItemDelegate>

namespace zn {

class NoteListModel;

class NoteListDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
};

class NoteListView : public QListView {
    Q_OBJECT
public:
    explicit NoteListView(NoteListModel* model, QWidget* parent = nullptr);

    QString currentNoteId() const;
    QString currentAttachmentKey() const;
    void selectNote(const QString& noteId, const QString& attachmentKey = {});
    void selectFirst();

signals:
    void noteSelected(const QString& noteId, const QString& attachmentKey);
    void contextMenuForNote(const QString& noteId, const QPoint& globalPos);
    void deleteRequested();

protected:
    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override;
    void paintEvent(QPaintEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    NoteListModel* model_;
};

}  // namespace zn
