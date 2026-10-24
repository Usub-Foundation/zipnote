#include "SearchQuery.h"

#include "TextNormalizer.h"

namespace zn {

namespace {

struct RawToken {
    QString text;
    bool quoted = false;
    bool negated = false;
};

bool isQuote(QChar c) {
    return c == u'"' || c == u'\u201C' || c == u'\u201D' || c == u'\u00AB' || c == u'\u00BB';
}

// Splits on whitespace and keeps "quoted parts" together, also as filter values: folder:"Work notes".
QList<RawToken> splitQuery(const QString& s) {
    QList<RawToken> out;
    RawToken cur;
    bool inQuote = false;
    bool started = false;
    auto flush = [&] {
        if (started && (!cur.text.isEmpty() || cur.quoted))
            out.append(cur);
        cur = RawToken{};
        started = false;
    };
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (isQuote(c)) {
            if (inQuote) {
                inQuote = false;
                flush();
            } else {
                if (!cur.text.isEmpty() && !cur.text.endsWith(u':'))
                    flush();
                if (!cur.text.endsWith(u':'))
                    cur.quoted = true;
                inQuote = true;
                started = true;
            }
            continue;
        }
        if (!inQuote && c.isSpace()) {
            flush();
            continue;
        }
        if (!inQuote && !started && c == u'-' && i + 1 < s.size() && !s[i + 1].isSpace()) {
            cur.negated = true;
            started = true;
            continue;
        }
        cur.text += c;
        started = true;
    }
    flush();
    return out;
}

QDate parseDate(const QString& v, QDate today) {
    const QString s = v.toLower();
    if (s == u"today")
        return today;
    if (s == u"yesterday")
        return today.addDays(-1);
    if (s == u"week")
        return today.addDays(-7);
    if (s == u"month")
        return today.addMonths(-1);
    if (s == u"year")
        return today.addYears(-1);
    if (s.size() >= 2 && s.back() == u'd') {
        bool ok = false;
        const int days = s.chopped(1).toInt(&ok);
        if (ok)
            return today.addDays(-days);
    }
    return QDate::fromString(v, Qt::ISODate);
}

}  // namespace

bool SearchQuery::hasPositiveTerms() const {
    for (const auto& t : terms)
        if (!t.negated && !t.words.isEmpty())
            return true;
    return false;
}

bool SearchQuery::hasFilters() const {
    return !tags.isEmpty() || !folderIds.isEmpty() || !folderNames.isEmpty() || !has.isEmpty() || pinnedOnly ||
           trashOnly || modifiedAfter.isValid() || modifiedBefore.isValid();
}

SearchQuery SearchQuery::parse(const QString& text, QDate today) {
    SearchQuery q;
    for (const RawToken& raw : splitQuery(text)) {
        QString t = raw.text;
        if (t.isEmpty())
            continue;

        if (!raw.quoted && !raw.negated) {
            if (t.startsWith(u'#') && t.size() > 1) {
                q.tags.append(text::fold(t.mid(1)));
                continue;
            }
            const int colon = t.indexOf(u':');
            if (colon > 0 && colon + 1 < t.size()) {
                const QString key = t.left(colon).toLower();
                const QString value = t.mid(colon + 1);
                const QString v = value.toLower();
                bool used = true;
                if (key == u"tag")
                    q.tags.append(text::fold(value.startsWith(u'#') ? value.mid(1) : value));
                else if ((key == u"in" || key == u"folder") && (v == u"trash" || v == u"deleted"))
                    q.trashOnly = true;
                else if (key == u"in" || key == u"folder")
                    q.folderNames.append(text::fold(value));
                else if (key == u"has")
                    q.has.insert(v == u"checklists" ? QStringLiteral("checklist") : v == u"diagrams" ? QStringLiteral("diagram") : v);
                else if (key == u"is" && v == u"pinned")
                    q.pinnedOnly = true;
                else if (key == u"is" && (v == u"todo" || v == u"open"))
                    q.has.insert(QStringLiteral("todo"));
                else if (key == u"is" && (v == u"deleted" || v == u"trash"))
                    q.trashOnly = true;
                else if (key == u"after" || key == u"modified" || key == u"since")
                    q.modifiedAfter = parseDate(value, today);
                else if (key == u"before")
                    q.modifiedBefore = parseDate(value, today);
                else
                    used = false;
                if (used)
                    continue;
            }
        }

        QueryTerm term;
        term.words = text::words(t);
        if (term.words.isEmpty())
            continue;
        term.negated = raw.negated;
        // "e-mail" or "2026-10-24" are one typed word but several tokens: keep them adjacent
        term.phrase = raw.quoted || term.words.size() > 1;
        q.terms.append(term);
    }
    return q;
}

}  // namespace zn
