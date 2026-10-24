#pragma once

#include "Crypto.h"
#include "Manifest.h"

#include <QByteArray>
#include <QHash>
#include <QLockFile>
#include <QString>
#include <QStringList>

#include <expected>
#include <map>
#include <memory>
#include <optional>

namespace zn {

// An encrypted notes vault stored as a single .zip file.
//
//   zipnotes.json          plaintext header: format, vault id, Argon2id parameters, wrapped data key
//   manifest.znx           encrypted: folders, note metadata, attachment metadata, hashes of all entries
//   notes/<uuid>.md.znx    encrypted Markdown, one entry per note
//   files/<uuid>.znx       encrypted attachments (Excalidraw scenes, images, rendered previews)
//
// Entry names are random UUIDs, so titles and folder names never appear in the zip directory.
// Every blob is XChaCha20-Poly1305 with the entry name in the associated data, so blobs cannot be moved
// between entries, and the manifest pins each blob by BLAKE2b hash, so a single entry cannot be rolled back.
class Vault {
public:
    static constexpr const char* kHeaderEntry = "zipnotes.json";
    static constexpr const char* kManifestEntry = "manifest.znx";

    static std::expected<std::unique_ptr<Vault>, QString> create(const QString& path, const QString& password,
                                                                 const crypto::KdfParams& kdf);
    static std::expected<std::unique_ptr<Vault>, QString> open(const QString& path, const QString& password);
    static bool looksLikeVault(const QString& path);

    ~Vault();
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    const QString& path() const { return path_; }
    QString displayName() const;

    Manifest& manifest() { return manifest_; }
    const Manifest& manifest() const { return manifest_; }

    std::expected<QByteArray, QString> read(const QString& entry) const;
    // Reads several entries with one pass over the archive. Missing or corrupt entries map to an error.
    QHash<QString, std::expected<QByteArray, QString>> readMany(const QStringList& entries) const;

    void write(const QString& entry, const QByteArray& plaintext);  // staged until commit()
    void remove(const QString& entry);
    bool hasPendingChanges() const { return !staged_.empty() || manifestDirty_; }
    void markManifestDirty() { manifestDirty_ = true; }

    // Writes staged entries and a new manifest. libzip writes a temporary file and renames it over the
    // vault, so a crash mid-save leaves the previous version intact.
    std::expected<void, QString> commit();
    std::expected<void, QString> changePassword(const QString& newPassword, const crypto::KdfParams& kdf);
    // Same with the key-encryption key already derived (on a worker thread) from the new password.
    std::expected<void, QString> rewrapKey(const crypto::SecretKey& kek, const crypto::KdfParams& kdf);
    bool checkPassword(const QString& password) const;  // runs the KDF: slow on purpose

    // Drops the key and all staged plaintext. The object is unusable afterwards.
    void lock();
    bool isLocked() const { return !key_.valid(); }

    static QString noteEntry(const QString& noteId) { return QStringLiteral("notes/%1.md.znx").arg(noteId); }
    static QString fileEntry(const QString& fileId) { return QStringLiteral("files/%1.znx").arg(fileId); }

private:
    Vault() = default;

    QByteArray entryAd(const QString& entry, quint8 flags) const;
    QByteArray sealEntry(const QString& entry, const QByteArray& plaintext) const;
    std::expected<QByteArray, QString> openEntry(const QString& entry, const QByteArray& blob) const;
    QByteArray headerJson() const;
    QByteArray keyWrapAd() const;

    QString path_;
    QByteArray vaultId_;
    crypto::KdfParams kdf_;
    QByteArray wrappedKey_;
    crypto::SecretKey key_;
    Manifest manifest_;
    bool manifestDirty_ = false;
    bool headerDirty_ = false;
    std::map<QString, std::optional<QByteArray>> staged_;  // sealed blobs; nullopt = delete
    std::unique_ptr<QLockFile> lockFile_;
};

}  // namespace zn
