#include "VaultController.h"

#include "DemoContent.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <optional>
#include <algorithm>

namespace zn {

namespace {

constexpr int kTrashRetentionDays = 30;  // Recently Deleted keeps notes for 30 days
constexpr int kSaveDelayMs = 1500;

IndexDoc makeIndexDoc(const NoteRecord& r, const NoteMeta& m, const Manifest& manifest) {
    IndexDoc d;
    d.id = r.id;
    d.folderId = m.folderId;
    d.folderName = manifest.folderPath(m.folderId);
    d.title = r.digest.title;
    d.headings = r.digest.headings;
    d.body = r.digest.body;
    d.code = r.digest.code;
    d.tags = r.digest.tags;
    d.created = m.created;
    d.modified = m.modified;
    d.pinned = m.pinned;
    d.trashed = m.trashed.isValid();
    for (const auto& dg : r.digest.diagrams) {
        d.attachments.append({QStringLiteral("%1/diagram/%2").arg(r.id).arg(dg.index), diagramKindName(dg.kind), dg.source});
        d.flags |= dg.kind == DiagramKind::Mermaid ? HasMermaid : HasPlantUml;
    }
    for (const auto& fileId : r.digest.fileRefs) {
        const FileMeta* f = manifest.file(fileId);
        if (!f)
            continue;
        d.attachments.append({fileId, f->kind, f->searchText});
        d.flags |= f->kind == u"excalidraw" ? HasExcalidraw : HasImage;
    }
    if (r.digest.checklistOpen + r.digest.checklistDone > 0)
        d.flags |= HasChecklist;
    if (r.digest.checklistOpen > 0)
        d.flags |= HasOpenTodo;
    if (r.digest.hasTable)
        d.flags |= HasTable;
    if (r.digest.hasLink)
        d.flags |= HasLink;
    if (r.digest.hasCode)
        d.flags |= HasCode;
    return d;
}

}  // namespace

struct VaultController::LoadResult {
    std::unique_ptr<Vault> vault;
    QString error;
    QHash<QString, NoteRecord> notes;
    QStringList warnings;
};

VaultController::VaultController(QObject* parent) : QObject(parent) {
    crypto::init();  // idempotent; libsodium aborts in sodium_malloc() if nobody called it
    qRegisterMetaType<zn::SearchResults>();
    saveTimer_.setSingleShot(true);
    connect(&saveTimer_, &QTimer::timeout, this, [this] { saveNow(); });
    searchPool_.setMaxThreadCount(1);
}

VaultController::~VaultController() {
    // an unlock or password change still running holds pointers into this object
    if (busy_)
        QThreadPool::globalInstance()->waitForDone();
    searchPool_.waitForDone();
    busy_ = false;
    if (vault_)
        lock();
}

void VaultController::setBusy(bool busy, const QString& message) {
    busy_ = busy;
    emit busyChanged(busy, message);
}

namespace {

// Runs on a worker thread: decrypts every note and fills the index before the UI sees the vault.
void loadNotes(VaultController::LoadResult* r, SearchIndex* index) {
    Manifest& m = r->vault->manifest();
    QStringList entries;
    for (const auto& n : m.notes)
        entries.append(Vault::noteEntry(n.id));
    const auto contents = r->vault->readMany(entries);
    for (const auto& n : m.notes) {
        NoteRecord rec;
        rec.id = n.id;
        const auto& c = contents.value(Vault::noteEntry(n.id), std::unexpected(QStringLiteral("missing")));
        if (c) {
            rec.markdown = QString::fromUtf8(*c);
            rec.digest = digestMarkdown(rec.markdown);
        } else {
            rec.loadError = c.error();
            rec.digest.title = QStringLiteral("Damaged note");
            rec.digest.snippet = c.error();
            r->warnings.append(c.error());
        }
        index->upsert(makeIndexDoc(rec, n, m));
        r->notes.insert(n.id, std::move(rec));
    }
}

}  // namespace

void VaultController::openVault(const QString& path, const QString& password) {
    if (busy_)
        return;
    if (vault_)
        lock();
    setBusy(true, tr("Unlocking…"));
    index_.clear();
    SearchIndex* index = &index_;
    auto* watcher = new QFutureWatcher<std::shared_ptr<LoadResult>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        finishOpen(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, password, index] {
        auto r = std::make_shared<LoadResult>();
        auto v = Vault::open(path, password);
        if (!v) {
            r->error = v.error();
            return r;
        }
        r->vault = std::move(*v);
        loadNotes(r.get(), index);
        return r;
    }));
}

