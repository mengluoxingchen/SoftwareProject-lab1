#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>
#include <functional>
#include <memory>

#include "domain/service_error.h"
#include "identity/identity_service.h"
#include "identity/password.h"
#include "infrastructure/database.h"

namespace {
const QString kPassword = QStringLiteral("Test-password-42");
struct Fixture {
    ledger::Database database{QStringLiteral(":memory:")};
    qint64 now = 1000000;
    std::unique_ptr<ledger::IdentityService> service;
    Fixture() {
        database.open();
        service = std::make_unique<ledger::IdentityService>(database, [this] { return now; });
    }
    QJsonObject account(const QString& name) {
        service->dispatch(QStringLiteral("register"), {{"username", name}, {"password", kPassword}},
                          {});
        return service->dispatch(QStringLiteral("login"),
                                 {{"username", name}, {"password", kPassword}}, {});
    }
    QJsonObject call(const QJsonObject& actor, const QString& method,
                     const QJsonObject& parameters = {}) {
        return service->dispatch(method, parameters, actor.value("token").toString());
    }
    int count(const QString& table) {
        QSqlQuery query(database.connection());
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !query.next()) {
            throw std::runtime_error("Cannot count test records.");
        }
        return query.value(0).toInt();
    }
};
bool denied(const std::function<void()>& action, const QString& expected) {
    try {
        action();
    } catch (const ledger::ServiceError& error) {
        return error.code() == expected;
    }
    return false;
}
}  // namespace

class IdentityTests final : public QObject {
    Q_OBJECT
private slots:
    void credentialsAreHashedAndUsernamesAreUnique();
    void usernamesAcceptSingleCharactersAndEnforceBoundaries();
    void registrationRejectsWeakPasswordsAndAcceptsLengthBoundaries();
    void existingPasswordsRemainValidAfterPolicyChange();
    void creatorBecomesAdminAndLedgersAreIsolated();
    void invitationRequiresItsRecipientAndAcceptance();
    void rolesAreScopedToLedgerAndRechecked();
    void sessionsExpireAndLogoutRevokesThem();
    void failedLoginIsThrottledWithoutBlockingForever();
    void ledgerAndAuditWritesAreAtomic();
};

void IdentityTests::credentialsAreHashedAndUsernamesAreUnique() {
    Fixture f;
    const auto alice = f.account(QStringLiteral("Alice"));
    f.account(QStringLiteral("Bob"));
    auto login = f.service->dispatch(QStringLiteral("login"),
                                     {{"username", "ALICE"}, {"password", kPassword}}, {});
    QCOMPARE(login.value("user").toObject(), alice.value("user").toObject());
    QVERIFY(denied(
        [&] {
            f.service->dispatch("register", {{"username", "alice"}, {"password", kPassword}}, {});
        },
        "conflict"));
    QVERIFY(denied(
        [&] {
            f.service->dispatch("login", {{"username", "Alice"}, {"password", "wrong-pass"}}, {});
        },
        "invalid_credentials"));
    QVERIFY(denied(
        [&] {
            f.service->dispatch("register", {{"username", "valid"}, {"password", "short"}}, {});
        },
        "validation"));
    QSqlQuery rows(f.database.connection());
    QVERIFY(rows.exec(QStringLiteral(
        "SELECT password_salt,password_hash,password_iterations FROM users ORDER BY username")));
    QVERIFY(rows.next());
    const QByteArray first_salt = rows.value(0).toByteArray();
    const QByteArray first_hash = rows.value(1).toByteArray();
    QCOMPARE(first_salt.size(), 16);
    QCOMPARE(first_hash.size(), 32);
    QCOMPARE(rows.value(2).toInt(), 600000);
    QVERIFY(first_hash != kPassword.toUtf8());
    QVERIFY(rows.next());
    QVERIFY(first_salt != rows.value(0).toByteArray());
    QVERIFY(first_hash != rows.value(1).toByteArray());
}

void IdentityTests::usernamesAcceptSingleCharactersAndEnforceBoundaries() {
    Fixture f;
    for (const QString& name : {QStringLiteral("甲"), QStringLiteral("A"), QStringLiteral("7"),
                                QString(32, QChar(0x4e59))}) {
        const auto login = f.account(name);
        QCOMPARE(login.value("user").toObject().value("username").toString(), name);
        QVERIFY(!login.value("token").toString().isEmpty());
    }
    for (const QString& name : {QStringLiteral(""), QStringLiteral(" "), QStringLiteral("甲 乙"),
                                QStringLiteral("甲!"), QString(33, QChar(0x4e59))}) {
        QVERIFY(denied(
            [&] {
                f.service->dispatch("register", {{"username", name}, {"password", kPassword}}, {});
            },
            "validation"));
    }
    QVERIFY(denied(
        [&] { f.service->dispatch("register", {{"username", "a"}, {"password", kPassword}}, {}); },
        "conflict"));
    QCOMPARE(f.count("users"), 4);
}

