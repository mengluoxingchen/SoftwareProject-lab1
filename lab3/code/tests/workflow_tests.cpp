#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>
#include <memory>

#include "domain/service_error.h"
#include "expense/expense_service.h"
#include "infrastructure/database.h"

namespace {
struct Fixture {
    ledger::Database database{":memory:"};
    qint64 now = 1000000;
    std::unique_ptr<ledger::ExpenseService> service;
    Fixture() {
        database.open();
        QSqlQuery sql(database.connection());
        for (const QString& id : QStringList{"admin", "member", "other", "reader", "outsider"}) {
            sql.prepare("INSERT INTO users VALUES(?,?,?,X'01',X'02',600000,1)");
            sql.addBindValue(id);
            sql.addBindValue(id);
            sql.addBindValue(id);
            if (!sql.exec()) {
                throw std::runtime_error("Cannot seed workflow user.");
            }
        }
        if (!sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                      "VALUES('book','Shared','admin',1)") ||
            !sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                      "VALUES('book','admin','admin',1),('book','member','member',1),('book','"
                      "other','member',1),('book','reader','read_only',1)")) {
            throw std::runtime_error("Cannot seed workflow book.");
        }
        service = std::make_unique<ledger::ExpenseService>(database, [this] { return now; });
    }
    QJsonObject call(const QString& actor, const QString& method, const QJsonObject& parameters,
                     const QString& session = {}) {
        return service->dispatch(actor, method, parameters,
                                 session.isEmpty() ? actor + "-session" : session);
    }
    QJsonObject form(const QString& description = "Shared purchase") {
        return {{"ledger_id", "book"},
                {"amount", "90.00"},
                {"date", "2026-11-09"},
                {"payer_id", "admin"},
                {"description", description},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", "admin"}, {"value", ""}},
                                      QJsonObject{{"user_id", "member"}, {"value", ""}},
                                      QJsonObject{{"user_id", "other"}, {"value", ""}}}}};
    }
    QJsonObject submit(const QString& actor = "member",
                       const QString& description = "Shared purchase") {
        return call(actor, "expense_submit", form(description)).value("expense").toObject();
    }
    QJsonObject ref(const QJsonObject& record) {
        return {{"ledger_id", "book"},
                {"expense_id", record.value("id")},
                {"version", record.value("version")}};
    }
    QJsonObject edit(const QString& actor, const QJsonObject& record, const QString& session = {}) {
        auto lease = call(actor, "expense_lock_acquire", ref(record), session);
        auto parameters = record.value("form").toObject();
        const auto identity = ref(record);
        for (auto it = identity.begin(); it != identity.end(); ++it) {
            parameters[it.key()] = it.value();
        }
        parameters["lock_token"] = lease.value("lock_token");
        parameters["change_reason"] = "Correct receipt";
        return parameters;
    }
    QJsonObject review(const QJsonObject& record, const QString& decision,
                       const QString& reason = "") {
        auto parameters = ref(record);
        parameters["decision"] = decision;
        parameters["reason"] = reason;
        parameters["confirm_similar"] = true;
        return call("admin", "expense_review", parameters);
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec("SELECT COUNT(*) FROM " + table) || !sql.next()) {
            throw std::runtime_error("Cannot count rows.");
        }
        return sql.value(0).toInt();
    }
};
bool denied(const std::function<void()>& action, const QString& code) {
    try {
        action();
    } catch (const ledger::ServiceError& error) {
        return error.code() == code;
    }
    return false;
}
}  // namespace

class WorkflowTests final : public QObject {
    Q_OBJECT
private slots:
    void reviewRequiresAdminAndRejectionReasonAndNotifiesOwner();
    void approvedChangesPreserveHistoryAndRequireReviewAgain();
    void leasesSeparateSessionsAndExpireWithoutAllowingOldTokens();
    void ownersAdminsAndTerminalStatesAreEnforced();
    void atMostOneSimilarExpenseCanPass();
    void withdrawalRequiresReasonAndPreservesEvidence();
    void failedWritesRollBackReviewRevisionAndWithdrawal();
};

