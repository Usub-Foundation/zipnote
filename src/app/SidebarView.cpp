#include "SidebarView.h"

#include "Icons.h"
#include "NoteListModel.h"
#include "Theme.h"
#include "VaultController.h"

#include <QDragEnterEvent>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QToolButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <functional>
#include <algorithm>

namespace zn {

namespace {

enum Roles { KindRole = Qt::UserRole + 1, IdRole, CountRole, IconRole };
constexpr int kHeaderKind = 100;

class SidebarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        const bool header = index.data(KindRole).toInt() == kHeaderKind;
        return {opt.rect.width(), header ? 30 : 30};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        const ThemePalette& pal = Theme::palette();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect;
        if (index.data(KindRole).toInt() == kHeaderKind) {
            p->setFont(Theme::instance().uiFont(QFont().pointSizeF() - 0.5, QFont::Bold));
            p->setPen(pal.tertiaryText);
            p->drawText(r.adjusted(14, 6, -10, 0), Qt::AlignLeft | Qt::AlignVCenter, index.data().toString());
            p->restore();
            return;
        }
        const bool selected = opt.state & QStyle::State_Selected;
        if (selected) {
            QPainterPath path;
            path.addRoundedRect(QRectF(r).adjusted(8, 1, -8, -1), 7, 7);
            const bool focus = opt.widget && opt.widget->hasFocus();
            p->fillPath(path, focus ? pal.selection : pal.sidebarSelectionInactive);
        }
        const int depth = [&] {
            int d = 0;
            for (QModelIndex i = index.parent(); i.isValid(); i = i.parent())
                ++d;
            return d;
        }();
        const int x = r.left() + 16 + std::max(0, depth - 1) * 16;
        const QString iconName = index.data(IconRole).toString();
        const QColor iconColor = iconName == u"trash" ? pal.secondaryText : pal.accent;
        p->drawPixmap(QRect(x, r.top() + (r.height() - 18) / 2, 18, 18),
                      icons::pixmap(iconName, 18, iconColor, p->device()->devicePixelRatioF()));
        const QString count = index.data(CountRole).toString();
        QFont f = Theme::instance().uiFont(QFont().pointSizeF() + 0.5);
        p->setFont(f);
        const QFontMetrics fm(f);
        const int countW = count.isEmpty() ? 0 : fm.horizontalAdvance(count) + 8;
        p->setPen(pal.title);
        const QRect textRect(x + 26, r.top(), r.right() - 16 - countW - (x + 26), r.height());
        p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(index.data().toString(), Qt::ElideRight, textRect.width()));
        if (!count.isEmpty()) {
            p->setPen(pal.tertiaryText);
            p->drawText(QRect(r.right() - 16 - countW, r.top(), countW, r.height()), Qt::AlignRight | Qt::AlignVCenter, count);
        }
        p->restore();
    }
};

