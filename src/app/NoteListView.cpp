#include "NoteListView.h"

#include "Icons.h"
#include "NoteListModel.h"
#include "Theme.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <algorithm>

namespace zn {

namespace {

constexpr int kHeaderHeight = 32;
constexpr int kRowHeight = 62;
constexpr int kRowWithFolderHeight = 80;
constexpr int kSidePadding = 20;
constexpr int kSelectionInset = 8;

QFont titleFont() { return Theme::instance().uiFont(QFont().pointSizeF() + 1.0, QFont::Bold); }
QFont bodyFont() { return Theme::instance().uiFont(QFont().pointSizeF() + 0.5); }
QFont smallFont() { return Theme::instance().uiFont(QFont().pointSizeF()); }

// One elided line with highlighted ranges, the way list cells show search matches.
void drawLine(QPainter* p, const QRectF& rect, const QString& text, const QList<Highlight>& hls, const QFont& font,
              const QColor& color, const QColor& highlight) {
    const QFontMetricsF fm(font);
    const QString elided = fm.elidedText(text, Qt::ElideRight, rect.width());
    QTextLayout layout(elided, font);
    QList<QTextLayout::FormatRange> ranges;
    for (const auto& h : hls) {
        if (h.start >= elided.size())
            continue;
        QTextLayout::FormatRange r;
        r.start = h.start;
        r.length = std::min<int>(h.length, static_cast<int>(elided.size()) - h.start);
        r.format.setBackground(highlight);
        r.format.setForeground(color);
        ranges.append(r);
    }
    layout.setFormats(ranges);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(rect.width() + 50);
    layout.endLayout();
    p->setPen(color);
    layout.draw(p, QPointF(rect.left(), rect.top() + (rect.height() - line.height()) / 2));
}

}  // namespace

QSize NoteListDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const {
    const auto* model = static_cast<const NoteListModel*>(index.model());
    const ListRow& r = model->row(index.row());
    if (r.kind == ListRow::Header)
        return {opt.rect.width(), kHeaderHeight};
    return {opt.rect.width(), r.folder.isEmpty() ? kRowHeight : kRowWithFolderHeight};
}

void NoteListDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
    const auto* model = static_cast<const NoteListModel*>(index.model());
    const ListRow& r = model->row(index.row());
    const ThemePalette& pal = Theme::palette();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRect rect = opt.rect;

    if (r.kind == ListRow::Header) {
        QFont f = Theme::instance().uiFont(QFont().pointSizeF() + 0.5, QFont::Bold);
        p->setFont(f);
        p->setPen(pal.title);
        p->drawText(rect.adjusted(kSidePadding, 8, -kSidePadding, 0), Qt::AlignLeft | Qt::AlignVCenter, r.header);
        p->restore();
        return;
    }

    const bool selected = opt.state & QStyle::State_Selected;
    const bool focused = opt.state & QStyle::State_HasFocus || (opt.widget && opt.widget->hasFocus());
    if (selected) {
        QPainterPath path;
        path.addRoundedRect(QRectF(rect).adjusted(kSelectionInset, 1, -kSelectionInset, -1), 8, 8);
        p->fillPath(path, focused ? pal.selection : pal.selectionInactive);
    } else {
        // separator under each row
        const int next = index.row() + 1;
        const bool nextSelected = opt.widget && next < model->rowCount() &&
                                  static_cast<const QAbstractItemView*>(opt.widget)->selectionModel()->isRowSelected(next);
        const bool nextHeader = next < model->rowCount() && model->row(next).kind == ListRow::Header;
        if (!nextSelected && !nextHeader && next < model->rowCount())
            p->fillRect(QRect(rect.left() + kSidePadding, rect.bottom(), rect.width() - 2 * kSidePadding, 1), pal.separator);
    }

