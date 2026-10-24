#include "Vault.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <zip.h>

using namespace zn;

// QVERIFY2 evaluates its message eagerly; error() on an expected holding a value is undefined behaviour.
#define ZN_VERIFY_OK(e) QVERIFY2((e).has_value(), (e).has_value() ? "" : qPrintable((e).error()))

namespace {

QStringList zipEntryNames(const QString& path) {
    int err = 0;
    zip_t* z = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &err);
    QStringList out;
    if (!z)
        return out;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; ++i)
        out.append(QString::fromUtf8(zip_get_name(z, static_cast<zip_uint64_t>(i), 0)));
    zip_discard(z);
    return out;
}

QByteArray rawEntry(const QString& path, const QString& name) {
    int err = 0;
    zip_t* z = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &err);
    zip_stat_t st;
    zip_stat(z, name.toUtf8().constData(), 0, &st);
    QByteArray out(static_cast<qsizetype>(st.size), '\0');
    zip_file_t* f = zip_fopen(z, name.toUtf8().constData(), 0);
    zip_fread(f, out.data(), st.size);
    zip_fclose(f);
    zip_discard(z);
    return out;
}

void replaceEntry(const QString& path, const QString& name, const QByteArray& data) {
    int err = 0;
    zip_t* z = zip_open(QFile::encodeName(path).constData(), 0, &err);
    zip_source_t* src = zip_source_buffer(z, data.constData(), static_cast<zip_uint64_t>(data.size()), 0);
    zip_file_add(z, name.toUtf8().constData(), src, ZIP_FL_OVERWRITE);
    zip_close(z);
}

}  // namespace

class TestVault : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString path(const char* name) { return dir.filePath(QString::fromLatin1(name)); }

