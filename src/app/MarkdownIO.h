#pragma once

#include <QString>

class QTextDocument;

namespace zn {

class NoteDocument;

// Markdown is the storage format of a note. Import uses Qt's md4c-based parser (CommonMark + GitHub
// extensions) with pre/post-processing for what it lacks; export is our own writer because
// QTextDocument::toMarkdown() re-wraps paragraphs at 80 columns and writes <u> as _..._, which other
// Markdown readers show as italics.
//
// Extensions on top of GitHub Markdown, chosen to stay readable in other tools:
//   ==highlight==            highlighted text (Obsidian / markdown-it-mark syntax)
//   <u>underline</u>         underline (inline HTML)
//   * item                   dashed list ("-" is a bulleted list)
//   ```mermaid / ```plantuml diagrams, rendered inline by the editor
//   ![Drawing](zn-file://id) Excalidraw drawing or image stored as an encrypted attachment
//   &nbsp;                   an intentionally empty paragraph

void importMarkdown(NoteDocument& doc, const QString& markdown);
QString exportMarkdown(const NoteDocument& doc);

}  // namespace zn
