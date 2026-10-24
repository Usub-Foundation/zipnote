#include "NoteDocument.h"

#include <QFontDatabase>
#include <QImage>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

namespace zn {

NoteDocument::NoteDocument(QObject* parent) : QTextDocument(parent) {
    styles_.bodyFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    styles_.monoFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    setDocumentMargin(4);
    setStyles(styles_);
}

QString NoteDocument::addDiagram(DiagramKind kind, const QString& source) {
    const QString id = QStringLiteral("d%1").arg(nextDiagram_++);
    diagrams_.insert(id, {kind, source});
    return id;
}

const DiagramRef* NoteDocument::diagram(const QString& id) const {
    auto it = diagrams_.constFind(id);
    return it == diagrams_.cend() ? nullptr : &*it;
}

void NoteDocument::setDiagramSource(const QString& id, const QString& source) {
    auto it = diagrams_.find(id);
    if (it == diagrams_.end() || it->source == source)
        return;
    it->source = source;
    refreshResource(diagramUrl(id));
    setModified(true);
    emit contentsChanged();
}

QString NoteDocument::diagramIdFromUrl(const QString& url) {
    return url.startsWith(u"zn-diagram://") ? url.mid(13) : QString();
}

QString NoteDocument::fileIdFromUrl(const QString& url) {
    return url.startsWith(u"zn-file://") ? url.mid(10) : QString();
}

void NoteDocument::setStyles(const TextStyles& styles) {
    styles_ = styles;
    QFont f(styles_.bodyFamily);
    f.setPointSizeF(styles_.bodySize);
    setDefaultFont(f);
    restyleAll();
}

bool isCodeBlock(const QTextBlock& block) {
    const QTextBlockFormat bf = block.blockFormat();
    return bf.hasProperty(QTextFormat::BlockCodeFence) || bf.hasProperty(QTextFormat::BlockCodeLanguage);
}

ParagraphStyle NoteDocument::paragraphStyle(const QTextBlock& block) const {
    const QTextBlockFormat bf = block.blockFormat();
    if (isCodeBlock(block))
        return ParagraphStyle::Monostyled;
    switch (bf.headingLevel()) {
    case 1: return ParagraphStyle::Title;
    case 2: return ParagraphStyle::Heading;
    case 0: break;
    default: return ParagraphStyle::Subheading;
    }
    if (QTextList* list = block.textList()) {
        if (bf.marker() != QTextBlockFormat::MarkerType::NoMarker)
            return ParagraphStyle::Checklist;
        if (list->format().boolProperty(kDashedListProperty))
            return ParagraphStyle::Dashed;
        switch (list->format().style()) {
        case QTextListFormat::ListDecimal:
        case QTextListFormat::ListLowerAlpha:
        case QTextListFormat::ListUpperAlpha:
        case QTextListFormat::ListLowerRoman:
        case QTextListFormat::ListUpperRoman:
            return ParagraphStyle::Numbered;
        case QTextListFormat::ListCircle:
            return ParagraphStyle::Dashed;
        default:
            return ParagraphStyle::Bulleted;
        }
    }
    return ParagraphStyle::Body;
}

void NoteDocument::restyleBlock(const QTextBlock& block) {
    if (!block.isValid())
        return;
    QTextCursor c(block);
    QTextBlockFormat bf = block.blockFormat();
    QTextCharFormat cf;
    const int level = bf.headingLevel();
    const bool code = isCodeBlock(block);

    if (code) {
        cf.setFontFamilies({styles_.monoFamily});
        cf.setFontFixedPitch(true);
        cf.setFontPointSize(styles_.monoSize);
        cf.setFontWeight(QFont::Normal);
        bf.setBackground(styles_.codeBackground);
        bf.setTopMargin(0);
        bf.setBottomMargin(0);
        bf.setLeftMargin(10);
        bf.setRightMargin(10);
        bf.setNonBreakableLines(false);
    } else if (level > 0) {
        cf.setFontFamilies({styles_.bodyFamily});
        cf.setFontPointSize(level == 1 ? styles_.titleSize : level == 2 ? styles_.headingSize : styles_.subheadingSize);
        cf.setFontWeight(level == 3 ? QFont::DemiBold : QFont::Bold);
        bf.clearBackground();
        bf.setTopMargin(level == 1 ? 4 : 10);
        bf.setBottomMargin(level == 1 ? 6 : 4);
        bf.setLeftMargin(0);
    } else {
        cf.setFontFamilies({styles_.bodyFamily});
        cf.setFontPointSize(styles_.bodySize);
        cf.setFontWeight(QFont::Normal);
        bf.clearBackground();
        bf.setTopMargin(block.textList() ? 1 : 3);
        bf.setBottomMargin(block.textList() ? 1 : 3);
        const int quote = bf.intProperty(QTextFormat::BlockQuoteLevel);
        bf.setLeftMargin(quote > 0 ? 18.0 * quote : 0);
    }
    // a paragraph holding only a diagram or drawing gets no extra line spacing under the image
    const bool objectOnly = block.text() == QString(QChar(0xFFFC));
    if (objectOnly)
        bf.setLineHeight(100, QTextBlockFormat::SingleHeight);
    else
        bf.setLineHeight(code ? 115 : 125, QTextBlockFormat::ProportionalHeight);
    // Qt draws checklist boxes as crossed squares; the per-block list style "undefined" switches that off
    // and the editor paints round check marks instead
    if (bf.marker() != QTextBlockFormat::MarkerType::NoMarker && block.textList())
        bf.setProperty(QTextFormat::ListStyle, int(QTextListFormat::ListStyleUndefined));
    else
        bf.clearProperty(QTextFormat::ListStyle);
    c.setBlockFormat(bf);

    // Size and family come from the paragraph style; inline formatting (bold, links, code spans) is kept.
    QTextCharFormat blockChar = c.blockCharFormat();
    blockChar.merge(cf);
    c.setBlockCharFormat(blockChar);
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        if (!frag.isValid())
            continue;
        QTextCharFormat ff = frag.charFormat();
        if (ff.isImageFormat()) {
            QTextImageFormat img = ff.toImageFormat();
            if (!img.hasProperty(QTextFormat::ImageMaxWidth)) {
                img.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
                QTextCursor ic(this);
                ic.setPosition(frag.position());
                ic.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
                ic.setCharFormat(img);
            }
            continue;
        }
        QTextCharFormat apply;
        apply.setFontPointSize(cf.fontPointSize());
        if (code || !ff.fontFixedPitch())
            apply.setFontFamilies(cf.fontFamilies().toStringList());
        if (level > 0)
            apply.setFontWeight(cf.fontWeight());
        else if (!code && ff.fontWeight() > QFont::Normal && ff.fontWeight() < QFont::Bold)
            apply.setFontWeight(QFont::Bold);
        if (ff.isAnchor())
            apply.setForeground(styles_.link);
        if (!code && ff.fontFixedPitch())
            apply.setBackground(styles_.codeBackground);
        QTextCursor fc(this);
        fc.setPosition(frag.position());
        fc.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
        fc.mergeCharFormat(apply);
    }
}

