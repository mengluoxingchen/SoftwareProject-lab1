#include <QJsonArray>
#include <QSqlQuery>
#include <QtTest>
#include <memory>

#include "domain/service_error.h"
#include "domain/split.h"
#include "expense/expense_service.h"
#include "finance/finance_service.h"
#include "infrastructure/database.h"
#include "settlement/settlement_service.h"

namespace {
struct Fixture {
    ledger::Database database{":memory:"};
    qint64 now = 1800000000;
    std::unique_ptr<ledger::ExpenseService> expenses;
    std::unique_ptr<ledger::FinanceService> finances;
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
                throw std::runtime_error("Cannot seed financial user.");
            }
        }
        if (!sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                      "VALUES('book','Shared','a',1)") ||
            !sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                      "VALUES('book','a','admin',1),('book','b','member',1),('book','c','member',1)"
                      ",('book','reader','read_only',1)")) {
            throw std::runtime_error("Cannot seed financial book.");
        }
        expenses = std::make_unique<ledger::ExpenseService>(database, [this] { return now; });
        finances = std::make_unique<ledger::FinanceService>(database, [this] { return now; });
        settlements = std::make_unique<ledger::SettlementService>(database, [this] { return now; });
    }
    QJsonObject form(const QString& amount = "90.00",
                     const QString& description = "Shared purchase") {
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
    QJsonObject approved(QJsonObject values = {}) {
        if (values.isEmpty()) {
            values = form();
        }
        const auto record =
            expenses->dispatch("b", "expense_submit", values).value("expense").toObject();
        return expenses->dispatch("a", "expense_review",
                                  {{"ledger_id", "book"},
                                   {"expense_id", record.value("id")},
                                   {"version", record.value("version")},
                                   {"decision", "approve"}});
    }
    QJsonObject overview() {
        return settlements->dispatch("a", "settlement_overview", {{"ledger_id", "book"}});
    }
    QJsonObject settle() {
        return settlements->dispatch(
            "a", "settlement_create",
            {{"ledger_id", "book"}, {"preview_token", overview().value("preview_token")}});
    }
    QJsonObject info(const QString& id) {
        return finances->dispatch("a", "finance_info", {{"ledger_id", "book"}, {"source_id", id}});
    }
    QJsonObject refundRequest(const QString& source, const QString& amount,
                              const QString& key = "refund-1", const QString& receiver = "a") {
        return {{"ledger_id", "book"}, {"source_id", source},
                {"amount", amount},    {"receiver_id", receiver},
                {"occurred_at", now},  {"reason", "Merchant refund"},
                {"request_key", key},  {"expected_token", info(source).value("token")}};
    }
    QJsonObject refund(const QString& source, const QString& amount,
                       const QString& key = "refund-1", const QString& receiver = "a") {
        return finances->dispatch("a", "finance_refund",
                                  refundRequest(source, amount, key, receiver));
    }
    QJsonObject adjustmentRequest(const QString& kind, const QString& amount,
                                  const QString& source = {}, const QString& key = "adjust-1",
                                  const QString& payer = "a") {
        auto request =
            form(amount, kind == "supplement" ? "Missed purchase" : "Corrected purchase");
        request["kind"] = kind;
        request["source_id"] = source;
        request["reason"] = "Verified correction";
        request["request_key"] = key;
        request["payer_id"] = payer;
        if (!source.isEmpty()) {
            request["expected_token"] = info(source).value("token");
        }
        const auto preview = finances->dispatch("a", "finance_adjustment_preview", request);
        request["confirmation_token"] = preview.value("confirmation_token");
        request["confirmed"] = true;
        return request;
    }
    QJsonObject adjust(const QString& kind, const QString& amount, const QString& source = {},
                       const QString& key = "adjust-1", const QString& payer = "a") {
        return finances->dispatch("a", "finance_adjustment",
                                  adjustmentRequest(kind, amount, source, key, payer));
    }
    QJsonObject snapshot(const QString& id) {
        return settlements->dispatch("a", "settlement_detail",
                                     {{"ledger_id", "book"}, {"settlement_id", id}});
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec("SELECT COUNT(*) FROM " + table) || !sql.next()) {
            throw std::runtime_error("Cannot count financial data.");
        }
        return sql.value(0).toInt();
    }
};
QString balance(const QJsonObject& object, const QString& id) {
    for (const auto& item : object.value("members").toArray()) {
        const auto row = item.toObject();
        if (row.value("user_id").toString() == id) {
            return row.value("balance").toString();
        }
    }
    return {};
}
}  // namespace

