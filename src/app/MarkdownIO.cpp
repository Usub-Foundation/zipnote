#include "MarkdownIO.h"

#include "NoteDocument.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <vector>

namespace zn {

namespace {

constexpr QChar kObjectChar(0xFFFC);
constexpr QChar kLineSeparator(0x2028);
constexpr QChar kNbsp(0x00A0);

// ------------------------------------------------------------------------------------------------ import

// Replaces ```mermaid / ```plantuml fences with an image paragraph pointing at the diagram's id.
QString extractDiagrams(NoteDocument& doc, const QString& md) {
    static const QRegularExpression open(QStringLiteral(R"(^( {0,3})(`{3,}|~{3,})\s*([^\s`]*)[^`]*$)"));
    QStringList out;
    const QStringList lines = md.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (line.endsWith(u'\r'))
            line.chop(1);
        const auto m = open.match(line);
        if (!m.hasMatch()) {
            out.append(line);
            continue;
        }
        const QString fence = m.captured(2);
        const DiagramKind kind = diagramKindFromInfo(m.capturedView(3));
        int end = i + 1;
        QStringList body;
        for (; end < lines.size(); ++end) {
            QString l = lines[end];
            if (l.endsWith(u'\r'))
                l.chop(1);
            const QString t = l.trimmed();
            if (t.size() >= fence.size() && t.count(fence.at(0)) == t.size())
                break;
            body.append(l);
        }
        if (kind == DiagramKind::None || kind == DiagramKind::Excalidraw) {
            // ordinary code: keep it verbatim, including its closing fence
            for (int j = i; j <= std::min(end, static_cast<int>(lines.size()) - 1); ++j)
                out.append(lines[j]);
            i = end;
            continue;
        }
        const QString id = doc.addDiagram(kind, body.join(u'\n'));
        out.append(QString());
        out.append(QStringLiteral("![%1](%2)").arg(diagramKindLabel(kind), NoteDocument::diagramUrl(id)));
        out.append(QString());
        i = end;
    }
    return out.join(u'\n');
}

constexpr QChar kHardBreakPlaceholder(0xE0B0);

// Qt's importer turns a hard line break into a new block (a new list item inside lists). Lines ending in
// a backslash or two spaces are joined with a placeholder that becomes a QChar::LineSeparator after import.
QString joinHardBreaks(const QString& md) {
    static const QRegularExpression fence(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,}))"));
    static const QRegularExpression blockStart(QStringLiteral(R"(^\s*(?:[-*+]\s|\d{1,9}[.)]\s|#{1,6}(?:\s|$)|>|`{3}|~{3}|\|))"));
    QStringList lines = md.split(u'\n');
    QStringList out;
    bool inFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (fence.match(line).hasMatch())
            inFence = !inFence;
        if (inFence) {
            out.append(line);
            continue;
        }
        while (i + 1 < lines.size()) {
            int trailingBackslashes = 0;
            for (int k = static_cast<int>(line.size()) - 1; k >= 0 && line[k] == u'\\'; --k)
                ++trailingBackslashes;
            const bool backslash = trailingBackslashes % 2 == 1;
            const bool spaces = line.endsWith(QStringLiteral("  ")) && !line.trimmed().isEmpty();
            const QString& next = lines[i + 1];
            if (!(backslash || spaces) || next.trimmed().isEmpty() || blockStart.match(next).hasMatch())
                break;
            if (backslash)
                line.chop(1);
            else
                while (line.endsWith(u' '))
                    line.chop(1);
            line += kHardBreakPlaceholder + next.trimmed();
            ++i;
        }
        out.append(line);
    }
    return out.join(u'\n');
}

void restoreHardBreaks(NoteDocument& doc) {
    QTextCursor c(&doc);
    while (true) {
        c = doc.find(QString(kHardBreakPlaceholder), c);
        if (c.isNull())
            break;
        c.insertText(QString(kLineSeparator));
    }
}

