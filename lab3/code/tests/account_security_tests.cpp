#include <QJsonArray>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>

#include "domain/service_error.h"
#include "identity/identity_service.h"
#include "identity/password.h"
#include "infrastructure/database.h"

namespace {
const QString old_password = "Original-42";
const QString new_password = "Changed-43";
struct Fixture {
    ledger::Database database{":memory:"};
    qint64 now = 1000000;
    std::unique_ptr<ledger::IdentityService> service;
    Fixture() {
        database.open();
        service = std::make_unique<ledger::IdentityService>(database, [this] { return now; });
    }
    QJsonObject registerUser(const QString& name) {
        return service->dispatch("register", {{"username", name}, {"password", old_password}}, {});
    }
    QJsonObject login(const QString& name, const QString& password = old_password) {
        return service->dispatch("login", {{"username", name}, {"password", password}}, {});
    }
    QJsonObject call(const QJsonObject& session, const QString& method,
                     const QJsonObject& parameters = {}) {
        return service->dispatch(method, parameters, session.value("token").toString());
    }
};
bool fails(const std::function<void()>& operation, const QString& code) {
    try {
        operation();
    } catch (const ledger::ServiceError& error) {
        return error.code() == code;
    }
    return false;
}
QJsonObject changeFields(const QString& current = old_password) {
    return {{"current_password", current},
            {"new_password", new_password},
            {"confirm_password", new_password}};
}
QJsonObject resetFields(const QString& name, const QString& code) {
    return {{"username", name},
            {"recovery_code", code},
            {"new_password", new_password},
            {"confirm_password", new_password}};
}
}  // namespace

class AccountSecurityTests final : public QObject {
    Q_OBJECT
private slots:
    void registrationOnlyReturnsSecretOnceAndStoresHashes();
    void passwordChangeRevokesOnlyOwnersSessionsAndPreservesExpenses();
    void resetConsumesCodeRotatesItAndRequiresNormalLogin();
    void invalidInputCannotChangeCredentials();
    void throttlingDoesNotLockNormalLoginAndRecovers();
    void failuresRollBackPasswordRecoverySessionsAndEvents();
    void upgradesVersionElevenWithoutChangingOldAccounts();
};