void VaultController::createVault(const QString& path, const QString& password, bool withSamples) {
    if (busy_)
        return;
    if (vault_)
        lock();
    setBusy(true, tr("Creating vault…"));
    index_.clear();
    SearchIndex* index = &index_;
    auto* watcher = new QFutureWatcher<std::shared_ptr<LoadResult>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        finishOpen(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, password, withSamples, index] {
        auto r = std::make_shared<LoadResult>();
        auto v = Vault::create(path, password, crypto::KdfParams::recommended());
        if (!v) {
            r->error = v.error();
            return r;
        }
        r->vault = std::move(*v);
        if (withSamples) {
            demo::populate(*r->vault);
            if (auto c = r->vault->commit(); !c)
                r->warnings.append(c.error());
        }
        loadNotes(r.get(), index);
        return r;
    }));
}

void VaultController::finishOpen(std::shared_ptr<LoadResult> r) {
    setBusy(false);
    if (!r->vault) {
        index_.clear();
        emit openFailed(r->error);
        return;
    }
    vault_ = std::move(r->vault);
    notes_ = std::move(r->notes);

    // Recently Deleted keeps notes for 30 days
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-kTrashRetentionDays);
    QStringList expired;
    for (const auto& n : vault_->manifest().notes)
        if (n.trashed.isValid() && n.trashed < cutoff)
            expired.append(n.id);
    for (const auto& id : expired)
        deleteNoteForever(id);

    emit opened();
    if (!r->warnings.isEmpty())
        emit integrityWarning(r->warnings.join(u'\n'));
}

void VaultController::changePassword(const QString& currentPassword, const QString& newPassword) {
    if (!vault_ || busy_)
        return;
    saveNow();
    setBusy(true, tr("Changing password…"));
    struct Derived {
        QString error;
        std::shared_ptr<crypto::SecretKey> kek;
        crypto::KdfParams params;
    };
    const Vault* v = vault_.get();
    auto* watcher = new QFutureWatcher<Derived>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        Derived d = watcher->result();
        watcher->deleteLater();
        setBusy(false);
        if (d.error.isEmpty() && vault_) {
            // the commit happens here, on the thread that owns the vault
            if (auto r = vault_->rewrapKey(*d.kek, d.params); !r)
                d.error = r.error();
        }
        emit passwordChanged(d.error.isEmpty(), d.error);
    });
    // Only the two slow key derivations run on the worker; they read immutable header fields of the vault.
    watcher->setFuture(QtConcurrent::run([v, currentPassword, newPassword]() -> Derived {
        if (!v->checkPassword(currentPassword))
            return {tr("The current password is not correct."), nullptr, {}};
        if (newPassword.isEmpty())
            return {tr("The new password must not be empty."), nullptr, {}};
        crypto::KdfParams params = crypto::KdfParams::recommended();
        auto kek = crypto::deriveKey(newPassword, params);
        if (!kek)
            return {kek.error(), nullptr, {}};
        return {{}, std::make_shared<crypto::SecretKey>(std::move(*kek)), params};
    }));
}

void VaultController::lock() {
    if (!vault_ || busy_)
        return;
    ++searchGeneration_;
    searchPool_.waitForDone();
    collectRenderGarbage();
    saveNow();
    vault_.reset();
    notes_.clear();
    fileCache_.clear();
    index_.clear();
    emit lockedVault();
}

bool VaultController::saveNow() {
    saveTimer_.stop();
    if (!vault_ || !vault_->hasPendingChanges())
        return true;
    if (busy_) {
        saveTimer_.start(kSaveDelayMs);
        return false;
    }
    auto r = vault_->commit();
    if (!r) {
        emit saveFailed(r.error());
        return false;
    }
    return true;
}

void VaultController::scheduleSave() {
    if (!vault_)
        return;
    vault_->markManifestDirty();
    saveTimer_.start(kSaveDelayMs);
}

// ------------------------------------------------------------------------------------------------ notes

QList<const NoteRecord*> VaultController::records() const {
    QList<const NoteRecord*> out;
    out.reserve(notes_.size());
    for (const auto& r : notes_)
        out.append(&r);
    return out;
}

const NoteRecord* VaultController::record(const QString& id) const {
    auto it = notes_.constFind(id);
    return it == notes_.cend() ? nullptr : &*it;
}

