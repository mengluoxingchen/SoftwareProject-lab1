#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include "domain/service_error.h"
#include "recovery/recovery_store.h"
class RecoveryTests final : public QObject {
    Q_OBJECT
private slots:
    void encryptedRoundtripAndIndependentWindows() {
        QTemporaryDir dir;
        ledger::RecoveryStore store(dir.path(), "alice", "book");
        const auto id = store.newId(), other = store.newId();
        auto lease = store.claim(id), peer = store.claim(other);
        QVERIFY(lease && peer);
        QVERIFY(!store.claim(id));
        const QJsonObject input{
            {"description", "Sensitive 37.25"}, {"amount", ""}, {"rows", QJsonArray{}}};
        store.write(id, input, *lease);
        QCOMPARE(store.read(id).value("snapshot").toObject(), input);
        QFile file(store.path(id));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("Sensitive 37.25"));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, store.write(id, input, *peer));
        lease.reset();
        QVERIFY(store.claim(id));
        QCOMPARE(store.entries().size(), 1);
    }
    void accountLedgerAndDirectoryBoundaries() {
        QTemporaryDir dir, other;
        ledger::RecoveryStore a(dir.path(), "alice", "book"), b(dir.path(), "bob", "book"),
            c(dir.path(), "alice", "other"), d(other.path(), "alice", "book");
        auto id = a.newId();
        auto lease = a.claim(id);
        a.write(id, {{"amount", "5"}}, *lease);
        QCOMPARE(b.entries().size(), 0);
        QCOMPARE(c.entries().size(), 0);
        QCOMPARE(d.entries().size(), 0);
        auto target = b.claim(id);
        QVERIFY(QFile::copy(a.path(id), b.path(id)));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, b.read(id));
        QVERIFY(QFile::exists(b.path(id)));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, a.path("../outside"));
    }
    void corruptFileIsRetained() {
        QTemporaryDir dir;
        ledger::RecoveryStore store(dir.path(), "alice", "book");
        auto id = store.newId();
        auto lease = store.claim(id);
        store.write(id, {{"amount", "5"}}, *lease);
        QFile file(store.path(id));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("broken"), qint64(6));
        file.close();
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, store.read(id));
        QCOMPARE(store.entries().size(), 1);
        store.remove(id, *lease);
        QVERIFY(store.entries().isEmpty());
    }
    void rejectedWritesKeepPreviousCopy() {
        QTemporaryDir dir;
        ledger::RecoveryStore store(dir.path(), "alice", "book");
        auto id = store.newId();
        auto lease = store.claim(id);
        store.write(id, {{"amount", "5"}}, *lease);
        for (const auto& key : QStringList{"password", "token", "lock_token"}) {
            QVERIFY_THROWS_EXCEPTION(
                ledger::ServiceError,
                store.write(id, {{"nested", QJsonObject{{key, "secret"}}}}, *lease));
        }
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 store.write(id, {{"description", QString(600000, 'x')}}, *lease));
        QCOMPARE(store.read(id).value("snapshot").toObject().value("amount").toString(),
                 QString("5"));
    }
};
QTEST_GUILESS_MAIN(RecoveryTests)
#include "recovery_tests.moc"
