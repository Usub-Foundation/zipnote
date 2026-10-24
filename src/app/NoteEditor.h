#pragma once

#include "NoteDocument.h"
#include "SearchIndex.h"

#include <QTextBlock>
#include <QTextEdit>

class QTextTable;

#include <functional>

namespace zn {

// WYSIWYG editor with paragraph styles (Title, Heading, Subheading, Body, Monostyled, bulleted / dashed /
// numbered lists, checklists, quotes) and inline diagrams.
class NoteEditor : public QTextEdit {
    Q_OBJECT
public:
    explicit NoteEditor(QWidget* parent = nullptr);

    void setNoteDocument(NoteDocument* doc);
    NoteDocument* noteDocument() const { return doc_; }

    ParagraphStyle currentParagraphStyle() const;
    void applyParagraphStyle(ParagraphStyle style);  // applying the current list style again turns it off
    void toggleBold();
    void toggleItalic();
    void toggleUnderline();
    void toggleStrikethrough();
    void toggleHighlight();
    void toggleInlineCode();
    void toggleBlockQuote();
    void changeIndent(int delta);
    bool isBold() const;
    bool isItalic() const;
    bool isUnderline() const;
    bool isStrikethrough() const;
    bool isHighlight() const;
    bool isInlineCode() const;
    bool isBlockQuote() const;

    void insertTable(int rows, int columns);
    void tableInsertRow(bool below);
    void tableInsertColumn(bool after);
    void tableRemoveRow();
    void tableRemoveColumn();
    bool isInTable() const;
    void insertHorizontalRule();
    void insertLink(const QString& url, const QString& text);
    QString insertDiagram(DiagramKind kind, const QString& source);
    void insertFile(const QString& fileId, const QString& alt);
    QString diagramAtCursor() const;

    // Highlights the words a search matched when a note is opened from search results.
    void setSearchMatches(const MatchSet& matches, bool scrollToFirst);
    void clearSearchMatches();
    void setFindSelections(const QList<QTextCursor>& matches, int current);

    // Pasted images become encrypted attachments; the main window stores them and returns the file id.
    std::function<QString(const QImage&)> storePastedImage;

signals:
    void diagramActivated(const QString& diagramId);
    void fileActivated(const QString& fileId);
    void formatChanged();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    QMimeData* createMimeDataFromSelection() const override;

private:
    QPointF docPos(const QPointF& viewportPos) const;
    bool handleMarkdownShortcut();
    bool handleReturn(QKeyEvent* e);
    bool handleTableDeletion(QKeyEvent* e);
    void removeTable(QTextTable* table);
    QRectF checkCircleRect(const QTextBlock& b) const;  // document coordinates
    QTextBlock checklistBlockAt(const QPointF& viewportPos) const;
    void toggleCheck(const QTextBlock& b);
    void mergeFormat(const QTextCharFormat& f);
    void editSelectionFormat(const std::function<void(QTextCharFormat&)>& edit);
    void insertMarkdown(const QString& markdown);
    void updateExtraSelections();
    void restyleCursorBlocks(const QTextCursor& c);

    NoteDocument* doc_ = nullptr;
    QTextDocument* empty_ = nullptr;
    bool swallowRelease_ = false;
    QList<QTextEdit::ExtraSelection> searchSelections_;
    QList<QTextEdit::ExtraSelection> findSelections_;
};

}  // namespace zn
