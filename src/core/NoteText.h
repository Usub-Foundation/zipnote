#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace zn {

// Diagram kinds stored as fenced code blocks in the note Markdown (```mermaid ... ```).
// Excalidraw scenes are binary-ish JSON and live in a separate encrypted file referenced by an image link.
enum class DiagramKind { None, Mermaid, PlantUml, Excalidraw };

DiagramKind diagramKindFromInfo(QStringView info);  // fence info string -> kind
QString diagramKindName(DiagramKind kind);          // "mermaid", "plantuml", "excalidraw"
QString diagramKindLabel(DiagramKind kind);         // "Mermaid", "PlantUML", "Excalidraw"

struct DiagramBlock {
    DiagramKind kind = DiagramKind::None;
    QString source;
    int index = 0;  // n-th diagram in the note
};

// What the list, the sidebar and the search index need to know about a note, derived from its Markdown:
// the first line is the title, the text after it the snippet.
struct NoteDigest {
    QString title;    // first non-empty line, Markdown stripped
    QString snippet;  // text after the title, one line, for the note list
    QString body;     // plain text without the title line
    QString headings;
    QString code;     // contents of non-diagram code blocks
    QStringList tags; // #hashtags without '#', in order of appearance, unique
    QList<DiagramBlock> diagrams;
    QStringList fileRefs;  // ids from zn-file://<id> links
    int checklistOpen = 0;
    int checklistDone = 0;
    bool hasTable = false;
    bool hasLink = false;
    bool hasCode = false;
};

NoteDigest digestMarkdown(const QString& markdown);

// Removes inline Markdown: emphasis markers, code ticks, links ([text](url) -> text), <u> and == highlights.
QString stripInline(QStringView line);

}  // namespace zn
