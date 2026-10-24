#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <vector>

namespace zn::text {

// Case- and diacritic-insensitive form of a text: NFKD, combining marks dropped, case folded. "Café" -> "cafe", "ёлка" -> "елка", "ﬁle" -> "file".
// "й" is kept as is: it is a separate letter in Russian, not "и" with a mark.
//
// `toSource[i]` is the index in the source of the character that produced text[i], so a match found in the
// normalized text can be highlighted in the original. toSource has text.size() + 1 entries.
struct Normalized {
    QString text;
    std::vector<int> toSource;

    // Maps a [start, end) range of `text` back to the source.
    std::pair<int, int> sourceRange(int start, int end, int sourceLength) const;
};

Normalized normalize(QStringView source);
QString fold(QStringView source);  // normalize() without the offset map

struct Token {
    int start = 0;  // UTF-16 offsets into the normalized text
    int end = 0;
};

// Words are runs of letters and digits. Han, Hiragana and Katakana characters are one token each, since
// those scripts do not separate words with spaces.
std::vector<Token> tokenize(QStringView normalized);
QStringList words(QStringView source);  // fold + tokenize

// Substring match ignoring case and diacritics, used for folder and tag suggestions.
bool containsFolded(QStringView haystack, QStringView foldedNeedle);

}  // namespace zn::text
