#pragma once

#include "SearchQuery.h"

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace zn {

// In-memory full-text index over decrypted notes. It is rebuilt after unlocking and never written to disk,
// so the vault does not need a plaintext (or separately encrypted) search index.
//
// Shape of the search (see docs/search.md):
//  * every query word is a case/diacritic-insensitive prefix (search-as-you-type);
//  * a primary pass with exact/prefix matches, then a supplemental pass (here: typo-tolerant) when the
//    primary one found little;
//  * results come back in sections: Top Hits (by relevance), Notes (by date edited), Attachments;
//  * each hit carries highlight ranges for the list, and a MatchSet to highlight matches in the editor.

enum class Field : int { Title = 0, Heading, Tags, Body, Diagram, Code, Count };

enum NoteFlag : quint32 {
    HasChecklist = 1u << 0,
    HasOpenTodo = 1u << 1,
    HasMermaid = 1u << 2,
    HasPlantUml = 1u << 3,
    HasExcalidraw = 1u << 4,
    HasTable = 1u << 5,
    HasLink = 1u << 6,
    HasCode = 1u << 7,
    HasImage = 1u << 8,
    HasTags = 1u << 9,
};

struct IndexAttachment {
    QString key;   // "<noteId>/diagram/<n>" or a file id
    QString kind;  // mermaid | plantuml | excalidraw | image
    QString text;  // diagram source, Excalidraw text labels
};

struct IndexDoc {
    QString id;
    QString folderId;
    QString folderName;
    QString title;
    QString headings;
    QString body;
    QString code;
    QStringList tags;
    QList<IndexAttachment> attachments;
    quint32 flags = 0;
    QDateTime created;
    QDateTime modified;
    bool pinned = false;
    bool trashed = false;
};

struct Highlight {
    int start = 0;
    int length = 0;
    bool operator==(const Highlight&) const = default;
};

struct NoteHit {
    QString noteId;
    double score = 0;
    bool fuzzy = false;  // found only by the typo-tolerant pass
    QList<Highlight> titleHighlights;
    QString snippet;  // empty when the match is in the title only: the list shows the usual snippet
    QList<Highlight> snippetHighlights;
};

struct AttachmentHit {
    QString noteId;
    QString key;
    QString kind;
    double score = 0;
    QString snippet;
    QList<Highlight> snippetHighlights;
};

// The words a search matched, used to highlight them anywhere (list cells, the opened note, find bar).
struct MatchSet {
    QStringList prefixes;  // folded query words; a token matches if it starts with one of them
    QSet<QString> exact;   // folded forms found by the typo-tolerant pass
    bool empty() const { return prefixes.isEmpty() && exact.isEmpty(); }
    int matchLength(QStringView foldedToken) const;  // matched prefix length, 0 = no match
};

struct SearchOptions {
    int maxTopHits = 3;
    bool notesByDate = true;  // Notes section sorted like a folder (date edited); Top Hits stay by relevance
    bool allowFuzzy = true;
    int fuzzyBelow = 3;       // run the typo-tolerant pass when the primary pass found fewer notes than this
    int maxResults = 1000;
};

struct SearchResults {
    QList<NoteHit> topHits;
    QList<NoteHit> notes;
    QList<AttachmentHit> attachments;
    MatchSet matches;
    bool usedFuzzy = false;
    bool cancelled = false;
    double elapsedMs = 0;
    int noteCount() const { return static_cast<int>(topHits.size() + notes.size()); }
};

using CancelCheck = std::function<bool()>;

class SearchIndex {
public:
    SearchIndex();
    ~SearchIndex();
    SearchIndex(const SearchIndex&) = delete;
    SearchIndex& operator=(const SearchIndex&) = delete;

    // Thread-safe: writers take an exclusive lock, search() a shared one.
    void upsert(const IndexDoc& doc);
    void remove(const QString& noteId);
    void clear();
    int size() const;

    SearchResults search(const SearchQuery& query, const SearchOptions& options = {},
                         const CancelCheck& cancelled = {}) const;

    struct TagCount {
        QString tag;
        int count = 0;
    };
    // Tags of live notes whose folded form contains `foldedNeedle`, most used first.
    QList<TagCount> tags(QStringView foldedNeedle = {}) const;

    // Ranges of `text` matched by `matches`, in `text` offsets.
    static QList<Highlight> highlight(QStringView text, const MatchSet& matches);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace zn
