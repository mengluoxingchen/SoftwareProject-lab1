#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QtTest>
#include <functional>
#include <memory>

#include "domain/service_error.h"
#include "domain/split.h"
#include "expense/expense_service.h"
#include "infrastructure/database.h"

namespace {
struct Fixture {
    ledger::Database database{QStringLiteral(":memory:")};
    std::unique_ptr<ledger::ExpenseService> service;
    Fixture() {
        database.open();
        QSqlQuery sql(database.connection());
        for (const QString& id : QStringList{"alice", "bob", "reader", "outsider"}) {
            sql.prepare(QStringLiteral("INSERT INTO users VALUES(?,?,?,X'01',X'02',600000,1)"));
            sql.addBindValue(id);
            sql.addBindValue(id);
            sql.addBindValue(id);
            if (!sql.exec()) {
                throw std::runtime_error("Cannot seed expense test users.");
            }
        }
        if (!sql.exec(QStringLiteral("INSERT INTO ledgers(id,name,created_by,created_at) "
                                     "VALUES('book','Shared','alice',1)")) ||
            !sql.exec(QStringLiteral("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                                     "VALUES('book','alice','admin',1),('book','bob','member',1),('"
                                     "book','reader','read_only',1)"))) {
            throw std::runtime_error("Cannot seed expense memberships.");
        }
        service = std::make_unique<ledger::ExpenseService>(database, [] { return 1000000; });
    }
    QJsonObject form() {
        return {{"ledger_id", "book"},
                {"amount", "100.00"},
                {"date", "2026-11-09"},
                {"payer_id", "alice"},
                {"description", QStringLiteral("团队采购")},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", "alice"}, {"value", ""}},
                                      QJsonObject{{"user_id", "bob"}, {"value", ""}}}}};
    }
    QJsonObject call(const QString& actor, const QString& method, const QJsonObject& form) {
        return service->dispatch(actor, method, form);
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !sql.next()) {
            throw std::runtime_error("Cannot count rows.");
        }
        return sql.value(0).toInt();
    }
};
bool denied(const std::function<void()>& action, const QString& code, const QString& field = {}) {
    try {
        action();
    } catch (const ledger::ServiceError& error) {
        return error.code() == code && (field.isEmpty() || error.field() == field);
    }
    return false;
}
}  // namespace

class ExpenseTests final : public QObject {
    Q_OBJECT
private slots:
    void requestReceiptsReplayWithoutDuplicatingOrGrantingLocks();
    void receiptConflictAndActorScope();
    void receiptAndAuditRollbackTogether();
    void splitsAreExactAndStable();
    void largeRatiosDoNotOverflow();
    void partialDraftIsPrivateAndResumable();
    void requiredFieldsAndMembersAreValidated();
    void duplicateAndSimilarSubmissionsAreDifferent();
    void readOnlyAndStaleUpdatesAreRejected();
    void submissionAndLogAreAtomic();
    void payerMayBeOutsideTheCostBearers();
    void legacyTypeIsIgnoredWhenReopeningAndSubmitting();
};

void ExpenseTests::splitsAreExactAndStable() {
    const QVector<ledger::SplitPart> parts{{"a", "1"}, {"b", "2"}, {"c", "3"}};
    const auto equal = ledger::calculateSplit(10000, "equal", parts);
    QCOMPARE(equal[0].amount, 3334);
    QCOMPARE(equal[1].amount, 3333);
    QCOMPARE(equal[2].amount, 3333);
    const auto ratio = ledger::calculateSplit(10000, "ratio", {{"a", "0.1"}, {"b", "0.2"}});
    QCOMPARE(ratio[0].amount, 3333);
    QCOMPARE(ratio[1].amount, 6667);
    const auto fixed = ledger::calculateSplit(10000, "fixed", {{"a", "30"}, {"b", "70.00"}});
    QCOMPARE(fixed[0].amount, 3000);
    QCOMPARE(fixed[1].amount, 7000);
    QVERIFY(denied(
        [&] {
            static_cast<void>(ledger::calculateSplit(10000, "fixed", {{"a", "30"}, {"b", "60"}}));
        },
        "validation", "splits"));
    QVERIFY(
        denied([&] { static_cast<void>(ledger::parseMoney("1.001")); }, "validation", "amount"));
    QVERIFY(denied([&] { static_cast<void>(ledger::parseMoney("-1")); }, "validation"));
    for (qint64 total = 1; total <= 200; ++total) {
        const auto values = ledger::calculateSplit(total, "ratio", parts);
        const auto reversed =
            ledger::calculateSplit(total, "ratio", {{"c", "3"}, {"b", "2"}, {"a", "1"}});
        QCOMPARE(values[0].amount + values[1].amount + values[2].amount, total);
        QCOMPARE(values[0].amount, reversed[2].amount);
        QCOMPARE(values[2].amount, reversed[0].amount);
    }
}