class FinanceTests final : public QObject {
    Q_OBJECT
private slots:
    void settledRefundKeepsOriginalAndSnapshot();
    void approvedAndFullyRefundedExpensesSettleAtNetValue();
    void repeatedTinyRefundsAreExactAndNeverNegative();
    void actualRefundReceiverMayDifferFromPayer();
    void administratorSupplementTakesEffectWithReviewEvidence();
    void memberSupplementRequiresApprovalAndRollsBackAtomically();
    void correctionsCarryRefundsAndPreserveEveryGeneration();
    void zeroCorrectionKeepsRepaymentsAndAllowsTheirReturn();
    void permissionsPrivacyAndInvalidInput();
    void staleTokensAndRequestReplaysCannotDuplicateMoney();
    void financialWritesAndSourceReplacementRollBackTogether();
    void historyLocksAndLargeRefundWeights();
};

void FinanceTests::settledRefundKeepsOriginalAndSnapshot() {
    Fixture f;
    const auto source = f.approved();
    const auto original = f.settle();
    const QString id = source.value("id").toString();
    const auto event = f.refund(id, "30");
    QCOMPARE(event.value("after").toObject().value("net_amount").toString(),
             QStringLiteral("60.00"));
    const auto current = f.overview();
    QCOMPARE(balance(current, "a"), QStringLiteral("40.00"));
    QCOMPARE(balance(current, "b"), QStringLiteral("-20.00"));
    QCOMPARE(balance(current, "c"), QStringLiteral("-20.00"));
    QCOMPARE(current.value("total_refunds").toString(), QStringLiteral("30.00"));
    QCOMPARE(f.snapshot(original.value("id").toString()), original);
    const auto detail =
        f.expenses->dispatch("a", "expense_detail", {{"ledger_id", "book"}, {"expense_id", id}});
    QCOMPARE(detail.value("form").toObject().value("amount").toString(), QStringLiteral("90.00"));
    for (const auto& part : detail.value("shares").toArray()) {
        QCOMPARE(part.toObject().value("amount").toString(), QStringLiteral("30.00"));
    }
}

void FinanceTests::approvedAndFullyRefundedExpensesSettleAtNetValue() {
    Fixture f;
    const auto source = f.approved();
    const QString id = source.value("id").toString();
    f.refund(id, "90");
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("0.00"));
    QCOMPARE(f.overview().value("pending_net_amount").toString(), QStringLiteral("0.00"));
    const auto snapshot = f.settle();
    QCOMPARE(snapshot.value("new_amount").toString(), QStringLiteral("90.00"));
    QCOMPARE(snapshot.value("total_expenses").toString(), QStringLiteral("0.00"));
    QCOMPARE(balance(snapshot, "a"), QStringLiteral("0.00"));
    QVERIFY(!f.info(id).value("can_refund").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.refund(id, "0.01", "too-much"));
}

void FinanceTests::repeatedTinyRefundsAreExactAndNeverNegative() {
    Fixture f;
    const auto source = f.approved(f.form("0.03"));
    f.settle();
    const auto id = source.value("id").toString();
    for (int index = 1; index <= 3; ++index) {
        const auto event = f.refund(id, "0.01", QString::number(index));
        qint64 sum = 0;
        for (const auto& share :
             event.value("after").toObject().value("effective_shares").toArray()) {
            const auto amount = ledger::parseMoney(share.toObject().value("amount").toString());
            QVERIFY(amount >= 0);
            sum += amount;
        }
        QCOMPARE(sum, 3 - index);
    }
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("0.00"));
    QCOMPARE(f.count("expense_finance_events"), 3);
}

void FinanceTests::actualRefundReceiverMayDifferFromPayer() {
    Fixture f;
    const auto source = f.approved();
    f.settle();
    f.refund(source.value("id").toString(), "30", "other-receiver", "b");
    const auto current = f.overview();
    QCOMPARE(balance(current, "a"), QStringLiteral("70.00"));
    QCOMPARE(balance(current, "b"), QStringLiteral("-50.00"));
    QCOMPARE(balance(current, "c"), QStringLiteral("-20.00"));
}

void FinanceTests::administratorSupplementTakesEffectWithReviewEvidence() {
    Fixture f;
    f.approved();
    const auto original = f.settle();
    const auto result = f.adjust("supplement", "60", {}, "supplement", "b");
    const QString id = result.value("after").toObject().value("expense_id").toString();
    const auto record =
        f.expenses->dispatch("a", "expense_detail", {{"ledger_id", "book"}, {"expense_id", id}});
    QVERIFY(record.value("is_adjustment").toBool());
    QCOMPARE(record.value("status").toString(), QStringLiteral("settled"));
    QCOMPARE(record.value("version").toInt(), 3);
    QCOMPARE(f.count("expense_reviews"), 2);
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("150.00"));
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("40.00"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("10.00"));
    QCOMPARE(balance(f.overview(), "c"), QStringLiteral("-50.00"));
    QCOMPARE(f.snapshot(original.value("id").toString()), original);
}