void NoteDocument::restyleAll() {
    QTextCursor c(this);
    c.beginEditBlock();
    for (QTextBlock b = begin(); b.isValid(); b = b.next())
        restyleBlock(b);
    for (QTextFrame* f : rootFrame()->childFrames())
        if (auto* t = qobject_cast<QTextTable*>(f))
            restyleTable(t);
    c.endEditBlock();
}

void NoteDocument::restyleTable(QTextTable* table) {
    QTextTableFormat tf = table->format();
    tf.setBorder(1);
    tf.setBorderBrush(styles_.tableBorder);
    tf.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    tf.setBorderCollapse(true);
    tf.setCellSpacing(0);
    tf.setCellPadding(6);
    tf.setTopMargin(6);
    tf.setBottomMargin(6);
    tf.setWidth(QTextLength(QTextLength::PercentageLength, 100));
    tf.clearColumnWidthConstraints();
    table->setFormat(tf);
}

void NoteDocument::refreshResource(const QString& url) {
    const QVariant v = loader_ ? loader_(QUrl(url), this) : QVariant();
    addResource(QTextDocument::ImageResource, QUrl(url), v);
    for (QTextBlock b = begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.charFormat().isImageFormat() && f.charFormat().toImageFormat().name() == url)
                markContentsDirty(f.position(), f.length());
        }
    }
}

QVariant NoteDocument::loadResource(int type, const QUrl& name) {
    if (type == QTextDocument::ImageResource && loader_) {
        const QString s = name.toString();
        if (s.startsWith(u"zn-diagram://") || s.startsWith(u"zn-file://")) {
            QVariant v = loader_(name, this);
            if (v.isValid()) {
                addResource(type, name, v);
                return v;
            }
            QImage placeholder(8, 8, QImage::Format_ARGB32_Premultiplied);
            placeholder.fill(Qt::transparent);
            return placeholder;
        }
    }
    // Never fetch anything else: a note must not be able to make the app touch the disk or network.
    if (name.scheme() == u"data")
        return QTextDocument::loadResource(type, name);
    return {};
}

}  // namespace zn