void ExpenseTests::largeRatiosDoNotOverflow() {
    const qint64 total = ledger::parseMoney("9999999999.99");
    const auto result =
        ledger::calculateSplit(total, "ratio", {{"a", "999999.9999"}, {"b", "999999.9999"}});
    QCOMPARE(result[0].amount, 500000000000LL);
    QCOMPARE(result[1].amount, 499999999999LL);
    QCOMPARE(result[0].amount + result[1].amount, total);
}

void ExpenseTests::partialDraftIsPrivateAndResumable() {
    Fixture f;
    auto form = f.form();
    form["amount"] = "";
    form["description"] = "";
    const auto draft = f.call("alice", "expense_save_draft", form);
    QCOMPARE(draft.value("status").toString(), QStringLiteral("draft"));
    QCOMPARE(draft.value("version").toInt(), 1);
    QCOMPARE(
        f.call("bob", "expense_list", {{"ledger_id", "book"}}).value("expenses").toArray().size(),
        0);
    QVERIFY(denied(
        [&] {
            f.call("bob", "expense_detail",
                   {{"ledger_id", "book"}, {"expense_id", draft.value("id")}});
        },
        "forbidden"));
    form = f.form();
    form["expense_id"] = draft.value("id");
    form["version"] = 1;
    form["lock_token"] = draft.value("lock_token");
    const auto saved = f.call("alice", "expense_save_draft", form);
    QCOMPARE(saved.value("version").toInt(), 2);
    form["version"] = 2;
    const auto submitted = f.call("alice", "expense_submit", form).value("expense").toObject();
    QCOMPARE(submitted.value("id"), draft.value("id"));
    QCOMPARE(submitted.value("status").toString(), QStringLiteral("pending"));
    QCOMPARE(submitted.value("shares").toArray().size(), 2);
    QVERIFY(denied([&] { f.call("alice", "expense_save_draft", form); }, "conflict"));
    QCOMPARE(f.count("expenses"), 1);
}

void ExpenseTests::requiredFieldsAndMembersAreValidated() {
    Fixture f;
    for (const QString& field : QStringList{"amount", "date", "payer_id", "description"}) {
        auto form = f.form();
        form[field] = "";
        QVERIFY2(denied([&] { f.call("alice", "expense_submit", form); }, "validation", field),
                 qPrintable(field));
    }
    for (const QString& amount : QStringList{"-1", "abc", "1.001", "0"}) {
        auto form = f.form();
        form["amount"] = amount;
        QVERIFY(denied([&] { f.call("alice", "expense_submit", form); }, "validation", "amount"));
    }
    auto form = f.form();
    form["date"] = "2026-02-30";
    QVERIFY(denied([&] { f.call("alice", "expense_submit", form); }, "validation", "date"));
    form = f.form();
    form["payer_id"] = "outsider";
    QVERIFY(denied([&] { f.call("alice", "expense_submit", form); }, "validation", "payer_id"));
    form = f.form();
    form["splits"] = QJsonArray{};
    QVERIFY(denied([&] { f.call("alice", "expense_submit", form); }, "validation", "splits"));
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "outsider"}, {"value", ""}}};
    QVERIFY(denied([&] { f.call("alice", "expense_submit", form); }, "validation", "splits"));
    QCOMPARE(f.count("expenses"), 0);
    form = f.form();
    const auto submitted = f.call("alice", "expense_submit", form);
    QVERIFY(!submitted.value("confirmation_required").toBool());
    QCOMPARE(f.count("expenses"), 1);
}

