#include <QCryptographicHash>
#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>
#include <memory>

#include "domain/service_error.h"
#include "identity/identity_service.h"
#include "infrastructure/database.h"

namespace {
struct Fixture {
    ledger::Database database{":memory:"};
    qint64 now = 1800000000;
    std::unique_ptr<ledger::IdentityService> service;
    Fixture() {
        database.open();
        QSqlQuery sql(database.connection());
        for (const QString& id : QStringList{"a", "b", "outsider"}) {
            sql.prepare("INSERT INTO users VALUES(?,?,?,X'01',X'02',600000,1)");
            sql.addBindValue(id);
            sql.addBindValue(id);
            sql.addBindValue(id);
            if (!sql.exec()) {
                throw std::runtime_error("Cannot seed lifecycle user.");
            }
            sql.prepare("INSERT INTO sessions VALUES(?,?,?)");
            sql.addBindValue(QCryptographicHash::hash(
                (id + "-token").leftJustified(64, '0').toUtf8(), QCryptographicHash::Sha256));
            sql.addBindValue(id);
            sql.addBindValue(now + 100000);
            if (!sql.exec()) {
                throw std::runtime_error("Cannot seed lifecycle session.");
            }
        }
        service = std::make_unique<ledger::IdentityService>(database, [this] { return now; });
        call("a", "create_ledger", {{"name", "Disposable ledger"}});
    }
    QJsonObject call(const QString& actor, const QString& method, QJsonObject parameters = {}) {
        return service->dispatch(method, parameters, (actor + "-token").leftJustified(64, '0'));
    }
    QString id() {
        return call("a", "list_ledgers")
            .value("ledgers")
            .toArray()[0]
            .toObject()
            .value("id")
            .toString();
    }
    QJsonObject preview() { return call("a", "ledger_close_preview", {{"ledger_id", id()}}); }
    QJsonObject closeRequest(const QString& mode = "delete") {
        const auto data = preview();
        return {{"ledger_id", data.value("id")},
                {"preview_token", data.value("preview_token")},
                {"mode", mode},
                {"confirm_name", data.value("name")},
                {"confirmed", true}};
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec("SELECT COUNT(*) FROM " + table) || !sql.next()) {
            throw std::runtime_error("Cannot count lifecycle data.");
        }
        return sql.value(0).toInt();
    }
    QJsonObject form(const QString& description = "Shared", const QString& amount = "90") {
        return {{"ledger_id", id()},
                {"amount", amount},
                {"date", "2026-11-09"},
                {"description", description},
                {"payer_id", "a"},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}},
                                      QJsonObject{{"user_id", "b"}, {"value", ""}}}}};
    }
    QJsonObject ref(const QJsonObject& record) {
        return {{"ledger_id", id()},
                {"expense_id", record.value("id")},
                {"version", record.value("version")}};
    }
    void addMember() {
        const auto invitation =
            call("a", "invite", {{"ledger_id", id()}, {"username", "b"}, {"role", "member"}});
        call("b", "accept_invitation", {{"invitation_id", invitation.value("id")}});
    }
    void seedBusiness() {
        addMember();
        auto record = call("b", "expense_submit", form()).value("expense").toObject();
        auto review = ref(record);
        review["decision"] = "approve";
        record = call("a", "expense_review", review);
        const auto lock = call("a", "expense_lock_acquire", ref(record));
        auto edit = form("Shared", "100");
        edit["expense_id"] = record.value("id");
        edit["version"] = record.value("version");
        edit["lock_token"] = lock.value("lock_token");
        edit["change_reason"] = "Correct receipt";
        record = call("a", "expense_submit", edit).value("expense").toObject();
        review = ref(record);
        review["decision"] = "approve";
        call("a", "expense_review", review);
        auto overview = call("a", "settlement_overview", {{"ledger_id", id()}});
        call("a", "settlement_create",
             {{"ledger_id", id()}, {"preview_token", overview.value("preview_token")}});
        overview = call("a", "settlement_overview", {{"ledger_id", id()}});
        call("b", "settlement_repay",
             {{"ledger_id", id()},
              {"payer_id", "b"},
              {"recipient_id", "a"},
              {"amount", "20"},
              {"paid_at", now},
              {"request_key", "test-payment"},
              {"balance_token", overview.value("balance_token")}});
        auto withdrawn =
            call("b", "expense_submit", form("Withdrawn", "10")).value("expense").toObject();
        auto withdraw = ref(withdrawn);
        withdraw["reason"] = "Cancelled";
        call("b", "expense_withdraw", withdraw);
        call("b", "expense_submit", form("Pending", "10"));
        const auto draft = call("a", "expense_save_draft", form("Draft", "30"));
        call("a", "expense_lock_release",
             {{"ledger_id", id()},
              {"expense_id", draft.value("id")},
              {"lock_token", draft.value("lock_token")}});
        call("a", "invite", {{"ledger_id", id()}, {"username", "outsider"}, {"role", "member"}});
        // Represents a previously exited member; the user-facing exit flow is a later feature.
        QSqlQuery sql(database.connection());
        sql.prepare(
            "UPDATE memberships SET status='exited',exited_at=? WHERE ledger_id=? AND user_id='b'");
        sql.addBindValue(now);
        sql.addBindValue(id());
        if (!sql.exec()) {
            throw std::runtime_error("Cannot seed former membership.");
        }
    }
};
}  // namespace

