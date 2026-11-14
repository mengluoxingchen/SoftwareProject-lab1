#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>
#include <limits>
#include <memory>
#include <stdexcept>

#include "domain/service_error.h"
#include "domain/split.h"
#include "expense/expense_service.h"
#include "infrastructure/database.h"
#include "settlement/settlement_service.h"

namespace {
struct Fixture {
    ledger::Database database{":memory:"};
    qint64 now = 1800000000;
    std::unique_ptr<ledger::ExpenseService> expenses;
    std::unique_ptr<ledger::SettlementService> settlements;
    Fixture() {
        database.open();
        QSqlQuery sql(database.connection());
        for (const QString& id : QStringList{"a", "b", "c", "reader", "outsider"}) {
            sql.prepare("INSERT INTO users VALUES(?,?,?,X'01',X'02',600000,1)");
            sql.addBindValue(id);
            sql.addBindValue(id);
            sql.addBindValue(id);
            if (!sql.exec()) {
                throw std::runtime_error("Cannot seed settlement user.");
            }
        }
        if (!sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                      "VALUES('book','Shared','a',1),('private','Private','outsider',1)") ||
            !sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                      "VALUES('book','a','admin',1),('book','b','member',1),('book','c','member',1)"
                      ",('book','reader','read_only',1),('private','outsider','admin',1)")) {
            throw std::runtime_error("Cannot seed settlement memberships.");
        }
        expenses = std::make_unique<ledger::ExpenseService>(database, [this] { return now; });
        settlements = std::make_unique<ledger::SettlementService>(database, [this] { return now; });
    }
    QJsonObject form(const QString& description = "Shared purchase",
                     const QString& amount = "90.00") {
        return {{"ledger_id", "book"},
                {"amount", amount},
                {"date", "2026-11-09"},
                {"payer_id", "a"},
                {"description", description},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}},
                                      QJsonObject{{"user_id", "b"}, {"value", ""}},
                                      QJsonObject{{"user_id", "c"}, {"value", ""}}}}};
    }
    QJsonObject approved(QJsonObject value = {}) {
        if (value.isEmpty()) {
            value = form();
        }
        auto record = expenses->dispatch("b", "expense_submit", value).value("expense").toObject();
        return expenses->dispatch("a", "expense_review",
                                  {{"ledger_id", "book"},
                                   {"expense_id", record.value("id")},
                                   {"version", record.value("version")},
                                   {"decision", "approve"}});
    }
    QJsonObject overview(const QString& actor = "a") {
        return settlements->dispatch(actor, "settlement_overview", {{"ledger_id", "book"}});
    }
    QJsonObject create(const QString& actor = "a", QString token = {}) {
        if (token.isEmpty()) {
            token = overview(actor).value("preview_token").toString();
        }
        return settlements->dispatch(actor, "settlement_create",
                                     {{"ledger_id", "book"}, {"preview_token", token}});
    }
    QJsonObject payment(const QString& key = "payment-1", const QString& amount = "20.00",
                        const QString& payer = "b") {
        return {{"ledger_id", "book"},
                {"payer_id", payer},
                {"recipient_id", "a"},
                {"amount", amount},
                {"paid_at", now},
                {"request_key", key},
                {"balance_token", overview().value("balance_token")}};
    }
    QJsonObject detail(const QString& id) {
        return settlements->dispatch("a", "settlement_detail",
                                     {{"ledger_id", "book"}, {"settlement_id", id}});
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec("SELECT COUNT(*) FROM " + table) || !sql.next()) {
            throw std::runtime_error("Cannot count settlement data.");
        }
        return sql.value(0).toInt();
    }
};
QString balance(const QJsonObject& result, const QString& id) {
    for (const auto& entry : result.value("members").toArray()) {
        if (entry.toObject().value("user_id").toString() == id) {
            return entry.toObject().value("balance").toString();
        }
    }
    return {};
}
}  // namespace

