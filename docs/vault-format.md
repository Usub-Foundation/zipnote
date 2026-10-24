# ZipNotes vault format, version 1

A vault is a regular `.zip`. Any archiver can open it and list its files, but apart from the header
everything inside is encrypted and the file names are random UUIDs.

```
zipnotes.json                 plaintext header
manifest.znx                  encrypted manifest
notes/<uuid>.md.znx           a note in Markdown, one file per note
files/<uuid>.znx              an attachment: Excalidraw scene, image, PNG preview
files/r-<hash>.znx            cache of rendered Mermaid/PlantUML diagrams
```

All entries are stored without zip compression (`ZIP_CM_STORE`): ciphertext does not compress, so
compression (zlib) happens before encryption. Every entry has the same modification time
(1980-01-01 12:00 UTC), so the zip directory does not reveal when individual notes were edited.

## Header `zipnotes.json`

```json
{
  "format": "zipnotes-vault",
  "version": 1,
  "vault_id": "<base64, 16 random bytes>",
  "kdf": { "alg": "argon2id13", "opslimit": "3", "memlimit": "268435456", "salt": "<base64, 16 bytes>" },
  "aead": "xchacha20poly1305-ietf",
  "wrapped_key": "<base64: nonce(24) || ciphertext(32) || tag(16)>"
}
```

## Keys

```
password ──Argon2id(salt, opslimit, memlimit)──▶ KEK (32 bytes)
KEK ──XChaCha20-Poly1305(AD = keywrap-AD)──▶ wrapped_key = encrypted DEK
DEK (32 random bytes) encrypts every entry
```

- `keywrap-AD = "zipnotes/1/keywrap|" || vault_id || "|" || opslimit || "|" || memlimit || "|" || salt`.
  The KDF parameters are part of the authenticated data: editing them in the plaintext header makes
  unlocking fail instead of quietly weakening the vault.
- Changing the password re-wraps `wrapped_key` only; the notes are not touched.
- Default parameters are libsodium’s `OPSLIMIT_MODERATE` / `MEMLIMIT_MODERATE` (3 passes, 256 MiB):
  about a second on a current laptop.
- In memory the DEK lives in `sodium_malloc` (guard pages, `mlock`) and is wiped when the vault is locked.

## Entries (`*.znx`)

```
"ZNE1" (4 bytes) | flags (1 byte) | nonce (24) | ciphertext | tag (16)
flags: bit 0 = content compressed with qCompress (zlib with a 4-byte length prefix)
AD   = "zipnotes/1/entry|" || vault_id || "|" || entry name in the zip (UTF-8) || "|" || flags
```

The entry name is part of the AD, so a blob cannot be moved under another name (for example to swap one
note for another): decryption fails.

## Manifest `manifest.znx`

Encrypted JSON:

```json
{
  "version": 1,
  "revision": "42",
  "folders": [ { "id": "…", "name": "Work", "parent": "", "order": 1 } ],
  "notes":   [ { "id": "…", "folder": "…", "created": "2026-10-24T18:00:00.000Z",
                 "modified": "…", "pinned": true, "trashed": "…" } ],
  "files":   [ { "id": "…", "note": "…", "kind": "excalidraw", "mime": "…",
                 "text": "drawing labels for search", "preview": "<id of the PNG preview>" } ],
  "entries": { "notes/….md.znx": "<base64 BLAKE2b-256 of the blob>" },
  "settings": { }
}
```

`entries` pins every entry by its hash. Replacing one note with an older version of the same file
(a partial rollback) is detected on read as a `hash mismatch`. Rolling back the whole vault (putting an
older `.zip` in place) is possible; an offline format cannot prevent that.

Entries missing from `entries` are never read: a foreign file added to the archive is ignored.

## Writing to disk

Changes are kept in memory and written after 1.5 s of inactivity, when the vault is locked and when the
window closes. libzip writes the new archive to a temporary file next to the vault and renames it over
the old one, so a crash mid-save leaves the previous version intact. While a vault is open,
`<vault>.zip.lock` (`QLockFile`) keeps a second process from writing to the same file.

## Note content

Markdown (CommonMark + GitHub extensions), see `src/app/MarkdownIO.h`:

| Style | Markdown |
|---|---|
| Title / Heading / Subheading | `#` / `##` / `###` |
| Monostyled | ```` ``` ```` block |
| Bulleted / Dashed / Numbered list | `- ` / `* ` / `1. ` |
| Checklist | `- [ ] ` / `- [x] ` |
| Block quote | `> ` |
| Bold / Italic / Underline / Strikethrough | `**` / `*` / `<u>…</u>` / `~~` |
| Highlight | `==…==` |
| Table | GFM table |
| Mermaid / PlantUML | ```` ```mermaid ```` / ```` ```plantuml ```` |
| Excalidraw drawing | `![Drawing](zn-file://<id>)` + `files/<id>.znx` (`.excalidraw` scene JSON) |
| Empty paragraph | `&nbsp;` |

A decrypted note opens fine in other Markdown tools; ```` ```mermaid ```` blocks render on GitHub too.

## What is not hidden

- The number of notes and attachments and the approximate size of each (ciphertext length).
- When the archive as a whole was last saved (the file system mtime of the `.zip`).
- Paths of recently opened vaults, in the app settings (`QSettings`); no note titles.
