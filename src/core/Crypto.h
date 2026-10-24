#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace zn::crypto {

// libsodium must be initialised once per process before any other call.
bool init();

constexpr std::size_t kKeyBytes = 32;    // XChaCha20-Poly1305 key
constexpr std::size_t kNonceBytes = 24;  // XChaCha20 nonce: random nonces are safe at this size
constexpr std::size_t kTagBytes = 16;    // Poly1305 tag
constexpr std::size_t kSaltBytes = 16;   // Argon2id salt
constexpr std::size_t kHashBytes = 32;   // BLAKE2b-256

// A 32-byte key in guarded, mlock()ed memory that is wiped on destruction.
class SecretKey {
public:
    SecretKey();
    ~SecretKey();
    SecretKey(SecretKey&& other) noexcept;
    SecretKey& operator=(SecretKey&& other) noexcept;
    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;

    static SecretKey random();
    static SecretKey zeroed();
    static SecretKey fromBytes(const unsigned char* bytes);

    bool valid() const { return data_ != nullptr; }
    const unsigned char* data() const { return data_; }
    unsigned char* data() { return data_; }
    void wipe();

private:
    unsigned char* data_ = nullptr;
};

// Argon2id parameters, stored in the vault header so they can be raised later without breaking old vaults.
struct KdfParams {
    std::uint64_t opsLimit = 0;
    std::uint64_t memLimit = 0;
    QByteArray salt;

    static KdfParams recommended();  // libsodium MODERATE: 3 passes, 256 MiB
    static KdfParams fast();         // for tests only
};

std::expected<SecretKey, QString> deriveKey(const QString& password, const KdfParams& params);

// Output layout: nonce(24) || ciphertext || tag(16). `ad` is authenticated but not encrypted.
QByteArray seal(const SecretKey& key, const QByteArray& plaintext, const QByteArray& ad);
std::expected<QByteArray, QString> open(const SecretKey& key, const QByteArray& sealed, const QByteArray& ad);

QByteArray randomBytes(std::size_t n);
QByteArray hash(const QByteArray& data);  // BLAKE2b-256
bool constantTimeEquals(const QByteArray& a, const QByteArray& b);

}  // namespace zn::crypto