class SettlementTests final : public QObject {
    Q_OBJECT
private slots:
    void sharedPayerOnlyReceivesTheOtherPersonsShare();
    void partialRepaymentKeepsSnapshotAndExpenseTotals();
    void personalRepaymentsSurviveOtherMembersRecentActivity();
    void payerMayBeExcludedOrPayForSelf();
    void onlyApprovedRecordsAndIncrementalSnapshots();
    void permissionsAndLedgerIsolation();
    void editLocksAndStaleSettlementPreview();
    void concurrentRepaymentsAndIdempotency();
    void overpaymentRequiresExplicitConfirmation();
    void transactionsRollBackStateLinksPaymentsAndAudit();
    void immutableHistoryAndCorruptShares();
    void exactNegativeCentsAndLargeCumulativeAmounts();
};

void SettlementTests::sharedPayerOnlyReceivesTheOtherPersonsShare() {
    Fixture f;
    auto form = f.form("Two people share ninety");
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}},
                                QJsonObject{{"user_id", "b"}, {"value", ""}}};
    f.approved(form);
    const auto snapshot = f.create();
    QCOMPARE(balance(snapshot, "a"), QStringLiteral("45.00"));
    QCOMPARE(balance(snapshot, "b"), QStringLiteral("-45.00"));
    const auto directions = snapshot.value("suggestions").toArray();
    QCOMPARE(directions.size(), 1);
    QCOMPARE(directions[0].toObject().value("payer_id").toString(), QStringLiteral("b"));
    QCOMPARE(directions[0].toObject().value("recipient_id").toString(), QStringLiteral("a"));
    QCOMPARE(directions[0].toObject().value("amount").toString(), QStringLiteral("45.00"));
    f.settlements->dispatch("b", "settlement_repay", f.payment("finish-shared", "45.00"));
    const auto current = f.overview();
    QCOMPARE(current.value("repayments").toArray().first().toObject().value("payer_id").toString(),
             QStringLiteral("b"));
    QCOMPARE(
        current.value("repayments").toArray().first().toObject().value("recipient_id").toString(),
        QStringLiteral("a"));
    QCOMPARE(balance(current, "a"), QStringLiteral("0.00"));
    QCOMPARE(balance(current, "b"), QStringLiteral("0.00"));
    QCOMPARE(current.value("total_expenses").toString(), QStringLiteral("90.00"));
}

void SettlementTests::partialRepaymentKeepsSnapshotAndExpenseTotals() {
    Fixture f;
    const auto record = f.approved();
    const auto snapshot = f.create("b");
    QCOMPARE(balance(snapshot, "a"), QStringLiteral("60.00"));
    QCOMPARE(balance(snapshot, "b"), QStringLiteral("-30.00"));
    QCOMPARE(balance(snapshot, "c"), QStringLiteral("-30.00"));
    f.settlements->dispatch("b", "settlement_repay", f.payment());
    const auto current = f.overview();
    QCOMPARE(balance(current, "a"), QStringLiteral("40.00"));
    QCOMPARE(balance(current, "b"), QStringLiteral("-10.00"));
    QCOMPARE(balance(current, "c"), QStringLiteral("-30.00"));
    QCOMPARE(current.value("total_expenses").toString(), QStringLiteral("90.00"));
    QCOMPARE(current.value("total_repayments").toString(), QStringLiteral("20.00"));
    QCOMPARE(f.detail(snapshot.value("id").toString()), snapshot);
    const auto detail = f.expenses->dispatch(
        "a", "expense_detail", {{"ledger_id", "book"}, {"expense_id", record.value("id")}});
    QCOMPARE(detail.value("status").toString(), QStringLiteral("settled"));
    QVERIFY(!detail.value("can_edit").toBool());
    QVERIFY_THROWS_EXCEPTION(
        ledger::ServiceError,
        f.expenses->dispatch("a", "expense_lock_acquire",
                             {{"ledger_id", "book"}, {"expense_id", record.value("id")}}));
}

