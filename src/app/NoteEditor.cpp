#include "NoteEditor.h"

#include "Icons.h"
#include "MarkdownIO.h"
#include "Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextList>
#include <QTextTable>
#include <QToolTip>
#include <cmath>

namespace zn {

namespace {

constexpr char kMarkdownMime[] = "text/markdown";

bool isListStyle(ParagraphStyle s) {
    return s == ParagraphStyle::Bulleted || s == ParagraphStyle::Dashed || s == ParagraphStyle::Numbered ||
           s == ParagraphStyle::Checklist;
}

QTextListFormat listFormatFor(ParagraphStyle s, int indent) {
    QTextListFormat lf;
    lf.setIndent(indent);
    switch (s) {
    case ParagraphStyle::Numbered:
        lf.setStyle(QTextListFormat::ListDecimal);
        break;
    case ParagraphStyle::Dashed:
        lf.setStyle(QTextListFormat::ListStyleUndefined);
        lf.setProperty(kDashedListProperty, true);
        break;
    default:
        lf.setStyle(QTextListFormat::ListDisc);
        break;
    }
    return lf;
}

bool sameListKind(const QTextListFormat& a, const QTextListFormat& b) {
    return a.style() == b.style() && a.indent() == b.indent() &&
           a.boolProperty(kDashedListProperty) == b.boolProperty(kDashedListProperty);
}

QList<QTextBlock> blocksOf(const QTextCursor& c) {
    QList<QTextBlock> out;
    QTextBlock b = c.document()->findBlock(c.selectionStart());
    const QTextBlock last = c.document()->findBlock(c.selectionEnd());
    for (; b.isValid(); b = b.next()) {
        out.append(b);
        if (b == last)
            break;
    }
    return out;
}

bool looksLikeMarkdown(const QString& text) {
    static const QRegularExpression re(QStringLiteral(R"((?m)^(?:#{1,6}\s|[-*+]\s|\d+[.)]\s|>\s?|```|~~~|\|.*\|\s*$))"));
    return text.contains(u'\n') && re.match(text).hasMatch();
}

}  // namespace

NoteEditor::NoteEditor(QWidget* parent) : QTextEdit(parent) {
    setFrameShape(QFrame::NoFrame);
    setAcceptRichText(true);
    setTabChangesFocus(false);
    setViewportMargins(28, 6, 28, 18);
    viewport()->setMouseTracking(true);
    setAutoFormatting(QTextEdit::AutoNone);
    connect(this, &QTextEdit::cursorPositionChanged, this, &NoteEditor::formatChanged);
    connect(this, &QTextEdit::currentCharFormatChanged, this, &NoteEditor::formatChanged);
}

void NoteEditor::setNoteDocument(NoteDocument* doc) {
    doc_ = doc;
    searchSelections_.clear();
    findSelections_.clear();
    setExtraSelections({});
    swallowRelease_ = false;
    if (doc) {
        setDocument(doc);
        moveCursor(QTextCursor::Start);
    } else {
        if (!empty_)
            empty_ = new QTextDocument(this);
        setDocument(empty_);
    }
    // QWidgetTextControl keeps the checklist block last hovered (blockWithMarkerUnderMouse) across
    // setDocument() and reads it on the next mouse move. Callers delete the previous document right after
    // this call, which turned that read into a use-after-free (a crash on macOS, which zeroes freed memory).
    // A hover far outside the text, while the old document still exists, makes Qt replace it.
    QMouseEvent away(QEvent::MouseMove, QPointF(-10000, -10000), QPointF(-10000, -10000), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QTextEdit::mouseMoveEvent(&away);
}

QPointF NoteEditor::docPos(const QPointF& p) const {
    return p + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

// ------------------------------------------------------------------------------------------------ styles

ParagraphStyle NoteEditor::currentParagraphStyle() const {
    return doc_ ? doc_->paragraphStyle(textCursor().block()) : ParagraphStyle::Body;
}

void NoteEditor::restyleCursorBlocks(const QTextCursor& c) {
    for (const QTextBlock& b : blocksOf(c))
        doc_->restyleBlock(b);
}

void NoteEditor::applyParagraphStyle(ParagraphStyle style) {
    if (!doc_ || isReadOnly())
        return;
    QTextCursor cur = textCursor();
    if (isListStyle(style) && currentParagraphStyle() == style)
        style = ParagraphStyle::Body;  // applying the current list style again turns it off

    cur.beginEditBlock();
    const QList<QTextBlock> blocks = blocksOf(cur);
    QTextList* targetList = nullptr;
    for (const QTextBlock& b : blocks) {
        QTextCursor bc(b);
        if (QTextList* l = b.textList(); l && !isListStyle(style))
            l->remove(b);
        QTextBlockFormat bf = b.blockFormat();
        bf.setHeadingLevel(0);
        bf.clearProperty(QTextFormat::BlockCodeFence);
        bf.clearProperty(QTextFormat::BlockCodeLanguage);
        bf.setMarker(QTextBlockFormat::MarkerType::NoMarker);
        bf.setIndent(0);
        switch (style) {
        case ParagraphStyle::Title: bf.setHeadingLevel(1); break;
        case ParagraphStyle::Heading: bf.setHeadingLevel(2); break;
        case ParagraphStyle::Subheading: bf.setHeadingLevel(3); break;
        case ParagraphStyle::Monostyled:
            bf.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("`"));
            bf.setProperty(QTextFormat::BlockCodeLanguage, QString());
            bf.setProperty(QTextFormat::BlockQuoteLevel, 0);
            break;
        case ParagraphStyle::Checklist: bf.setMarker(QTextBlockFormat::MarkerType::Unchecked); break;
        default: break;
        }
        bc.setBlockFormat(bf);

        if (isListStyle(style)) {
            QTextList* existing = b.textList();
            const int indent = existing ? existing->format().indent() : 1;
            const QTextListFormat want = listFormatFor(style, indent);
            if (existing && sameListKind(existing->format(), want)) {
                targetList = existing;
            } else if (targetList) {
                if (existing)
                    existing->remove(b);
                targetList->add(b);
            } else {
                // continue a list of the same kind right above, so numbering carries on
                const QTextBlock prev = b.previous();
                if (prev.isValid() && prev.textList() && sameListKind(prev.textList()->format(), want)) {
                    if (existing)
                        existing->remove(b);
                    prev.textList()->add(b);
                    targetList = prev.textList();
                } else {
                    targetList = bc.createList(want);
                }
            }
        }
        doc_->restyleBlock(b);
    }
    cur.endEditBlock();
    emit formatChanged();
}

void NoteEditor::mergeFormat(const QTextCharFormat& f) {
    QTextCursor c = textCursor();
    if (c.hasSelection())
        c.mergeCharFormat(f);
    mergeCurrentCharFormat(f);
    emit formatChanged();
}

void NoteEditor::editSelectionFormat(const std::function<void(QTextCharFormat&)>& edit) {
    QTextCursor c = textCursor();
    if (!c.hasSelection()) {
        QTextCharFormat f = currentCharFormat();
        edit(f);
        setCurrentCharFormat(f);
        emit formatChanged();
        return;
    }
    const int start = c.selectionStart();
    const int end = c.selectionEnd();
    c.beginEditBlock();
    for (QTextBlock b = document()->findBlock(start); b.isValid() && b.position() < end; b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            const int s = std::max(start, fr.position());
            const int e = std::min(end, fr.position() + fr.length());
            if (s >= e)
                continue;
            QTextCharFormat f = fr.charFormat();
            edit(f);
            QTextCursor fc(document());
            fc.setPosition(s);
            fc.setPosition(e, QTextCursor::KeepAnchor);
            fc.setCharFormat(f);
        }
    }
    c.endEditBlock();
    emit formatChanged();
}

bool NoteEditor::isBold() const { return currentCharFormat().fontWeight() >= QFont::DemiBold && currentParagraphStyle() == ParagraphStyle::Body; }
bool NoteEditor::isItalic() const { return currentCharFormat().fontItalic(); }
bool NoteEditor::isUnderline() const { return currentCharFormat().fontUnderline() && !currentCharFormat().isAnchor(); }
bool NoteEditor::isStrikethrough() const { return currentCharFormat().fontStrikeOut(); }
bool NoteEditor::isInlineCode() const { return currentCharFormat().fontFixedPitch() && currentParagraphStyle() != ParagraphStyle::Monostyled; }
bool NoteEditor::isHighlight() const {
    const QTextCharFormat f = currentCharFormat();
    return !f.fontFixedPitch() && f.background().style() != Qt::NoBrush && f.background().color().alpha() > 0;
}
bool NoteEditor::isBlockQuote() const { return textCursor().blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0; }

void NoteEditor::toggleBold() {
    QTextCharFormat f;
    f.setFontWeight(isBold() ? QFont::Normal : QFont::Bold);
    mergeFormat(f);
}

void NoteEditor::toggleItalic() {
    QTextCharFormat f;
    f.setFontItalic(!isItalic());
    mergeFormat(f);
}

void NoteEditor::toggleUnderline() {
    QTextCharFormat f;
    f.setFontUnderline(!isUnderline());
    mergeFormat(f);
}

void NoteEditor::toggleStrikethrough() {
    QTextCharFormat f;
    f.setFontStrikeOut(!isStrikethrough());
    mergeFormat(f);
}

void NoteEditor::toggleHighlight() {
    const bool on = !isHighlight();
    const QColor color = doc_ ? doc_->styles().highlight : Theme::palette().searchHighlight;
    editSelectionFormat([on, color](QTextCharFormat& f) {
        if (on)
            f.setBackground(color);
        else
            f.clearBackground();
    });
}

void NoteEditor::toggleInlineCode() {
    if (!doc_)
        return;
    const bool on = !isInlineCode();
    const TextStyles st = doc_->styles();
    editSelectionFormat([on, st](QTextCharFormat& f) {
        f.setFontFixedPitch(on);
        f.setFontFamilies({on ? st.monoFamily : st.bodyFamily});
        if (on)
            f.setBackground(st.codeBackground);
        else
            f.clearBackground();
    });
}

void NoteEditor::toggleBlockQuote() {
    if (!doc_ || isReadOnly())
        return;
    const int level = isBlockQuote() ? 0 : 1;
    QTextCursor c = textCursor();
    c.beginEditBlock();
    for (const QTextBlock& b : blocksOf(c)) {
        QTextCursor bc(b);
        QTextBlockFormat bf = b.blockFormat();
        if (level > 0 && isCodeBlock(b))
            continue;
        bf.setProperty(QTextFormat::BlockQuoteLevel, level);
        bc.setBlockFormat(bf);
        doc_->restyleBlock(b);
    }
    c.endEditBlock();
    viewport()->update();
    emit formatChanged();
}

void NoteEditor::changeIndent(int delta) {
    QTextCursor c = textCursor();
    c.beginEditBlock();
    for (const QTextBlock& b : blocksOf(c)) {
        QTextList* list = b.textList();
        if (!list)
            continue;
        QTextListFormat lf = list->format();
        const int indent = lf.indent() + delta;
        list->remove(b);
        QTextCursor bc(b);
        QTextBlockFormat bf = b.blockFormat();
        bf.setIndent(0);
        bc.setBlockFormat(bf);
        if (indent < 1) {
            doc_->restyleBlock(b);
            continue;
        }
        lf.setIndent(indent);
        // join the closest list above at the same depth and kind
        QTextList* join = nullptr;
        for (QTextBlock p = b.previous(); p.isValid() && p.textList(); p = p.previous()) {
            const QTextListFormat pf = p.textList()->format();
            if (pf.indent() == indent && sameListKind(pf, lf)) {
                join = p.textList();
                break;
            }
            if (pf.indent() < indent)
                break;
        }
        if (join)
            join->add(b);
        else
            bc.createList(lf);
        doc_->restyleBlock(b);
    }
    c.endEditBlock();
    viewport()->update();
}

// ------------------------------------------------------------------------------------------------ inserts

void NoteEditor::insertTable(int rows, int columns) {
    if (!doc_)
        return;
    QTextCursor c = textCursor();
    c.beginEditBlock();
    if (!c.block().text().isEmpty()) {
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    QTextTable* t = c.insertTable(rows, columns);
    doc_->restyleTable(t);
    QTextCharFormat bold;
    bold.setFontWeight(QFont::Bold);
    for (int col = 0; col < columns; ++col)
        t->cellAt(0, col).setFormat(bold);
    c.endEditBlock();
    setTextCursor(t->cellAt(0, 0).firstCursorPosition());
}

bool NoteEditor::isInTable() const {
    return textCursor().currentTable() != nullptr;
}

void NoteEditor::tableInsertRow(bool below) {
    if (QTextTable* t = textCursor().currentTable()) {
        const QTextTableCell cell = t->cellAt(textCursor());
        t->insertRows(cell.row() + (below ? 1 : 0), 1);
    }
}

void NoteEditor::tableInsertColumn(bool after) {
    if (QTextTable* t = textCursor().currentTable()) {
        const QTextTableCell cell = t->cellAt(textCursor());
        t->insertColumns(cell.column() + (after ? 1 : 0), 1);
        doc_->restyleTable(t);
    }
}

void NoteEditor::tableRemoveRow() {
    if (QTextTable* t = textCursor().currentTable()) {
        if (t->rows() <= 1)
            return;
        t->removeRows(t->cellAt(textCursor()).row(), 1);
    }
}

void NoteEditor::tableRemoveColumn() {
    if (QTextTable* t = textCursor().currentTable()) {
        if (t->columns() <= 1)
            return;
        t->removeColumns(t->cellAt(textCursor()).column(), 1);
        doc_->restyleTable(t);
    }
}

void NoteEditor::insertHorizontalRule() {
    QTextCursor c = textCursor();
    c.beginEditBlock();
    if (!c.block().text().isEmpty()) {
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    QTextBlockFormat hr;
    hr.setProperty(QTextFormat::BlockTrailingHorizontalRulerWidth, QTextLength(QTextLength::PercentageLength, 100));
    c.setBlockFormat(hr);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    c.endEditBlock();
    setTextCursor(c);
}

void NoteEditor::insertLink(const QString& url, const QString& text) {
    if (!doc_ || url.isEmpty())
        return;
    QTextCharFormat f;
    f.setAnchor(true);
    f.setAnchorHref(url);
    f.setForeground(doc_->styles().link);
    f.setFontUnderline(true);
    QTextCursor c = textCursor();
    if (c.hasSelection() && text.isEmpty()) {
        c.mergeCharFormat(f);
    } else {
        c.insertText(text.isEmpty() ? url : text, f);
        c.setCharFormat(QTextCharFormat());
        setCurrentCharFormat(QTextCharFormat());
        doc_->restyleBlock(c.block());
    }
}

namespace {

void insertObjectParagraph(QTextCursor& c, const QString& name, const QString& alt) {
    c.beginEditBlock();
    if (!c.block().text().isEmpty()) {
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    } else if (c.block().textList()) {
        c.block().textList()->remove(c.block());
    }
    QTextBlockFormat bf;
    bf.setTopMargin(6);
    bf.setBottomMargin(6);
    c.setBlockFormat(bf);
    QTextImageFormat img;
    img.setName(name);
    img.setProperty(QTextFormat::ImageAltText, alt);
    img.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
    c.insertImage(img);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    c.endEditBlock();
}

}  // namespace

QString NoteEditor::insertDiagram(DiagramKind kind, const QString& source) {
    if (!doc_)
        return {};
    const QString id = doc_->addDiagram(kind, source);
    QTextCursor c = textCursor();
    insertObjectParagraph(c, NoteDocument::diagramUrl(id), diagramKindLabel(kind));
    doc_->restyleBlock(c.block());
    setTextCursor(c);
    return id;
}

void NoteEditor::insertFile(const QString& fileId, const QString& alt) {
    if (!doc_)
        return;
    QTextCursor c = textCursor();
    insertObjectParagraph(c, NoteDocument::fileUrl(fileId), alt);
    doc_->restyleBlock(c.block());
    setTextCursor(c);
}

QString NoteEditor::diagramAtCursor() const {
    QTextCursor c = textCursor();
    for (int pos : {c.position(), c.position() - 1}) {
        if (pos < 0)
            continue;
        QTextCursor probe(document());
        probe.setPosition(pos);
        probe.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        const QTextCharFormat f = probe.charFormat();
        if (probe.selectedText() == QString(QChar(0xFFFC)) && f.isImageFormat()) {
            const QString id = NoteDocument::diagramIdFromUrl(f.toImageFormat().name());
            if (!id.isEmpty())
                return id;
        }
    }
    return {};
}

// ------------------------------------------------------------------------------------------------ search highlights

void NoteEditor::setSearchMatches(const MatchSet& matches, bool scrollToFirst) {
    searchSelections_.clear();
    if (!matches.empty()) {
        QTextCharFormat f;
        f.setBackground(Theme::palette().searchHighlight);
        for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
            for (const auto& h : SearchIndex::highlight(b.text(), matches)) {
                QTextEdit::ExtraSelection sel;
                sel.cursor = QTextCursor(document());
                sel.cursor.setPosition(b.position() + h.start);
                sel.cursor.setPosition(b.position() + h.start + h.length, QTextCursor::KeepAnchor);
                sel.format = f;
                searchSelections_.append(sel);
            }
        }
    }
    updateExtraSelections();
    if (scrollToFirst && !searchSelections_.isEmpty()) {
        QTextCursor c = searchSelections_.first().cursor;
        c.clearSelection();
        setTextCursor(c);
        ensureCursorVisible();
    }
}

void NoteEditor::clearSearchMatches() {
    if (searchSelections_.isEmpty())
        return;
    searchSelections_.clear();
    updateExtraSelections();
}

void NoteEditor::setFindSelections(const QList<QTextCursor>& matches, int current) {
    findSelections_.clear();
    for (int i = 0; i < matches.size(); ++i) {
        QTextEdit::ExtraSelection sel;
        sel.cursor = matches[i];
        sel.format.setBackground(i == current ? Theme::palette().findCurrent : Theme::palette().searchHighlight);
        findSelections_.append(sel);
    }
    updateExtraSelections();
}

void NoteEditor::updateExtraSelections() {
    setExtraSelections(searchSelections_ + findSelections_);
}

// ------------------------------------------------------------------------------------------------ keys

bool NoteEditor::handleMarkdownShortcut() {
    QTextCursor c = textCursor();
    if (c.hasSelection() || !doc_ || isCodeBlock(c.block()))
        return false;
    const QString before = c.block().text().left(c.positionInBlock());
    struct Rule {
        const char16_t* prefix;
        ParagraphStyle style;
    };
    static const Rule rules[] = {
        {u"###", ParagraphStyle::Subheading}, {u"##", ParagraphStyle::Heading}, {u"#", ParagraphStyle::Title},
        {u"- [ ]", ParagraphStyle::Checklist}, {u"[ ]", ParagraphStyle::Checklist}, {u"[]", ParagraphStyle::Checklist},
        {u"-", ParagraphStyle::Bulleted},      {u"*", ParagraphStyle::Dashed},     {u"1.", ParagraphStyle::Numbered},
    };
    for (const auto& r : rules) {
        if (before == QStringView(r.prefix)) {
            if (c.block().textList() && r.style != ParagraphStyle::Checklist)
                return false;
            c.beginEditBlock();
            c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            c.endEditBlock();
            setTextCursor(c);
            applyParagraphStyle(r.style);
            return true;
        }
    }
    if (before == u">") {
        c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        setTextCursor(c);
        if (!isBlockQuote())
            toggleBlockQuote();
        return true;
    }
    return false;
}

bool NoteEditor::handleReturn(QKeyEvent* e) {
    if (!doc_ || (e->modifiers() & Qt::ShiftModifier))
        return false;
    QTextCursor c = textCursor();
    const QTextBlock block = c.block();
    const ParagraphStyle style = doc_->paragraphStyle(block);

    if (block.text() == u"```" && style != ParagraphStyle::Monostyled) {
        c.select(QTextCursor::BlockUnderCursor);
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        setTextCursor(c);
        applyParagraphStyle(ParagraphStyle::Monostyled);
        return true;
    }
    // Return on an empty list item or quote line ends the list / quote
    if (block.text().isEmpty() && (block.textList() || isBlockQuote())) {
        if (block.textList())
            applyParagraphStyle(ParagraphStyle::Body);
        if (isBlockQuote())
            toggleBlockQuote();
        return true;
    }
    // two empty code lines in a row leave the code block
    if (style == ParagraphStyle::Monostyled && block.text().isEmpty() && isCodeBlock(block.previous()) &&
        block.previous().text().isEmpty()) {
        c.beginEditBlock();
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::EndOfBlock);
        c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.endEditBlock();
        setTextCursor(c);
        applyParagraphStyle(ParagraphStyle::Body);
        return true;
    }

    c.beginEditBlock();
    QTextBlockFormat next = block.blockFormat();
    QTextCharFormat nextChar = c.charFormat();
    if (style == ParagraphStyle::Title || style == ParagraphStyle::Heading || style == ParagraphStyle::Subheading) {
        // after a title or heading the next paragraph is body text
        if (c.atBlockEnd()) {
            next.setHeadingLevel(0);
            nextChar = QTextCharFormat();
        }
    }
    if (style == ParagraphStyle::Checklist)
        next.setMarker(QTextBlockFormat::MarkerType::Unchecked);
    nextChar.clearProperty(QTextFormat::AnchorHref);
    nextChar.setAnchor(false);
    c.insertBlock(next, nextChar);
    doc_->restyleBlock(c.block());
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
    return true;
}

bool NoteEditor::handleTableDeletion(QKeyEvent* e) {
    if (e->key() != Qt::Key_Backspace && e->key() != Qt::Key_Delete)
        return false;
    QTextCursor c = textCursor();
    if (c.hasComplexSelection()) {
        QTextTable* table = c.currentTable();
        int row = 0, rows = 0, col = 0, cols = 0;
        c.selectedTableCells(&row, &rows, &col, &cols);
        if (table && rows == table->rows() && cols == table->columns()) {
            removeTable(table);
            return true;
        }
        return false;  // part of a table: Qt clears those cells
    }
    if (e->key() == Qt::Key_Backspace && !c.hasSelection() && c.atBlockStart() && !c.currentTable()) {
        QTextCursor before(c);
        if (before.movePosition(QTextCursor::PreviousCharacter)) {
            if (QTextTable* table = before.currentTable()) {
                // first Backspace selects the table above, the second one deletes it
                QTextCursor sel(document());
                sel.setPosition(table->firstPosition());
                sel.setPosition(table->lastPosition(), QTextCursor::KeepAnchor);
                setTextCursor(sel);
                return true;
            }
        }
    }
    return false;
}

void NoteEditor::removeTable(QTextTable* table) {
    QTextCursor c(document());
    c.setPosition(table->firstPosition());
    c.beginEditBlock();
    table->removeRows(0, table->rows());  // removing every row removes the table frame
    c.endEditBlock();
}

void NoteEditor::keyPressEvent(QKeyEvent* e) {
    if (!isReadOnly() && doc_ && handleTableDeletion(e))
        return;
    if (!isReadOnly() && doc_) {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && handleReturn(e))
            return;
        if (e->key() == Qt::Key_Space && e->modifiers() == Qt::NoModifier && handleMarkdownShortcut())
            return;
        if ((e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab) && textCursor().block().textList()) {
            changeIndent(e->key() == Qt::Key_Backtab || (e->modifiers() & Qt::ShiftModifier) ? -1 : 1);
            return;
        }
        if (e->key() == Qt::Key_Backspace && !textCursor().hasSelection() && textCursor().atBlockStart()) {
            const QTextBlock b = textCursor().block();
            if (b.textList() || b.blockFormat().headingLevel() > 0 || isBlockQuote()) {
                if (b.textList() && b.textList()->format().indent() > 1)
                    changeIndent(-1);
                else if (isBlockQuote())
                    toggleBlockQuote();
                else
                    applyParagraphStyle(ParagraphStyle::Body);
                return;
            }
        }
    }
    QTextEdit::keyPressEvent(e);
}