void ExpenseTests::duplicateAndSimilarSubmissionsAreDifferent() {
    Fixture f;
    auto form = f.form();
    f.call("alice", "expense_submit", form);
    form["confirm_similar"] = true;
    QVERIFY(denied([&] { f.call("alice", "expense_submit", form); }, "duplicate"));
    form.remove("confirm_similar");
    const auto possible = f.call("bob", "expense_submit", form);
    QVERIFY(possible.value("confirmation_required").toBool());
    QCOMPARE(f.count("expenses"), 1);
    form["confirm_similar"] = true;
    const auto accepted = f.call("bob", "expense_submit", form);
    QVERIFY(!accepted.value("confirmation_required").toBool());
    QCOMPARE(f.count("expenses"), 2);
    QCOMPARE(f.count("expense_shares"), 4);
}

void ExpenseTests::readOnlyAndStaleUpdatesAreRejected() {
    Fixture f;
    auto form = f.form();
    QVERIFY(denied([&] { f.call("reader", "expense_submit", form); }, "forbidden"));
    QVERIFY(denied([&] { f.call("reader", "expense_save_draft", form); }, "forbidden"));
    QVERIFY(
        denied([&] { f.call("outsider", "expense_list", {{"ledger_id", "book"}}); }, "forbidden"));
    const auto draft = f.call("alice", "expense_save_draft", form);
    form["expense_id"] = draft.value("id");
    form["version"] = 1;
    form["lock_token"] = draft.value("lock_token");
    QVERIFY(denied([&] { f.call("bob", "expense_save_draft", form); }, "forbidden"));
    form["description"] = "First update";
    f.call("alice", "expense_save_draft", form);
    form["description"] = "Stale overwrite";
    QVERIFY(denied([&] { f.call("alice", "expense_save_draft", form); }, "conflict"));
    const auto latest = f.call("alice", "expense_detail",
                               {{"ledger_id", "book"}, {"expense_id", draft.value("id")}});
    QCOMPARE(latest.value("form").toObject().value("description").toString(),
             QStringLiteral("First update"));
}

void ExpenseTests::submissionAndLogAreAtomic() {
    Fixture f;
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec(QStringLiteral("CREATE TRIGGER fail_share BEFORE INSERT ON expense_shares BEGIN "
                                "SELECT RAISE(ABORT,'test'); END")));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.call("alice", "expense_submit", f.form()));
    QCOMPARE(f.count("expenses"), 0);
    QCOMPARE(f.count("expense_shares"), 0);
    QCOMPARE(f.count("operation_logs"), 0);
    QVERIFY(sql.exec(QStringLiteral("DROP TRIGGER fail_share")));
    f.call("alice", "expense_submit", f.form());
    QCOMPARE(f.count("expenses"), 1);
    QCOMPARE(f.count("operation_logs"), 1);
}

void ExpenseTests::payerMayBeOutsideTheCostBearers() {
    Fixture f;
    auto form = f.form();
    form["amount"] = "90.00";
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "bob"}, {"value", ""}}};
    const auto expense = f.call("alice", "expense_submit", form).value("expense").toObject();
    QCOMPARE(expense.value("payer").toString(), QStringLiteral("alice"));
    const auto shares = expense.value("shares").toArray();
    QCOMPARE(shares.size(), 1);
    QCOMPARE(shares[0].toObject().value("user_id").toString(), QStringLiteral("bob"));
    QCOMPARE(shares[0].toObject().value("amount").toString(), QStringLiteral("90.00"));
    QVERIFY(!expense.value("form").toObject().contains("expense_type"));
    const auto rows =
        f.call("alice", "expense_list", {{"ledger_id", "book"}}).value("expenses").toArray();
    QVERIFY(!rows[0].toObject().contains("expense_type"));
    QCOMPARE(rows[0].toObject().value("participants").toString(), QStringLiteral("bob"));
    form["description"] = "Self expense";
    form["splits"] = QJsonArray{QJsonObject{{"user_id", "alice"}, {"value", ""}}};
    const auto self = f.call("alice", "expense_submit", form).value("expense").toObject();
    QCOMPARE(self.value("shares").toArray()[0].toObject().value("amount").toString(),
             QStringLiteral("90.00"));
}

