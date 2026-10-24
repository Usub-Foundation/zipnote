#include "Crypto.h"

#include <QTest>

using namespace zn;

class TestCrypto : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(crypto::init()); }

    void sealOpenRoundTrip() {
        auto key = crypto::SecretKey::random();
        const QByteArray msg = "заметка с секретом";
        const QByteArray sealed = crypto::seal(key, msg, "ad");
        QCOMPARE(sealed.size(), qsizetype(crypto::kNonceBytes + msg.size() + crypto::kTagBytes));
        auto opened = crypto::open(key, sealed, "ad");
        QVERIFY(opened.has_value());
        QCOMPARE(*opened, msg);
    }

    void nonceIsRandom() {
        auto key = crypto::SecretKey::random();
        QVERIFY(crypto::seal(key, "x", {}) != crypto::seal(key, "x", {}));
    }

    void tamperDetected() {
        auto key = crypto::SecretKey::random();
        QByteArray sealed = crypto::seal(key, "hello", "ad");
        sealed[30] = char(sealed[30] ^ 1);
        QVERIFY(!crypto::open(key, sealed, "ad").has_value());
    }

    void wrongAdRejected() {
        auto key = crypto::SecretKey::random();
        const QByteArray sealed = crypto::seal(key, "hello", "notes/a");
        QVERIFY(!crypto::open(key, sealed, "notes/b").has_value());
    }

    void wrongKeyRejected() {
        auto a = crypto::SecretKey::random();
        auto b = crypto::SecretKey::random();
        QVERIFY(!crypto::open(b, crypto::seal(a, "hello", {}), {}).has_value());
    }

    void kdfDeterministicPerSalt() {
        auto params = crypto::KdfParams::fast();
        auto k1 = crypto::deriveKey("пароль", params);
        auto k2 = crypto::deriveKey("пароль", params);
        auto k3 = crypto::deriveKey("пароль!", params);
        QVERIFY(k1 && k2 && k3);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(k1->data()), 32),
                 QByteArray(reinterpret_cast<const char*>(k2->data()), 32));
        QVERIFY(QByteArray(reinterpret_cast<const char*>(k1->data()), 32) !=
                QByteArray(reinterpret_cast<const char*>(k3->data()), 32));
    }

    void kdfRejectsWeakParams() {
        crypto::KdfParams p{1, 1024, crypto::randomBytes(crypto::kSaltBytes)};
        QVERIFY(!crypto::deriveKey("x", p).has_value());
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "test_crypto.moc"