void IdentityTests::registrationRejectsWeakPasswordsAndAcceptsLengthBoundaries() {
    Fixture f;
    const QStringList invalid_passwords{
        QStringLiteral("12345678"),  QStringLiteral("abcdefgh"),
        QStringLiteral("Abc1234"),   QStringLiteral("Aa") + QString(31, '1'),
        QStringLiteral("Abc 12345"), QStringLiteral("中文Ab1234"),
        QStringLiteral("Abc1234\n"), QStringLiteral("!!!!!!!!")};
    for (const QString& password : invalid_passwords) {
        QVERIFY2(denied(
                     [&] {
                         f.service->dispatch(
                             "register", {{"username", "policy_user"}, {"password", password}}, {});
                     },
                     "validation"),
                 "The service must reject the password even when the UI is bypassed.");
        QCOMPARE(f.count("users"), 0);
    }
    const QString minimum = QStringLiteral("Abc12345");
    const QString maximum = QStringLiteral("Aa") + QString(30, '1');
    QVERIFY(
        !f.service->dispatch("register", {{"username", "policy_min"}, {"password", minimum}}, {})
             .value("id")
             .toString()
             .isEmpty());
    QVERIFY(
        !f.service->dispatch("register", {{"username", "policy_max"}, {"password", maximum}}, {})
             .value("id")
             .toString()
             .isEmpty());
    QVERIFY(!f.service->dispatch("login", {{"username", "policy_max"}, {"password", maximum}}, {})
                 .value("token")
                 .toString()
                 .isEmpty());
    QCOMPARE(f.count("users"), 2);
}

void IdentityTests::existingPasswordsRemainValidAfterPolicyChange() {
    Fixture f;
    // Simulate an account created under the previous length-only policy.
    const QString old_password(40, '7');
    const auto record = ledger::hashPassword(old_password);
    QSqlQuery insert(f.database.connection());
    QVERIFY(insert.prepare(QStringLiteral("INSERT INTO users VALUES(?,?,?,?,?,?,?)")));
    for (const QVariant& value : QVariantList{
             QStringLiteral("legacy-id"), QStringLiteral("legacy_user"),
             QStringLiteral("legacy_user"), record.salt, record.hash, record.iterations, f.now}) {
        insert.addBindValue(value);
    }
    QVERIFY(insert.exec());
    const auto login =
        f.service->dispatch("login", {{"username", "legacy_user"}, {"password", old_password}}, {});
    QCOMPARE(login.value("user").toObject().value("id").toString(), QStringLiteral("legacy-id"));
    QVERIFY(!login.value("token").toString().isEmpty());
}

void IdentityTests::creatorBecomesAdminAndLedgersAreIsolated() {
    Fixture f;
    const auto alice = f.account("alice");
    const auto bob = f.account("bob");
    const auto book = f.call(alice, "create_ledger",
                             {{"name", QStringLiteral("合租账本")},
                              {"user_id", bob.value("user").toObject().value("id")}});
    QCOMPARE(book.value("role").toString(), QStringLiteral("admin"));
    QCOMPARE(book.value("members").toArray().size(), 1);
    QCOMPARE(book.value("members").toArray()[0].toObject().value("id"),
             alice.value("user").toObject().value("id"));
    QCOMPARE(f.call(bob, "list_ledgers").value("ledgers").toArray().size(), 0);
    QVERIFY(denied([&] { f.call(bob, "ledger_details", {{"ledger_id", book.value("id")}}); },
                   "forbidden"));
    QVERIFY(denied([&] { f.service->dispatch("list_ledgers", {}, {}); }, "unauthenticated"));
    QVERIFY(denied([&] { f.call(alice, "create_ledger", {{"name", "   "}}); }, "validation"));
    QCOMPARE(f.count("ledgers"), 1);
    QCOMPARE(f.count("memberships"), 1);
}

void IdentityTests::invitationRequiresItsRecipientAndAcceptance() {
    Fixture f;
    const auto owner = f.account("owner");
    const auto member = f.account("member");
    const auto other = f.account("other");
    const auto book = f.call(owner, "create_ledger", {{"name", "Book"}});
    const auto invitation =
        f.call(owner, "invite",
               {{"ledger_id", book.value("id")}, {"username", "member"}, {"role", "member"}});
    QCOMPARE(f.call(member, "list_ledgers").value("ledgers").toArray().size(), 0);
    QCOMPARE(f.call(member, "invitations").value("invitations").toArray().size(), 1);
    QCOMPARE(f.call(other, "invitations").value("invitations").toArray().size(), 0);
    QVERIFY(denied(
        [&] { f.call(other, "accept_invitation", {{"invitation_id", invitation.value("id")}}); },
        "forbidden"));
    QVERIFY(denied(
        [&] {
            f.call(owner, "invite",
                   {{"ledger_id", book.value("id")}, {"username", "member"}, {"role", "member"}});
        },
        "conflict"));
    const auto joined =
        f.call(member, "accept_invitation", {{"invitation_id", invitation.value("id")}});
    QCOMPARE(joined.value("role").toString(), QStringLiteral("member"));
    QCOMPARE(joined.value("members").toArray().size(), 2);
    QVERIFY(denied(
        [&] { f.call(member, "accept_invitation", {{"invitation_id", invitation.value("id")}}); },
        "forbidden"));
    QVERIFY(denied(
        [&] {
            f.call(member, "invite",
                   {{"ledger_id", book.value("id")}, {"username", "other"}, {"role", "member"}});
        },
        "forbidden"));
}