QStandardItem* makeItem(const QString& text, int kind, const QString& id, const QString& icon, int count) {
    auto* it = new QStandardItem(text);
    it->setData(kind, KindRole);
    it->setData(id, IdRole);
    it->setData(icon, IconRole);
    it->setData(count > 0 ? QString::number(count) : QString(), CountRole);
    it->setEditable(kind == SidebarSelection::Folder);
    it->setDropEnabled(kind == SidebarSelection::Folder);
    it->setDragEnabled(false);
    return it;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ TagCloud

TagCloud::TagCloud(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void TagCloud::setTags(const QStringList& tags) {
    tags_ = tags;
    updateGeometry();
    update();
}

void TagCloud::setSelectedTag(const QString& tag) {
    selected_ = tag;
    update();
}

QList<QRect> TagCloud::layoutChips(int width) const {
    QList<QRect> out;
    const QFontMetrics fm(Theme::instance().uiFont(QFont().pointSizeF()));
    int x = 12;
    int y = 2;
    const int h = fm.height() + 8;
    for (const auto& t : tags_) {
        const int w = std::min(width - 24, fm.horizontalAdvance(u'#' + t) + 18);
        if (x + w > width - 12 && x > 12) {
            x = 12;
            y += h + 6;
        }
        out.append(QRect(x, y, w, h));
        x += w + 6;
    }
    return out;
}

int TagCloud::heightForWidth(int w) const {
    const auto rects = layoutChips(w);
    return rects.isEmpty() ? 0 : rects.last().bottom() + 8;
}

QSize TagCloud::sizeHint() const {
    return {200, heightForWidth(width() > 0 ? width() : 200)};
}

void TagCloud::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const ThemePalette& pal = Theme::palette();
    const QFont f = Theme::instance().uiFont(QFont().pointSizeF());
    p.setFont(f);
    const auto rects = layoutChips(width());
    for (int i = 0; i < tags_.size(); ++i) {
        const bool sel = tags_[i].compare(selected_, Qt::CaseInsensitive) == 0;
        QPainterPath path;
        path.addRoundedRect(QRectF(rects[i]), rects[i].height() / 2.0, rects[i].height() / 2.0);
        p.fillPath(path, sel ? pal.accent : (i == hover_ ? pal.chip.darker(105) : pal.chip));
        p.setPen(sel ? QColor(0x1D, 0x1D, 0x1F) : pal.chipText);
        p.drawText(rects[i], Qt::AlignCenter, QFontMetrics(f).elidedText(u'#' + tags_[i], Qt::ElideRight, rects[i].width() - 12));
    }
}

void TagCloud::mousePressEvent(QMouseEvent* e) {
    const auto rects = layoutChips(width());
    for (int i = 0; i < rects.size(); ++i)
        if (rects[i].contains(e->pos())) {
            emit tagClicked(tags_[i]);
            return;
        }
}

void TagCloud::mouseMoveEvent(QMouseEvent* e) {
    const auto rects = layoutChips(width());
    int h = -1;
    for (int i = 0; i < rects.size(); ++i)
        if (rects[i].contains(e->pos()))
            h = i;
    if (h != hover_) {
        hover_ = h;
        setCursor(h >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void TagCloud::leaveEvent(QEvent*) {
    hover_ = -1;
    update();
}

// ------------------------------------------------------------------------------------------------ FolderTree

void FolderTree::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(QString::fromLatin1(NoteListModel::kNoteMime)))
        e->acceptProposedAction();
}

void FolderTree::dragMoveEvent(QDragMoveEvent* e) {
    const QModelIndex i = indexAt(e->position().toPoint());
    if (i.isValid() && i.data(KindRole).toInt() == SidebarSelection::Folder) {
        e->acceptProposedAction();
        setCurrentIndex(i);
    } else {
        e->ignore();
    }
}

void FolderTree::dropEvent(QDropEvent* e) {
    const QModelIndex i = indexAt(e->position().toPoint());
    const QStringList ids = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(NoteListModel::kNoteMime))).split(u',');
    if (i.isValid() && i.data(KindRole).toInt() == SidebarSelection::Folder) {
        e->acceptProposedAction();
        emit notesDropped(ids, i);
    }
}

void FolderTree::closeEditor(QWidget* editor, QAbstractItemDelegate::EndEditHint hint) {
    QTreeView::closeEditor(editor, hint);
    emit editorClosed();
}

void FolderTree::drawRow(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
    QStyleOptionViewItem o = opt;
    o.rect = QRect(0, opt.rect.top(), viewport()->width(), opt.rect.height());
    // QTreeView::drawRow is what adds the row's own state; the delegate is called directly here
    o.state.setFlag(QStyle::State_Selected, selectionModel()->isSelected(index));
    itemDelegateForIndex(index)->paint(p, o, index);
}

// ------------------------------------------------------------------------------------------------ SidebarView