void ExpenseTests::legacyTypeIsIgnoredWhenReopeningAndSubmitting() {
    Fixture f;
    auto form = f.form();
    form["expense_type"] = "advance";
    const auto draft = f.call("alice", "expense_save_draft", form);
    form["expense_id"] = draft.value("id");
    form["version"] = draft.value("version");
    form["lock_token"] = draft.value("lock_token");
    QSqlQuery sql(f.database.connection());
    sql.prepare("UPDATE expenses SET legacy_expense_type='advance',payload=? WHERE id=?");
    sql.addBindValue(QString::fromUtf8(QJsonDocument(form).toJson(QJsonDocument::Compact)));
    sql.addBindValue(draft.value("id").toString());
    QVERIFY(sql.exec());
    const auto reopened = f.call("alice", "expense_detail",
                                 {{"ledger_id", "book"}, {"expense_id", draft.value("id")}});
    QVERIFY(!reopened.value("form").toObject().contains("expense_type"));
    // Even an obsolete client tag cannot control the new allocation rules.
    form["expense_type"] = "unknown-obsolete-label";
    const auto expense = f.call("alice", "expense_submit", form).value("expense").toObject();
    QCOMPARE(expense.value("shares").toArray().size(), 2);
    QVERIFY(!expense.value("form").toObject().contains("expense_type"));
    sql.prepare("SELECT legacy_expense_type FROM expenses WHERE id=?");
    sql.addBindValue(draft.value("id").toString());
    QVERIFY(sql.exec());
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("advance"));
}

void ExpenseTests::requestReceiptsReplayWithoutDuplicatingOrGrantingLocks() {
    Fixture f;
    auto form = f.form();
    form["request_key"] = "draft-request";
    const auto saved = f.call("alice", "expense_save_draft", form);
    auto update = form;
    update["expense_id"] = saved.value("id");
    update["version"] = saved.value("version");
    update["lock_token"] = saved.value("lock_token");
    update["request_key"] = "submit-request";
    const auto submitted = f.call("alice", "expense_submit", update).value("expense").toObject();
    QCOMPARE(submitted.value("version").toInt(), 2);
    const auto replay = f.service->dispatch("alice", "expense_save_draft", form, "other-session");
    QVERIFY(replay.value("reused").toBool());
    QVERIFY(!replay.contains("lock_token"));
    QCOMPARE(replay.value("version").toInt(), 1);
    QVERIFY(f.call("alice", "expense_submit", update).value("reused").toBool());
    QCOMPARE(f.count("expenses"), 1);
    QCOMPARE(f.count("expense_requests"), 2);
    QCOMPARE(f.count("operation_logs"), 2);
}
void ExpenseTests::receiptConflictAndActorScope() {
    Fixture f;
    auto form = f.form();
    form["request_key"] = "shared-key";
    f.call("alice", "expense_save_draft", form);
    auto changed = form;
    changed["amount"] = "200";
    QVERIFY(denied([&] { f.call("alice", "expense_save_draft", changed); }, "request_conflict"));
    const QJsonObject ref{{"ledger_id", "book"}, {"request_key", "shared-key"}};
    QVERIFY(f.call("alice", "expense_write_status", ref).value("found").toBool());
    QVERIFY(!f.call("bob", "expense_write_status", ref).value("found").toBool());
    QVERIFY(denied([&] { f.call("outsider", "expense_write_status", ref); }, "forbidden"));
    f.call("bob", "expense_save_draft", form);
    QCOMPARE(f.count("expense_requests"), 2);
    QSqlQuery sql(f.database.connection());
    QVERIFY(!sql.exec("UPDATE expense_requests SET version=99"));
    QVERIFY(!sql.exec("DELETE FROM expense_requests"));
}
void ExpenseTests::receiptAndAuditRollbackTogether() {
    Fixture f;
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_receipt BEFORE INSERT ON expense_requests BEGIN "
                 "SELECT RAISE(ABORT,'simulated disk error'); END"));
    auto form = f.form();
    form["request_key"] = "atomic";
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.call("alice", "expense_submit", form));
    QCOMPARE(f.count("expenses"), 0);
    QCOMPARE(f.count("expense_requests"), 0);
    QCOMPARE(f.count("operation_logs"), 0);
    QCOMPARE(f.count("audit_details"), 0);
}
QTEST_GUILESS_MAIN(ExpenseTests)
#include "expense_tests.moc"
