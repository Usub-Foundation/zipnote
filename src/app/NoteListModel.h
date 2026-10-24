#pragma once

#include "SearchIndex.h"

#include <QAbstractListModel>
#include <QDateTime>

namespace zn {

class VaultController;

struct ListRow {
    enum Kind { Header, Note, Attachment };
    Kind kind = Note;
    QString header;
    QString noteId;
    QString attachmentKey;
    QString attachmentKind;
    QString title;
    QList<Highlight> titleHighlights;
    QString snippet;
    QList<Highlight> snippetHighlights;
    QDateTime modified;
    QString folder;  // shown when the list mixes folders (search, All Notes)
    bool damaged = false;
    bool fuzzy = false;
};

enum class NoteSort { DateEdited, DateCreated, Title };

// Rows of the middle column. In a folder: Pinned, then date sections (Today, Yesterday, Previous 7 Days,
// Previous 30 Days, months, years). In search: Top Hits, Notes, Attachments.
class NoteListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit NoteListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    static constexpr char kNoteMime[] = "application/x-zipnotes-note-ids";

    const ListRow& row(int r) const { return rows_[r]; }
    int rowOfNote(const QString& noteId, const QString& attachmentKey = {}) const;
    bool isSearch() const { return search_; }
    QString emptyText() const { return search_ ? tr("No Results") : tr("No Notes"); }

    void showNotes(VaultController& vc, const QStringList& noteIds, NoteSort sort, bool showFolder, bool trash);
    void showSearch(VaultController& vc, const SearchResults& results);
    void refreshNote(VaultController& vc, const QString& noteId);  // title/snippet/date after an edit

    static QString dateLabel(const QDateTime& dt);
    QString layoutSignature() const;  // headers and note order; equal signatures = no visible reordering  // "14:05", "Yesterday", "Monday", "24.10.2026"

private:
    QList<ListRow> rows_;
    bool search_ = false;
};

}  // namespace zn
