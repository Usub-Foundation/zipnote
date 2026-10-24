#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <expected>

namespace zn {

struct Folder {
    QString id;
    QString name;
    QString parentId;  // empty = top level
    int order = 0;
};

struct NoteMeta {
    QString id;
    QString folderId;
    QDateTime created;
    QDateTime modified;
    bool pinned = false;
    QDateTime trashed;  // invalid = not in Recently Deleted
};

// A binary attachment of a note: an Excalidraw scene, a pasted image, a cached preview.
struct FileMeta {
    QString id;
    QString noteId;
    QString kind;        // "excalidraw", "image"
    QString mime;
    QString searchText;  // text extracted for the search index (Excalidraw labels), kept encrypted in the manifest
    QString previewId;   // id of the FileMeta holding a rendered PNG, if any
};

// Everything that is not note content. Stored encrypted as a single entry; it also authenticates every other
// entry by hash, so swapping or rolling back a single note file is detected.
struct Manifest {
    int version = 1;
    quint64 revision = 0;
    QList<Folder> folders;
    QList<NoteMeta> notes;
    QList<FileMeta> files;
    QHash<QString, QByteArray> entryHashes;  // zip entry name -> BLAKE2b-256 of the stored blob
    QJsonObject settings;

    QByteArray toJson() const;
    static std::expected<Manifest, QString> fromJson(const QByteArray& json);

    NoteMeta* note(const QString& id);
    const NoteMeta* note(const QString& id) const;
    Folder* folder(const QString& id);
    const Folder* folder(const QString& id) const;
    FileMeta* file(const QString& id);
    const FileMeta* file(const QString& id) const;
    QString folderPath(const QString& folderId) const;  // "Work / Projects"
};

QString newId();  // UUID v4 without braces

}  // namespace zn