// ------------------------------------------------------------------------------------------------ mouse

QRectF NoteEditor::checkCircleRect(const QTextBlock& b) const {
    if (!b.isValid() || !b.textList() || b.blockFormat().marker() == QTextBlockFormat::MarkerType::NoMarker ||
        !b.layout() || b.layout()->lineCount() == 0)
        return {};
    const QTextLine line = b.layout()->lineAt(0);
    const QPointF lp = b.layout()->position();
    const QFontMetricsF fm(b.charFormat().font());
    const qreal d = std::round(fm.height() * 0.82);
    const qreal textX = lp.x() + line.naturalTextRect().left();
    return QRectF(textX - d - fm.horizontalAdvance(u' ') * 0.9, lp.y() + line.y() + (line.height() - d) / 2, d, d);
}

QTextBlock NoteEditor::checklistBlockAt(const QPointF& viewportPos) const {
    const QPointF p = docPos(viewportPos);
    const QTextBlock near = cursorForPosition(viewportPos.toPoint()).block();
    for (const QTextBlock& b : {near, near.previous(), near.next()}) {
        const QRectF r = checkCircleRect(b);
        if (!r.isNull() && r.adjusted(-6, -4, 4, 4).contains(p))
            return b;
    }
    return {};
}

void NoteEditor::toggleCheck(const QTextBlock& b) {
    QTextBlockFormat bf = b.blockFormat();
    bf.setMarker(bf.marker() == QTextBlockFormat::MarkerType::Checked ? QTextBlockFormat::MarkerType::Unchecked
                                                                     : QTextBlockFormat::MarkerType::Checked);
    QTextCursor(b).setBlockFormat(bf);
    viewport()->update();
}

void NoteEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && !isReadOnly()) {
        if (const QTextBlock b = checklistBlockAt(e->position()); b.isValid()) {
            toggleCheck(b);
            // QTextEdit toggles checkboxes on release too, in the block of the text cursor: keep it out
            swallowRelease_ = true;
            e->accept();
            return;
        }
    }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)) {
        const QUrl href(anchorAt(e->position().toPoint()));
        // only web and mail links: a note must not be able to launch local files or custom URL handlers
        if (href.scheme() == u"https" || href.scheme() == u"http" || href.scheme() == u"mailto") {
            QDesktopServices::openUrl(href);
            e->accept();
            return;
        }
    }
    QTextEdit::mousePressEvent(e);
}

void NoteEditor::mouseReleaseEvent(QMouseEvent* e) {
    if (swallowRelease_) {
        swallowRelease_ = false;
        e->accept();
        return;
    }
    QTextEdit::mouseReleaseEvent(e);
}

void NoteEditor::mouseMoveEvent(QMouseEvent* e) {
    const QPointF p = docPos(e->position());
    const bool onMarker = checklistBlockAt(e->position()).isValid();
    const bool onLink = !anchorAt(e->position().toPoint()).isEmpty();
    const QString image = document()->documentLayout()->imageAt(p);
    if (onMarker || (onLink && (e->modifiers() & Qt::ControlModifier)))
        viewport()->setCursor(Qt::PointingHandCursor);
    else if (!image.isEmpty())
        viewport()->setCursor(Qt::ArrowCursor);
    else
        viewport()->setCursor(Qt::IBeamCursor);
    if (onLink && e->buttons() == Qt::NoButton)
#ifdef Q_OS_MACOS
        QToolTip::showText(e->globalPosition().toPoint(), tr("%1-click to open").arg(QStringLiteral("\u2318")), this);
#else
        QToolTip::showText(e->globalPosition().toPoint(), tr("%1-click to open").arg(QStringLiteral("Ctrl")), this);
#endif
    QTextEdit::mouseMoveEvent(e);
}

void NoteEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && !isReadOnly()) {
        if (const QTextBlock b = checklistBlockAt(e->position()); b.isValid()) {
            toggleCheck(b);
            swallowRelease_ = true;
            e->accept();
            return;
        }
    }
    const QString image = document()->documentLayout()->imageAt(docPos(e->position()));
    if (!image.isEmpty()) {
        if (const QString id = NoteDocument::diagramIdFromUrl(image); !id.isEmpty()) {
            emit diagramActivated(id);
            return;
        }
        if (const QString id = NoteDocument::fileIdFromUrl(image); !id.isEmpty()) {
            emit fileActivated(id);
            return;
        }
    }
    QTextEdit::mouseDoubleClickEvent(e);
}

void NoteEditor::contextMenuEvent(QContextMenuEvent* e) {
    std::unique_ptr<QMenu> menu(createStandardContextMenu(e->pos()));
    const QString image = document()->documentLayout()->imageAt(docPos(e->pos()));
    if (const QString id = NoteDocument::diagramIdFromUrl(image); !id.isEmpty()) {
        menu->insertSeparator(menu->actions().value(0));
        auto* edit = new QAction(icons::icon("diagram"), tr("Edit Diagram…"), menu.get());
        connect(edit, &QAction::triggered, this, [this, id] { emit diagramActivated(id); });
        menu->insertAction(menu->actions().value(0), edit);
    } else if (const QString fid = NoteDocument::fileIdFromUrl(image); !fid.isEmpty()) {
        menu->insertSeparator(menu->actions().value(0));
        auto* edit = new QAction(icons::icon("drawing"), tr("Edit Drawing…"), menu.get());
        connect(edit, &QAction::triggered, this, [this, fid] { emit fileActivated(fid); });
        menu->insertAction(menu->actions().value(0), edit);
    }
    if (isInTable() && !isReadOnly()) {
        menu->addSeparator();
        menu->addAction(tr("Add Row Above"), this, [this] { tableInsertRow(false); });
        menu->addAction(tr("Add Row Below"), this, [this] { tableInsertRow(true); });
        menu->addAction(tr("Add Column Before"), this, [this] { tableInsertColumn(false); });
        menu->addAction(tr("Add Column After"), this, [this] { tableInsertColumn(true); });
        menu->addAction(tr("Delete Row"), this, [this] { tableRemoveRow(); });
        menu->addAction(tr("Delete Column"), this, [this] { tableRemoveColumn(); });
        menu->addAction(tr("Delete Table"), this, [this] {
            if (QTextTable* t = textCursor().currentTable())
                removeTable(t);
        });
    }
    menu->exec(e->globalPos());
}