    int x = rect.left() + kSidePadding;
    const int right = rect.right() - kSidePadding;
    if (r.kind == ListRow::Attachment) {
        const QRect tile(x, rect.top() + (rect.height() - 40) / 2, 40, 40);
        QPainterPath path;
        path.addRoundedRect(QRectF(tile), 8, 8);
        p->fillPath(path, pal.dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 12));
        const QString iconName = r.attachmentKind == u"mermaid"    ? QStringLiteral("mermaid")
                                 : r.attachmentKind == u"plantuml" ? QStringLiteral("plantuml")
                                 : r.attachmentKind == u"image"    ? QStringLiteral("image")
                                                                   : QStringLiteral("drawing");
        p->drawPixmap(tile.adjusted(9, 9, -9, -9), icons::pixmap(iconName, 22, pal.secondaryText, p->device()->devicePixelRatioF()));
        x = tile.right() + 12;
    }

    const int top = rect.top() + 10;
    const QColor titleColor = r.damaged ? QColor(0xD7, 0x3A, 0x3A) : pal.title;
    drawLine(p, QRectF(x, top, right - x, 20), r.title, r.titleHighlights, titleFont(), titleColor, pal.searchHighlight);

    QString date = NoteListModel::dateLabel(r.modified);
    if (r.kind == ListRow::Attachment) {
        const QString kind = r.attachmentKind == u"mermaid"    ? QStringLiteral("Mermaid")
                             : r.attachmentKind == u"plantuml" ? QStringLiteral("PlantUML")
                             : r.attachmentKind == u"image"    ? QObject::tr("Image")
                                                               : QStringLiteral("Excalidraw");
        date = kind;
    }
    const QFont dateFont = Theme::instance().uiFont(bodyFont().pointSizeF(), QFont::DemiBold);
    p->setFont(dateFont);
    p->setPen(pal.secondaryText);
    const QFontMetricsF dfm(dateFont);
    const qreal dateWidth = dfm.horizontalAdvance(date);
    const QRectF second(x, top + 22, right - x, 18);
    p->drawText(second, Qt::AlignLeft | Qt::AlignVCenter, date);
    const qreal snippetX = second.left() + dateWidth + 8;
    drawLine(p, QRectF(snippetX, second.top(), second.right() - snippetX, second.height()), r.snippet,
             r.snippetHighlights, bodyFont(), pal.secondaryText, pal.searchHighlight);

    if (!r.folder.isEmpty()) {
        const QRectF third(x, top + 42, right - x, 18);
        p->drawPixmap(QRectF(third.left(), third.top() + 2, 14, 14).toRect(),
                      icons::pixmap("folder", 14, pal.tertiaryText, p->device()->devicePixelRatioF()));
        p->setFont(smallFont());
        p->setPen(pal.tertiaryText);
        p->drawText(third.adjusted(19, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetricsF(smallFont()).elidedText(r.folder, Qt::ElideRight, third.width() - 19));
    }
    p->restore();
}

NoteListView::NoteListView(NoteListModel* model, QWidget* parent) : QListView(parent), model_(model) {
    setModel(model);
    setItemDelegate(new NoteListDelegate(this));
    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setUniformItemSizes(false);
    setMouseTracking(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    viewport()->setAutoFillBackground(false);
    connect(selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& cur) {
        if (!cur.isValid() || model_->row(cur.row()).kind == ListRow::Header)
            return;
        const ListRow& r = model_->row(cur.row());
        emit noteSelected(r.noteId, r.attachmentKey);
    });
}

QString NoteListView::currentNoteId() const {
    const QModelIndex i = currentIndex();
    return i.isValid() && model_->row(i.row()).kind != ListRow::Header ? model_->row(i.row()).noteId : QString();
}

QString NoteListView::currentAttachmentKey() const {
    const QModelIndex i = currentIndex();
    return i.isValid() ? model_->row(i.row()).attachmentKey : QString();
}

void NoteListView::selectNote(const QString& noteId, const QString& attachmentKey) {
    const int row = model_->rowOfNote(noteId, attachmentKey);
    if (row < 0)
        return;
    const QSignalBlocker block(selectionModel());
    setCurrentIndex(model_->index(row));
    scrollTo(model_->index(row));
    viewport()->update();
}

void NoteListView::selectFirst() {
    for (int i = 0; i < model_->rowCount(); ++i)
        if (model_->row(i).kind != ListRow::Header) {
            setCurrentIndex(model_->index(i));
            return;
        }
}

QModelIndex NoteListView::moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) {
    QModelIndex idx = QListView::moveCursor(action, modifiers);
    // section headers are not selectable: step over them in the direction of travel
    const bool up = action == MoveUp || action == MovePrevious || action == MovePageUp || action == MoveHome;
    while (idx.isValid() && model_->row(idx.row()).kind == ListRow::Header) {
        const int next = idx.row() + (up ? -1 : 1);
        if (next < 0 || next >= model_->rowCount())
            return currentIndex();
        idx = model_->index(next);
    }
    return idx;
}

void NoteListView::paintEvent(QPaintEvent* e) {
    QPainter bg(viewport());
    bg.fillRect(viewport()->rect(), Theme::palette().list);
    bg.end();
    QListView::paintEvent(e);
    if (model_->rowCount() == 0) {
        QPainter p(viewport());
        p.setPen(Theme::palette().tertiaryText);
        p.setFont(Theme::instance().uiFont(QFont().pointSizeF() + 3, QFont::DemiBold));
        p.drawText(viewport()->rect(), Qt::AlignCenter, model_->emptyText());
    }
}

void NoteListView::contextMenuEvent(QContextMenuEvent* e) {
    const QModelIndex i = indexAt(e->pos());
    if (!i.isValid() || model_->row(i.row()).kind == ListRow::Header)
        return;
    setCurrentIndex(i);
    emit contextMenuForNote(model_->row(i.row()).noteId, e->globalPos());
}

void NoteListView::keyPressEvent(QKeyEvent* e) {
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && !currentNoteId().isEmpty()) {
        emit deleteRequested();
        return;
    }
    QListView::keyPressEvent(e);
}

void NoteListView::focusInEvent(QFocusEvent* e) {
    QListView::focusInEvent(e);
    viewport()->update();
}

void NoteListView::focusOutEvent(QFocusEvent* e) {
    QListView::focusOutEvent(e);
    viewport()->update();
}

}  // namespace zn
