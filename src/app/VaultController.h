#pragma once

#include "NoteText.h"
#include "SearchIndex.h"
#include "Vault.h"

#include <QHash>
#include <QJsonValue>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <memory>
#include <optional>

namespace zn {

struct NoteRecord {
    QString id;
    QString markdown;
    NoteDigest digest;
    QString loadError;  // the entry could not be decrypted or failed its hash check
};

// Owns the unlocked vault: decrypted notes, the search index, autosave. Everything heavy (key derivation,
// decrypting all notes, indexing, searching) runs on worker threads; results come back as signals.
class VaultController : public QObject {
    Q_OBJECT
public:
    struct LoadResult;

    explicit VaultController(QObject* parent = nullptr);
    ~VaultController() override;

    bool isOpen() const { return vault_ != nullptr; }
    bool isBusy() const { return busy_; }
    QString vaultPath() const { return vault_ ? vault_->path() : QString(); }
    QString vaultName() const { return vault_ ? vault_->displayName() : QString(); }

    void openVault(const QString& path, const QString& password);
    void createVault(const QString& path, const QString& password, bool withSamples);
    void changePassword(const QString& currentPassword, const QString& newPassword);
    void lock();
    bool saveNow();

    // ---- notes
    QList<const NoteRecord*> records() const;
    const NoteRecord* record(const QString& id) const;
    const NoteMeta* meta(const QString& id) const;
    QString createNote(const QString& folderId, const QString& markdown = {});
    void updateNoteContent(const QString& id, const QString& markdown);
    void setPinned(const QString& id, bool pinned);
    void moveNote(const QString& id, const QString& folderId);
    void trashNote(const QString& id);
    void restoreNote(const QString& id);
    void deleteNoteForever(const QString& id);
    void emptyTrash();
    int trashCount() const;

    // ---- folders
    const QList<Folder>& folders() const;
    QString defaultFolderId() const;
    QString createFolder(const QString& name, const QString& parentId = {});
    void renameFolder(const QString& id, const QString& name);
    void deleteFolder(const QString& id);  // its notes go to Recently Deleted
    QStringList folderWithDescendants(const QString& id) const;
    QString folderPath(const QString& id) const;
    int noteCount(const QString& folderId) const;  // live notes in the folder and its subfolders

    // ---- attachments (Excalidraw scenes, images, rendered diagram cache)
    QString addFile(const QString& noteId, const QString& kind, const QString& mime, const QByteArray& data,
                    const QString& searchText = {});
    void updateFile(const QString& id, const QByteArray& data, const QString& searchText);
    void setFilePreview(const QString& id, const QByteArray& png, bool dark = false);
    std::optional<QByteArray> readFile(const QString& id);
    const FileMeta* fileMeta(const QString& id) const;
    std::optional<QByteArray> cachedRender(const QString& key);
    void storeRender(const QString& key, const QByteArray& png);
    static QString renderKey(DiagramKind kind, const QString& source, bool dark);

    // ---- search
    // Starts a search on a worker thread and cancels the previous one. Returns the generation that
    // searchFinished() will carry.
    quint64 search(const SearchQuery& query, const SearchOptions& options = {});
    QList<SearchIndex::TagCount> tags(QStringView foldedNeedle = {}) const { return index_.tags(foldedNeedle); }

    // ---- settings stored inside the encrypted manifest
    QJsonValue setting(const QString& key) const;
    void setSetting(const QString& key, const QJsonValue& value);

signals:
    void busyChanged(bool busy, const QString& message);
    void opened();
    void openFailed(const QString& error);
    void lockedVault();
    void passwordChanged(bool ok, const QString& error);
    void saveFailed(const QString& error);
    void notesChanged();
    void noteContentChanged(const QString& id);
    void foldersChanged();
    void searchFinished(quint64 generation, const zn::SearchResults& results);
    void integrityWarning(const QString& message);

private:
    void finishOpen(std::shared_ptr<LoadResult> result);
    void scheduleSave();
    void reindex(const QString& id);
    IndexDoc indexDoc(const NoteRecord& r, const NoteMeta& m) const;
    void touch(const QString& id);
    void collectRenderGarbage();
    void setBusy(bool busy, const QString& message = {});

    std::unique_ptr<Vault> vault_;
    QHash<QString, NoteRecord> notes_;
    QHash<QString, QByteArray> fileCache_;
    SearchIndex index_;
    QTimer saveTimer_;
    QThreadPool searchPool_;
    std::atomic<quint64> searchGeneration_{0};
    bool busy_ = false;
};

}  // namespace zn

Q_DECLARE_METATYPE(zn::SearchResults)