class LedgerLifecycleTests final : public QObject {
    Q_OBJECT
private slots:
    void emptyLedgerCanBeDeletedAndAuditRemains();
    void archiveRestorePreservesAllBusinessDataAndRevokesInvites();
    void deletionPurgesEveryBusinessTableButKeepsOriginalLogs();
    void memberAndOutsiderCannotCloseOrReadClosedLogs();
    void confirmationStalePreviewAndEditLockAreChecked();
    void failureRollsBackDeletionAndImmutableHistoryGuardsRemain();
};

void LedgerLifecycleTests::emptyLedgerCanBeDeletedAndAuditRemains() {
    Fixture f;
    const auto request = f.closeRequest();
    const auto saved = f.call("a", "ledger_close", request);
    QCOMPARE(saved.value("status").toString(), QStringLiteral("deleted"));
    QVERIFY(f.call("a", "ledger_close", request).value("reused").toBool());
    QCOMPARE(f.call("a", "list_ledgers").value("ledgers").toArray().size(), 0);
    QCOMPARE(f.count("operation_logs"), 2);
    const auto closed = f.call("a", "closed_ledgers").value("ledgers").toArray();
    QCOMPARE(closed.size(), 1);
    QCOMPARE(f.call("a", "closed_ledger_logs", {{"ledger_id", saved.value("id")}})
                 .value("logs")
                 .toArray()
                 .size(),
             2);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "restore_ledger",
                                    {{"ledger_id", saved.value("id")},
                                     {"close_token", request.value("preview_token")},
                                     {"confirmed", true}}));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "ledger_details", {{"ledger_id", saved.value("id")}}));
}

void LedgerLifecycleTests::archiveRestorePreservesAllBusinessDataAndRevokesInvites() {
    Fixture f;
    f.seedBusiness();
    const QString id = f.id();
    const int expenses = f.count("expenses"), reviews = f.count("expense_reviews");
    const auto request = f.closeRequest("archive");
    f.call("a", "ledger_close", request);
    QCOMPARE(f.count("expenses"), expenses);
    QCOMPARE(f.count("expense_reviews"), reviews);
    QCOMPARE(f.count("settlements"), 1);
    QCOMPARE(f.count("repayments"), 1);
    QCOMPARE(f.call("outsider", "invitations").value("invitations").toArray().size(), 0);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "settlement_overview", {{"ledger_id", id}}));
    const QJsonObject restore{
        {"ledger_id", id}, {"close_token", request.value("preview_token")}, {"confirmed", true}};
    f.call("a", "restore_ledger", restore);
    QVERIFY(f.call("a", "restore_ledger", restore).value("reused").toBool());
    const auto overview = f.call("a", "settlement_overview", {{"ledger_id", id}});
    QCOMPARE(overview.value("total_expenses").toString(), QStringLiteral("100.00"));
    QCOMPARE(overview.value("total_repayments").toString(), QStringLiteral("20.00"));
    QCOMPARE(f.count("expense_revisions"), 1);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "ledger_close", request));
}

