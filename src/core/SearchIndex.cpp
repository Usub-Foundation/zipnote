#include "SearchIndex.h"

#include "TextNormalizer.h"

#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zn {

namespace {

constexpr int kFieldCount = static_cast<int>(Field::Count);
// Title > tags > headings > body > diagrams > code: a match in the title says more than one in the body.
constexpr std::array<double, kFieldCount> kFieldWeight = {3.0, 2.0, 2.5, 1.0, 0.8, 0.6};
constexpr double kBm25K1 = 1.2;
constexpr double kBm25B = 0.75;
constexpr float kFuzzyQuality = 0.45f;
constexpr float kFuzzyPrefixQuality = 0.35f;
constexpr int kMaxExpansions = 4000;
constexpr int kSnippetChars = 140;
constexpr int kSnippetLead = 12;  // list cells are narrow: keep the first match near the start

struct Occ {
    quint32 term;
    int start;  // offsets into the normalized field text
    int end;
};

struct FieldData {
    QString source;
    std::vector<Occ> occ;
};

struct AttachmentData {
    QString key;
    QString kind;
    FieldData field;
};

struct DocData {
    bool alive = false;
    QString id;
    QString folderId;
    QString folderNameFolded;
    QStringList foldedTags;
    QStringList tags;
    quint32 flags = 0;
    QDateTime created;
    QDateTime modified;
    bool pinned = false;
    bool trashed = false;
    std::array<FieldData, kFieldCount> fields;
    std::vector<AttachmentData> attachments;
    std::vector<quint32> terms;  // unique term ids, sorted
};

class Bits {
public:
    explicit Bits(size_t n = 0) : w_((n + 63) / 64, 0) {}
    void set(size_t i) { w_[i >> 6] |= quint64(1) << (i & 63); }
    bool test(size_t i) const { return (w_[i >> 6] >> (i & 63)) & 1; }
    void andWith(const Bits& o) {
        for (size_t i = 0; i < w_.size(); ++i)
            w_[i] &= o.w_[i];
    }
    void andNot(const Bits& o) {
        for (size_t i = 0; i < w_.size(); ++i)
            w_[i] &= ~o.w_[i];
    }
    void fill() { std::fill(w_.begin(), w_.end(), ~quint64(0)); }
    int count() const {
        int c = 0;
        for (auto w : w_)
            c += std::popcount(w);
        return c;
    }
    template <class F>
    void forEach(size_t limit, F&& f) const {
        for (size_t i = 0; i < w_.size(); ++i) {
            quint64 w = w_[i];
            while (w) {
                const size_t bit = static_cast<size_t>(std::countr_zero(w));
                const size_t idx = (i << 6) | bit;
                if (idx >= limit)
                    return;
                f(idx);
                w &= w - 1;
            }
        }
    }

private:
    std::vector<quint64> w_;
};

// Optimal string alignment distance with an early exit once every cell of a row exceeds `maxDist`.
int boundedDistance(QStringView a, QStringView b, int maxDist) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    if (std::abs(n - m) > maxDist)
        return maxDist + 1;
    std::vector<int> prev2(static_cast<size_t>(m) + 1), prev(static_cast<size_t>(m) + 1), cur(static_cast<size_t>(m) + 1);
    for (int j = 0; j <= m; ++j)
        prev[static_cast<size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int rowMin = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = std::min({prev[static_cast<size_t>(j)] + 1, cur[static_cast<size_t>(j) - 1] + 1,
                              prev[static_cast<size_t>(j) - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                v = std::min(v, prev2[static_cast<size_t>(j) - 2] + 1);
            cur[static_cast<size_t>(j)] = v;
            rowMin = std::min(rowMin, v);
        }
        if (rowMin > maxDist)
            return maxDist + 1;
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[static_cast<size_t>(m)];
}

struct Expansion {
    quint32 term;
    float quality;  // 1 exact, <1 prefix, lower still for typo matches
};

// Per query word: every dictionary term it matched and how well.
struct WordMatch {
    QString word;
    std::vector<Expansion> expansions;
    Bits docs;
    double idf = 0;
};

struct Scored {
    quint32 slot;
    double score;
    bool titleHasAll;
    bool fuzzy;
};

bool hasFilterMatches(const QString& value, quint32 flags) {
    // has:che -> checklist: the field accepts any prefix of a known value, like the suggestion list does
    struct Known {
        const char16_t* name;
        quint32 mask;
    };
    static const Known known[] = {
        {u"checklist", HasChecklist}, {u"todo", HasOpenTodo},
        {u"diagram", HasMermaid | HasPlantUml | HasExcalidraw},
        {u"mermaid", HasMermaid}, {u"plantuml", HasPlantUml},
        {u"excalidraw", HasExcalidraw}, {u"drawing", HasExcalidraw},
        {u"table", HasTable}, {u"link", HasLink}, {u"code", HasCode},
        {u"image", HasImage}, {u"tag", HasTags},
        {u"attachment", HasMermaid | HasPlantUml | HasExcalidraw | HasImage},
    };
    for (const auto& k : known)
        if (QStringView(k.name).startsWith(value))
            return (flags & k.mask) != 0;
    return false;
}

}  // namespace

int MatchSet::matchLength(QStringView token) const {
    if (exact.contains(token.toString()))
        return static_cast<int>(token.size());
    int best = 0;
    for (const auto& p : prefixes)
        if (token.startsWith(p))
            best = std::max(best, static_cast<int>(p.size()));
    return best;
}

struct SearchIndex::Impl {
    mutable std::shared_mutex mu;
    std::vector<DocData> docs;
    std::vector<quint32> freeSlots;
    QHash<QString, quint32> byId;
    std::map<QString, quint32> dict;
    std::vector<QString> termText;
    std::vector<std::vector<quint32>> postings;  // sorted slots
    std::array<double, kFieldCount> totalTokens{};
    int alive = 0;

    quint32 termId(const QString& t) {
        auto [it, inserted] = dict.try_emplace(t, static_cast<quint32>(termText.size()));
        if (inserted) {
            termText.push_back(t);
            postings.emplace_back();
        }
        return it->second;
    }

    void indexField(FieldData& f, const QString& source, std::vector<quint32>& terms) {
        f.source = source;
        f.occ.clear();
        if (source.isEmpty())
            return;
        const text::Normalized norm = text::normalize(source);
        const auto tokens = text::tokenize(norm.text);
        f.occ.reserve(tokens.size());
        for (const auto& tk : tokens) {
            const quint32 id = termId(norm.text.mid(tk.start, tk.end - tk.start));
            f.occ.push_back({id, tk.start, tk.end});
            terms.push_back(id);
        }
    }

    void unlink(quint32 slot) {
        DocData& doc = docs[slot];
        if (!doc.alive)
            return;
        for (quint32 t : doc.terms) {
            auto& p = postings[t];
            auto it = std::lower_bound(p.begin(), p.end(), slot);
            if (it != p.end() && *it == slot)
                p.erase(it);
        }
        for (int f = 0; f < kFieldCount; ++f)
            totalTokens[static_cast<size_t>(f)] -= static_cast<double>(doc.fields[static_cast<size_t>(f)].occ.size());
        byId.remove(doc.id);
        doc = DocData{};
        freeSlots.push_back(slot);
        --alive;
    }

    std::vector<Expansion> expand(const QString& word) const {
        std::vector<Expansion> out;
        for (auto it = dict.lower_bound(word); it != dict.end() && it->first.startsWith(word); ++it) {
            if (postings[it->second].empty())
                continue;
            const float q = it->first.size() == word.size()
                                ? 1.0f
                                : 0.55f + 0.35f * static_cast<float>(word.size()) / static_cast<float>(it->first.size());
            out.push_back({it->second, q});
            if (static_cast<int>(out.size()) >= kMaxExpansions)
                break;
        }
        return out;
    }

    std::vector<Expansion> fuzzyExpand(const QString& word, const std::vector<Expansion>& already,
                                       const CancelCheck& cancelled) const {
        std::vector<Expansion> out;
        const int len = static_cast<int>(word.size());
        if (len < 4)
            return out;
        const int maxDist = len >= 8 ? 2 : 1;
        std::unordered_set<quint32> skip;
        for (const auto& e : already)
            skip.insert(e.term);
        int n = 0;
        for (const auto& [t, id] : dict) {
            if ((++n & 1023) == 0 && cancelled && cancelled())
                break;
            if (postings[id].empty() || skip.contains(id))
                continue;
            if (len < 6 && t[0] != word[0])  // typos in the first letter of short words are rare, matches are noise
                continue;
            const int tl = static_cast<int>(t.size());
            if (std::abs(tl - len) <= maxDist && boundedDistance(word, t, maxDist) <= maxDist)
                out.push_back({id, kFuzzyQuality});
            else if (tl > len && boundedDistance(word, QStringView(t).left(len), maxDist) <= maxDist)
                out.push_back({id, kFuzzyPrefixQuality});
        }
        return out;
    }

    Bits docsFor(const std::vector<Expansion>& ex) const {
        Bits b(docs.size());
        for (const auto& e : ex)
            for (quint32 slot : postings[e.term])
                b.set(slot);
        return b;
    }

    bool passesFilters(const DocData& doc, const SearchQuery& q) const {
        if (doc.trashed != q.trashOnly)
            return false;
        if (q.pinnedOnly && !doc.pinned)
            return false;
        for (const auto& tag : q.tags) {
            const bool found = std::any_of(doc.foldedTags.begin(), doc.foldedTags.end(),
                                           [&](const QString& t) { return t.startsWith(tag); });
            if (!found)
                return false;
        }
        if (!q.folderIds.isEmpty() && !q.folderIds.contains(doc.folderId))
            return false;
        for (const auto& name : q.folderNames)
            if (!doc.folderNameFolded.contains(name))
                return false;
        for (const auto& h : q.has)
            if (!hasFilterMatches(h, doc.flags))
                return false;
        const QDate modified = doc.modified.toLocalTime().date();
        if (q.modifiedAfter.isValid() && modified < q.modifiedAfter)
            return false;
        if (q.modifiedBefore.isValid() && modified >= q.modifiedBefore)
            return false;
        return true;
    }
};

SearchIndex::SearchIndex() : d(std::make_unique<Impl>()) {}
SearchIndex::~SearchIndex() = default;

void SearchIndex::upsert(const IndexDoc& in) {
    std::unique_lock lock(d->mu);
    if (auto it = d->byId.constFind(in.id); it != d->byId.cend())
        d->unlink(*it);

    quint32 slot;
    if (!d->freeSlots.empty()) {
        slot = d->freeSlots.back();
        d->freeSlots.pop_back();
    } else {
        slot = static_cast<quint32>(d->docs.size());
        d->docs.emplace_back();
    }
    DocData& doc = d->docs[slot];
    doc.alive = true;
    doc.id = in.id;
    doc.folderId = in.folderId;
    doc.folderNameFolded = text::fold(in.folderName);
    doc.tags = in.tags;
    doc.foldedTags.clear();
    for (const auto& t : in.tags)
        doc.foldedTags.append(text::fold(t));
    doc.flags = in.flags | (in.tags.isEmpty() ? 0u : quint32(HasTags));
    doc.created = in.created;
    doc.modified = in.modified;
    doc.pinned = in.pinned;
    doc.trashed = in.trashed;

    QStringList diagramTexts;
    for (const auto& a : in.attachments)
        diagramTexts.append(a.text);

    std::vector<quint32> terms;
    const std::array<QString, kFieldCount> sources = {in.title, in.headings, in.tags.join(u' '),
                                                      in.body, diagramTexts.join(u'\n'), in.code};
    for (int f = 0; f < kFieldCount; ++f) {
        d->indexField(doc.fields[static_cast<size_t>(f)], sources[static_cast<size_t>(f)], terms);
        d->totalTokens[static_cast<size_t>(f)] += static_cast<double>(doc.fields[static_cast<size_t>(f)].occ.size());
    }
    doc.attachments.clear();
    for (const auto& a : in.attachments) {
        AttachmentData ad{a.key, a.kind, {}};
        std::vector<quint32> ignored;
        d->indexField(ad.field, a.text, ignored);
        doc.attachments.push_back(std::move(ad));
    }

    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    doc.terms = std::move(terms);
    for (quint32 t : doc.terms) {
        auto& p = d->postings[t];
        p.insert(std::lower_bound(p.begin(), p.end(), slot), slot);
    }
    d->byId.insert(in.id, slot);
    ++d->alive;
}

void SearchIndex::remove(const QString& noteId) {
    std::unique_lock lock(d->mu);
    if (auto it = d->byId.constFind(noteId); it != d->byId.cend())
        d->unlink(*it);
}

void SearchIndex::clear() {
    std::unique_lock lock(d->mu);
    d->docs.clear();
    d->freeSlots.clear();
    d->byId.clear();
    d->dict.clear();
    d->termText.clear();
    d->postings.clear();
    d->totalTokens = {};
    d->alive = 0;
}

int SearchIndex::size() const {
    std::shared_lock lock(d->mu);
    return d->alive;
}

QList<SearchIndex::TagCount> SearchIndex::tags(QStringView needle) const {
    std::shared_lock lock(d->mu);
    QHash<QString, TagCount> counts;  // folded -> display form of the first occurrence
    for (const auto& doc : d->docs) {
        if (!doc.alive || doc.trashed)
            continue;
        for (int i = 0; i < doc.tags.size(); ++i) {
            const QString& folded = doc.foldedTags[i];
            if (!needle.isEmpty() && !folded.contains(needle))
                continue;
            auto& c = counts[folded];
            if (c.tag.isEmpty())
                c.tag = doc.tags[i];
            ++c.count;
        }
    }
    QList<TagCount> out = counts.values();
    std::sort(out.begin(), out.end(), [](const TagCount& a, const TagCount& b) {
        return a.count != b.count ? a.count > b.count : a.tag.compare(b.tag, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QList<Highlight> SearchIndex::highlight(QStringView source, const MatchSet& matches) {
    QList<Highlight> out;
    if (matches.empty() || source.isEmpty())
        return out;
    const text::Normalized norm = text::normalize(source);
    for (const auto& tk : text::tokenize(norm.text)) {
        const int len = matches.matchLength(QStringView(norm.text).mid(tk.start, tk.end - tk.start));
        if (len <= 0)
            continue;
        const auto [s, e] = norm.sourceRange(tk.start, tk.start + len, static_cast<int>(source.size()));
        if (e > s)
            out.append({s, e - s});
    }
    return out;
}

namespace {

// Picks the window of `field` with the most distinct matched words and returns it as a one-line snippet.
bool buildSnippet(const FieldData& field, const std::unordered_map<quint32, std::vector<std::pair<int, float>>>& termWords,
                  const MatchSet& matches, QString* snippet, QList<Highlight>* highlights) {
    if (field.source.isEmpty())
        return false;
    std::vector<std::pair<int, int>> hits;  // (normalized start, word index)
    for (const auto& o : field.occ) {
        auto it = termWords.find(o.term);
        if (it == termWords.end())
            continue;
        for (const auto& [wi, q] : it->second)
            hits.emplace_back(o.start, wi);
    }
    if (hits.empty())
        return false;

    size_t bestFirst = 0;
    int bestDistinct = -1;
    for (size_t i = 0; i < hits.size(); ++i) {
        std::unordered_set<int> distinct;
        for (size_t j = i; j < hits.size() && hits[j].first - hits[i].first < kSnippetChars - kSnippetLead; ++j)
            distinct.insert(hits[j].second);
        if (static_cast<int>(distinct.size()) > bestDistinct) {
            bestDistinct = static_cast<int>(distinct.size());
            bestFirst = i;
        }
    }

    const text::Normalized norm = text::normalize(field.source);
    const int srcLen = static_cast<int>(field.source.size());
    const int matchSrc = norm.sourceRange(hits[bestFirst].first, hits[bestFirst].first + 1, srcLen).first;
    int start = std::max(0, matchSrc - kSnippetLead);
    if (start > 0) {  // begin at a word boundary
        int s = start;
        while (s < matchSrc && !field.source[s].isSpace())
            ++s;
        start = s < matchSrc ? s + 1 : start;
    }
    int end = std::min(srcLen, start + kSnippetChars);
    if (end < srcLen) {
        int e = end;
        while (e > start + kSnippetChars / 2 && !field.source[e - 1].isSpace())
            --e;
        end = e > start + kSnippetChars / 2 ? e - 1 : end;
    }
    QString body = field.source.mid(start, end - start);
    static const QRegularExpression spaces(QStringLiteral(R"(\s+)"));
    body.replace(spaces, QStringLiteral(" "));
    const QString lead = start > 0 ? QStringLiteral("…") : QString();
    *snippet = lead + body + (end < srcLen ? QStringLiteral("…") : QString());
    *highlights = SearchIndex::highlight(*snippet, matches);
    return true;
}

}  // namespace

SearchResults SearchIndex::search(const SearchQuery& query, const SearchOptions& opt, const CancelCheck& cancelled) const {
    QElapsedTimer timer;
    timer.start();
    SearchResults res;
    if (query.isEmpty() && query.terms.isEmpty())
        return res;

    std::shared_lock lock(d->mu);
    const size_t nSlots = d->docs.size();
    const double nDocs = std::max(1, d->alive);
    auto isCancelled = [&] { return cancelled && cancelled(); };

    // ---- words of positive terms, phrases kept as groups
    struct TermGroup {
        std::vector<int> words;  // indexes into `words`
        bool phrase;
    };
    std::vector<WordMatch> words;
    std::vector<TermGroup> groups;
    QHash<QString, int> wordIndex;
    for (const auto& t : query.terms) {
        if (t.negated)
            continue;
        TermGroup g{{}, t.phrase};
        for (const auto& w : t.words) {
            auto it = wordIndex.constFind(w);
            int idx;
            if (it == wordIndex.cend()) {
                idx = static_cast<int>(words.size());
                wordIndex.insert(w, idx);
                words.push_back({w, d->expand(w), Bits(nSlots), 0});
                res.matches.prefixes.append(w);
            } else {
                idx = *it;
            }
            g.words.push_back(idx);
        }
        groups.push_back(std::move(g));
    }

    // ---- documents excluded by -word
    Bits excluded(nSlots);
    for (const auto& t : query.terms) {
        if (!t.negated)
            continue;
        Bits all(nSlots);
        all.fill();
        for (const auto& w : t.words)
            all.andWith(d->docsFor(d->expand(w)));
        for (size_t i = 0; i < nSlots; ++i)
            if (all.test(i))
                excluded.set(i);
    }

    // ---- candidate set: AND over words, then filters
    auto candidatesFor = [&](std::vector<WordMatch>& ws) {
        Bits cand(nSlots);
        cand.fill();
        for (auto& w : ws) {
            w.docs = d->docsFor(w.expansions);
            const double df = w.docs.count();
            w.idf = std::log(1.0 + (nDocs - df + 0.5) / (df + 0.5));
            cand.andWith(w.docs);
        }
        cand.andNot(excluded);
        return cand;
    };

    auto termWordsOf = [](const std::vector<WordMatch>& ws) {
        std::unordered_map<quint32, std::vector<std::pair<int, float>>> tw;
        for (int wi = 0; wi < static_cast<int>(ws.size()); ++wi)
            for (const auto& e : ws[static_cast<size_t>(wi)].expansions)
                tw[e.term].emplace_back(wi, e.quality);
        return tw;
    };

    std::array<double, kFieldCount> avgLen{};
    for (int f = 0; f < kFieldCount; ++f)
        avgLen[static_cast<size_t>(f)] = std::max(1.0, d->totalTokens[static_cast<size_t>(f)] / nDocs);

    auto phraseMatches = [&](const DocData& doc, const TermGroup& g,
                             const std::unordered_map<quint32, std::vector<std::pair<int, float>>>& tw) {
        auto wordAt = [&](const Occ& o, int wi) {
            auto it = tw.find(o.term);
            if (it == tw.end())
                return false;
            return std::any_of(it->second.begin(), it->second.end(), [&](const auto& p) { return p.first == wi; });
        };
        for (const auto& field : doc.fields) {
            const auto& occ = field.occ;
            for (size_t i = 0; i + g.words.size() <= occ.size(); ++i) {
                bool ok = true;
                for (size_t j = 0; j < g.words.size() && ok; ++j)
                    ok = wordAt(occ[i + j], g.words[j]);
                if (ok)
                    return true;
            }
        }
        return false;
    };

    std::vector<Scored> scored;
    auto scoreCandidates = [&](const Bits& cand, const std::vector<WordMatch>& ws, bool fuzzyPass,
                               const std::unordered_set<quint32>& skip) {
        const auto tw = termWordsOf(ws);
        int n = 0;
        bool stop = false;
        cand.forEach(nSlots, [&](size_t slot) {
            if (stop)
                return;
            if ((++n & 127) == 0 && isCancelled()) {
                stop = true;
                return;
            }
            if (skip.contains(static_cast<quint32>(slot)))
                return;
            const DocData& doc = d->docs[slot];
            if (!doc.alive || !d->passesFilters(doc, query))
                return;
            for (const auto& g : groups)
                if (g.phrase && g.words.size() > 1 && !phraseMatches(doc, g, tw))
                    return;

            std::vector<std::array<double, kFieldCount>> tf(ws.size());
            std::vector<bool> exact(ws.size(), false);
            for (int f = 0; f < kFieldCount; ++f) {
                for (const auto& o : doc.fields[static_cast<size_t>(f)].occ) {
                    auto it = tw.find(o.term);
                    if (it == tw.end())
                        continue;
                    for (const auto& [wi, q] : it->second) {
                        tf[static_cast<size_t>(wi)][static_cast<size_t>(f)] += q;
                        if (q >= 1.0f)
                            exact[static_cast<size_t>(wi)] = true;
                    }
                }
            }
            double score = 0;
            bool titleHasAll = !ws.empty();
            bool allExact = true;
            for (size_t wi = 0; wi < ws.size(); ++wi) {
                double s = 0;
                for (int f = 0; f < kFieldCount; ++f) {
                    const double t = tf[wi][static_cast<size_t>(f)];
                    if (t <= 0)
                        continue;
                    const double len = static_cast<double>(doc.fields[static_cast<size_t>(f)].occ.size());
                    const double norm = t * (kBm25K1 + 1) /
                                        (t + kBm25K1 * (1 - kBm25B + kBm25B * len / avgLen[static_cast<size_t>(f)]));
                    s += kFieldWeight[static_cast<size_t>(f)] * norm;
                }
                score += ws[wi].idf * s;
                titleHasAll = titleHasAll && tf[wi][static_cast<size_t>(Field::Title)] > 0;
                allExact = allExact && exact[wi];
            }
            if (ws.empty())
                score = 1.0;
            if (titleHasAll)
                score *= 1.6;
            if (allExact)
                score *= 1.1;
            if (doc.pinned)
                score *= 1.05;
            if (doc.modified.isValid()) {  // a little recency: recently edited notes are more likely to be the one you want
                const double ageDays = std::max<qint64>(0, doc.modified.daysTo(QDateTime::currentDateTimeUtc()));
                score *= 1.0 + 0.15 * std::exp(-ageDays / 45.0);
            }
            if (fuzzyPass)
                score *= 0.5;
            scored.push_back({static_cast<quint32>(slot), score, titleHasAll, fuzzyPass});
        });
        return !stop;
    };

    // ---- primary pass
    Bits cand = candidatesFor(words);
    if (!scoreCandidates(cand, words, false, {})) {
        res.cancelled = true;
        return res;
    }

    // ---- supplemental pass: typo tolerance when the primary pass found little
    const size_t primaryCount = scored.size();
    if (opt.allowFuzzy && !words.empty() && static_cast<int>(primaryCount) < opt.fuzzyBelow) {
        std::vector<WordMatch> fuzzyWords = words;
        bool any = false;
        for (auto& w : fuzzyWords) {
            auto extra = d->fuzzyExpand(w.word, w.expansions, cancelled);
            if (isCancelled()) {
                res.cancelled = true;
                return res;
            }
            for (const auto& e : extra)
                res.matches.exact.insert(d->termText[e.term]);
            any = any || !extra.empty();
            w.expansions.insert(w.expansions.end(), extra.begin(), extra.end());
        }
        if (any) {
            std::unordered_set<quint32> skip;
            for (const auto& s : scored)
                skip.insert(s.slot);
            Bits fc = candidatesFor(fuzzyWords);
            if (!scoreCandidates(fc, fuzzyWords, true, skip)) {
                res.cancelled = true;
                return res;
            }
            res.usedFuzzy = scored.size() > primaryCount;
            words = std::move(fuzzyWords);
        }
    }

    // ---- sections
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.fuzzy != b.fuzzy)
            return !a.fuzzy;
        return a.score > b.score;
    });
    if (static_cast<int>(scored.size()) > opt.maxResults)
        scored.resize(static_cast<size_t>(opt.maxResults));

    const auto tw = termWordsOf(words);
    auto makeHit = [&](const Scored& s) {
        const DocData& doc = d->docs[s.slot];
        NoteHit h;
        h.noteId = doc.id;
        h.score = s.score;
        h.fuzzy = s.fuzzy;
        h.titleHighlights = highlight(doc.fields[static_cast<size_t>(Field::Title)].source, res.matches);
        if (!words.empty()) {
            for (Field f : {Field::Body, Field::Heading, Field::Tags, Field::Diagram, Field::Code})
                if (buildSnippet(doc.fields[static_cast<size_t>(f)], tw, res.matches, &h.snippet, &h.snippetHighlights))
                    break;
        }
        return h;
    };

    const bool hasText = !words.empty();
    std::vector<bool> isTop(scored.size(), false);
    if (hasText && scored.size() >= 2) {
        const double best = scored.front().score;
        int taken = 0;
        for (size_t i = 0; i < scored.size() && taken < opt.maxTopHits; ++i) {
            const auto& s = scored[i];
            if (s.fuzzy)
                break;
            if (s.titleHasAll || s.score >= 0.8 * best) {
                isTop[i] = true;
                ++taken;
            }
        }
    }

    std::vector<size_t> noteOrder;
    for (size_t i = 0; i < scored.size(); ++i) {
        if (isTop[i])
            res.topHits.append(makeHit(scored[i]));
        else
            noteOrder.push_back(i);
    }
    if (opt.notesByDate || !hasText) {
        std::stable_sort(noteOrder.begin(), noteOrder.end(), [&](size_t a, size_t b) {
            if (scored[a].fuzzy != scored[b].fuzzy)
                return !scored[a].fuzzy;
            return d->docs[scored[a].slot].modified > d->docs[scored[b].slot].modified;
        });
    }
    for (size_t i : noteOrder) {
        if ((i & 63) == 0 && isCancelled()) {
            res.cancelled = true;
            return res;
        }
        res.notes.append(makeHit(scored[i]));
    }

    // ---- attachments: diagrams and drawings whose own text matches every word
    if (hasText) {
        for (const auto& s : scored) {
            const DocData& doc = d->docs[s.slot];
            for (const auto& a : doc.attachments) {
                std::vector<double> tf(words.size(), 0);
                for (const auto& o : a.field.occ) {
                    auto it = tw.find(o.term);
                    if (it == tw.end())
                        continue;
                    for (const auto& [wi, q] : it->second)
                        tf[static_cast<size_t>(wi)] += q;
                }
                if (std::any_of(tf.begin(), tf.end(), [](double v) { return v <= 0; }))
                    continue;
                AttachmentHit ah;
                ah.noteId = doc.id;
                ah.key = a.key;
                ah.kind = a.kind;
                for (size_t wi = 0; wi < words.size(); ++wi)
                    ah.score += words[wi].idf * tf[wi] / (tf[wi] + kBm25K1);
                buildSnippet(a.field, tw, res.matches, &ah.snippet, &ah.snippetHighlights);
                res.attachments.append(ah);
            }
        }
        std::stable_sort(res.attachments.begin(), res.attachments.end(),
                         [](const AttachmentHit& a, const AttachmentHit& b) { return a.score > b.score; });
    }

    res.elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    return res;
}

}  // namespace zn