void WorkflowTests::reviewRequiresAdminAndRejectionReasonAndNotifiesOwner() {
    Fixture f;
    const auto record = f.submit();
    auto parameters = f.ref(record);
    parameters["decision"] = "reject";
    QVERIFY(denied([&] { f.call("member", "expense_review", parameters); }, "forbidden"));
    QVERIFY(denied([&] { f.call("member", "expense_review_queue", {{"ledger_id", "book"}}); },
                   "forbidden"));
    QCOMPARE(f.call("admin", "expense_review_queue", {{"ledger_id", "book"}})
                 .value("expenses")
                 .toArray()
                 .size(),
             1);
    QVERIFY(denied([&] { f.call("admin", "expense_review", parameters); }, "validation"));
    parameters["reason"] = "Missing invoice";
    const auto rejected = f.call("admin", "expense_review", parameters);
    QCOMPARE(rejected.value("status").toString(), QStringLiteral("rejected"));
    QVERIFY(denied([&] { f.call("admin", "expense_review", parameters); }, "conflict"));
    const auto notices = f.call("member", "expense_notifications", {{"ledger_id", "book"}})
                             .value("notices")
                             .toArray();
    QCOMPARE(notices.size(), 1);
    QVERIFY(notices[0].toObject().value("message").toString().contains("Missing invoice"));
    QVERIFY(f.call("other", "expense_notifications", {{"ledger_id", "book"}})
                .value("notices")
                .toArray()
                .isEmpty());
    auto marked =
        QJsonObject{{"ledger_id", "book"}, {"notice_id", notices[0].toObject().value("id")}};
    QVERIFY(denied([&] { f.call("other", "expense_notice_read", marked); }, "forbidden"));
    QVERIFY(f.call("member", "expense_notice_read", marked)
                .value("notices")
                .toArray()[0]
                .toObject()
                .value("read")
                .toBool());
    QCOMPARE(f.count("expense_reviews"), 1);
}

void WorkflowTests::approvedChangesPreserveHistoryAndRequireReviewAgain() {
    Fixture f;
    auto record = f.review(f.submit(), "approve");
    QCOMPARE(record.value("status").toString(), QStringLiteral("approved"));
    auto form = f.edit("member", record);
    form["amount"] = "120.00";
    QVERIFY(denied([&] { f.call("member", "expense_save_draft", form); }, "conflict"));
    form["change_reason"] = " ";
    QVERIFY(denied([&] { f.call("member", "expense_submit", form); }, "validation"));
    form["change_reason"] = "Corrected total";
    record = f.call("member", "expense_submit", form).value("expense").toObject();
    QCOMPARE(record.value("status").toString(), QStringLiteral("pending"));
    QVERIFY(record.value("corrected").toBool());
    QCOMPARE(record.value("shares").toArray()[0].toObject().value("amount").toString(),
             QStringLiteral("40.00"));
    QVERIFY(!record.value("locked").toBool());
    const auto history = f.call("reader", "expense_history", f.ref(record));
    QCOMPARE(history.value("reviews").toArray().size(), 1);
    const auto revisions = history.value("revisions").toArray();
    QCOMPARE(revisions.size(), 1);
    const auto revision = revisions[0].toObject();
    QCOMPARE(revision.value("before").toObject().value("amount").toString(),
             QStringLiteral("90.00"));
    QCOMPARE(revision.value("after").toObject().value("amount").toString(),
             QStringLiteral("120.00"));
    QCOMPARE(revision.value("before_shares").toArray()[0].toObject().value("amount").toString(),
             QStringLiteral("30.00"));
    QSqlQuery sql(f.database.connection());
    QVERIFY(!sql.exec("UPDATE expense_revisions SET reason='changed'"));
    QVERIFY(!sql.exec("DELETE FROM expense_reviews"));
    record = f.review(record, "approve");
    QCOMPARE(record.value("status").toString(), QStringLiteral("approved"));
    QCOMPARE(f.call("member", "expense_history", f.ref(record)).value("reviews").toArray().size(),
             2);
}