SidebarView::SidebarView(VaultController& vc, QWidget* parent) : QWidget(parent), vc_(vc) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 6, 0, 8);
    lay->setSpacing(0);

    tree_ = new FolderTree(this);
    tree_->setModel(&model_);
    tree_->setItemDelegate(new SidebarDelegate(tree_));
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setIndentation(0);
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(false);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(false);
    tree_->setDragDropMode(QAbstractItemView::DropOnly);
    tree_->setAutoFillBackground(false);
    tree_->viewport()->setAutoFillBackground(false);
    tree_->setStyleSheet(QStringLiteral("QTreeView { background: transparent; }"));
    lay->addWidget(tree_, 1);

    // Tags: a collapsible section whose cloud scrolls inside a capped height, so many tags never push the
    // folders out of the sidebar
    tagsHeader_ = new QToolButton(this);
    tagsHeader_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tagsHeader_->setAutoRaise(true);
    tagsHeader_->setFocusPolicy(Qt::NoFocus);
    tagsHeader_->setCursor(Qt::PointingHandCursor);
    tagsHeader_->setFont(Theme::instance().uiFont(QFont().pointSizeF() - 0.5, QFont::Bold));
    tagsHeader_->setIconSize(QSize(12, 12));
    tagsHeader_->setStyleSheet(QStringLiteral("QToolButton { border: none; background: transparent; padding: 8px 8px 6px 10px; }"));
    lay->addWidget(tagsHeader_);
    tagsScroll_ = new QScrollArea(this);
    tagsScroll_->setFrameShape(QFrame::NoFrame);
    tagsScroll_->setWidgetResizable(true);
    tagsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tagsScroll_->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    tags_ = new TagCloud(tagsScroll_);
    tagsScroll_->setWidget(tags_);
    lay->addWidget(tagsScroll_);
    tagsCollapsed_ = QSettings().value(QStringLiteral("sidebar/tagsCollapsed"), false).toBool();
    connect(tagsHeader_, &QToolButton::clicked, this, [this] {
        tagsCollapsed_ = !tagsCollapsed_;
        QSettings().setValue(QStringLiteral("sidebar/tagsCollapsed"), tagsCollapsed_);
        updateTagsArea();
    });

    auto* newFolder = new QPushButton(icons::icon("folderplus"), tr("New Folder"), this);
    newFolder->setFlat(true);
    newFolder->setStyleSheet(QStringLiteral("QPushButton { border: none; background: transparent; text-align: left; padding: 6px 14px; }"));
    lay->addWidget(newFolder);

    connect(newFolder, &QPushButton::clicked, this, [this] { emit newFolderRequested({}); });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur) { onCurrentChanged(cur); });
    // clicks as well as current-row changes: re-clicking a row after picking a tag must select it again
    connect(tree_, &QAbstractItemView::clicked, this, [this](const QModelIndex& i) { onCurrentChanged(i); });
    connect(tree_, &QWidget::customContextMenuRequested, this, &SidebarView::showContextMenu);
    connect(tree_, &FolderTree::notesDropped, this, [this](const QStringList& ids, const QModelIndex& target) {
        emit notesDroppedOnFolder(ids, target.data(IdRole).toString());
    });
    connect(&model_, &QStandardItemModel::itemChanged, this, [this](QStandardItem* item) {
        if (rebuilding_ || item->data(KindRole).toInt() != SidebarSelection::Folder)
            return;
        // the rename rebuilds this model, and itemChanged comes from the model's own dataChanged slot: the
        // tree gets the same dataChanged after us, so rebuilding here hands it an index into freed rows
        QMetaObject::invokeMethod(this, [this, id = item->data(IdRole).toString(), name = item->text()] {
            emit renameFolderRequested(id, name);
        }, Qt::QueuedConnection);
    });
    connect(tree_, &FolderTree::editorClosed, this, [this] {
        if (rebuildPending_)
            QMetaObject::invokeMethod(this, [this] {
                rebuildPending_ = false;
                rebuild();
            }, Qt::QueuedConnection);
    });
    connect(tags_, &TagCloud::tagClicked, this, [this](const QString& tag) {
        if (selection_.kind == SidebarSelection::Tag && selection_.id == tag)
            select({SidebarSelection::AllNotes, {}});
        else
            select({SidebarSelection::Tag, tag});
    });
    connect(&Theme::instance(), &Theme::changed, this, [this] { update(); });
}

