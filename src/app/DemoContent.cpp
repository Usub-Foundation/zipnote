#include "DemoContent.h"

#include "Vault.h"

#include <QFile>

namespace zn::demo {

namespace {

struct Sample {
    const char* folder;  // empty = default folder
    double daysAgo;
    bool pinned;
    const char* markdown;
    bool drawing = false;  // append the bundled Excalidraw whiteboard
};

const Sample kSamples[] = {
    {"", 0.01, true, R"(# Welcome to ZipNotes

Your notes live in **one encrypted .zip file**. Folders, titles and text are encrypted; nothing is ever written to disk in plain text. #welcome

## Text styles

Use the **Aa** menu or type Markdown shortcuts: `# ` for a title, `## ` for a heading, `- ` for a bulleted list, `[] ` for a checklist, `> ` for a quote.

- **Bold**, *italic*, <u>underline</u>, ~~strikethrough~~ and ==highlight==
- `inline code` and [links](https://example.com)
* dashed lists

> Quotes get a yellow bar on the left.

## Search

Click the search field (**Ctrl+Alt+F**, or **⌥⌘F** on a Mac) and start typing. Every word matches as a prefix, ignoring case and accents: `cafe` finds *Café*, `елка` finds *Ёлка*.

| Type | Finds |
| --- | --- |
| `#travel` | notes with that tag |
| `has:todo` | notes with unchecked items |
| `has:diagram` | notes with Mermaid, PlantUML or Excalidraw |
| `"exact phrase"` | words next to each other |
| `-draft` | notes without the word |
| `modified:7d` | edited in the last week |

## Checklist

- [x] Create a vault
- [ ] Write the first note
- [ ] Draw a diagram with **Insert ▸ Mermaid**
)"},
    {"Projects", 0.2, false, R"(# Smart home: evening scene

What happens when the living room switches to the evening scene. #home #ideas

```mermaid
sequenceDiagram
    participant Phone
    participant Hub
    participant Lamp
    Phone->>Hub: run "Evening"
    Hub->>Lamp: dim to 30%, warm white
    Lamp-->>Hub: done
    Hub-->>Phone: scene active
```

```yaml
automation:
  trigger: sunset
  action: scene.evening
```

## Open questions

- [ ] Which bulbs support dimming
- [ ] A motion sensor for the hallway
- [x] Name the scenes
)"},
    {"Personal", 1.3, false, R"(# Trip to Lisbon

Plan for the spring trip. #travel

```plantuml
@startuml
start
:Book flights;
if (Passport valid?) then (yes)
  :Book a room in Alfama;
else (no)
  :Renew the passport;
endif
:Pack;
stop
@enduml
```

1. Tram 28 early in the morning
2. Pastéis in Belém
3. Sunset at the Miradouro da Senhora do Monte
)"},
    {"Projects", 4, false, R"(# ZipNotes ideas

## Ideas

- Export a note as PDF
- Share a single note as an encrypted file
- ==Decide on the search index format==

## Decisions

The index stays **in memory** and is rebuilt after unlocking. Nothing about note contents is stored unencrypted.

- [ ] Write the user guide
- [ ] Try the app on a tablet #ideas
)", true},
    {"Personal", 0.6, false, R"(# Weekend groceries

- [ ] Milk
- [ ] Bread
- [x] Coffee beans from the café on the corner
- [ ] Apples

#home
)"},
    {"Personal", 9, false, R"(# Идеи для отпуска

Поехать на **Байкал** в феврале: лёд, ёлки и чай с облепихой. #отпуск

- Забронировать жильё в Листвянке
- Взять кошки для льда
- Проверить прогноз погоды

> Поиск не различает «е» и «ё»: попробуйте найти «елки».
)"},
    {"Personal", 35, false, R"(# Reading list

1. *The Design of Everyday Things* — Don Norman
2. *Thinking, Fast and Slow* — Daniel Kahneman
3. *The Old Man and the Sea* — Ernest Hemingway

> Good design is actually a lot harder to notice than poor design.

#books
)"},
};

}  // namespace

void populate(Vault& vault) {
    Manifest& m = vault.manifest();
    if (m.folders.isEmpty())
        m.folders.append({newId(), QStringLiteral("Notes"), {}, 0});
    QHash<QString, QString> folderIds{{QString(), m.folders.first().id}};
    for (const char* name : {"Projects", "Personal"}) {
        const QString id = newId();
        m.folders.append({id, QString::fromUtf8(name), {}, static_cast<int>(m.folders.size())});
        folderIds.insert(QString::fromUtf8(name), id);
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const auto& s : kSamples) {
        NoteMeta n;
        n.id = newId();
        n.folderId = folderIds.value(QString::fromUtf8(s.folder));
        n.modified = now.addSecs(static_cast<qint64>(-s.daysAgo * 86400));
        n.created = n.modified.addDays(-1);
        n.pinned = s.pinned;
        m.notes.append(n);
        QByteArray markdown(s.markdown);
        QFile scene(QStringLiteral(":/demo/whiteboard.excalidraw"));
        QFile preview(QStringLiteral(":/demo/whiteboard.png"));
        if (s.drawing && scene.open(QIODevice::ReadOnly) && preview.open(QIODevice::ReadOnly)) {
            // resources exist only in the app binary, so tests get the notes without the drawing
            FileMeta png{newId(), n.id, QStringLiteral("preview"), QStringLiteral("image/png"), {}, {}};
            FileMeta drawing{newId(), n.id, QStringLiteral("excalidraw"), QStringLiteral("application/vnd.excalidraw+json"),
                             QStringLiteral("Editor\nSearch index\nin memory only\nVault (.zip)\nXChaCha20 +\nArgon2id"), png.id};
            m.files.append(png);
            m.files.append(drawing);
            vault.write(Vault::fileEntry(png.id), preview.readAll());
            vault.write(Vault::fileEntry(drawing.id), scene.readAll());
            markdown += "\n## Whiteboard\n\n![Drawing](zn-file://" + drawing.id.toUtf8() + ")\n";
        }
        vault.write(Vault::noteEntry(n.id), markdown);
    }
    vault.markManifestDirty();
}

}  // namespace zn::demo
