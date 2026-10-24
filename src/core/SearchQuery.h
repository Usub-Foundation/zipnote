#pragma once

#include <QDate>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace zn {

// A free-text part of the query. Every word is matched as a prefix (search-as-you-type).
struct QueryTerm {
    QStringList words;  // folded
    bool phrase = false;   // "quoted words" must be adjacent
    bool negated = false;  // -word
};

// Restrictions on notes. In the UI they are tokens ("chips") in the search field; in text they use the
// key:value syntax below.
struct SearchQuery {
    QList<QueryTerm> terms;
    QStringList tags;          // #tag, tag:x (folded, without '#')
    QStringList folderIds;     // set by the UI from a folder chip (includes subfolders)
    QStringList folderNames;   // folder:x / in:x typed by hand, folded
    QSet<QString> has;         // has:checklist|todo|diagram|mermaid|plantuml|excalidraw|table|link|code|image|tag
    bool pinnedOnly = false;   // is:pinned
    bool trashOnly = false;    // in:trash / is:deleted
    QDate modifiedAfter;       // after:2026-01-31, modified:7d, modified:today
    QDate modifiedBefore;      // before:2026-01-31

    bool hasPositiveTerms() const;
    bool hasFilters() const;
    bool isEmpty() const { return !hasPositiveTerms() && !hasFilters(); }

    // `today` is a parameter so tests do not depend on the clock.
    static SearchQuery parse(const QString& text, QDate today = QDate::currentDate());
};

}  // namespace zn