void SidebarView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Theme::palette().sidebar);
}

void SidebarView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    updateTagsArea();
}

void SidebarView::updateTagsArea() {
    tagsHeader_->setVisible(tagCount_ > 0);
    tagsHeader_->setText(QStringLiteral("%1  %2").arg(tr("Tags")).arg(tagCount_));
    tagsHeader_->setIcon(icons::icon(tagsCollapsed_ ? QStringLiteral("chevronright") : QStringLiteral("chevrondown"),
                                     Theme::palette().tertiaryText));
    const bool show = tagCount_ > 0 && !tagsCollapsed_;
    tagsScroll_->setVisible(show);
    if (!show)
        return;
    // at most about a third of the sidebar; the rest scrolls
    const int cap = std::max(72, height() / 3);
    const int needed = tags_->heightForWidth(std::max(120, tagsScroll_->viewport()->width()));
    tagsScroll_->setFixedHeight(std::min(needed, cap));
    tagsScroll_->setVerticalScrollBarPolicy(needed > cap ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void SidebarView::rebuild() {
    // typing calls this often: skip when nothing visible changed
    QString sig = vc_.vaultName() + u'|' + QString::number(vc_.trashCount());
    for (const auto& f : vc_.folders())
        sig += f.id + f.name + f.parentId + QString::number(vc_.noteCount(f.id)) + u'|';
    for (const auto& t : vc_.tags())
        sig += t.tag + u'|';
    int all = 0;
    for (const NoteRecord* r : vc_.records())
        if (const NoteMeta* m = vc_.meta(r->id); m && !m->trashed.isValid())
            ++all;
    sig += QString::number(all);
    if (sig == signature_ && model_.rowCount() > 0)
        return;
    // resetting the model would close the name editor and drop what was typed: rebuild once it closes
    if (tree_->isEditing()) {
        rebuildPending_ = true;
        return;
    }
    signature_ = sig;
    rebuilding_ = true;
    model_.clear();
    QStandardItem* root = model_.invisibleRootItem();
    root->appendRow(makeItem(vc_.vaultName(), kHeaderKind, {}, {}, 0));
    root->child(0)->setFlags(Qt::ItemIsEnabled);

    root->appendRow(makeItem(tr("All Notes"), SidebarSelection::AllNotes, {}, QStringLiteral("notes"), all));

    QList<Folder> folders = vc_.folders();
    std::stable_sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) { return a.order < b.order; });
    QHash<QString, QStandardItem*> items;
    const QString defaultId = vc_.defaultFolderId();
    // parents before children: iterate until every folder is placed (orphans go to the top level)
    QList<Folder> pending = folders;
    for (int guard = 0; !pending.isEmpty() && guard < 64; ++guard) {
        QList<Folder> next;
        for (const auto& f : pending) {
            QStandardItem* parent = f.parentId.isEmpty() ? root : items.value(f.parentId);
            if (!parent && guard < 63) {
                next.append(f);
                continue;
            }
            if (!parent)
                parent = root;
            auto* it = makeItem(f.name, SidebarSelection::Folder, f.id, QStringLiteral("folder"), vc_.noteCount(f.id));
            it->setEditable(true);
            if (f.id == defaultId)
                it->setDragEnabled(false);
            parent->appendRow(it);
            items.insert(f.id, it);
        }
        pending = next;
    }
    const int trash = vc_.trashCount();
    if (trash > 0)
        root->appendRow(makeItem(tr("Recently Deleted"), SidebarSelection::Trash, {}, QStringLiteral("trash"), trash));
    tree_->expandAll();

    QStringList tagNames;
    for (const auto& t : vc_.tags())
        tagNames.append(t.tag);
    tags_->setTags(tagNames);
    tagCount_ = static_cast<int>(tagNames.size());
    updateTagsArea();
    rebuilding_ = false;

    // keep the selection if it still exists
    if (selection_.kind == SidebarSelection::Tag && !tagNames.contains(selection_.id, Qt::CaseInsensitive))
        selection_ = {SidebarSelection::AllNotes, {}};
    if (selection_.kind == SidebarSelection::Trash && trash == 0)
        selection_ = {SidebarSelection::AllNotes, {}};
    if (selection_.kind == SidebarSelection::Folder && !items.contains(selection_.id))
        selection_ = {SidebarSelection::AllNotes, {}};
    const SidebarSelection keep = selection_;
    selection_ = {SidebarSelection::Kind(-1), {}};
    select(keep);
}

