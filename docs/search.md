# How search works

Search has to work on an encrypted vault without leaking anything to disk, and it has to feel instant
while you type. ZipNotes therefore keeps a full-text index **in memory only**: it is built on a worker
thread right after the vault is unlocked, updated on every save, and dropped when the vault is locked.
There is no index file, encrypted or not.

Code: `src/core/TextNormalizer`, `NoteText`, `SearchQuery`, `SearchIndex`; UI in `src/app/SearchField`,
`NoteListModel`, `NoteEditor`.

## Text pipeline

1. **Digest.** `digestMarkdown()` turns a note into fields: title (first line), headings, body, tags,
   code blocks, and the text of its attachments — Mermaid/PlantUML sources and the labels of Excalidraw
   drawings.
2. **Folding.** `text::normalize()` applies NFKD, drops combining marks and case-folds:
   `Café → cafe`, `Ёлка → елка`, `ﬁle → file`, `Straße → strasse`. `й` is kept: it is a letter of its own,
   not `и` with a mark. A map from folded offsets back to the source lets matches be highlighted in the
   original text.
3. **Tokens.** Runs of letters and digits; Han, Hiragana and Katakana characters are one token each.

## Index

- A sorted term dictionary (`std::map`), so a prefix is a contiguous range: typing `mee` expands to
  `meet`, `meeting`, `meetup`… in one lookup.
- Posting lists per term (sorted note ids) and, per note and field, the token positions (for phrases,
  proximity and snippets).
- Upserts and removals per note; a shared mutex lets searches run on a worker while the UI saves.

## Query

`SearchQuery::parse()` understands:

| Syntax | Meaning |
|---|---|
| `word` | every word is a prefix; all words must match (AND) |
| `"two words"` | adjacent words |
| `-word` | exclude notes containing the word |
| `#tag`, `tag:x` | tag (prefix, so `#work` also matches `work/projects`) |
| `folder:x`, `in:x` | folder name contains x; `in:trash` searches Recently Deleted |
| `has:checklist`, `todo`, `diagram`, `mermaid`, `plantuml`, `drawing`, `table`, `link`, `code`, `image`, `tag` | note features |
| `is:pinned` | pinned notes |
| `modified:7d`, `after:2026-10-01`, `before:…` | date edited |

In the UI the same filters appear as tokens (“chips”) picked from the suggestion list; suggestions for
folders and tags use a substring match that ignores case and accents.

## Execution

Each keystroke starts a new search on a single worker thread; a generation counter cancels the previous
one mid-flight, so stale results never reach the UI.

1. **Primary pass.** Exact and prefix matches. Candidates are the intersection of the posting lists of
   every word, then filters, exclusions and phrase checks.
2. **Supplemental pass.** Only when the primary pass found fewer than three notes: every dictionary term
   within Damerau–Levenshtein distance 1 (words of 4–7 letters) or 2 (8+) of a query word, also as a
   prefix — `meetnig` finds “Meeting”. These hits rank below exact ones.
3. **Ranking.** BM25 per field with weights title 3 · tags 2.5 · headings 2 · body 1 · diagrams 0.8 ·
   code 0.6; exact matches score above prefix matches, prefix above typo matches. Bonuses: all words in
   the title, pinned, recently edited.

## Results

- **Top Hits** — up to three notes by relevance (title matches or scores close to the best one).
- **Notes** — the rest, ordered by date edited, like the folder list.
- **Attachments** — diagrams and drawings whose own text matches every word; opening one scrolls the
  note to it.

Each hit carries highlight ranges for its title and a snippet window chosen around the densest cluster
of matches. Opening a note from the results highlights the matched words in the editor and scrolls to the
first one. Find in Note (Ctrl+F) is a separate substring search inside the open note with the same
folding rules.

## Performance

On 5,000 notes a two-word query takes about 5 ms (see `tests/test_search.cpp`). Unlocking decrypts and
indexes all notes on a worker thread; the list appears when that is done.