void SettlementTests::personalRepaymentsSurviveOtherMembersRecentActivity() {
    Fixture f;
    f.approved();
    f.create();
    for (int index = 1; index <= 4; ++index) {
        f.settlements->dispatch(
            "b", "settlement_repay",
            f.payment(QStringLiteral("mine-%1").arg(index), QString::number(index) + ".00"));
    }
    for (int index = 0; index < 101; ++index) {
        f.settlements->dispatch("c", "settlement_repay",
                                f.payment(QStringLiteral("other-%1").arg(index), "0.01", "c"));
    }
    const auto overview = f.overview("b");
    QCOMPARE(overview.value("repayments").toArray().size(), 100);
    for (const auto& item : overview.value("repayments").toArray()) {
        QCOMPARE(item.toObject().value("payer_id").toString(), QStringLiteral("c"));
    }
    const auto personal = overview.value("personal_repayments").toArray();
    QCOMPARE(personal.size(), 3);
    for (int index = 0; index < personal.size(); ++index) {
        const auto row = personal[index].toObject();
        QCOMPARE(row.value("payer_id").toString(), QStringLiteral("b"));
        QCOMPARE(row.value("recipient_id").toString(), QStringLiteral("a"));
        QCOMPARE(row.value("amount").toString(), QString::number(4 - index) + ".00");
    }
    QVERIFY(f.overview("reader").value("personal_repayments").toArray().isEmpty());
    QCOMPARE(balance(overview, "b"), QStringLiteral("-20.00"));
    QCOMPARE(overview.value("total_repayments").toString(), QStringLiteral("11.01"));
}

void SettlementTests::payerMayBeExcludedOrPayForSelf() {
    Fixture f;
    auto form = f.form("Paid for B");
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "b"}, {"value", ""}}};
    f.approved(form);
    auto snapshot = f.create();
    QCOMPARE(balance(snapshot, "a"), QStringLiteral("90.00"));
    QCOMPARE(balance(snapshot, "b"), QStringLiteral("-90.00"));
    form = f.form("Self purchase");
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}}};
    f.approved(form);
    snapshot = f.create();
    QCOMPARE(balance(snapshot, "a"), QStringLiteral("90.00"));
    QCOMPARE(balance(snapshot, "b"), QStringLiteral("-90.00"));
    QCOMPARE(snapshot.value("total_expenses").toString(), QStringLiteral("180.00"));
}

void SettlementTests::onlyApprovedRecordsAndIncrementalSnapshots() {
    Fixture f;
    auto pending =
        f.expenses->dispatch("b", "expense_submit", f.form("Pending")).value("expense").toObject();
    auto rejected =
        f.expenses->dispatch("b", "expense_submit", f.form("Rejected")).value("expense").toObject();
    f.expenses->dispatch("a", "expense_review",
                         {{"ledger_id", "book"},
                          {"expense_id", rejected.value("id")},
                          {"version", rejected.value("version")},
                          {"decision", "reject"},
                          {"reason", "Wrong receipt"}});
    f.expenses->dispatch("a", "expense_save_draft", f.form("Draft"));
    auto withdrawn = f.expenses->dispatch("b", "expense_submit", f.form("Withdrawn"))
                         .value("expense")
                         .toObject();
    f.expenses->dispatch("b", "expense_withdraw",
                         {{"ledger_id", "book"},
                          {"expense_id", withdrawn.value("id")},
                          {"version", withdrawn.value("version")},
                          {"reason", "Duplicate"}});
    f.approved();
    const auto token = f.overview().value("preview_token").toString();
    const auto original = f.create("a", token);
    QCOMPARE(original.value("new_count").toInt(), 1);
    QCOMPARE(original.value("new_amount").toString(), QStringLiteral("90.00"));
    QVERIFY(f.create("b", token).value("reused").toBool());
    QCOMPARE(f.count("settlements"), 1);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.create());
    f.settlements->dispatch("b", "settlement_repay", f.payment());
    f.approved(f.form("Next batch", "60"));
    const auto next = f.create();
    QCOMPARE(next.value("new_count").toInt(), 1);
    QCOMPARE(next.value("new_amount").toString(), QStringLiteral("60.00"));
    QCOMPARE(next.value("total_expenses").toString(), QStringLiteral("150.00"));
    QCOMPARE(balance(next, "a"), QStringLiteral("80.00"));
    QCOMPARE(f.detail(original.value("id").toString()), original);
    QCOMPARE(f.count("settlement_expenses"), 2);
    QCOMPARE(f.expenses
                 ->dispatch("b", "expense_detail",
                            {{"ledger_id", "book"}, {"expense_id", pending.value("id")}})
                 .value("status")
                 .toString(),
             QStringLiteral("pending"));
}