void applyHighlights(NoteDocument& doc) {
    static const QRegularExpression re(QStringLiteral(R"(==(?=\S)(.+?)(?<=\S)==)"));
    QTextCharFormat hl;
    hl.setBackground(doc.styles().highlight);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (isCodeBlock(b))
            continue;
        const QString text = b.text();
        if (!text.contains(u"=="))
            continue;
        std::vector<std::pair<int, int>> spans;  // inner start, inner length
        for (auto it = re.globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            spans.emplace_back(m.capturedStart(1), m.capturedLength(1));
        }
        for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
            const int start = b.position() + it->first;
            QTextCursor c(&doc);
            c.setPosition(start);
            c.setPosition(start + it->second, QTextCursor::KeepAnchor);
            if (c.charFormat().fontFixedPitch())
                continue;  // inside a code span
            c.mergeCharFormat(hl);
            QTextCursor del(&doc);
            del.setPosition(start + it->second);
            del.setPosition(start + it->second + 2, QTextCursor::KeepAnchor);
            del.removeSelectedText();
            del.setPosition(start - 2);
            del.setPosition(start, QTextCursor::KeepAnchor);
            del.removeSelectedText();
        }
    }
}

void fixupBlocks(NoteDocument& doc) {
    QTextCursor c(&doc);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat bf = b.blockFormat();
        bool changed = false;
        // Qt's importer leaks the task-list marker of the last checklist item into following paragraphs.
        if (!b.textList() && bf.marker() != QTextBlockFormat::MarkerType::NoMarker) {
            bf.setMarker(QTextBlockFormat::MarkerType::NoMarker);
            changed = true;
        }
        if (changed) {
            c.setPosition(b.position());
            c.setBlockFormat(bf);
        }
        if (QTextList* list = b.textList(); list && list->format().style() == QTextListFormat::ListCircle) {
            QTextListFormat lf = list->format();
            lf.setStyle(QTextListFormat::ListStyleUndefined);
            lf.setProperty(kDashedListProperty, true);
            list->setFormat(lf);
        }
        if (b.text() == QString(kNbsp)) {  // &nbsp; = intentionally empty paragraph
            c.setPosition(b.position());
            c.setPosition(b.position() + 1, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
    }
}

// ------------------------------------------------------------------------------------------------ export

struct InlineStyle {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool underline = false;
    bool highlight = false;
    bool code = false;
    QString href;
    bool operator==(const InlineStyle&) const = default;
};

enum class Marker { Link, Bold, Italic, Strike, Underline, Highlight };

QString openMarker(Marker m) {
    switch (m) {
    case Marker::Link: return QStringLiteral("[");
    case Marker::Bold: return QStringLiteral("**");
    case Marker::Italic: return QStringLiteral("*");
    case Marker::Strike: return QStringLiteral("~~");
    case Marker::Underline: return QStringLiteral("<u>");
    case Marker::Highlight: return QStringLiteral("==");
    }
    return {};
}

QString closeMarker(Marker m, const QString& href) {
    switch (m) {
    case Marker::Link: {
        QString h = href;
        h.replace(u' ', QStringLiteral("%20")).replace(u')', QStringLiteral("%29")).replace(u'(', QStringLiteral("%28"));
        return QStringLiteral("](") + h + u')';
    }
    case Marker::Underline: return QStringLiteral("</u>");
    default: return openMarker(m);
    }
}

std::vector<Marker> markersFor(const InlineStyle& s) {
    std::vector<Marker> m;
    if (!s.href.isEmpty())
        m.push_back(Marker::Link);
    if (s.bold)
        m.push_back(Marker::Bold);
    if (s.italic)
        m.push_back(Marker::Italic);
    if (s.strike)
        m.push_back(Marker::Strike);
    if (s.underline)
        m.push_back(Marker::Underline);
    if (s.highlight)
        m.push_back(Marker::Highlight);
    return m;
}

bool isWordChar(QChar c) {
    return c.isLetterOrNumber();
}

QString escapeText(const QString& s, bool inTable) {
    QString out;
    out.reserve(s.size() + 8);
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        const QChar prev = i > 0 ? s[i - 1] : QChar();
        const QChar next = i + 1 < s.size() ? s[i + 1] : QChar();
        switch (c.unicode()) {
        case '\\':
        case '`':
        case '*':
        case '[':
        case ']':
            out += u'\\';
            break;
        case '_':
            if (!(isWordChar(prev) && isWordChar(next)))
                out += u'\\';
            break;
        case '<':
            if (next.isLetter() || next == u'/' || next == u'!' || next == u'?')
                out += u'\\';
            break;
        case '~':
        case '=':
            if (next == c || prev == c)
                out += u'\\';
            break;
        case '&':
            if (next.isLetter() || next == u'#')
                out += u'\\';
            break;
        case '|':
            if (inTable)
                out += u'\\';
            break;
        default:
            break;
        }
        out += c;
    }
    return out;
}