void WorkflowTests::leasesSeparateSessionsAndExpireWithoutAllowingOldTokens() {
    Fixture f;
    const auto record = f.submit();
    auto first = f.edit("member", record, "window-one");
    QVERIFY(denied([&] { f.edit("member", record, "window-two"); }, "locked"));
    QVERIFY(denied([&] { f.edit("admin", record, "admin-window"); }, "locked"));
    QVERIFY(denied([&] { f.review(record, "approve"); }, "locked"));
    QVERIFY(
        denied([&] { f.call("member", "expense_lock_renew", first, "window-two"); }, "lock_lost"));
    auto another = f.submit("member", "Other purchase");
    QVERIFY(!f.edit("member", another, "window-two").value("lock_token").toString().isEmpty());
    f.now += 121;
    QVERIFY(
        denied([&] { f.call("member", "expense_lock_renew", first, "window-one"); }, "lock_lost"));
    auto second = f.edit("admin", record, "admin-window");
    QVERIFY(
        !f.call("member", "expense_lock_release", first, "window-one").value("released").toBool());
    first["amount"] = "100.00";
    QVERIFY(denied([&] { f.call("member", "expense_submit", first, "window-one"); }, "lock_lost"));
    QVERIFY(
        f.call("admin", "expense_lock_release", second, "admin-window").value("released").toBool());
    QCOMPARE(f.review(record, "approve").value("status").toString(), QStringLiteral("approved"));
}

void WorkflowTests::ownersAdminsAndTerminalStatesAreEnforced() {
    Fixture f;
    auto record = f.submit();
    QVERIFY(denied([&] { f.edit("other", record); }, "forbidden"));
    QVERIFY(denied([&] { f.edit("reader", record); }, "forbidden"));
    QVERIFY(denied([&] { f.call("outsider", "expense_history", f.ref(record)); }, "forbidden"));
    auto edited = f.edit("admin", record);
    edited["description"] = "Admin correction";
    record = f.call("admin", "expense_submit", edited).value("expense").toObject();
    QCOMPARE(record.value("creator_id").toString(), QStringLiteral("member"));
    auto stale = f.ref(record);
    stale["version"] = 1;
    QVERIFY(denied([&] { f.call("member", "expense_lock_acquire", stale); }, "conflict"));
    QSqlQuery sql(f.database.connection());
    sql.prepare("UPDATE expenses SET status='settled' WHERE id=?");
    sql.addBindValue(record.value("id").toString());
    QVERIFY(sql.exec());
    QVERIFY(denied([&] { f.edit("admin", record); }, "forbidden"));
    auto withdrawal = f.ref(record);
    withdrawal["reason"] = "No longer needed";
    QVERIFY(denied([&] { f.call("member", "expense_withdraw", withdrawal); }, "forbidden"));
}

void WorkflowTests::atMostOneSimilarExpenseCanPass() {
    Fixture f;
    const auto first = f.submit();
    auto form = f.form();
    form["confirm_similar"] = true;
    auto second = f.call("other", "expense_submit", form).value("expense").toObject();
    auto parameters = f.ref(first);
    parameters["decision"] = "approve";
    QVERIFY(denied([&] { f.call("admin", "expense_review", parameters); }, "validation"));
    const auto approved = f.review(first, "approve");
    QVERIFY(denied([&] { f.review(second, "approve"); }, "duplicate_approved"));
    QSqlQuery sql(f.database.connection());
    sql.prepare("UPDATE expenses SET status='approved' WHERE id=?");
    sql.addBindValue(second.value("id").toString());
    QVERIFY(!sql.exec());
    second = f.review(second, "reject", "Same invoice as accepted expense");
    auto corrected = f.edit("other", second);
    corrected["description"] = "Different invoice number 02";
    second = f.call("other", "expense_submit", corrected).value("expense").toObject();
    QCOMPARE(f.review(second, "approve").value("status").toString(), QStringLiteral("approved"));
    QCOMPARE(approved.value("status").toString(), QStringLiteral("approved"));
}