void SettlementTests::permissionsAndLedgerIsolation() {
    Fixture f;
    f.approved();
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.create("reader"));
    const auto snapshot = f.create("c");
    QVERIFY(!f.overview("reader").value("can_write").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.overview("outsider"));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("reader", "settlement_repay", f.payment()));
    QVERIFY_THROWS_EXCEPTION(
        ledger::ServiceError,
        f.settlements->dispatch("b", "settlement_repay", f.payment("spoof", "10", "c")));
    auto payment = f.payment();
    payment["recipient_id"] = "outsider";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("a", "settlement_repay", payment));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("outsider", "settlement_detail",
                                                     {{"ledger_id", "private"},
                                                      {"settlement_id", snapshot.value("id")}}));
    f.settlements->dispatch("a", "settlement_repay", f.payment("delegated", "10", "c"));
    QCOMPARE(balance(f.overview(), "c"), QStringLiteral("-20.00"));
}

void SettlementTests::editLocksAndStaleSettlementPreview() {
    Fixture f;
    const auto record = f.approved();
    const auto preview = f.overview();
    const QJsonObject ref{{"ledger_id", "book"},
                          {"expense_id", record.value("id")},
                          {"version", record.value("version")}};
    f.expenses->dispatch("a", "expense_lock_acquire", ref, "active-session");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.create("a", preview.value("preview_token").toString()));
    f.now += 121;
    f.approved(f.form("New approval"));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.create("a", preview.value("preview_token").toString()));
    QCOMPARE(f.count("settlements"), 0);
    QCOMPARE(f.create().value("new_count").toInt(), 2);
}

void SettlementTests::concurrentRepaymentsAndIdempotency() {
    Fixture f;
    f.approved();
    f.create();
    const auto first = f.payment();
    const auto second = f.payment("concurrent", "20.00");
    const auto saved = f.settlements->dispatch("b", "settlement_repay", first);
    const auto replay = f.settlements->dispatch("b", "settlement_repay", first);
    QCOMPARE(saved.value("id"), replay.value("id"));
    QVERIFY(replay.value("reused").toBool());
    QCOMPARE(f.count("repayments"), 1);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("b", "settlement_repay", second));
    auto changed = first;
    changed["amount"] = "10.00";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("b", "settlement_repay", changed));
    f.settlements->dispatch("b", "settlement_repay", f.payment("fresh", "10.00"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("0.00"));
    QCOMPARE(f.count("repayments"), 2);
}

void SettlementTests::overpaymentRequiresExplicitConfirmation() {
    Fixture f;
    f.approved();
    const auto snapshot = f.create();
    auto payment = f.payment("overpaid", "40.00");
    try {
        f.settlements->dispatch("b", "settlement_repay", payment);
        QFAIL("Overpayment needs confirmation.");
    } catch (const ledger::ServiceError& error) {
        QCOMPARE(error.code(), QStringLiteral("confirmation_required"));
    }
    QCOMPARE(f.count("repayments"), 0);
    payment["confirm_overpayment"] = true;
    f.settlements->dispatch("b", "settlement_repay", payment);
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("10.00"));
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("20.00"));
    QCOMPARE(f.detail(snapshot.value("id").toString()), snapshot);
    auto self = f.payment("self");
    self["recipient_id"] = "b";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("b", "settlement_repay", self));
    auto invalid = f.payment("negative", "-1");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("b", "settlement_repay", invalid));
    invalid = f.payment("future");
    invalid["paid_at"] = f.now + 1000;
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.settlements->dispatch("b", "settlement_repay", invalid));
}