void FinanceTests::memberSupplementRequiresApprovalAndRollsBackAtomically() {
    Fixture f;
    f.approved();
    f.settle();
    auto form = f.form("60", "Member missed receipt");
    form["post_settlement_adjustment"] = true;
    form["adjustment_reason"] = "Found missing receipt";
    const auto pending =
        f.expenses->dispatch("b", "expense_submit", form).value("expense").toObject();
    QVERIFY(pending.value("is_adjustment").toBool());
    QCOMPARE(pending.value("status").toString(), QStringLiteral("pending"));
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("90.00"));
    QJsonObject review{{"ledger_id", "book"},
                       {"expense_id", pending.value("id")},
                       {"version", pending.value("version")},
                       {"decision", "approve"}};
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_activation BEFORE INSERT ON expense_finance_events BEGIN "
                 "SELECT RAISE(ABORT,'Injected failure'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             f.expenses->dispatch("a", "expense_review", review));
    QCOMPARE(f.count("expense_reviews"), 1);
    QCOMPARE(f.count("expense_finance_events"), 0);
    QVERIFY(sql.exec("DROP TRIGGER fail_activation"));
    const auto approved = f.expenses->dispatch("a", "expense_review", review);
    QCOMPARE(approved.value("status").toString(), QStringLiteral("settled"));
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("150.00"));
    QCOMPARE(f.count("expense_finance_events"), 1);
}

void FinanceTests::correctionsCarryRefundsAndPreserveEveryGeneration() {
    Fixture f;
    const auto source = f.approved();
    const auto original = f.settle();
    const auto id = source.value("id").toString();
    const auto refund = f.refund(id, "30");
    auto request = f.adjustmentRequest("correction", "120", id);
    // Same description is legitimate in an explicit correction; the old record is excluded from
    // duplicate keys.
    request["description"] = "Shared purchase";
    request["confirmation_token"] = f.finances->dispatch("a", "finance_adjustment_preview", request)
                                        .value("confirmation_token");
    const auto correction = f.finances->dispatch("a", "finance_adjustment", request);
    const auto next = correction.value("after").toObject().value("expense_id").toString();
    QVERIFY(f.info(id).value("superseded").toBool());
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("90.00"));
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("60.00"));
    f.refund(next, "15", "refund-next", "c");
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("65.00"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("-25.00"));
    QCOMPARE(balance(f.overview(), "c"), QStringLiteral("-40.00"));
    const auto final = f.adjust("correction", "150", next, "correct-again", "b");
    QCOMPARE(final.value("after").toObject().value("refunded_amount").toString(),
             QStringLiteral("45.00"));
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("105.00"));
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("-65.00"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("115.00"));
    QCOMPARE(balance(f.overview(), "c"), QStringLiteral("-50.00"));
    QCOMPARE(f.finances->dispatch("a", "finance_event",
                                  {{"ledger_id", "book"}, {"event_id", refund.value("id")}}),
             refund);
    QCOMPARE(f.snapshot(original.value("id").toString()), original);
}

void FinanceTests::zeroCorrectionKeepsRepaymentsAndAllowsTheirReturn() {
    Fixture f;
    const auto source = f.approved();
    f.settle();
    f.settlements->dispatch("b", "settlement_repay",
                            {{"ledger_id", "book"},
                             {"payer_id", "b"},
                             {"recipient_id", "a"},
                             {"amount", "20"},
                             {"paid_at", f.now},
                             {"request_key", "paid-before-void"},
                             {"balance_token", f.overview().value("balance_token")}});
    const auto correction = f.adjust("correction", "0", source.value("id").toString());
    QVERIFY(correction.value("after").toObject().value("expense_id").toString().isEmpty());
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("-20.00"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("20.00"));
    QCOMPARE(f.overview().value("total_repayments").toString(), QStringLiteral("20.00"));
    f.settlements->dispatch("a", "settlement_repay",
                            {{"ledger_id", "book"},
                             {"payer_id", "a"},
                             {"recipient_id", "b"},
                             {"amount", "20"},
                             {"paid_at", f.now},
                             {"request_key", "return-after-void"},
                             {"balance_token", f.overview().value("balance_token")}});
    QCOMPARE(balance(f.overview(), "a"), QStringLiteral("0.00"));
    QCOMPARE(balance(f.overview(), "b"), QStringLiteral("0.00"));
}