void WorkflowTests::withdrawalRequiresReasonAndPreservesEvidence() {
    Fixture f;
    auto record = f.review(f.submit(), "approve");
    auto withdrawal = f.ref(record);
    QVERIFY(denied([&] { f.call("member", "expense_withdraw", withdrawal); }, "validation"));
    auto held = f.edit("member", record);
    withdrawal["reason"] = "Wrong invoice";
    QVERIFY(denied([&] { f.call("member", "expense_withdraw", withdrawal); }, "locked"));
    f.call("member", "expense_lock_release", held);
    auto stale = withdrawal;
    stale["version"] = 0;
    QVERIFY(denied([&] { f.call("member", "expense_withdraw", stale); }, "conflict"));
    record = f.call("member", "expense_withdraw", withdrawal);
    QCOMPARE(record.value("status").toString(), QStringLiteral("withdrawn"));
    QVERIFY(f.call("member", "expense_list", {{"ledger_id", "book"}})
                .value("expenses")
                .toArray()
                .isEmpty());
    QCOMPARE(f.call("reader", "expense_list", {{"ledger_id", "book"}, {"status", "withdrawn"}})
                 .value("expenses")
                 .toArray()
                 .size(),
             1);
    const auto history = f.call("reader", "expense_history", f.ref(record));
    QCOMPARE(history.value("withdrawals").toArray()[0].toObject().value("reason").toString(),
             QStringLiteral("Wrong invoice"));
    QVERIFY(denied([&] { f.edit("member", record); }, "forbidden"));
    QCOMPARE(f.count("expense_shares"), 3);
}

void WorkflowTests::failedWritesRollBackReviewRevisionAndWithdrawal() {
    Fixture f;
    auto record = f.submit();
    QSqlQuery sql(f.database.connection());
    const int original_logs = f.count("operation_logs");
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_notice BEFORE INSERT ON expense_notices BEGIN SELECT "
                 "RAISE(ABORT,'test'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.review(record, "reject", "Test reason"));
    QCOMPARE(f.call("member", "expense_detail", f.ref(record)).value("status").toString(),
             QStringLiteral("pending"));
    QCOMPARE(f.count("expense_reviews"), 0);
    QCOMPARE(f.count("operation_logs"), original_logs);
    QVERIFY(sql.exec("DROP TRIGGER fail_notice"));
    auto change = f.edit("member", record);
    change["amount"] = "120.00";
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_revision BEFORE INSERT ON expense_revisions BEGIN SELECT "
                 "RAISE(ABORT,'test'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.call("member", "expense_submit", change));
    auto current = f.call("member", "expense_detail", f.ref(record));
    QCOMPARE(current.value("form").toObject().value("amount").toString(), QStringLiteral("90.00"));
    QVERIFY(current.value("locked").toBool());
    QCOMPARE(f.count("expense_revisions"), 0);
    QCOMPARE(f.count("operation_logs"), original_logs);
    QVERIFY(sql.exec("DROP TRIGGER fail_revision"));
    f.call("member", "expense_lock_release", change);
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_withdrawal BEFORE INSERT ON expense_withdrawals BEGIN SELECT "
                 "RAISE(ABORT,'test'); END"));
    auto withdrawal = f.ref(record);
    withdrawal["reason"] = "Test reason";
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.call("member", "expense_withdraw", withdrawal));
    QCOMPARE(f.call("member", "expense_detail", f.ref(record)).value("status").toString(),
             QStringLiteral("pending"));
    QCOMPARE(f.count("operation_logs"), original_logs);
}

QTEST_GUILESS_MAIN(WorkflowTests)
#include "workflow_tests.moc"