void AccountSecurityTests::registrationOnlyReturnsSecretOnceAndStoresHashes() {
    Fixture f;
    const auto registration = f.registerUser(QStringLiteral("甲"));
    const QString code = registration.value("recovery_code").toString();
    QCOMPARE(code.size(), 39);
    const auto session = f.login(QStringLiteral("甲"));
    QVERIFY(!session.value("user").toObject().contains("recovery_code"));
    const auto status = f.call(session, "account_security");
    QVERIFY(status.value("has_recovery_code").toBool());
    QVERIFY(!status.contains("recovery_code"));
    QVERIFY(!f.call(session, "me").contains("recovery_code"));
    QSqlQuery sql(f.database.connection());
    QVERIFY(sql.exec("SELECT salt,hash,iterations FROM account_recovery"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toByteArray().size(), 16);
    QCOMPARE(sql.value(1).toByteArray().size(), 32);
    QVERIFY(ledger::verifyPassword(
        QString(code).remove('-'),
        {sql.value(0).toByteArray(), sql.value(1).toByteArray(), sql.value(2).toInt()}));
    QVERIFY(sql.value(1).toByteArray() != code.toUtf8());
    QVERIFY(sql.exec("SELECT action FROM account_security_events"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QString("recovery_generated"));
}

void AccountSecurityTests::passwordChangeRevokesOnlyOwnersSessionsAndPreservesExpenses() {
    Fixture f;
    f.registerUser("owner");
    f.registerUser("other");
    const auto owner = f.login("owner");
    const auto second = f.login("owner");
    const auto other = f.login("other");
    const auto book = f.call(owner, "create_ledger", {{"name", "Preserved"}});
    const auto expense = f.call(
        owner, "expense_submit",
        {{"ledger_id", book.value("id")},
         {"amount", "90"},
         {"date", "2026-11-14"},
         {"description", "Original"},
         {"payer_id", owner.value("user").toObject().value("id")},
         {"split_method", "equal"},
         {"splits", QJsonArray{QJsonObject{{"user_id", owner.value("user").toObject().value("id")},
                                           {"value", ""}}}}});
    auto fields = changeFields();
    fields.insert("user_id", other.value("user").toObject().value("id"));
    f.call(owner, "password_change", fields);
    QVERIFY(fails([&] { f.call(owner, "me"); }, "unauthenticated"));
    QVERIFY(fails([&] { f.call(second, "me"); }, "unauthenticated"));
    QVERIFY(!f.call(other, "me").isEmpty());
    QVERIFY(fails([&] { f.login("owner"); }, "invalid_credentials"));
    const auto restored = f.login("owner", new_password);
    QCOMPARE(restored.value("user"), owner.value("user"));
    QCOMPARE(f.call(restored, "list_ledgers").value("ledgers").toArray().size(), 1);
    const auto detail = f.call(restored, "expense_detail",
                               {{"ledger_id", book.value("id")},
                                {"expense_id", expense.value("expense").toObject().value("id")}});
    QCOMPARE(detail.value("shares").toArray().first().toObject().value("amount").toString(),
             QString("90.00"));
    QVERIFY(!f.login("other").isEmpty());
}

void AccountSecurityTests::resetConsumesCodeRotatesItAndRequiresNormalLogin() {
    Fixture f;
    const QString original = f.registerUser("owner").value("recovery_code").toString();
    const auto session = f.login("owner");
    const auto rotated = f.call(session, "recovery_generate", {{"current_password", old_password}})
                             .value("recovery_code")
                             .toString();
    QVERIFY(original != rotated);
    QVERIFY(
        fails([&] { f.service->dispatch("password_reset", resetFields("owner", original), {}); },
              "invalid_recovery"));
    const auto reset = f.service->dispatch(
        "password_reset", resetFields("OWNER", QString(rotated).remove('-').toLower()), {});
    QVERIFY(!reset.contains("token"));
    const QString next = reset.value("recovery_code").toString();
    QCOMPARE(next.size(), 39);
    QVERIFY(next != rotated);
    QVERIFY(fails([&] { f.call(session, "me"); }, "unauthenticated"));
    QVERIFY(fails([&] { f.service->dispatch("password_reset", resetFields("owner", rotated), {}); },
                  "invalid_recovery"));
    QVERIFY(fails([&] { f.login("owner"); }, "invalid_credentials"));
    QVERIFY(!f.login("owner", new_password).isEmpty());
    QVERIFY(!f.service->dispatch("password_reset", resetFields("owner", next), {}).isEmpty());
}

void AccountSecurityTests::invalidInputCannotChangeCredentials() {
    Fixture f;
    f.registerUser("owner");
    const auto session = f.login("owner");
    QVERIFY(fails([&] { f.service->dispatch("account_security", {}, {}); }, "unauthenticated"));
    QVERIFY(fails([&] { f.service->dispatch("password_change", changeFields(), {}); },
                  "unauthenticated"));
    QVERIFY(fails(
        [&] { f.service->dispatch("recovery_generate", {{"current_password", old_password}}, {}); },
        "unauthenticated"));
    QVERIFY(fails([&] { f.call(session, "password_change", changeFields("Wrong-42")); },
                  "invalid_credentials"));
    auto fields = changeFields();
    fields.insert("confirm_password", "Mismatch-42");
    QVERIFY(fails([&] { f.call(session, "password_change", fields); }, "validation"));
    fields = changeFields();
    fields.insert("new_password", "12345678");
    fields.insert("confirm_password", "12345678");
    QVERIFY(fails([&] { f.call(session, "password_change", fields); }, "validation"));
    fields = changeFields();
    fields.insert("new_password", old_password);
    fields.insert("confirm_password", old_password);
    QVERIFY(fails([&] { f.call(session, "password_change", fields); }, "validation"));
    QVERIFY(!f.login("owner").isEmpty());
    // Current credentials from older password policies remain usable to set a new valid password.
    const QString legacy(40, '7');
    const auto hash = ledger::hashPassword(legacy);
    QSqlQuery sql(f.database.connection());
    sql.prepare("UPDATE users SET password_salt=?,password_hash=?,password_iterations=?");
    sql.addBindValue(hash.salt);
    sql.addBindValue(hash.hash);
    sql.addBindValue(hash.iterations);
    QVERIFY(sql.exec());
    f.call(session, "password_change", changeFields(legacy));
    QVERIFY(!f.login("owner", new_password).isEmpty());
}

void AccountSecurityTests::throttlingDoesNotLockNormalLoginAndRecovers() {
    Fixture f;
    const auto registration = f.registerUser("owner");
    for (int i = 0; i < 5; ++i)
        QVERIFY(
            fails([&] { f.service->dispatch("password_reset", resetFields("owner", "BAD"), {}); },
                  "invalid_recovery"));
    QVERIFY(fails(
        [&] {
            f.service->dispatch(
                "password_reset",
                resetFields("owner", registration.value("recovery_code").toString()), {});
        },
        "rate_limited"));
    QVERIFY(!f.login("owner").isEmpty());
    QVERIFY(fails([&] { f.service->dispatch("password_reset", resetFields("unknown", "BAD"), {}); },
                  "invalid_recovery"));
    f.now += 31;
    QVERIFY(!f.service
                 ->dispatch("password_reset",
                            resetFields("owner", registration.value("recovery_code").toString()),
                            {})
                 .isEmpty());
    const auto session = f.login("owner", new_password);
    for (int i = 0; i < 5; ++i)
        QVERIFY(
            fails([&] { f.call(session, "recovery_generate", {{"current_password", "Wrong-42"}}); },
                  "invalid_credentials"));
    QVERIFY(
        fails([&] { f.call(session, "recovery_generate", {{"current_password", new_password}}); },
              "rate_limited"));
    QVERIFY(!f.call(session, "me").isEmpty());
    f.now += 31;
    QVERIFY(!f.call(session, "recovery_generate", {{"current_password", new_password}}).isEmpty());
}

void AccountSecurityTests::failuresRollBackPasswordRecoverySessionsAndEvents() {
    Fixture f;
    const auto registration = f.registerUser("owner");
    const auto session = f.login("owner");
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_security BEFORE INSERT ON account_security_events BEGIN "
                 "SELECT RAISE(ABORT,'fixture'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             f.call(session, "password_change", changeFields()));
    QVERIFY_THROWS_EXCEPTION(
        std::runtime_error,
        f.service->dispatch("password_reset",
                            resetFields("owner", registration.value("recovery_code").toString()),
                            {}));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.registerUser("not_created"));
    QVERIFY(!f.call(session, "me").isEmpty());
    QVERIFY(!f.login("owner").isEmpty());
    QVERIFY(sql.exec("SELECT COUNT(*) FROM users"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 1);
    QVERIFY(sql.exec("SELECT COUNT(*) FROM account_security_events"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 1);
    QVERIFY(sql.exec("DROP TRIGGER fail_security"));
    QVERIFY(!f.service
                 ->dispatch("password_reset",
                            resetFields("owner", registration.value("recovery_code").toString()),
                            {})
                 .isEmpty());
}

void AccountSecurityTests::upgradesVersionElevenWithoutChangingOldAccounts() {
    QTemporaryDir directory;
    const QString path = directory.filePath("old.sqlite3");
    QString owner_id;
    {
        ledger::Database db(path);
        db.open();
        ledger::IdentityService service(db);
        const auto registration =
            service.dispatch("register", {{"username", "legacy"}, {"password", old_password}}, {});
        owner_id = registration.value("id").toString();
        const auto session =
            service.dispatch("login", {{"username", "legacy"}, {"password", old_password}}, {});
        service.dispatch("create_ledger", {{"name", "Keep history"}},
                         session.value("token").toString());
        QSqlQuery sql(db.connection());
        QVERIFY(sql.exec("DROP TABLE account_recovery"));
        QVERIFY(sql.exec("DROP TABLE account_security_events"));
        QVERIFY(sql.exec("PRAGMA user_version=11"));
    }
    ledger::Database db(path);
    db.open();
    QCOMPARE(db.schemaVersion(), 12);
    ledger::IdentityService service(db);
    const auto session =
        service.dispatch("login", {{"username", "legacy"}, {"password", old_password}}, {});
    QCOMPARE(session.value("user").toObject().value("id").toString(), owner_id);
    const QString token = session.value("token").toString();
    QVERIFY(!service.dispatch("account_security", {}, token).value("has_recovery_code").toBool());
    QCOMPARE(service.dispatch("list_ledgers", {}, token).value("ledgers").toArray().size(), 1);
    QVERIFY(fails([&] { service.dispatch("password_reset", resetFields("legacy", "BAD"), {}); },
                  "invalid_recovery"));
    const auto generated =
        service.dispatch("recovery_generate", {{"current_password", old_password}}, token);
    QVERIFY(!service
                 .dispatch("password_reset",
                           resetFields("legacy", generated.value("recovery_code").toString()), {})
                 .isEmpty());
}

QTEST_GUILESS_MAIN(AccountSecurityTests)
#include "account_security_tests.moc"