private slots:
    void initTestCase() {
        QVERIFY(crypto::init());
        QVERIFY(dir.isValid());
    }

    void createWriteReopen() {
        const QString p = path("a.zip");
        QString noteId;
        {
            auto v = Vault::create(p, "correct horse", crypto::KdfParams::fast());
            ZN_VERIFY_OK(v);
            QCOMPARE((*v)->manifest().folders.size(), 1);
            noteId = newId();
            NoteMeta n;
            n.id = noteId;
            n.folderId = (*v)->manifest().folders.first().id;
            n.created = n.modified = QDateTime::currentDateTimeUtc();
            (*v)->manifest().notes.append(n);
            (*v)->write(Vault::noteEntry(noteId), "# Secret plans\n\nTop secret body " + QByteArray(500, 'x'));
            (*v)->markManifestDirty();
            auto r = (*v)->commit();
            ZN_VERIFY_OK(r);
        }
        QVERIFY(Vault::looksLikeVault(p));

        auto v = Vault::open(p, "correct horse");
        ZN_VERIFY_OK(v);
        QCOMPARE((*v)->manifest().notes.size(), 1);
        auto body = (*v)->read(Vault::noteEntry(noteId));
        ZN_VERIFY_OK(body);
        QVERIFY(body->startsWith("# Secret plans"));
    }

    void nothingLeaksIntoZipDirectory() {
        const QString p = path("a.zip");
        const QStringList names = zipEntryNames(p);
        QVERIFY(names.contains("zipnotes.json"));
        QVERIFY(names.contains("manifest.znx"));
        for (const auto& n : names)
            QVERIFY2(!n.contains("Secret"), qPrintable(n));
        QFile f(p);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray all = f.readAll();
        QVERIFY(!all.contains("Secret plans"));
        QVERIFY(!all.contains("\"name\":\"Notes\""));  // folder names live in the encrypted manifest
    }

    void wrongPasswordRejected() {
        auto v = Vault::open(path("a.zip"), "wrong");
        QVERIFY(!v.has_value());
        QCOMPARE(v.error(), QStringLiteral("wrong password"));
    }

    void secondOpenIsLocked() {
        auto first = Vault::open(path("a.zip"), "correct horse");
        QVERIFY(first.has_value());
        auto second = Vault::open(path("a.zip"), "correct horse");
        QVERIFY(!second.has_value());
    }

    void tamperedEntryDetected() {
        const QString p = path("a.zip");
        QString entry;
        for (const auto& n : zipEntryNames(p))
            if (n.startsWith("notes/"))
                entry = n;
        QVERIFY(!entry.isEmpty());
        QByteArray blob = rawEntry(p, entry);
        blob[blob.size() - 3] = char(blob[blob.size() - 3] ^ 0x40);
        replaceEntry(p, entry, blob);

        auto v = Vault::open(p, "correct horse");
        QVERIFY(v.has_value());
        auto body = (*v)->read(entry);
        QVERIFY(!body.has_value());
        QVERIFY(body.error().contains("hash mismatch"));
    }

    void swappedEntryDetected() {
        // A valid blob copied under another entry name must not decrypt: the name is in the AEAD data.
        const QString p = path("b.zip");
        auto v = Vault::create(p, "pw", crypto::KdfParams::fast());
        QVERIFY(v.has_value());
        (*v)->write("notes/one.md.znx", "one");
        (*v)->write("notes/two.md.znx", "two");
        QVERIFY((*v)->commit().has_value());
        v->reset();

        replaceEntry(p, "notes/two.md.znx", rawEntry(p, "notes/one.md.znx"));
        auto again = Vault::open(p, "pw");
        QVERIFY(again.has_value());
        QVERIFY((*again)->read("notes/one.md.znx").has_value());
        QVERIFY(!(*again)->read("notes/two.md.znx").has_value());
    }

    void deleteEntry() {
        const QString p = path("c.zip");
        auto v = Vault::create(p, "pw", crypto::KdfParams::fast());
        QVERIFY(v.has_value());
        (*v)->write("notes/x.md.znx", "x");
        QVERIFY((*v)->commit().has_value());
        (*v)->remove("notes/x.md.znx");
        QVERIFY((*v)->commit().has_value());
        QVERIFY(!zipEntryNames(p).contains("notes/x.md.znx"));
        QVERIFY(!(*v)->manifest().entryHashes.contains("notes/x.md.znx"));
    }

    void changePassword() {
        const QString p = path("d.zip");
        {
            auto v = Vault::create(p, "old", crypto::KdfParams::fast());
            QVERIFY(v.has_value());
            (*v)->write("notes/x.md.znx", "keep me");
            QVERIFY((*v)->commit().has_value());
            QVERIFY((*v)->changePassword("new", crypto::KdfParams::fast()).has_value());
        }
        QVERIFY(!Vault::open(p, "old").has_value());
        auto v = Vault::open(p, "new");
        QVERIFY(v.has_value());
        QCOMPARE(*(*v)->read("notes/x.md.znx"), QByteArray("keep me"));
    }

    void headerKdfTamperBreaksUnlock() {
        const QString p = path("e.zip");
        {
            auto v = Vault::create(p, "pw", crypto::KdfParams::fast());
            QVERIFY(v.has_value());
        }
        QByteArray header = rawEntry(p, "zipnotes.json");
        // raise opslimit: derivation succeeds but the wrapped key is bound to the original parameters
        header.replace("\"opslimit\": \"1\"", "\"opslimit\": \"2\"");
        replaceEntry(p, "zipnotes.json", header);
        auto v = Vault::open(p, "pw");
        QVERIFY(!v.has_value());
    }

    void largeEntryCompressed() {
        const QString p = path("f.zip");
        auto v = Vault::create(p, "pw", crypto::KdfParams::fast());
        QVERIFY(v.has_value());
        const QByteArray big = QByteArray("lorem ipsum dolor sit amet ").repeated(4000);
        (*v)->write("notes/big.md.znx", big);
        QVERIFY((*v)->commit().has_value());
        QVERIFY(rawEntry(p, "notes/big.md.znx").size() < big.size() / 10);
        QCOMPARE(*(*v)->read("notes/big.md.znx"), big);
    }
};

QTEST_GUILESS_MAIN(TestVault)
#include "test_vault.moc"