// Escapes what would turn the start of a line into block syntax.
QString escapeLineStart(const QString& line) {
    static const QRegularExpression ordered(QStringLiteral(R"(^(\d{1,9})([.)])(\s|$))"));
    static const QRegularExpression setext(QStringLiteral(R"(^(?:=+|-+)\s*$)"));
    if (line.isEmpty())
        return line;
    const QChar c = line[0];
    if (c == u'#' || c == u'>' || c == u'+' || ((c == u'-' || c == u'*') && (line.size() == 1 || line[1].isSpace())))
        return u'\\' + line;
    if (setext.match(line).hasMatch())
        return u'\\' + line;
    if (auto m = ordered.match(line); m.hasMatch())
        return m.captured(1) + u'\\' + line.mid(m.capturedLength(1));
    return line;
}

QString codeSpan(const QString& text) {
    int longest = 0;
    int run = 0;
    for (QChar c : text) {
        run = c == u'`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const QString ticks(longest + 1, u'`');
    const bool pad = text.startsWith(u'`') || text.endsWith(u'`') || (text.startsWith(u' ') && text.endsWith(u' '));
    return ticks + (pad ? QStringLiteral(" ") : QString()) + text + (pad ? QStringLiteral(" ") : QString()) + ticks;
}

QString escapeAlt(QString s) {
    return s.replace(u'[', QStringLiteral("\\[")).replace(u']', QStringLiteral("\\]"));
}

enum class ChunkKind { Paragraph, Heading, ListItem, Quote, Code, Rule, Table, Diagram, Empty };

struct Chunk {
    ChunkKind kind;
    QString text;
    int quoteLevel = 0;
};

class Writer {
public:
    explicit Writer(const NoteDocument& doc) : doc_(doc) {}

    QString run() {
        writeFrame(doc_.rootFrame());
        while (!chunks_.empty() && chunks_.back().kind == ChunkKind::Empty)
            chunks_.pop_back();
        QString out;
        for (size_t i = 0; i < chunks_.size(); ++i) {
            if (i > 0)
                out += separator(chunks_[i - 1], chunks_[i]);
            out += chunks_[i].text;
        }
        if (!out.isEmpty())
            out += u'\n';
        return out;
    }

private:
    static QString separator(const Chunk& a, const Chunk& b) {
        if (a.kind == ChunkKind::ListItem && b.kind == ChunkKind::ListItem)
            return QStringLiteral("\n");
        if (a.kind == ChunkKind::Quote && b.kind == ChunkKind::Quote) {
            const int level = std::min(a.quoteLevel, b.quoteLevel);
            return u'\n' + QString(level, u'>') + u'\n';
        }
        return QStringLiteral("\n\n");
    }

    void writeFrame(QTextFrame* frame) {
        bool afterTable = false;
        for (auto it = frame->begin(); !it.atEnd(); ++it) {
            if (QTextFrame* child = it.currentFrame()) {
                if (auto* table = qobject_cast<QTextTable*>(child)) {
                    writeTable(table);
                    afterTable = true;
                    continue;
                }
                writeFrame(child);
            } else {
                const QTextBlock b = it.currentBlock();
                // QTextDocument always keeps a block after a table frame; it is not a paragraph of the note
                if (!(afterTable && b.text().isEmpty() && !b.textList()))
                    writeBlock(b);
            }
            afterTable = false;
        }
        flushCode();
    }

    void flushCode() {
        if (codeLines_.isEmpty() && !inCode_)
            return;
        const QString body = codeLines_.join(u'\n');
        int longest = 2;
        int run = 0;
        for (QChar c : body) {
            run = c == codeFence_ ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        const QString fence(longest + 1, codeFence_);
        chunks_.push_back({ChunkKind::Code, fence + codeLanguage_ + u'\n' + body + u'\n' + fence});
        codeLines_.clear();
        inCode_ = false;
    }

    void writeBlock(const QTextBlock& block) {
        const QTextBlockFormat bf = block.blockFormat();
        if (isCodeBlock(block)) {
            const QString lang = bf.stringProperty(QTextFormat::BlockCodeLanguage);
            QChar fence = u'`';
            if (bf.hasProperty(QTextFormat::BlockCodeFence)) {
                const QVariant v = bf.property(QTextFormat::BlockCodeFence);
                const QString f = v.typeId() == QMetaType::QString ? v.toString() : QString(QChar(v.toInt()));
                if (f.startsWith(u'~'))
                    fence = u'~';
            }
            if (inCode_ && (lang != codeLanguage_ || fence != codeFence_))
                flushCode();
            inCode_ = true;
            codeLanguage_ = lang;
            codeFence_ = fence;
            QString text = block.text();
            text.replace(kLineSeparator, u'\n');
            codeLines_.append(text);
            return;
        }
        flushCode();
        listWidths_.resize(block.textList() ? listWidths_.size() : 0);

        if (bf.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            chunks_.push_back({ChunkKind::Rule, QStringLiteral("---")});
            return;
        }

        const int quote = bf.intProperty(QTextFormat::BlockQuoteLevel);
        const QString quotePrefix = quote > 0 ? QString(quote, u'>') + u' ' : QString();
        const int heading = bf.headingLevel();
        QTextList* list = block.textList();

        QString prefix;
        QString continuation = quotePrefix;
        if (list) {
            const QTextListFormat lf = list->format();
            const int level = std::max(1, lf.indent());
            listWidths_.resize(static_cast<size_t>(level - 1), 2);
            int indent = 0;
            for (int w : listWidths_)
                indent += w;
            QString marker;
            switch (lf.style()) {
            case QTextListFormat::ListDecimal:
            case QTextListFormat::ListLowerAlpha:
            case QTextListFormat::ListUpperAlpha:
            case QTextListFormat::ListLowerRoman:
            case QTextListFormat::ListUpperRoman:
                marker = QString::number(list->itemNumber(block) + std::max(1, lf.start())) + u'.';
                break;
            case QTextListFormat::ListSquare:
                marker = QStringLiteral("+");
                break;
            default:
                marker = lf.boolProperty(kDashedListProperty) || lf.style() == QTextListFormat::ListCircle
                             ? QStringLiteral("*")
                             : QStringLiteral("-");
                break;
            }
            listWidths_.push_back(static_cast<int>(marker.size()) + 1);
            prefix = quotePrefix + QString(indent, u' ') + marker + u' ';
            continuation = quotePrefix + QString(indent + static_cast<int>(marker.size()) + 1, u' ');
            if (bf.marker() == QTextBlockFormat::MarkerType::Checked)
                prefix += QStringLiteral("[x] ");
            else if (bf.marker() == QTextBlockFormat::MarkerType::Unchecked)
                prefix += QStringLiteral("[ ] ");
        } else if (heading > 0) {
            prefix = quotePrefix + QString(std::min(heading, 6), u'#') + u' ';
        } else {
            prefix = quotePrefix;
        }

        // A diagram inside a paragraph splits it: fences are blocks in Markdown.
        std::vector<std::pair<bool, QString>> parts;  // (isDiagram, text)
        writeInline(block, heading > 0, continuation, false, &parts);

        bool wroteText = false;
        for (const auto& [isDiagram, text] : parts) {
            if (isDiagram) {
                chunks_.push_back({ChunkKind::Diagram, text});
                continue;
            }
            QString t = text;
            if (heading > 0) {
                if (t.endsWith(u'#'))
                    t.insert(t.size() - 1, u'\\');
            } else if (!list) {
                t = escapeLineStart(t);
            }
            if (t.trimmed().isEmpty() && (wroteText || parts.size() > 1))
                continue;
            wroteText = true;
            const ChunkKind kind = list ? ChunkKind::ListItem
                                   : heading > 0 ? ChunkKind::Heading
                                   : quote > 0 ? ChunkKind::Quote
                                               : ChunkKind::Paragraph;
            if (t.trimmed().isEmpty() && !list && heading == 0) {
                chunks_.push_back({ChunkKind::Empty, quotePrefix + QStringLiteral("&nbsp;"), quote});
                continue;
            }
            chunks_.push_back({kind, prefix + t, quote});
        }
        if (parts.empty())
            chunks_.push_back(list ? Chunk{ChunkKind::ListItem, prefix.trimmed(), quote}
                                   : Chunk{ChunkKind::Empty, quotePrefix + QStringLiteral("&nbsp;"), quote});
    }

    InlineStyle styleOf(const QTextCharFormat& f, bool inHeading) const {
        InlineStyle s;
        s.code = f.fontFixedPitch();
        if (f.isAnchor() && !f.anchorHref().isEmpty())
            s.href = f.anchorHref();
        if (!s.code) {
            s.bold = !inHeading && f.fontWeight() >= QFont::DemiBold;
            s.italic = f.fontItalic();
            s.strike = f.fontStrikeOut();
            s.underline = s.href.isEmpty() && f.fontUnderline();
            s.highlight = f.background().style() != Qt::NoBrush && f.background().color().alpha() > 0;
        }
        return s;
    }

    // Serializes a block's fragments with markers that hug non-whitespace text, as CommonMark requires.
    void writeInline(const QTextBlock& block, bool inHeading, const QString& continuation, bool inTable,
                     std::vector<std::pair<bool, QString>>* parts) {
        QString out;
        std::vector<Marker> open;
        QString openHref;
        QString pendingSpace;

        auto closeTo = [&](size_t k) {
            while (open.size() > k) {
                out += closeMarker(open.back(), openHref);
                open.pop_back();
            }
        };
        auto finishText = [&] {
            closeTo(0);
            parts->push_back({false, out});
            out.clear();
            pendingSpace.clear();
        };
        auto emitCore = [&](const InlineStyle& s, const QString& core) {
            const auto want = markersFor(s);
            size_t k = 0;
            while (k < open.size() && k < want.size() && open[k] == want[k] &&
                   (open[k] != Marker::Link || openHref == s.href))
                ++k;
            closeTo(k);
            out += pendingSpace;
            pendingSpace.clear();
            for (size_t i = k; i < want.size(); ++i) {
                out += openMarker(want[i]);
                open.push_back(want[i]);
            }
            if (!s.href.isEmpty())
                openHref = s.href;
            QString text = s.code ? codeSpan(core) : escapeText(core, inTable);
            text.replace(kLineSeparator, inTable ? QStringLiteral("<br>") : QStringLiteral("\\\n") + continuation);
            out += text;
        };

        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            const QTextCharFormat f = frag.charFormat();
            if (f.isImageFormat()) {
                const QTextImageFormat img = f.toImageFormat();
                const QString name = img.name();
                const QString diagramId = NoteDocument::diagramIdFromUrl(name);
                for (int i = 0; i < frag.text().count(kObjectChar); ++i) {
                    if (const DiagramRef* d = diagramId.isEmpty() ? nullptr : doc_.diagram(diagramId)) {
                        if (!inTable) {
                            finishText();
                            parts->push_back({true, QStringLiteral("```%1\n%2\n```").arg(diagramKindName(d->kind), d->source)});
                            continue;
                        }
                    }
                    closeTo(0);
                    out += pendingSpace;
                    pendingSpace.clear();
                    const QString alt = img.stringProperty(QTextFormat::ImageAltText);
                    out += QStringLiteral("![%1](%2)").arg(escapeAlt(alt), QString(name).replace(u' ', QStringLiteral("%20")));
                }
                continue;
            }
            const InlineStyle s = styleOf(f, inHeading);
            const QString text = frag.text();
            // whitespace never sits right inside a marker
            int a = 0;
            int b = static_cast<int>(text.size());
            if (!s.code) {
                while (a < b && text[a].isSpace() && text[a] != kLineSeparator)
                    ++a;
                while (b > a && text[b - 1].isSpace() && text[b - 1] != kLineSeparator)
                    --b;
            }
            const bool styled = !markersFor(s).empty() || s.code;
            if (a == b) {  // whitespace-only fragment: keep markers open only if the style continues
                if (!styled)
                    closeTo(0);
                pendingSpace += text;
                continue;
            }
            pendingSpace += text.left(a);
            emitCore(s, text.mid(a, b - a));
            pendingSpace += text.mid(b);
        }
        closeTo(0);
        out += pendingSpace;
        while (out.endsWith(u' '))
            out.chop(1);
        if (!out.isEmpty() || parts->empty())
            parts->push_back({false, out});
    }

    void writeTable(QTextTable* table) {
        flushCode();
        listWidths_.clear();
        const int rows = table->rows();
        const int cols = table->columns();
        if (rows == 0 || cols == 0)
            return;
        QStringList lines;
        auto cellText = [&](int r, int c) {
            const bool header = r == 0;  // Qt makes header cells bold; GFM headers are bold anyway
            const QTextTableCell cell = table->cellAt(r, c);
            if (!cell.isValid() || cell.row() != r || cell.column() != c)
                return QString();
            QStringList pieces;
            for (auto it = cell.begin(); !it.atEnd(); ++it) {
                const QTextBlock b = it.currentBlock();
                if (!b.isValid())
                    continue;
                std::vector<std::pair<bool, QString>> parts;
                writeInline(b, header, {}, true, &parts);
                for (const auto& p : parts)
                    if (!p.second.isEmpty())
                        pieces.append(p.second);
            }
            return pieces.join(QStringLiteral("<br>"));
        };
        for (int r = 0; r < rows; ++r) {
            QStringList cells;
            for (int c = 0; c < cols; ++c)
                cells.append(cellText(r, c));
            lines.append(QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |"));
            if (r == 0) {
                QStringList sep;
                for (int c = 0; c < cols; ++c) {
                    // Qt puts the column alignment on body cells only; explicit "left" equals the default
                    const QTextTableCell cell = table->cellAt(rows > 1 ? 1 : 0, c);
                    const Qt::Alignment al = cell.isValid() ? cell.firstCursorPosition().blockFormat().alignment() : Qt::AlignLeft;
                    if (al & Qt::AlignHCenter)
                        sep.append(QStringLiteral(":---:"));
                    else if (al & Qt::AlignRight)
                        sep.append(QStringLiteral("---:"));
                    else
                        sep.append(QStringLiteral("---"));
                }
                lines.append(QStringLiteral("| ") + sep.join(QStringLiteral(" | ")) + QStringLiteral(" |"));
            }
        }
        chunks_.push_back({ChunkKind::Table, lines.join(u'\n')});
    }

    const NoteDocument& doc_;
    std::vector<Chunk> chunks_;
    std::vector<int> listWidths_;
    QStringList codeLines_;
    QString codeLanguage_;
    QChar codeFence_ = u'`';
    bool inCode_ = false;
};

}  // namespace

void importMarkdown(NoteDocument& doc, const QString& markdown) {
    const bool undo = doc.isUndoRedoEnabled();
    doc.setUndoRedoEnabled(false);
    doc.resetDiagrams();
    const QString md = joinHardBreaks(extractDiagrams(doc, markdown));
    doc.setMarkdown(md, QTextDocument::MarkdownDialectGitHub);
    restoreHardBreaks(doc);
    fixupBlocks(doc);
    applyHighlights(doc);
    doc.restyleAll();
    doc.setUndoRedoEnabled(undo);
    doc.setModified(false);
}

QString exportMarkdown(const NoteDocument& doc) {
    return Writer(doc).run();
}

}  // namespace zn