// ------------------------------------------------------------------------------------------------ painting

void NoteEditor::paintEvent(QPaintEvent* e) {
    QTextEdit::paintEvent(e);
    if (!doc_)
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF offset(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
    const QRectF visible = QRectF(viewport()->rect()).translated(-offset);
    auto* layout = document()->documentLayout();
    const ThemePalette& pal = Theme::palette();

    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const QRectF r = layout->blockBoundingRect(b);
        if (r.bottom() < visible.top())
            continue;
        if (r.top() > visible.bottom())
            break;
        const QTextBlockFormat bf = b.blockFormat();
        const int quote = bf.intProperty(QTextFormat::BlockQuoteLevel);
        for (int q = 0; q < quote; ++q) {
            const QRectF bar(r.left() + 2 + q * 18.0, r.top() + 1, 3, r.height() - 2);
            p.fillRect(bar.translated(offset), pal.quoteBar);
        }
        QTextList* list = b.textList();
        if (const QRectF circle = checkCircleRect(b); !circle.isNull()) {
            // checklist circle: outline when open, filled accent with a check mark when done
            const qreal d = circle.width();
            const QRectF c = circle.translated(offset);
            if (bf.marker() == QTextBlockFormat::MarkerType::Checked) {
                p.setPen(Qt::NoPen);
                p.setBrush(pal.accent);
                p.drawEllipse(c);
                QPen tick(Qt::white, std::max<qreal>(1.6, d / 9.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                p.setPen(tick);
                p.setBrush(Qt::NoBrush);
                QPainterPath check;
                check.moveTo(c.left() + d * 0.28, c.top() + d * 0.52);
                check.lineTo(c.left() + d * 0.44, c.top() + d * 0.68);
                check.lineTo(c.left() + d * 0.73, c.top() + d * 0.34);
                p.drawPath(check);
            } else {
                p.setPen(QPen(pal.dark ? QColor(0x63, 0x63, 0x66) : QColor(0xC7, 0xC7, 0xCC), 1.4));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c.adjusted(0.7, 0.7, -0.7, -0.7));
            }
        } else if (list && list->format().boolProperty(kDashedListProperty) && b.layout() && b.layout()->lineCount() > 0) {
            const QTextLine line = b.layout()->lineAt(0);
            const QPointF lp = b.layout()->position();
            QFont f = b.charFormat().font();
            p.setFont(f);
            p.setPen(pal.text);
            const QFontMetricsF fm(f);
            const QString dash = QStringLiteral("–");
            const qreal x = lp.x() + line.x() - fm.horizontalAdvance(dash) - fm.horizontalAdvance(u' ') * 1.2;
            p.drawText(QPointF(x, lp.y() + line.y() + line.ascent()) + offset, dash);
        }
    }
}

// ------------------------------------------------------------------------------------------------ clipboard

QMimeData* NoteEditor::createMimeDataFromSelection() const {
    QMimeData* data = QTextEdit::createMimeDataFromSelection();
    if (!doc_)
        return data;
    NoteDocument tmp;
    QTextCursor(&tmp).insertFragment(textCursor().selection());
    for (const QString& id : doc_->diagramIds())
        tmp.setDiagram(id, *doc_->diagram(id));
    data->setData(kMarkdownMime, exportMarkdown(tmp).toUtf8());
    return data;
}

bool NoteEditor::canInsertFromMimeData(const QMimeData* source) const {
    return source->hasImage() || source->hasFormat(kMarkdownMime) || QTextEdit::canInsertFromMimeData(source);
}

void NoteEditor::insertMarkdown(const QString& markdown) {
    NoteDocument tmp;
    tmp.setStyles(doc_->styles());
    importMarkdown(tmp, markdown);
    // give pasted diagrams ids of this document
    for (const QString& oldId : tmp.diagramIds()) {
        const DiagramRef ref = *tmp.diagram(oldId);
        const QString newId = doc_->addDiagram(ref.kind, ref.source);
        for (QTextBlock b = tmp.begin(); b.isValid(); b = b.next()) {
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment fr = it.fragment();
                if (!fr.charFormat().isImageFormat() || fr.charFormat().toImageFormat().name() != NoteDocument::diagramUrl(oldId))
                    continue;
                QTextImageFormat img = fr.charFormat().toImageFormat();
                img.setName(NoteDocument::diagramUrl(newId));
                QTextCursor c(&tmp);
                c.setPosition(fr.position());
                c.setPosition(fr.position() + fr.length(), QTextCursor::KeepAnchor);
                c.setCharFormat(img);
            }
        }
    }
    QTextCursor c = textCursor();
    c.beginEditBlock();
    c.insertFragment(QTextDocumentFragment(&tmp));
    c.endEditBlock();
    setTextCursor(c);
    doc_->restyleAll();
}

void NoteEditor::insertFromMimeData(const QMimeData* source) {
    if (!doc_ || isReadOnly())
        return;
    if (source->hasFormat(kMarkdownMime)) {
        insertMarkdown(QString::fromUtf8(source->data(kMarkdownMime)));
        return;
    }
    if (source->hasImage() && storePastedImage) {
        const QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull()) {
            const QString id = storePastedImage(img);
            if (!id.isEmpty())
                insertFile(id, tr("Image"));
            return;
        }
    }
    if (source->hasHtml()) {
        // Keep structure (headings, lists, emphasis, links, tables), drop fonts and colors: go through Markdown.
        NoteDocument tmp;
        tmp.setHtml(source->html());
        insertMarkdown(exportMarkdown(tmp));
        return;
    }
    if (source->hasText()) {
        const QString text = source->text();
        if (looksLikeMarkdown(text)) {
            insertMarkdown(text);
            return;
        }
        textCursor().insertText(text);
        return;
    }
    QTextEdit::insertFromMimeData(source);
}

}  // namespace zn
