#include "NoteListModel.h"

#include "VaultController.h"

#include <QLocale>
#include <QMimeData>
#include <algorithm>

namespace zn {

namespace {

QString dateSection(const QDateTime& utc, const QDate& today) {
    const QDate d = utc.toLocalTime().date();
    const qint64 days = d.daysTo(today);
    if (days <= 0)
        return NoteListModel::tr("Today");
    if (days == 1)
        return NoteListModel::tr("Yesterday");
    if (days < 7)
        return NoteListModel::tr("Previous 7 Days");
    if (days < 30)
        return NoteListModel::tr("Previous 30 Days");
    if (d.year() == today.year())
        return QLocale().standaloneMonthName(d.month());
    return QString::number(d.year());
}

ListRow headerRow(const QString& text) {
    ListRow r;
    r.kind = ListRow::Header;
    r.header = text;
    return r;
}

ListRow noteRow(VaultController& vc, const QString& id, bool showFolder) {
    ListRow r;
    r.kind = ListRow::Note;
    r.noteId = id;
    const NoteRecord* rec = vc.record(id);
    const NoteMeta* m = vc.meta(id);
    if (rec) {
        r.title = rec->digest.title.isEmpty() ? NoteListModel::tr("New Note") : rec->digest.title;
        r.snippet = rec->digest.snippet.isEmpty() ? NoteListModel::tr("No additional text") : rec->digest.snippet;
        r.damaged = !rec->loadError.isEmpty();
    }
    if (m) {
        r.modified = m->modified;
        if (showFolder)
            r.folder = vc.folderPath(m->folderId);
    }
    return r;
}

}  // namespace

NoteListModel::NoteListModel(QObject* parent) : QAbstractListModel(parent) {}

int NoteListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant NoteListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size())
        return {};
    const ListRow& r = rows_[index.row()];
    if (role == Qt::DisplayRole)
        return r.kind == ListRow::Header ? r.header : r.title;
    if (role == Qt::ToolTipRole && r.kind != ListRow::Header)
        return r.folder.isEmpty() ? r.title : r.title + u'\n' + r.folder;
    return {};
}

Qt::ItemFlags NoteListModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    if (rows_[index.row()].kind == ListRow::Header)
        return Qt::ItemIsEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList NoteListModel::mimeTypes() const {
    return {QString::fromLatin1(kNoteMime)};
}

QMimeData* NoteListModel::mimeData(const QModelIndexList& indexes) const {
    QStringList ids;
    for (const auto& i : indexes)
        if (i.isValid() && rows_[i.row()].kind != ListRow::Header)
            ids.append(rows_[i.row()].noteId);
    if (ids.isEmpty())
        return nullptr;
    auto* m = new QMimeData;
    m->setData(QString::fromLatin1(kNoteMime), ids.join(u',').toUtf8());
    return m;
}

QString NoteListModel::layoutSignature() const {
    QString sig;
    for (const auto& r : rows_)
        sig += (r.kind == ListRow::Header ? r.header : r.noteId) + u'|';
    return sig;
}

int NoteListModel::rowOfNote(const QString& noteId, const QString& attachmentKey) const {
    for (int i = 0; i < rows_.size(); ++i)
        if (rows_[i].kind != ListRow::Header && rows_[i].noteId == noteId &&
            (attachmentKey.isEmpty() || rows_[i].attachmentKey == attachmentKey))
            return i;
    return -1;
}

QString NoteListModel::dateLabel(const QDateTime& utc) {
    if (!utc.isValid())
        return {};
    const QDateTime local = utc.toLocalTime();
    const QDate today = QDate::currentDate();
    const qint64 days = local.date().daysTo(today);
    if (days <= 0)
        return QLocale().toString(local.time(), QLocale::ShortFormat);
    if (days == 1)
        return tr("Yesterday");
    if (days < 7)
        return QLocale().dayName(local.date().dayOfWeek());
    return QLocale().toString(local.date(), QLocale::ShortFormat);
}

