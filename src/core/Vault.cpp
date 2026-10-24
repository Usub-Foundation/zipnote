#include "Vault.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <zip.h>

#include <ctime>
#include <vector>

namespace zn {

namespace {

constexpr char kFormat[] = "zipnotes-vault";
constexpr int kFormatVersion = 1;
constexpr char kBlobMagic[] = "ZNE1";
constexpr quint8 kFlagCompressed = 0x01;
// Every entry gets the same timestamp so the zip directory does not reveal when individual notes were edited.
// DOS time cannot go below 1980; noon avoids underflow in negative UTC offsets.
constexpr std::time_t kFixedMtime = 315576000;  // 1980-01-01 12:00:00 UTC

QByteArray zipPath(const QString& path) {
#ifdef Q_OS_WIN
    return path.toUtf8();
#else
    return QFile::encodeName(path);
#endif
}

QString zipErrorString(int code) {
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    QString s = QString::fromUtf8(zip_error_strerror(&error));
    zip_error_fini(&error);
    return s;
}

struct ZipCloser {
    void operator()(zip_t* z) const {
        if (z)
            zip_discard(z);
    }
};
using ZipHandle = std::unique_ptr<zip_t, ZipCloser>;

std::expected<ZipHandle, QString> openZip(const QString& path, int flags) {
    int err = 0;
    zip_t* z = zip_open(zipPath(path).constData(), flags, &err);
    if (!z)
        return std::unexpected(QStringLiteral("cannot open %1: %2").arg(path, zipErrorString(err)));
    return ZipHandle(z);
}

std::expected<QByteArray, QString> readRaw(zip_t* z, const QString& name) {
    const QByteArray n = name.toUtf8();
    const zip_int64_t idx = zip_name_locate(z, n.constData(), ZIP_FL_ENC_UTF_8);
    if (idx < 0)
        return std::unexpected(QStringLiteral("entry %1 is missing").arg(name));
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(z, static_cast<zip_uint64_t>(idx), 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE))
        return std::unexpected(QStringLiteral("cannot stat %1").arg(name));
    if (st.size > (zip_uint64_t(1) << 31))
        return std::unexpected(QStringLiteral("entry %1 is too large").arg(name));
    zip_file_t* f = zip_fopen_index(z, static_cast<zip_uint64_t>(idx), 0);
    if (!f)
        return std::unexpected(QStringLiteral("cannot open %1: %2").arg(name, QString::fromUtf8(zip_strerror(z))));
    QByteArray data(static_cast<qsizetype>(st.size), Qt::Uninitialized);
    const zip_int64_t got = zip_fread(f, data.data(), st.size);
    zip_fclose(f);
    if (got != static_cast<zip_int64_t>(st.size))
        return std::unexpected(QStringLiteral("short read on %1").arg(name));
    return data;
}

// The buffer must stay alive until zip_close(): libzip reads it lazily.
std::expected<void, QString> addBuffer(zip_t* z, const QString& name, const QByteArray& data) {
    zip_source_t* src = zip_source_buffer(z, data.constData(), static_cast<zip_uint64_t>(data.size()), 0);
    if (!src)
        return std::unexpected(QString::fromUtf8(zip_strerror(z)));
    const QByteArray n = name.toUtf8();
    const zip_int64_t idx = zip_file_add(z, n.constData(), src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (idx < 0) {
        zip_source_free(src);
        return std::unexpected(QStringLiteral("cannot add %1: %2").arg(name, QString::fromUtf8(zip_strerror(z))));
    }
    // Ciphertext does not compress; entries are compressed before encryption instead.
    zip_set_file_compression(z, static_cast<zip_uint64_t>(idx), ZIP_CM_STORE, 0);
    zip_file_set_mtime(z, static_cast<zip_uint64_t>(idx), kFixedMtime, 0);
    return {};
}

}  // namespace

Vault::~Vault() {
    lock();
}

QString Vault::displayName() const {
    return QFileInfo(path_).completeBaseName();
}

QByteArray Vault::keyWrapAd() const {
    // Binds the wrapped key to the header fields: editing the KDF parameters in zipnotes.json breaks unlocking
    // instead of silently weakening the vault.
    return QByteArrayLiteral("zipnotes/1/keywrap|") + vaultId_ + '|' + QByteArray::number(kdf_.opsLimit) + '|' +
           QByteArray::number(kdf_.memLimit) + '|' + kdf_.salt;
}

QByteArray Vault::entryAd(const QString& entry, quint8 flags) const {
    return QByteArrayLiteral("zipnotes/1/entry|") + vaultId_ + '|' + entry.toUtf8() + '|' + char(flags);
}

QByteArray Vault::sealEntry(const QString& entry, const QByteArray& plaintext) const {
    quint8 flags = 0;
    QByteArray payload = plaintext;
    if (plaintext.size() >= 128) {
        QByteArray c = qCompress(plaintext, 6);
        if (c.size() + 16 < plaintext.size()) {
            payload = std::move(c);
            flags |= kFlagCompressed;
        }
    }
    QByteArray blob(kBlobMagic, 4);
    blob.append(char(flags));
    blob.append(crypto::seal(key_, payload, entryAd(entry, flags)));
    return blob;
}

std::expected<QByteArray, QString> Vault::openEntry(const QString& entry, const QByteArray& blob) const {
    if (blob.size() < 5 || !blob.startsWith(kBlobMagic))
        return std::unexpected(QStringLiteral("%1 is not a ZipNotes entry").arg(entry));
    const auto flags = static_cast<quint8>(blob.at(4));
    auto plain = crypto::open(key_, blob.mid(5), entryAd(entry, flags));
    if (!plain)
        return std::unexpected(QStringLiteral("%1: %2").arg(entry, plain.error()));
    if (flags & kFlagCompressed) {
        QByteArray out = qUncompress(*plain);
        if (out.isEmpty())
            return std::unexpected(QStringLiteral("%1: decompression failed").arg(entry));
        return out;
    }
    return std::move(*plain);
}

QByteArray Vault::headerJson() const {
    QJsonObject kdf{{"alg", "argon2id13"},
                    {"opslimit", QString::number(kdf_.opsLimit)},
                    {"memlimit", QString::number(kdf_.memLimit)},
                    {"salt", QString::fromLatin1(kdf_.salt.toBase64())}};
    QJsonObject root{{"format", kFormat},
                     {"version", kFormatVersion},
                     {"vault_id", QString::fromLatin1(vaultId_.toBase64())},
                     {"kdf", kdf},
                     {"aead", "xchacha20poly1305-ietf"},
                     {"wrapped_key", QString::fromLatin1(wrappedKey_.toBase64())},
                     {"about", "Encrypted notes vault. Open with ZipNotes."}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::expected<std::unique_ptr<Vault>, QString> Vault::create(const QString& path, const QString& password,
                                                             const crypto::KdfParams& kdf) {
    if (QFileInfo::exists(path))
        return std::unexpected(QStringLiteral("%1 already exists").arg(path));
    if (password.isEmpty())
        return std::unexpected(QStringLiteral("password must not be empty"));

    std::unique_ptr<Vault> v(new Vault);
    v->path_ = QFileInfo(path).absoluteFilePath();
    v->vaultId_ = crypto::randomBytes(16);
    v->kdf_ = kdf;
    auto kek = crypto::deriveKey(password, kdf);
    if (!kek)
        return std::unexpected(kek.error());
    v->key_ = crypto::SecretKey::random();
    QByteArray dek(reinterpret_cast<const char*>(v->key_.data()), crypto::kKeyBytes);
    v->wrappedKey_ = crypto::seal(*kek, dek, v->keyWrapAd());
    dek.fill('\0');

    v->manifest_.folders.append({newId(), QStringLiteral("Notes"), {}, 0});
    v->headerDirty_ = true;
    v->manifestDirty_ = true;

    v->lockFile_ = std::make_unique<QLockFile>(v->path_ + QStringLiteral(".lock"));
    if (!v->lockFile_->tryLock(0))
        return std::unexpected(QStringLiteral("the vault is locked by another process"));
    if (auto r = v->commit(); !r)
        return std::unexpected(r.error());
    return v;
}

bool Vault::looksLikeVault(const QString& path) {
    auto z = openZip(path, ZIP_RDONLY);
    if (!z)
        return false;
    auto header = readRaw(z->get(), QString::fromLatin1(kHeaderEntry));
    if (!header)
        return false;
    return QJsonDocument::fromJson(*header).object()["format"].toString() == QLatin1String(kFormat);
}

std::expected<std::unique_ptr<Vault>, QString> Vault::open(const QString& path, const QString& password) {
    std::unique_ptr<Vault> v(new Vault);
    v->path_ = QFileInfo(path).absoluteFilePath();
    v->lockFile_ = std::make_unique<QLockFile>(v->path_ + QStringLiteral(".lock"));
    if (!v->lockFile_->tryLock(0))
        return std::unexpected(QStringLiteral("the vault is already open in another window or process"));

    auto z = openZip(v->path_, ZIP_RDONLY);
    if (!z)
        return std::unexpected(z.error());
    auto header = readRaw(z->get(), QString::fromLatin1(kHeaderEntry));
    if (!header)
        return std::unexpected(QStringLiteral("not a ZipNotes vault (%1)").arg(header.error()));

    const QJsonObject h = QJsonDocument::fromJson(*header).object();
    if (h["format"].toString() != QLatin1String(kFormat))
        return std::unexpected(QStringLiteral("not a ZipNotes vault"));
    if (h["version"].toInt() > kFormatVersion)
        return std::unexpected(QStringLiteral("vault format %1 is newer than this app").arg(h["version"].toInt()));
    const QJsonObject kdf = h["kdf"].toObject();
    if (kdf["alg"].toString() != QLatin1String("argon2id13"))
        return std::unexpected(QStringLiteral("unsupported key derivation %1").arg(kdf["alg"].toString()));
    v->vaultId_ = QByteArray::fromBase64(h["vault_id"].toString().toLatin1());
    v->kdf_.opsLimit = kdf["opslimit"].toString().toULongLong();
    v->kdf_.memLimit = kdf["memlimit"].toString().toULongLong();
    v->kdf_.salt = QByteArray::fromBase64(kdf["salt"].toString().toLatin1());
    v->wrappedKey_ = QByteArray::fromBase64(h["wrapped_key"].toString().toLatin1());

    auto kek = crypto::deriveKey(password, v->kdf_);
    if (!kek)
        return std::unexpected(kek.error());
    auto dek = crypto::open(*kek, v->wrappedKey_, v->keyWrapAd());
    if (!dek || dek->size() != static_cast<qsizetype>(crypto::kKeyBytes))
        return std::unexpected(QStringLiteral("wrong password"));
    v->key_ = crypto::SecretKey::fromBytes(reinterpret_cast<const unsigned char*>(dek->constData()));
    dek->fill('\0');

    auto blob = readRaw(z->get(), QString::fromLatin1(kManifestEntry));
    if (!blob)
        return std::unexpected(blob.error());
    auto json = v->openEntry(QString::fromLatin1(kManifestEntry), *blob);
    if (!json)
        return std::unexpected(QStringLiteral("manifest is damaged: %1").arg(json.error()));
    auto manifest = Manifest::fromJson(*json);
    if (!manifest)
        return std::unexpected(manifest.error());
    v->manifest_ = std::move(*manifest);
    return v;
}

std::expected<QByteArray, QString> Vault::read(const QString& entry) const {
    auto many = readMany({entry});
    return many.value(entry, std::unexpected(QStringLiteral("entry %1 is missing").arg(entry)));
}

QHash<QString, std::expected<QByteArray, QString>> Vault::readMany(const QStringList& entries) const {
    QHash<QString, std::expected<QByteArray, QString>> out;
    if (isLocked()) {
        for (const auto& e : entries)
            out.insert(e, std::unexpected(QStringLiteral("vault is locked")));
        return out;
    }
    std::expected<ZipHandle, QString> z = std::unexpected(QString());
    for (const auto& entry : entries) {
        if (auto it = staged_.find(entry); it != staged_.end()) {
            if (it->second)
                out.insert(entry, openEntry(entry, *it->second));
            else
                out.insert(entry, std::unexpected(QStringLiteral("entry %1 was deleted").arg(entry)));
            continue;
        }
        const auto expected = manifest_.entryHashes.constFind(entry);
        if (expected == manifest_.entryHashes.cend()) {
            out.insert(entry, std::unexpected(QStringLiteral("entry %1 is not listed in the manifest").arg(entry)));
            continue;
        }
        if (!z) {
            z = openZip(path_, ZIP_RDONLY);
            if (!z) {
                out.insert(entry, std::unexpected(z.error()));
                continue;
            }
        }
        auto blob = readRaw(z->get(), entry);
        if (!blob) {
            out.insert(entry, std::unexpected(blob.error()));
            continue;
        }
        if (!crypto::constantTimeEquals(crypto::hash(*blob), *expected)) {
            out.insert(entry, std::unexpected(
                                  QStringLiteral("%1 was changed outside ZipNotes (hash mismatch)").arg(entry)));
            continue;
        }
        out.insert(entry, openEntry(entry, *blob));
    }
    return out;
}

void Vault::write(const QString& entry, const QByteArray& plaintext) {
    if (isLocked())
        return;
    staged_[entry] = sealEntry(entry, plaintext);
}

void Vault::remove(const QString& entry) {
    staged_[entry] = std::nullopt;
}

std::expected<void, QString> Vault::commit() {
    if (isLocked())
        return std::unexpected(QStringLiteral("vault is locked"));
    if (!hasPendingChanges() && !headerDirty_)
        return {};

    Manifest next = manifest_;
    next.revision += 1;
    for (const auto& [name, blob] : staged_) {
        if (blob)
            next.entryHashes.insert(name, crypto::hash(*blob));
        else
            next.entryHashes.remove(name);
    }
    const QByteArray manifestBlob = sealEntry(QString::fromLatin1(kManifestEntry), next.toJson());
    const QByteArray header = headerJson();

    auto z = openZip(path_, ZIP_CREATE);
    if (!z)
        return std::unexpected(z.error());
    if (headerDirty_)
        if (auto r = addBuffer(z->get(), QString::fromLatin1(kHeaderEntry), header); !r)
            return r;
    for (const auto& [name, blob] : staged_) {
        if (blob) {
            if (auto r = addBuffer(z->get(), name, *blob); !r)
                return r;
        } else {
            const QByteArray n = name.toUtf8();
            const zip_int64_t idx = zip_name_locate(z->get(), n.constData(), ZIP_FL_ENC_UTF_8);
            if (idx >= 0)
                zip_delete(z->get(), static_cast<zip_uint64_t>(idx));
        }
    }
    if (auto r = addBuffer(z->get(), QString::fromLatin1(kManifestEntry), manifestBlob); !r)
        return r;

    zip_t* raw = z->release();
    if (zip_close(raw) != 0) {
        const QString err = QString::fromUtf8(zip_strerror(raw));
        zip_discard(raw);
        return std::unexpected(QStringLiteral("cannot save %1: %2").arg(path_, err));
    }
    manifest_ = std::move(next);
    staged_.clear();
    manifestDirty_ = false;
    headerDirty_ = false;
    return {};
}

std::expected<void, QString> Vault::changePassword(const QString& newPassword, const crypto::KdfParams& kdf) {
    if (isLocked())
        return std::unexpected(QStringLiteral("vault is locked"));
    if (newPassword.isEmpty())
        return std::unexpected(QStringLiteral("password must not be empty"));
    auto kek = crypto::deriveKey(newPassword, kdf);
    if (!kek)
        return std::unexpected(kek.error());
    return rewrapKey(*kek, kdf);
}

std::expected<void, QString> Vault::rewrapKey(const crypto::SecretKey& kek, const crypto::KdfParams& kdf) {
    if (isLocked() || !kek.valid())
        return std::unexpected(QStringLiteral("vault is locked"));
    const auto oldKdf = kdf_;
    const auto oldWrapped = wrappedKey_;
    kdf_ = kdf;
    QByteArray dek(reinterpret_cast<const char*>(key_.data()), crypto::kKeyBytes);
    wrappedKey_ = crypto::seal(kek, dek, keyWrapAd());
    dek.fill('\0');
    headerDirty_ = true;
    if (auto r = commit(); !r) {
        kdf_ = oldKdf;
        wrappedKey_ = oldWrapped;
        headerDirty_ = false;
        return r;
    }
    return {};
}

bool Vault::checkPassword(const QString& password) const {
    auto kek = crypto::deriveKey(password, kdf_);
    if (!kek)
        return false;
    auto dek = crypto::open(*kek, wrappedKey_, keyWrapAd());
    if (!dek)
        return false;
    dek->fill('\0');
    return true;
}

void Vault::lock() {
    key_.wipe();
    staged_.clear();
    manifest_ = Manifest{};
    lockFile_.reset();
}

}  // namespace zn
