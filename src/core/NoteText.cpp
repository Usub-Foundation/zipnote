#include "NoteText.h"

#include <QRegularExpression>
#include <QSet>

#include <cctype>

namespace zn {

namespace {

const QRegularExpression& fenceRe() {
    static const QRegularExpression re(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,})\s*([^\s`]*)[^`]*$)"));
    return re;
}

const QRegularExpression& headingRe() {
    static const QRegularExpression re(QStringLiteral(R"(^ {0,3}(#{1,6})(?:\s+(.*?))?(?:\s+#+)?\s*$)"));
    return re;
}

const QRegularExpression& listRe() {
    // bullet or ordered marker, optional task box
    static const QRegularExpression re(QStringLiteral(R"(^\s*(?:[-*+]|\d{1,9}[.)])\s+(?:\[([ xX])\]\s+)?)"));
    return re;
}

const QRegularExpression& tagRe() {
    // '#' not preceded by a word character, '/', '&' or another '#' (URLs, entities, headings),
    // followed by a run that contains at least one letter.
    static const QRegularExpression re(
        QStringLiteral(R"((?<![\p{L}\p{N}_/&#])#([\p{L}\p{N}_][\p{L}\p{N}_\-/]*))"));
    return re;
}

const QRegularExpression& fileRefRe() {
    static const QRegularExpression re(QStringLiteral(R"(zn-file://([0-9A-Za-z\-]+))"));
    return re;
}

bool isHorizontalRule(QStringView line) {
    static const QRegularExpression re(QStringLiteral(R"(^ {0,3}([-*_])(?:\s*\1){2,}\s*$)"));
    return re.matchView(line).hasMatch();
}

bool isTableSeparator(QStringView line) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*\|?\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)*\|?\s*$)"));
    return line.contains(u'-') && re.matchView(line).hasMatch();
}

QString collapseSpaces(QString s) {
    static const QRegularExpression re(QStringLiteral(R"(\s+)"));
    return s.replace(re, QStringLiteral(" ")).trimmed();
}

}  // namespace

DiagramKind diagramKindFromInfo(QStringView info) {
    const QString i = info.toString().toLower();
    if (i == u"mermaid")
        return DiagramKind::Mermaid;
    if (i == u"plantuml" || i == u"puml" || i == u"uml")
        return DiagramKind::PlantUml;
    if (i == u"excalidraw")
        return DiagramKind::Excalidraw;
    return DiagramKind::None;
}

QString diagramKindName(DiagramKind kind) {
    switch (kind) {
    case DiagramKind::Mermaid: return QStringLiteral("mermaid");
    case DiagramKind::PlantUml: return QStringLiteral("plantuml");
    case DiagramKind::Excalidraw: return QStringLiteral("excalidraw");
    case DiagramKind::None: break;
    }
    return {};
}

QString diagramKindLabel(DiagramKind kind) {
    switch (kind) {
    case DiagramKind::Mermaid: return QStringLiteral("Mermaid");
    case DiagramKind::PlantUml: return QStringLiteral("PlantUML");
    case DiagramKind::Excalidraw: return QStringLiteral("Excalidraw");
    case DiagramKind::None: break;
    }
    return {};
}

QString stripInline(QStringView line) {
    QString s = line.toString();
    static const QRegularExpression image(QStringLiteral(R"(!\[([^\]]*)\]\([^)]*\))"));
    static const QRegularExpression link(QStringLiteral(R"(\[([^\]]*)\]\([^)]*\))"));
    static const QRegularExpression autolink(QStringLiteral(R"(<((?:https?|mailto):[^>\s]+)>)"));
    static const QRegularExpression html(QStringLiteral(R"(</?(?:u|mark|span|b|i|s|em|strong|sub|sup)(?:\s[^>]*)?>)"));
    static const QRegularExpression emphasis(QStringLiteral(R"(\*\*|__|~~|==|(?<![\p{L}\p{N}\\])[*_]|[*_](?![\p{L}\p{N}]))"));
    static const QRegularExpression ticks(QStringLiteral("`+"));
    // Backslash escapes are parked in the private use area so the emphasis rules below leave them alone.
    for (int i = 0; i + 1 < s.size(); ++i) {
        const char16_t c = s[i + 1].unicode();
        if (s[i] == u'\\' && c < 0x80 && std::ispunct(static_cast<unsigned char>(c))) {
            s[i + 1] = QChar(0xE000 + c);
            s.remove(i, 1);
        }
    }
    s.replace(image, QStringLiteral("\\1"));
    s.replace(link, QStringLiteral("\\1"));
    s.replace(autolink, QStringLiteral("\\1"));
    s.replace(html, QString());
    s.replace(ticks, QString());
    s.replace(emphasis, QString());
    for (QChar& c : s)
        if (c.unicode() >= 0xE000 && c.unicode() < 0xE080)
            c = QChar(c.unicode() - 0xE000);
    return s;
}