void LedgerLifecycleTests::deletionPurgesEveryBusinessTableButKeepsOriginalLogs() {
    Fixture f;
    f.seedBusiness();
    const QString id = f.id();
    QSqlQuery source(f.database.connection());
    QVERIFY(source.exec("SELECT id FROM expenses WHERE status='settled'"));
    QVERIFY(source.next());
    const auto info =
        f.call("a", "finance_info", {{"ledger_id", id}, {"source_id", source.value(0).toString()}});
    f.call("a", "finance_refund",
           {{"ledger_id", id},
            {"source_id", source.value(0).toString()},
            {"receiver_id", "a"},
            {"amount", "10"},
            {"reason", "Merchant refund"},
            {"occurred_at", f.now},
            {"expected_token", info.value("token")},
            {"request_key", "close-refund"}});
    QCOMPARE(f.count("expense_finance_events"), 1);
    QSqlQuery original(f.database.connection());
    QVERIFY(original.exec("SELECT id,detail FROM operation_logs ORDER BY id"));
    QMap<QString, QString> evidence;
    while (original.next()) {
        evidence.insert(original.value(0).toString(), original.value(1).toString());
    }
    f.call("a", "ledger_close", f.closeRequest());
    for (const QString& table : QStringList{
             "expenses", "expense_shares", "expense_reviews", "expense_revisions",
             "expense_withdrawals", "expense_notices", "expense_locks", "settlements",
             "settlement_expenses", "repayments", "invitations", "expense_finance_events"}) {
        QCOMPARE(f.count(table), 0);
    }
    QCOMPARE(f.count("users"), 3);
    QCOMPARE(f.count("memberships"), 2);
    QCOMPARE(f.count("operation_logs"), evidence.size() + 1);
    QVERIFY(original.exec("SELECT id,detail FROM operation_logs ORDER BY id"));
    while (original.next()) {
        const auto key = original.value(0).toString();
        if (evidence.contains(key)) {
            QCOMPARE(original.value(1).toString(), evidence.value(key));
            evidence.remove(key);
        }
    }
    QVERIFY(evidence.isEmpty());
    QCOMPARE(f.call("a", "closed_ledger_logs", {{"ledger_id", id}}).value("name").toString(),
             QStringLiteral("Disposable ledger"));
    QVERIFY(!original.exec("DELETE FROM operation_logs"));
    QVERIFY(!original.exec("UPDATE operation_logs SET detail='changed'"));
}

void LedgerLifecycleTests::memberAndOutsiderCannotCloseOrReadClosedLogs() {
    Fixture f;
    f.addMember();
    const QString id = f.id();
    const auto preview = f.preview();
    QVERIFY(!preview.value("can_close").toBool());
    const auto request = f.closeRequest();
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "ledger_close", request));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("b", "ledger_close_preview", {{"ledger_id", id}}));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", "ledger_close", request));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("outsider", "ledger_close", request));
    QSqlQuery sql(f.database.connection());
    QVERIFY(sql.exec("UPDATE memberships SET status='exited' WHERE user_id='b'"));
    f.call("a", "ledger_close", f.closeRequest());
    QCOMPARE(f.call("outsider", "closed_ledgers").value("ledgers").toArray().size(), 0);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("outsider", "closed_ledger_logs", {{"ledger_id", id}}));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("b", "closed_ledger_logs", {{"ledger_id", id}}));
}

void LedgerLifecycleTests::confirmationStalePreviewAndEditLockAreChecked() {
    Fixture f;
    const auto request = f.closeRequest();
    auto invalid = request;
    invalid["confirmed"] = false;
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "ledger_close", invalid));
    invalid = request;
    invalid["confirm_name"] = "Other ledger";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "ledger_close", invalid));
    f.call("a", "invite", {{"ledger_id", f.id()}, {"username", "outsider"}, {"role", "member"}});
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "ledger_close", request));
    auto form = f.form("Draft");
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}}};
    const auto draft = f.call("a", "expense_save_draft", form);
    QVERIFY(!f.preview().value("can_close").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "ledger_close", f.closeRequest()));
    f.now += 121;
    QVERIFY(f.preview().value("can_close").toBool());
    f.call("a", "ledger_close", f.closeRequest());
    QCOMPARE(f.count("expenses"), 0);
}

void LedgerLifecycleTests::failureRollsBackDeletionAndImmutableHistoryGuardsRemain() {
    Fixture f;
    f.seedBusiness();
    QSqlQuery sql(f.database.connection());
    QVERIFY(!sql.exec("DELETE FROM expense_reviews"));
    QVERIFY(!sql.exec("DELETE FROM expense_revisions"));
    QVERIFY(!sql.exec("DELETE FROM settlements"));
    QVERIFY(!sql.exec("DELETE FROM repayments"));
    QVERIFY(sql.exec(
        "CREATE TRIGGER fail_close BEFORE INSERT ON operation_logs WHEN "
        "NEW.action='delete_ledger_data' BEGIN SELECT RAISE(ABORT,'Injected failure'); END"));
    const int count = f.count("expenses"), logs = f.count("operation_logs");
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.call("a", "ledger_close", f.closeRequest()));
    QCOMPARE(f.count("expenses"), count);
    QCOMPARE(f.count("operation_logs"), logs);
    QCOMPARE(f.count("settlements"), 1);
    QCOMPARE(f.count("repayments"), 1);
    QCOMPARE(f.call("a", "list_ledgers").value("ledgers").toArray().size(), 1);
    QVERIFY(!sql.exec("DELETE FROM settlements"));
    QVERIFY(sql.exec("PRAGMA foreign_key_check"));
    QVERIFY(!sql.next());
}

QTEST_GUILESS_MAIN(LedgerLifecycleTests)
#include "ledger_lifecycle_tests.moc"