void SettlementTests::transactionsRollBackStateLinksPaymentsAndAudit() {
    Fixture f;
    const auto record = f.approved();
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_settlement BEFORE INSERT ON settlement_expenses BEGIN SELECT "
                 "RAISE(ABORT,'Injected failure'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.create());
    QCOMPARE(f.count("settlements"), 0);
    QCOMPARE(f.count("settlement_expenses"), 0);
    QCOMPARE(f.overview().value("pending_count").toInt(), 1);
    QVERIFY(sql.exec("DROP TRIGGER fail_settlement"));
    f.create();
    const int logs = f.count("operation_logs");
    QVERIFY(sql.exec(
        "CREATE TRIGGER fail_payment_log BEFORE INSERT ON operation_logs WHEN "
        "NEW.action='record_repayment' BEGIN SELECT RAISE(ABORT,'Injected failure'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             f.settlements->dispatch("b", "settlement_repay", f.payment()));
    QCOMPARE(f.count("repayments"), 0);
    QCOMPARE(f.count("operation_logs"), logs);
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("-30.00"));
}

void SettlementTests::immutableHistoryAndCorruptShares() {
    Fixture f;
    const auto record = f.approved();
    QSqlQuery sql(f.database.connection());
    sql.prepare("UPDATE expense_shares SET amount_cents=1 WHERE expense_id=? AND user_id='b'");
    sql.addBindValue(record.value("id").toString());
    QVERIFY(sql.exec());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.create());
    QCOMPARE(f.count("settlements"), 0);
    sql.prepare("UPDATE expense_shares SET amount_cents=3000 WHERE expense_id=? AND user_id='b'");
    sql.addBindValue(record.value("id").toString());
    QVERIFY(sql.exec());
    f.create();
    f.settlements->dispatch("b", "settlement_repay", f.payment());
    QVERIFY(!sql.exec("UPDATE settlements SET snapshot='{}'"));
    QVERIFY(!sql.exec("DELETE FROM settlements"));
    QVERIFY(!sql.exec("UPDATE settlement_expenses SET form='{}'"));
    QVERIFY(!sql.exec("DELETE FROM settlement_expenses"));
    QVERIFY(!sql.exec("UPDATE repayments SET amount_cents=1"));
    QVERIFY(!sql.exec("DELETE FROM repayments"));
}

void SettlementTests::exactNegativeCentsAndLargeCumulativeAmounts() {
    QCOMPARE(ledger::formatMoney(-1), QStringLiteral("-0.01"));
    QCOMPARE(ledger::formatMoney(-101), QStringLiteral("-1.01"));
    QCOMPARE(ledger::formatMoney(std::numeric_limits<qint64>::min()),
             QStringLiteral("-92233720368547758.08"));
    Fixture f;
    auto form = f.form("Tiny", "0.01");
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "b"}, {"value", ""}}};
    f.approved(form);
    f.create();
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("-0.01"));
    f.settlements->dispatch("b", "settlement_repay", f.payment("tiny", "0.01"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("0.00"));
    f.approved(f.form("Large1", "9999999999.99"));
    f.approved(f.form("Large2", "9999999999.99"));
    const auto snapshot = f.create();
    QCOMPARE(snapshot.value("new_amount").toString(), QStringLiteral("19999999999.98"));
    QCOMPARE(snapshot.value("total_expenses").toString(), QStringLiteral("19999999999.99"));
}

QTEST_GUILESS_MAIN(SettlementTests)
#include "settlement_tests.moc"