void FinanceTests::permissionsPrivacyAndInvalidInput() {
    Fixture f;
    const auto source = f.approved();
    f.settle();
    const auto id = source.value("id").toString();
    const auto request = f.refundRequest(id, "10");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("b", "finance_refund", request));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("reader", "finance_refund", request));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("outsider", "finance_info",
                                                  {{"ledger_id", "book"}, {"source_id", id}}));
    const auto draft =
        f.expenses->dispatch("b", "expense_save_draft", f.form("10", "Private draft"));
    QVERIFY_THROWS_EXCEPTION(
        ledger::ServiceError,
        f.finances->dispatch("a", "finance_info",
                             {{"ledger_id", "book"}, {"source_id", draft.value("id")}}));
    auto invalid = request;
    invalid["receiver_id"] = "outsider";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_refund", invalid));
    invalid = request;
    invalid["amount"] = "91";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_refund", invalid));
    invalid = request;
    invalid["reason"] = "";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_refund", invalid));
    invalid = request;
    invalid["occurred_at"] = f.now + 1000;
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_refund", invalid));
    f.refund(id, "30");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.adjustmentRequest("correction", "20", id));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.adjustmentRequest("correction", "0", id));
    QCOMPARE(f.count("expense_finance_events"), 1);
}

void FinanceTests::staleTokensAndRequestReplaysCannotDuplicateMoney() {
    Fixture f;
    const auto source = f.approved();
    f.settle();
    const auto id = source.value("id").toString();
    const auto stale_refund = f.refundRequest(id, "10", "stale");
    const auto stale_adjust = f.adjustmentRequest("correction", "120", id);
    const auto request = f.refundRequest(id, "10", "first");
    const auto saved = f.finances->dispatch("a", "finance_refund", request);
    QVERIFY(f.finances->dispatch("a", "finance_refund", request).value("reused").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_refund", stale_refund));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_adjustment", stale_adjust));
    auto changed = request;
    changed["amount"] = "5";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.finances->dispatch("a", "finance_refund", changed));
    const auto fresh = f.adjustmentRequest("correction", "120", id, "fresh");
    const auto correction = f.finances->dispatch("a", "finance_adjustment", fresh);
    const auto replay = f.finances->dispatch("a", "finance_adjustment", fresh);
    QCOMPARE(replay.value("id"), correction.value("id"));
    QVERIFY(replay.value("reused").toBool());
    QVERIFY(f.finances->dispatch("a", "finance_refund", request).value("reused").toBool());
    QCOMPARE(f.count("expense_finance_events"), 2);
}

void FinanceTests::financialWritesAndSourceReplacementRollBackTogether() {
    Fixture f;
    const auto source = f.approved();
    f.settle();
    const auto id = source.value("id").toString();
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_finance BEFORE INSERT ON expense_finance_events BEGIN SELECT "
                 "RAISE(ABORT,'Injected failure'); END"));
    const auto request = f.adjustmentRequest("correction", "120", id);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             f.finances->dispatch("a", "finance_adjustment", request));
    QCOMPARE(f.count("expenses"), 1);
    QCOMPARE(f.count("expense_reviews"), 1);
    QVERIFY(!f.info(id).value("superseded").toBool());
    QVERIFY(sql.exec("DROP TRIGGER fail_finance"));
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_finance_log BEFORE INSERT ON operation_logs WHEN "
                 "NEW.action='record_refund' BEGIN SELECT RAISE(ABORT,'Injected failure'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.refund(id, "30"));
    QCOMPARE(f.count("expense_finance_events"), 0);
    QCOMPARE(f.info(id).value("net_amount").toString(), QStringLiteral("90.00"));
    QCOMPARE(f.overview().value("total_expenses").toString(), QStringLiteral("90.00"));
}

void FinanceTests::historyLocksAndLargeRefundWeights() {
    Fixture f;
    const auto source = f.approved();
    const auto id = source.value("id").toString();
    f.expenses->dispatch(
        "a", "expense_lock_acquire",
        {{"ledger_id", "book"}, {"expense_id", id}, {"version", source.value("version")}},
        "other-window");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.refund(id, "1"));
    f.now += 121;
    f.refund(id, "1");
    QVERIFY_THROWS_EXCEPTION(
        ledger::ServiceError,
        f.expenses->dispatch("a", "expense_lock_acquire",
                             {{"ledger_id", "book"}, {"expense_id", id}}, "new-window"));
    QSqlQuery sql(f.database.connection());
    QVERIFY(!sql.exec("UPDATE expense_finance_events SET reason='changed'"));
    QVERIFY(!sql.exec("DELETE FROM expense_finance_events"));
    f.settle();
    const auto large = f.approved(f.form("9999999999.99", "Large purchase"));
    f.settle();
    f.refund(large.value("id").toString(), "9999999999.99", "large-refund");
    QCOMPARE(f.info(large.value("id").toString()).value("net_amount").toString(),
             QStringLiteral("0.00"));
}

QTEST_GUILESS_MAIN(FinanceTests)
#include "finance_tests.moc"