const NoteMeta* VaultController::meta(const QString& id) const {
    return vault_ ? vault_->manifest().note(id) : nullptr;
}

void VaultController::reindex(const QString& id) {
    const NoteRecord* r = record(id);
    const NoteMeta* m = meta(id);
    if (r && m)
        index_.upsert(makeIndexDoc(*r, *m, vault_->manifest()));
}

void VaultController::touch(const QString& id) {
    if (NoteMeta* m = vault_->manifest().note(id))
        m->modified = QDateTime::currentDateTimeUtc();
}

QString VaultController::createNote(const QString& folderId, const QString& markdown) {
    if (!vault_)
        return {};
    NoteMeta m;
    m.id = newId();
    m.folderId = vault_->manifest().folder(folderId) ? folderId : defaultFolderId();
    m.created = m.modified = QDateTime::currentDateTimeUtc();
    vault_->manifest().notes.append(m);
    NoteRecord r;
    r.id = m.id;
    r.markdown = markdown;
    r.digest = digestMarkdown(markdown);
    notes_.insert(m.id, r);
    vault_->write(Vault::noteEntry(m.id), markdown.toUtf8());
    reindex(m.id);
    scheduleSave();
    emit notesChanged();
    return m.id;
}

void VaultController::updateNoteContent(const QString& id, const QString& markdown) {
    auto it = notes_.find(id);
    if (!vault_ || it == notes_.end() || !it->loadError.isEmpty() || it->markdown == markdown)
        return;
    it->markdown = markdown;
    it->digest = digestMarkdown(markdown);
    touch(id);
    vault_->write(Vault::noteEntry(id), markdown.toUtf8());
    reindex(id);
    scheduleSave();
    emit noteContentChanged(id);
}

void VaultController::setPinned(const QString& id, bool pinned) {
    if (NoteMeta* m = vault_ ? vault_->manifest().note(id) : nullptr; m && m->pinned != pinned) {
        m->pinned = pinned;
        reindex(id);
        scheduleSave();
        emit notesChanged();
    }
}

void VaultController::moveNote(const QString& id, const QString& folderId) {
    NoteMeta* m = vault_ ? vault_->manifest().note(id) : nullptr;
    if (!m || !vault_->manifest().folder(folderId) || m->folderId == folderId)
        return;
    m->folderId = folderId;
    m->trashed = {};
    reindex(id);
    scheduleSave();
    emit notesChanged();
}

void VaultController::trashNote(const QString& id) {
    NoteMeta* m = vault_ ? vault_->manifest().note(id) : nullptr;
    if (!m)
        return;
    if (m->trashed.isValid()) {  // deleting from Recently Deleted removes it for good
        deleteNoteForever(id);
        return;
    }
    m->trashed = QDateTime::currentDateTimeUtc();
    m->pinned = false;
    reindex(id);
    scheduleSave();
    emit notesChanged();
}

void VaultController::restoreNote(const QString& id) {
    NoteMeta* m = vault_ ? vault_->manifest().note(id) : nullptr;
    if (!m || !m->trashed.isValid())
        return;
    m->trashed = {};
    if (!vault_->manifest().folder(m->folderId))
        m->folderId = defaultFolderId();
    reindex(id);
    scheduleSave();
    emit notesChanged();
}

void VaultController::deleteNoteForever(const QString& id) {
    if (!vault_)
        return;
    auto& notes = vault_->manifest().notes;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const NoteMeta& n) { return n.id == id; }), notes.end());
    auto& files = vault_->manifest().files;
    for (auto it = files.begin(); it != files.end();) {
        if (it->noteId == id) {
            vault_->remove(Vault::fileEntry(it->id));
            fileCache_.remove(it->id);
            it = files.erase(it);
        } else {
            ++it;
        }
    }
    vault_->remove(Vault::noteEntry(id));
    notes_.remove(id);
    index_.remove(id);
    scheduleSave();
    emit notesChanged();
}

void VaultController::emptyTrash() {
    if (!vault_)
        return;
    QStringList ids;
    for (const auto& n : vault_->manifest().notes)
        if (n.trashed.isValid())
            ids.append(n.id);
    for (const auto& id : ids)
        deleteNoteForever(id);
}

int VaultController::trashCount() const {
    if (!vault_)
        return 0;
    return static_cast<int>(std::count_if(vault_->manifest().notes.cbegin(), vault_->manifest().notes.cend(),
                                          [](const NoteMeta& n) { return n.trashed.isValid(); }));
}