QStandardItem* SidebarView::itemFor(const SidebarSelection& s) const {
    std::function<QStandardItem*(QStandardItem*)> walk = [&](QStandardItem* parent) -> QStandardItem* {
        for (int i = 0; i < parent->rowCount(); ++i) {
            QStandardItem* it = parent->child(i);
            if (it->data(KindRole).toInt() == s.kind && (s.kind != SidebarSelection::Folder || it->data(IdRole).toString() == s.id))
                return it;
            if (QStandardItem* found = walk(it))
                return found;
        }
        return nullptr;
    };
    return walk(model_.invisibleRootItem());
}

void SidebarView::select(const SidebarSelection& s) {
    if (s == selection_)
        return;
    selection_ = s;
    tags_->setSelectedTag(s.kind == SidebarSelection::Tag ? s.id : QString());
    {
        const QSignalBlocker block(tree_->selectionModel());
        if (QStandardItem* it = s.kind == SidebarSelection::Tag ? nullptr : itemFor(s)) {
            tree_->setCurrentIndex(it->index());
        } else {
            // no current row either: otherwise clicking the folder that was current changes nothing
            tree_->selectionModel()->clearSelection();
            tree_->selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
        }
    }
    tree_->viewport()->update();
    emit selectionChanged(selection_);
}

void SidebarView::onCurrentChanged(const QModelIndex& index) {
    if (rebuilding_ || !index.isValid())
        return;
    const int kind = index.data(KindRole).toInt();
    if (kind == kHeaderKind)
        return;
    select({SidebarSelection::Kind(kind), index.data(IdRole).toString()});
}

void SidebarView::editFolderName(const QString& folderId) {
    if (QStandardItem* it = itemFor({SidebarSelection::Folder, folderId})) {
        tree_->setCurrentIndex(it->index());
        tree_->edit(it->index());
    }
}

void SidebarView::showContextMenu(const QPoint& pos) {
    const QModelIndex i = tree_->indexAt(pos);
    QMenu menu(this);
    const int kind = i.isValid() ? i.data(KindRole).toInt() : -1;
    const QString id = i.data(IdRole).toString();
    if (kind == SidebarSelection::Folder) {
        menu.addAction(icons::icon("folderplus"), tr("New Subfolder"), this, [this, id] { emit newFolderRequested(id); });
        menu.addAction(tr("Rename Folder"), this, [this, i] { tree_->edit(i); });
        auto* del = menu.addAction(icons::icon("trash"), tr("Delete Folder"), this, [this, id] { emit deleteFolderRequested(id); });
        del->setEnabled(id != vc_.defaultFolderId());
    } else if (kind == SidebarSelection::Trash) {
        menu.addAction(icons::icon("trash"), tr("Empty Recently Deleted"), this, [this] { emit emptyTrashRequested(); });
    } else {
        menu.addAction(icons::icon("folderplus"), tr("New Folder"), this, [this] { emit newFolderRequested({}); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

}  // namespace zn