NoteDigest digestMarkdown(const QString& markdown) {
    NoteDigest d;
    QStringList bodyLines;
    QStringList headings;
    QStringList code;
    QSet<QString> seenTags;
    bool titleTaken = false;

    QString fenceMarker;
    DiagramKind fenceKind = DiagramKind::None;
    QStringList fenceLines;

    const QStringList lines = markdown.split(u'\n');
    for (QString line : lines) {
        if (line.endsWith(u'\r'))
            line.chop(1);

        if (!fenceMarker.isEmpty()) {
            const QString t = line.trimmed();
            if (t.startsWith(fenceMarker) && t.count(fenceMarker.at(0)) == t.size() && t.size() >= fenceMarker.size()) {
                if (fenceKind != DiagramKind::None)
                    d.diagrams.append({fenceKind, fenceLines.join(u'\n'), static_cast<int>(d.diagrams.size())});
                else
                    code.append(fenceLines.join(u'\n'));
                fenceMarker.clear();
                fenceLines.clear();
            } else {
                fenceLines.append(line);
            }
            continue;
        }

        if (auto m = fenceRe().matchView(line); m.hasMatch()) {
            fenceMarker = m.captured(1);
            fenceKind = diagramKindFromInfo(m.capturedView(2));
            if (fenceKind == DiagramKind::None)
                d.hasCode = true;
            continue;
        }

        for (auto it = fileRefRe().globalMatchView(line); it.hasNext();)
            d.fileRefs.append(it.next().captured(1));

        if (line.trimmed().isEmpty() || line.trimmed() == u"&nbsp;" || isHorizontalRule(line))
            continue;

        QString text;
        bool isHeading = false;
        if (auto m = headingRe().matchView(line); m.hasMatch()) {
            text = m.captured(2);
            isHeading = true;
        } else if (line.trimmed().startsWith(u'|')) {
            d.hasTable = true;
            if (isTableSeparator(line))
                continue;
            text = line.trimmed();
            text.replace(u'|', u' ');
        } else {
            text = line;
            static const QRegularExpression quote(QStringLiteral(R"(^\s*(?:>\s?)+)"));
            text.remove(quote);
            if (auto lm = listRe().matchView(text); lm.hasMatch()) {
                if (lm.capturedLength(1) > 0) {
                    if (lm.captured(1) == u" ")
                        ++d.checklistOpen;
                    else
                        ++d.checklistDone;
                }
                text = text.mid(lm.capturedEnd(0));
            }
        }

        static const QRegularExpression linkRe(QStringLiteral(R"(\]\((?!zn-file:)[^)]+\)|https?://|mailto:)"));
        if (!d.hasLink && linkRe.matchView(text).hasMatch())
            d.hasLink = true;
        // tags are looked up before stripping, inline code spans excluded
        {
            static const QRegularExpression codeSpan(QStringLiteral("`[^`]*`"));
            QString forTags = text;
            forTags.remove(codeSpan);
            for (auto it = tagRe().globalMatchView(forTags); it.hasNext();) {
                const QString tag = it.next().captured(1);
                static const QRegularExpression letter(QStringLiteral(R"(\p{L})"));
                if (!tag.contains(letter))
                    continue;
                const QString key = tag.toCaseFolded();
                if (!seenTags.contains(key)) {
                    seenTags.insert(key);
                    d.tags.append(tag);
                }
            }
        }

        text = collapseSpaces(stripInline(text));
        if (text.isEmpty())
            continue;
        if (!titleTaken) {
            d.title = text;
            titleTaken = true;
            continue;
        }
        if (isHeading)
            headings.append(text);
        bodyLines.append(text);
    }
    if (!fenceMarker.isEmpty()) {  // unterminated fence runs to the end of the note
        if (fenceKind != DiagramKind::None)
            d.diagrams.append({fenceKind, fenceLines.join(u'\n'), static_cast<int>(d.diagrams.size())});
        else
            code.append(fenceLines.join(u'\n'));
    }

    d.body = bodyLines.join(u'\n');
    d.headings = headings.join(u'\n');
    d.code = code.join(u'\n');
    QString snippet;
    for (const auto& l : bodyLines) {
        if (!snippet.isEmpty())
            snippet += u' ';
        snippet += l;
        if (snippet.size() > 160)
            break;
    }
    d.snippet = snippet.left(200);
    return d;
}

}  // namespace zn
