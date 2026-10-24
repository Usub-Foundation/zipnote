#include "Crypto.h"

#include <sodium.h>

#include <cstring>
#include <utility>

namespace zn::crypto {

bool init() {
    return sodium_init() >= 0;
}

SecretKey::SecretKey() = default;

SecretKey::~SecretKey() {
    wipe();
}

SecretKey::SecretKey(SecretKey&& other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

SecretKey& SecretKey::operator=(SecretKey&& other) noexcept {
    if (this != &other) {
        wipe();
        data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
}

void SecretKey::wipe() {
    if (data_) {
        sodium_free(data_);  // zeroes and unlocks the pages
        data_ = nullptr;
    }
}

SecretKey SecretKey::random() {
    SecretKey k;
    k.data_ = static_cast<unsigned char*>(sodium_malloc(kKeyBytes));
    if (k.data_)
        crypto_aead_xchacha20poly1305_ietf_keygen(k.data_);
    return k;
}

SecretKey SecretKey::zeroed() {
    SecretKey k;
    k.data_ = static_cast<unsigned char*>(sodium_malloc(kKeyBytes));
    if (k.data_)
        sodium_memzero(k.data_, kKeyBytes);
    return k;
}

SecretKey SecretKey::fromBytes(const unsigned char* bytes) {
    SecretKey k;
    k.data_ = static_cast<unsigned char*>(sodium_malloc(kKeyBytes));
    if (k.data_)
        std::memcpy(k.data_, bytes, kKeyBytes);
    return k;
}

KdfParams KdfParams::recommended() {
    return {crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE, randomBytes(kSaltBytes)};
}

KdfParams KdfParams::fast() {
    return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN, randomBytes(kSaltBytes)};
}

std::expected<SecretKey, QString> deriveKey(const QString& password, const KdfParams& params) {
    if (params.salt.size() != static_cast<qsizetype>(kSaltBytes))
        return std::unexpected(QStringLiteral("invalid KDF salt"));
    if (params.opsLimit < crypto_pwhash_OPSLIMIT_MIN || params.memLimit < crypto_pwhash_MEMLIMIT_MIN)
        return std::unexpected(QStringLiteral("KDF parameters below the libsodium minimum"));

    QByteArray utf8 = password.toUtf8();
    SecretKey key = SecretKey::zeroed();
    if (!key.valid())
        return std::unexpected(QStringLiteral("out of secure memory"));

    const int rc = crypto_pwhash(key.data(), kKeyBytes, utf8.constData(), static_cast<unsigned long long>(utf8.size()),
                                 reinterpret_cast<const unsigned char*>(params.salt.constData()), params.opsLimit,
                                 static_cast<size_t>(params.memLimit), crypto_pwhash_ALG_ARGON2ID13);
    sodium_memzero(utf8.data(), static_cast<size_t>(utf8.size()));
    if (rc != 0)
        return std::unexpected(QStringLiteral("key derivation failed (not enough memory?)"));
    return key;
}

QByteArray seal(const SecretKey& key, const QByteArray& plaintext, const QByteArray& ad) {
    QByteArray out(static_cast<qsizetype>(kNonceBytes + plaintext.size() + kTagBytes), Qt::Uninitialized);
    auto* nonce = reinterpret_cast<unsigned char*>(out.data());
    randombytes_buf(nonce, kNonceBytes);
    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        nonce + kNonceBytes, &clen, reinterpret_cast<const unsigned char*>(plaintext.constData()),
        static_cast<unsigned long long>(plaintext.size()), reinterpret_cast<const unsigned char*>(ad.constData()),
        static_cast<unsigned long long>(ad.size()), nullptr, nonce, key.data());
    out.resize(static_cast<qsizetype>(kNonceBytes + clen));
    return out;
}

std::expected<QByteArray, QString> open(const SecretKey& key, const QByteArray& sealed, const QByteArray& ad) {
    if (sealed.size() < static_cast<qsizetype>(kNonceBytes + kTagBytes))
        return std::unexpected(QStringLiteral("ciphertext is truncated"));
    const auto* nonce = reinterpret_cast<const unsigned char*>(sealed.constData());
    const auto clen = static_cast<unsigned long long>(sealed.size() - static_cast<qsizetype>(kNonceBytes));
    QByteArray plain(static_cast<qsizetype>(clen - kTagBytes), Qt::Uninitialized);
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plain.data()), &mlen, nullptr, nonce + kNonceBytes, clen,
            reinterpret_cast<const unsigned char*>(ad.constData()), static_cast<unsigned long long>(ad.size()), nonce,
            key.data()) != 0)
        return std::unexpected(QStringLiteral("authentication failed"));
    plain.resize(static_cast<qsizetype>(mlen));
    return plain;
}

QByteArray randomBytes(std::size_t n) {
    QByteArray out(static_cast<qsizetype>(n), Qt::Uninitialized);
    randombytes_buf(out.data(), n);
    return out;
}

QByteArray hash(const QByteArray& data) {
    QByteArray out(static_cast<qsizetype>(kHashBytes), Qt::Uninitialized);
    crypto_generichash(reinterpret_cast<unsigned char*>(out.data()), kHashBytes,
                       reinterpret_cast<const unsigned char*>(data.constData()),
                       static_cast<unsigned long long>(data.size()), nullptr, 0);
    return out;
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
    return a.size() == b.size() && sodium_memcmp(a.constData(), b.constData(), static_cast<size_t>(a.size())) == 0;
}

}  // namespace zn::crypto