void NoteListModel::showNotes(VaultController& vc, const QStringList& noteIds, NoteSort sort, bool showFolder, bool trash) {
    beginResetModel();
    rows_.clear();
    search_ = false;

    struct Item {
        QString id;
        const NoteMeta* meta;
        QString title;
    };
    QList<Item> items;
    for (const auto& id : noteIds)
        if (const NoteMeta* m = vc.meta(id)) {
            const NoteRecord* r = vc.record(id);
            items.append({id, m, r ? r->digest.title : QString()});
        }
    std::stable_sort(items.begin(), items.end(), [sort](const Item& a, const Item& b) {
        switch (sort) {
        case NoteSort::Title: return a.title.localeAwareCompare(b.title) < 0;
        case NoteSort::DateCreated: return a.meta->created > b.meta->created;
        case NoteSort::DateEdited: break;
        }
        return a.meta->modified > b.meta->modified;
    });

    const QDate today = QDate::currentDate();
    auto append = [&](const QList<Item>& list, bool sections) {
        QString current;
        for (const auto& it : list) {
            if (sections && sort != NoteSort::Title) {
                const QString s = dateSection(sort == NoteSort::DateCreated ? it.meta->created : it.meta->modified, today);
                if (s != current) {
                    rows_.append(headerRow(s));
                    current = s;
                }
            }
            rows_.append(noteRow(vc, it.id, showFolder));
        }
    };

    QList<Item> pinned, rest;
    for (const auto& it : items)
        (it.meta->pinned && !trash ? pinned : rest).append(it);
    if (!pinned.isEmpty()) {
        rows_.append(headerRow(tr("Pinned")));
        append(pinned, false);
    }
    append(rest, true);
    endResetModel();
}

void NoteListModel::showSearch(VaultController& vc, const SearchResults& res) {
    beginResetModel();
    rows_.clear();
    search_ = true;
    auto addHit = [&](const NoteHit& h) {
        ListRow r = noteRow(vc, h.noteId, true);
        r.titleHighlights = h.titleHighlights;
        if (!h.snippet.isEmpty()) {
            r.snippet = h.snippet;
            r.snippetHighlights = h.snippetHighlights;
        } else {
            r.snippetHighlights = SearchIndex::highlight(r.snippet, res.matches);
        }
        r.fuzzy = h.fuzzy;
        rows_.append(r);
    };
    if (!res.topHits.isEmpty()) {
        rows_.append(headerRow(tr("Top Hits")));
        for (const auto& h : res.topHits)
            addHit(h);
    }
    if (!res.notes.isEmpty()) {
        rows_.append(headerRow(tr("Notes")));
        for (const auto& h : res.notes)
            addHit(h);
    }
    if (!res.attachments.isEmpty()) {
        rows_.append(headerRow(tr("Attachments")));
        for (const auto& a : res.attachments) {
            ListRow r = noteRow(vc, a.noteId, true);
            r.kind = ListRow::Attachment;
            r.attachmentKey = a.key;
            r.attachmentKind = a.kind;
            r.snippet = a.snippet;
            r.snippetHighlights = a.snippetHighlights;
            r.titleHighlights = SearchIndex::highlight(r.title, res.matches);
            rows_.append(r);
        }
    }
    endResetModel();
}

void NoteListModel::refreshNote(VaultController& vc, const QString& noteId) {
    for (int i = 0; i < rows_.size(); ++i) {
        ListRow& r = rows_[i];
        if (r.kind != ListRow::Note || r.noteId != noteId)
            continue;
        const ListRow fresh = noteRow(vc, noteId, !r.folder.isEmpty());
        r.title = fresh.title;
        r.modified = fresh.modified;
        if (!search_) {
            r.snippet = fresh.snippet;
            r.snippetHighlights.clear();
        }
        r.titleHighlights.clear();
        emit dataChanged(index(i), index(i));
    }
}

}  // namespace zn
