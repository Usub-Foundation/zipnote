#include "TextNormalizer.h"

#include <QHash>
#include <QStringList>

#include <algorithm>

namespace zn::text {

namespace {

bool isMark(char32_t cp) {
    switch (QChar::category(cp)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
        return true;
    default:
        return false;
    }
}

const QString& foldCodePoint(char32_t cp) {
    thread_local QHash<char32_t, QString> cache;
    auto it = cache.constFind(cp);
    if (it != cache.cend())
        return *it;

    QString out;
    if (cp == 0x0419 || cp == 0x0439) {  // Й й
        out = QString(QChar(0x0439));
    } else if (cp == 0x00DF || cp == 0x1E9E) {  // ß ẞ
        out = QStringLiteral("ss");
    } else {
        const QString decomposed = QString::fromUcs4(&cp, 1).normalized(QString::NormalizationForm_KD);
        for (char32_t c : decomposed.toUcs4()) {
            if (!isMark(c))
                out += QString::fromUcs4(&c, 1).toCaseFolded();
        }
    }
    return *cache.insert(cp, out);
}

bool isSingleCharScript(char32_t cp) {
    switch (QChar::script(cp)) {
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
        return true;
    default:
        return false;
    }
}

char32_t codePointAt(QStringView s, int i, int* len) {
    const QChar c = s[i];
    if (c.isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate()) {
        *len = 2;
        return QChar::surrogateToUcs4(c, s[i + 1]);
    }
    *len = 1;
    return c.unicode();
}

}  // namespace

std::pair<int, int> Normalized::sourceRange(int start, int end, int sourceLength) const {
    if (start >= end || toSource.empty())
        return {0, 0};
    const int n = static_cast<int>(text.size());
    start = std::clamp(start, 0, n);
    end = std::clamp(end, 0, n);
    const int s = toSource[static_cast<size_t>(start)];
    int e = toSource[static_cast<size_t>(end)];
    // A single source character can expand into several normalized ones ("ﬁ" -> "fi"); never cut it in half.
    if (e <= toSource[static_cast<size_t>(end - 1)]) {
        int i = end;
        while (i < n && toSource[static_cast<size_t>(i)] <= toSource[static_cast<size_t>(end - 1)])
            ++i;
        e = i < n ? toSource[static_cast<size_t>(i)] : sourceLength;
    }
    return {s, std::min(e, sourceLength)};
}

Normalized normalize(QStringView source) {
    Normalized out;
    out.text.reserve(source.size());
    out.toSource.reserve(static_cast<size_t>(source.size()) + 1);
    for (int i = 0; i < source.size();) {
        int len = 1;
        const char32_t cp = codePointAt(source, i, &len);
        if (cp < 0x80) {
            out.text += QChar(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
            out.toSource.push_back(i);
        } else {
            const QString& f = foldCodePoint(cp);
            out.text += f;
            out.toSource.insert(out.toSource.end(), static_cast<size_t>(f.size()), i);
        }
        i += len;
    }
    out.toSource.push_back(static_cast<int>(source.size()));
    return out;
}

QString fold(QStringView source) {
    QString out;
    out.reserve(source.size());
    for (int i = 0; i < source.size();) {
        int len = 1;
        const char32_t cp = codePointAt(source, i, &len);
        if (cp < 0x80)
            out += QChar(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
        else
            out += foldCodePoint(cp);
        i += len;
    }
    return out;
}

std::vector<Token> tokenize(QStringView s) {
    std::vector<Token> tokens;
    int start = -1;
    for (int i = 0; i < s.size();) {
        int len = 1;
        const char32_t cp = codePointAt(s, i, &len);
        const bool word = QChar::isLetterOrNumber(cp);
        if (word && isSingleCharScript(cp)) {
            if (start >= 0)
                tokens.push_back({start, i});
            tokens.push_back({i, i + len});
            start = -1;
        } else if (word) {
            if (start < 0)
                start = i;
        } else if (start >= 0) {
            tokens.push_back({start, i});
            start = -1;
        }
        i += len;
    }
    if (start >= 0)
        tokens.push_back({start, static_cast<int>(s.size())});
    return tokens;
}

QStringList words(QStringView source) {
    const QString f = fold(source);
    QStringList out;
    for (const auto& t : tokenize(f))
        out.append(f.mid(t.start, t.end - t.start));
    return out;
}

bool containsFolded(QStringView haystack, QStringView foldedNeedle) {
    if (foldedNeedle.isEmpty())
        return true;
    return fold(haystack).contains(foldedNeedle);
}

}  // namespace zn::text
