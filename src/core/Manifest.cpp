#include "Manifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace zn {

namespace {

QString toIso(const QDateTime& dt) {
    return dt.isValid() ? dt.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime fromIso(const QJsonValue& v) {
    const QString s = v.toString();
    if (s.isEmpty())
        return {};
    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(s, Qt::ISODate);
    return dt;
}

template <class T>
T* findById(QList<T>& list, const QString& id) {
    for (auto& item : list)
        if (item.id == id)
            return &item;
    return nullptr;
}

template <class T>
const T* findById(const QList<T>& list, const QString& id) {
    for (const auto& item : list)
        if (item.id == id)
            return &item;
    return nullptr;
}

}  // namespace

QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray Manifest::toJson() const {
    QJsonArray jf;
    for (const auto& f : folders)
        jf.append(QJsonObject{{"id", f.id}, {"name", f.name}, {"parent", f.parentId}, {"order", f.order}});
    QJsonArray jn;
    for (const auto& n : notes) {
        QJsonObject o{{"id", n.id}, {"folder", n.folderId}, {"created", toIso(n.created)}, {"modified", toIso(n.modified)}};
        if (n.pinned)
            o["pinned"] = true;
        if (n.trashed.isValid())
            o["trashed"] = toIso(n.trashed);
        jn.append(o);
    }
    QJsonArray jfiles;
    for (const auto& f : files) {
        QJsonObject o{{"id", f.id}, {"note", f.noteId}, {"kind", f.kind}, {"mime", f.mime}};
        if (!f.searchText.isEmpty())
            o["text"] = f.searchText;
        if (!f.previewId.isEmpty())
            o["preview"] = f.previewId;
        jfiles.append(o);
    }
    QJsonObject hashes;
    for (auto it = entryHashes.cbegin(); it != entryHashes.cend(); ++it)
        hashes[it.key()] = QString::fromLatin1(it.value().toBase64());

    QJsonObject root{{"version", version},        {"revision", QString::number(revision)},
                     {"folders", jf},             {"notes", jn},
                     {"files", jfiles},           {"entries", hashes},
                     {"settings", settings}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::expected<Manifest, QString> Manifest::fromJson(const QByteArray& json) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (!doc.isObject())
        return std::unexpected(QStringLiteral("manifest is not valid JSON: %1").arg(err.errorString()));
    const QJsonObject root = doc.object();

    Manifest m;
    m.version = root["version"].toInt(1);
    if (m.version > 1)
        return std::unexpected(QStringLiteral("manifest version %1 is newer than this app supports").arg(m.version));
    m.revision = root["revision"].toString().toULongLong();
    for (const auto& v : root["folders"].toArray()) {
        const auto o = v.toObject();
        m.folders.append({o["id"].toString(), o["name"].toString(), o["parent"].toString(), o["order"].toInt()});
    }
    for (const auto& v : root["notes"].toArray()) {
        const auto o = v.toObject();
        NoteMeta n;
        n.id = o["id"].toString();
        n.folderId = o["folder"].toString();
        n.created = fromIso(o["created"]);
        n.modified = fromIso(o["modified"]);
        n.pinned = o["pinned"].toBool();
        n.trashed = fromIso(o["trashed"]);
        m.notes.append(n);
    }
    for (const auto& v : root["files"].toArray()) {
        const auto o = v.toObject();
        m.files.append({o["id"].toString(), o["note"].toString(), o["kind"].toString(), o["mime"].toString(),
                        o["text"].toString(), o["preview"].toString()});
    }
    const auto hashes = root["entries"].toObject();
    for (auto it = hashes.begin(); it != hashes.end(); ++it)
        m.entryHashes.insert(it.key(), QByteArray::fromBase64(it.value().toString().toLatin1()));
    m.settings = root["settings"].toObject();
    return m;
}

NoteMeta* Manifest::note(const QString& id) { return findById(notes, id); }
const NoteMeta* Manifest::note(const QString& id) const { return findById(notes, id); }
Folder* Manifest::folder(const QString& id) { return findById(folders, id); }
const Folder* Manifest::folder(const QString& id) const { return findById(folders, id); }
FileMeta* Manifest::file(const QString& id) { return findById(files, id); }
const FileMeta* Manifest::file(const QString& id) const { return findById(files, id); }

QString Manifest::folderPath(const QString& folderId) const {
    QStringList parts;
    const Folder* f = folder(folderId);
    for (int guard = 0; f && guard < 64; ++guard) {
        parts.prepend(f->name);
        f = f->parentId.isEmpty() ? nullptr : folder(f->parentId);
    }
    return parts.join(QStringLiteral(" / "));
}

}  // namespace zn
