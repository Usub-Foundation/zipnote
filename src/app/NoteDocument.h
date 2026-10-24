#pragma once

#include "NoteText.h"

#include <QColor>
#include <QHash>
#include <QTextDocument>
#include <QUrl>

#include <functional>

class QTextBlock;
class QTextCursor;
class QTextTable;

namespace zn {

// Property of a QTextListFormat marking a "dashed list". Qt has no dash bullet, so such lists use
// ListStyleUndefined (no marker) and the editor paints the dash.
constexpr int kDashedListProperty = 0x100000 + 1;  // QTextFormat::UserProperty + 1

struct DiagramRef {
    DiagramKind kind = DiagramKind::None;
    QString source;
};

// Paragraph styles. Stored as Markdown: Title/Heading/Subheading are #/##/###, Monostyled is a
// fenced code block, lists keep their marker character.
enum class ParagraphStyle { Body, Title, Heading, Subheading, Monostyled, Bulleted, Dashed, Numbered, Checklist };

struct TextStyles {
    QString bodyFamily;
    QString monoFamily;
    qreal bodySize = 14;
    qreal titleSize = 27;
    qreal headingSize = 20;
    qreal subheadingSize = 16.5;
    qreal monoSize = 13;
    QColor text = QColor(0x45, 0x45, 0x45);
    QColor codeBackground = QColor(0, 0, 0, 14);
    QColor highlight = QColor(0xFC, 0xB8, 0x27, 110);  // accent, translucent
    QColor link = QColor(0xC2, 0x7C, 0x0E);
    QColor tableBorder = QColor(0xE3, 0xE3, 0xE1);
};

// The editor's document. Diagrams are inline images with a zn-diagram://<id> URL whose source lives here;
// attachments (Excalidraw drawings, images) are zn-file://<fileId> URLs resolved by the resource loader.
class NoteDocument : public QTextDocument {
    Q_OBJECT
public:
    explicit NoteDocument(QObject* parent = nullptr);

    using ResourceLoader = std::function<QVariant(const QUrl& url, const NoteDocument* doc)>;
    void setResourceLoader(ResourceLoader loader) { loader_ = std::move(loader); }

    QString addDiagram(DiagramKind kind, const QString& source);
    void resetDiagrams() {
        diagrams_.clear();
        nextDiagram_ = 1;
    }
    const DiagramRef* diagram(const QString& id) const;
    void setDiagram(const QString& id, const DiagramRef& ref) { diagrams_.insert(id, ref); }
    void setDiagramSource(const QString& id, const QString& source);
    QStringList diagramIds() const { return diagrams_.keys(); }

    static QString diagramUrl(const QString& id) { return QStringLiteral("zn-diagram://") + id; }
    static QString fileUrl(const QString& fileId) { return QStringLiteral("zn-file://") + fileId; }
    static QString diagramIdFromUrl(const QString& url);
    static QString fileIdFromUrl(const QString& url);

    void setStyles(const TextStyles& styles);
    const TextStyles& styles() const { return styles_; }

    // Applies fonts, sizes and spacing of the block's paragraph style. Called after import and whenever
    // the user changes a paragraph style; it never changes what the Markdown export will produce.
    void restyleBlock(const QTextBlock& block);
    void restyleAll();
    void restyleTable(QTextTable* table);
    ParagraphStyle paragraphStyle(const QTextBlock& block) const;

    // Re-fetches an image resource (a diagram finished rendering) and relayouts the blocks that use it.
    void refreshResource(const QString& url);

protected:
    QVariant loadResource(int type, const QUrl& name) override;

private:
    QHash<QString, DiagramRef> diagrams_;
    int nextDiagram_ = 1;
    ResourceLoader loader_;
    TextStyles styles_;
};

bool isCodeBlock(const QTextBlock& block);

}  // namespace zn