// ------------------------------------------------------------------------------------------------ folders

const QList<Folder>& VaultController::folders() const {
    static const QList<Folder> empty;
    return vault_ ? vault_->manifest().folders : empty;
}

QString VaultController::defaultFolderId() const {
    return vault_ && !vault_->manifest().folders.isEmpty() ? vault_->manifest().folders.first().id : QString();
}

QString VaultController::createFolder(const QString& name, const QString& parentId) {
    if (!vault_)
        return {};
    Folder f;
    f.id = newId();
    f.name = name.trimmed().isEmpty() ? tr("New Folder") : name.trimmed();
    f.parentId = vault_->manifest().folder(parentId) ? parentId : QString();
    int order = 0;
    for (const auto& o : vault_->manifest().folders)
        order = std::max(order, o.order + 1);
    f.order = order;
    vault_->manifest().folders.append(f);
    scheduleSave();
    emit foldersChanged();
    return f.id;
}

void VaultController::renameFolder(const QString& id, const QString& name) {
    Folder* f = vault_ ? vault_->manifest().folder(id) : nullptr;
    if (!f || name.trimmed().isEmpty() || f->name == name.trimmed())
        return;
    f->name = name.trimmed();
    for (const auto& r : notes_)
        if (const NoteMeta* m = meta(r.id); m && folderWithDescendants(id).contains(m->folderId))
            reindex(r.id);
    scheduleSave();
    emit foldersChanged();
}

QStringList VaultController::folderWithDescendants(const QString& id) const {
    QStringList out{id};
    for (int i = 0; i < out.size(); ++i)
        for (const auto& f : folders())
            if (f.parentId == out[i] && !out.contains(f.id))
                out.append(f.id);
    return out;
}

void VaultController::deleteFolder(const QString& id) {
    if (!vault_ || id == defaultFolderId())
        return;
    const QStringList doomed = folderWithDescendants(id);
    const QString fallback = defaultFolderId();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto& n : vault_->manifest().notes) {
        if (!doomed.contains(n.folderId))
            continue;
        n.folderId = fallback;
        if (!n.trashed.isValid())
            n.trashed = now;
        n.pinned = false;
    }
    auto& fs = vault_->manifest().folders;
    fs.erase(std::remove_if(fs.begin(), fs.end(), [&](const Folder& f) { return doomed.contains(f.id); }), fs.end());
    for (const auto& r : notes_)
        reindex(r.id);
    scheduleSave();
    emit foldersChanged();
    emit notesChanged();
}

QString VaultController::folderPath(const QString& id) const {
    return vault_ ? vault_->manifest().folderPath(id) : QString();
}

int VaultController::noteCount(const QString& folderId) const {
    if (!vault_)
        return 0;
    const QStringList ids = folderWithDescendants(folderId);
    int n = 0;
    for (const auto& m : vault_->manifest().notes)
        if (!m.trashed.isValid() && ids.contains(m.folderId))
            ++n;
    return n;
}

// ------------------------------------------------------------------------------------------------ files

QString VaultController::addFile(const QString& noteId, const QString& kind, const QString& mime,
                                 const QByteArray& data, const QString& searchText) {
    if (!vault_)
        return {};
    FileMeta f;
    f.id = newId();
    f.noteId = noteId;
    f.kind = kind;
    f.mime = mime;
    f.searchText = searchText;
    vault_->manifest().files.append(f);
    vault_->write(Vault::fileEntry(f.id), data);
    fileCache_.insert(f.id, data);
    scheduleSave();
    return f.id;
}

void VaultController::updateFile(const QString& id, const QByteArray& data, const QString& searchText) {
    FileMeta* f = vault_ ? vault_->manifest().file(id) : nullptr;
    if (!f)
        return;
    f->searchText = searchText;
    vault_->write(Vault::fileEntry(id), data);
    fileCache_.insert(id, data);
    if (!f->noteId.isEmpty())
        reindex(f->noteId);
    scheduleSave();
}

void VaultController::setFilePreview(const QString& id, const QByteArray& png, bool dark) {
    FileMeta* f = vault_ ? vault_->manifest().file(id) : nullptr;
    if (!f)
        return;
    const QString current = dark ? f->darkPreviewId : f->previewId;
    if (current.isEmpty() || !vault_->manifest().file(current)) {
        const QString noteId = f->noteId;
        const QString previewId = addFile(noteId, QStringLiteral("preview"), QStringLiteral("image/png"), png);
        if ((f = vault_->manifest().file(id)))  // addFile may have reallocated the list
            (dark ? f->darkPreviewId : f->previewId) = previewId;
    } else {
        vault_->write(Vault::fileEntry(current), png);
        fileCache_.insert(current, png);
    }
    scheduleSave();
}