void IdentityTests::rolesAreScopedToLedgerAndRechecked() {
    Fixture f;
    const auto owner = f.account("owner");
    const auto reader = f.account("reader");
    const auto book = f.call(owner, "create_ledger", {{"name", "Shared"}});
    const auto personal = f.call(reader, "create_ledger", {{"name", "Personal"}});
    const auto invite =
        f.call(owner, "invite",
               {{"ledger_id", book.value("id")}, {"username", "reader"}, {"role", "read_only"}});
    auto joined = f.call(reader, "accept_invitation", {{"invitation_id", invite.value("id")}});
    QVERIFY(!joined.value("capabilities").toObject().value("write_expense").toBool());
    QVERIFY(!joined.value("capabilities").toObject().value("review").toBool());
    QVERIFY(denied(
        [&] {
            f.call(reader, "change_role",
                   {{"ledger_id", book.value("id")},
                    {"user_id", reader.value("user").toObject().value("id")},
                    {"role", "member"}});
        },
        "forbidden"));
    QCOMPARE(f.call(reader, "ledger_details", {{"ledger_id", personal.value("id")}})
                 .value("role")
                 .toString(),
             QStringLiteral("admin"));
    f.call(owner, "change_role",
           {{"ledger_id", book.value("id")},
            {"user_id", reader.value("user").toObject().value("id")},
            {"role", "member"}});
    joined = f.call(reader, "ledger_details", {{"ledger_id", book.value("id")}});
    QVERIFY(joined.value("capabilities").toObject().value("write_expense").toBool());
    QVERIFY(!joined.value("capabilities").toObject().value("manage_members").toBool());
    QVERIFY(denied(
        [&] {
            f.call(owner, "change_role",
                   {{"ledger_id", book.value("id")},
                    {"user_id", owner.value("user").toObject().value("id")},
                    {"role", "member"}});
        },
        "conflict"));
    QVERIFY(denied(
        [&] {
            f.call(owner, "change_role",
                   {{"ledger_id", book.value("id")},
                    {"user_id", reader.value("user").toObject().value("id")},
                    {"role", "admin"}});
        },
        "validation"));
}

void IdentityTests::sessionsExpireAndLogoutRevokesThem() {
    Fixture f;
    const auto account = f.account("tester");
    f.call(account, "logout");
    QVERIFY(denied([&] { f.call(account, "me"); }, "unauthenticated"));
    const auto login =
        f.service->dispatch("login", {{"username", "tester"}, {"password", kPassword}}, {});
    f.now += 8 * 60 * 60;
    QVERIFY(denied([&] { f.call(login, "me"); }, "unauthenticated"));
    QVERIFY(denied([&] { f.service->dispatch("me", {}, QString(64, 'a')); }, "unauthenticated"));
}

void IdentityTests::failedLoginIsThrottledWithoutBlockingForever() {
    Fixture f;
    f.account("tester");
    for (int index = 0; index < 5; ++index) {
        QVERIFY(denied(
            [&] {
                f.service->dispatch("login", {{"username", "tester"}, {"password", "incorrect"}},
                                    {});
            },
            "invalid_credentials"));
    }
    QVERIFY(denied(
        [&] {
            f.service->dispatch("login", {{"username", "tester"}, {"password", kPassword}}, {});
        },
        "rate_limited"));
    f.now += 31;
    QVERIFY(!f.service->dispatch("login", {{"username", "tester"}, {"password", kPassword}}, {})
                 .value("token")
                 .toString()
                 .isEmpty());
}

void IdentityTests::ledgerAndAuditWritesAreAtomic() {
    Fixture f;
    const auto owner = f.account("owner");
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec(QStringLiteral("CREATE TRIGGER fail_log BEFORE INSERT ON operation_logs BEGIN "
                                "SELECT RAISE(ABORT,'fixture failure'); END")));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             f.call(owner, "create_ledger", {{"name", "Rollback"}}));
    QCOMPARE(f.count("ledgers"), 0);
    QCOMPARE(f.count("memberships"), 0);
    QVERIFY(sql.exec(QStringLiteral("DROP TRIGGER fail_log")));
    f.call(owner, "create_ledger", {{"name", "Saved"}});
    QCOMPARE(f.count("operation_logs"), 1);
    QVERIFY(!sql.exec(QStringLiteral("DELETE FROM operation_logs")));
    QVERIFY(!sql.exec(QStringLiteral("UPDATE operation_logs SET detail='tampered'")));
}

QTEST_GUILESS_MAIN(IdentityTests)
#include "identity_tests.moc"