std::optional<QByteArray> VaultController::readFile(const QString& id) {
    if (!vault_)
        return std::nullopt;
    if (auto it = fileCache_.constFind(id); it != fileCache_.cend())
        return *it;
    if (!vault_->manifest().file(id))
        return std::nullopt;
    auto data = vault_->read(Vault::fileEntry(id));
    if (!data) {
        emit integrityWarning(data.error());
        return std::nullopt;
    }
    fileCache_.insert(id, *data);
    return *data;
}

const FileMeta* VaultController::fileMeta(const QString& id) const {
    return vault_ ? vault_->manifest().file(id) : nullptr;
}

QString VaultController::renderKey(DiagramKind kind, const QString& source, bool dark) {
    // bump when rendering changes (scale, themes, renderer versions) so cached images are redrawn
    constexpr char kRenderVersion[] = "r2";
    const QByteArray material = QByteArray(kRenderVersion) + '\n' + diagramKindName(kind).toUtf8() + '\n' + (dark ? "dark" : "light") + '\n' + source.toUtf8();
    return QString::fromLatin1(crypto::hash(material).left(20).toHex());
}

std::optional<QByteArray> VaultController::cachedRender(const QString& key) {
    return readFile(QStringLiteral("r-") + key);
}

void VaultController::storeRender(const QString& key, const QByteArray& png) {
    if (!vault_ || busy_)  // the cache is optional; never race a password change
        return;
    const QString id = QStringLiteral("r-") + key;
    if (vault_->manifest().file(id))
        return;
    FileMeta f;
    f.id = id;
    f.kind = QStringLiteral("render");
    f.mime = QStringLiteral("image/png");
    vault_->manifest().files.append(f);
    vault_->write(Vault::fileEntry(id), png);
    fileCache_.insert(id, png);
    scheduleSave();
}

// Drops cached renders no diagram uses any more and attachments no note links to.
void VaultController::collectRenderGarbage() {
    if (!vault_)
        return;
    QSet<QString> keep;
    for (const auto& r : notes_) {
        for (const auto& d : r.digest.diagrams) {
            keep.insert(QStringLiteral("r-") + renderKey(d.kind, d.source, false));
            keep.insert(QStringLiteral("r-") + renderKey(d.kind, d.source, true));
        }
        for (const auto& fileId : r.digest.fileRefs) {
            keep.insert(fileId);
            if (const FileMeta* f = vault_->manifest().file(fileId)) {
                if (!f->previewId.isEmpty())
                    keep.insert(f->previewId);
                if (!f->darkPreviewId.isEmpty())
                    keep.insert(f->darkPreviewId);
            }
        }
    }
    auto& files = vault_->manifest().files;
    const auto before = files.size();
    for (auto it = files.begin(); it != files.end();) {
        if (!keep.contains(it->id)) {
            vault_->remove(Vault::fileEntry(it->id));
            fileCache_.remove(it->id);
            it = files.erase(it);
        } else {
            ++it;
        }
    }
    if (files.size() != before)
        vault_->markManifestDirty();
}

// ------------------------------------------------------------------------------------------------ search

quint64 VaultController::search(const SearchQuery& query, const SearchOptions& options) {
    const quint64 gen = ++searchGeneration_;
    if (!vault_)
        return gen;
    searchPool_.start([this, query, options, gen] {
        SearchResults res = index_.search(query, options, [this, gen] { return searchGeneration_.load() != gen; });
        if (res.cancelled)
            return;
        QMetaObject::invokeMethod(
            this,
            [this, gen, res = std::move(res)] {
                if (gen == searchGeneration_.load())
                    emit searchFinished(gen, res);
            },
            Qt::QueuedConnection);
    });
    return gen;
}

QJsonValue VaultController::setting(const QString& key) const {
    return vault_ ? vault_->manifest().settings.value(key) : QJsonValue();
}

void VaultController::setSetting(const QString& key, const QJsonValue& value) {
    if (!vault_ || vault_->manifest().settings.value(key) == value)
        return;
    vault_->manifest().settings.insert(key, value);
    scheduleSave();
}

}  // namespace zn
