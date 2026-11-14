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
    QString id;
    Fixture() {
        database.open();
        QSqlQuery sql(database.connection());
        for (const auto& user : QStringList{"a", "b", "c", "outside"}) {
            sql.prepare("INSERT INTO users VALUES(?,?,?,X'01',X'02',600000,1)");
            sql.addBindValue(user);
            sql.addBindValue(user);
            sql.addBindValue(user);
            if (!sql.exec()) {
                throw std::runtime_error("Seed user");
            }
            sql.prepare("INSERT INTO sessions VALUES(?,?,?)");
            sql.addBindValue(QCryptographicHash::hash(user.leftJustified(64, '0').toUtf8(),
                                                      QCryptographicHash::Sha256));
            sql.addBindValue(user);
            sql.addBindValue(now + 100000);
            if (!sql.exec()) {
                throw std::runtime_error("Seed session");
            }
        }
        service = std::make_unique<ledger::IdentityService>(database, [this] { return now; });
        id = call("a", "create_ledger", {{"name", "Membership book"}}).value("id").toString();
        join("b");
        join("c", "read_only");
    }
    QJsonObject call(const QString& actor, const QString& method, QJsonObject parameters = {}) {
        if (!parameters.contains("ledger_id")) {
            parameters["ledger_id"] = id;
        }
        return service->dispatch(method, parameters, actor.leftJustified(64, '0'));
    }
    void join(const QString& actor, const QString& role = "member") {
        const auto invite = call("a", "invite", {{"username", actor}, {"role", role}});
        call(actor, "accept_invitation", {{"invitation_id", invite.value("id")}});
    }
    QJsonObject form(QString description = "Shared", QString amount = "90") {
        return {{"amount", amount},
                {"date", "2026-11-09"},
                {"description", description},
                {"payer_id", "a"},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}},
                                      QJsonObject{{"user_id", "b"}, {"value", ""}},
                                      QJsonObject{{"user_id", "c"}, {"value", ""}}}}};
    }
    QJsonObject seed() {
        const auto record = call("b", "expense_submit", form()).value("expense").toObject();
        call("a", "expense_review",
             {{"expense_id", record.value("id")},
              {"version", record.value("version")},
              {"decision", "approve"}});
        const auto preview = call("a", "settlement_overview");
        call("a", "settlement_create", {{"preview_token", preview.value("preview_token")}});
        return record;
    }
    QJsonObject request(const QString& actor, bool keep = true, const QString& successor = "") {
        return {{"preview_token", call(actor, "membership_exit_preview").value("preview_token")},
                {"confirmed", true},
                {"keep_history", keep},
                {"successor_id", successor},
                {"request_key", actor + "-exit"},
                {"settlement_deadline", "2080-01-01"}};
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec("SELECT COUNT(*) FROM " + table) || !sql.next()) {
            throw std::runtime_error("Count");
        }
        return sql.value(0).toInt();
    }
};
}  // namespace
class MembershipTests final : public QObject {
    Q_OBJECT
private slots:
    void exitWithDebtKeepsFrozenHistoryAndDeadline();
    void noHistoryExitRevokesEveryReadAndWrite();
    void laterBackdatedRecordsAndDraftPublicationAreDenied();
    void laterRefundAndRevisionDoNotLeakIntoHistory();
    void adminTransferAndExitAreAtomic();
    void standaloneTransferPromotesCurrentReadOnlyMember();
    void invalidSuccessorStalePreviewAndConfirmationAreRejected();
    void failuresRollBackTransferExitAndSnapshots();
    void rejoinThenExitReplacesAuthorizedScope();
    void archiveRestoreAndDeletionRespectHistoricalScope();
};

void MembershipTests::exitWithDebtKeepsFrozenHistoryAndDeadline() {
    Fixture f;
    const auto record = f.seed();
    const auto request = f.request("b");
    const auto result = f.call("b", "exit_ledger", request);
    QCOMPARE(result.value("balance").toString(), QStringLiteral("-30.00"));
    QCOMPARE(result.value("settlement_deadline").toString(), QStringLiteral("2080-01-01"));
    QCOMPARE(f.call("a", "ledger_details").value("members").toArray().size(), 2);
    QVERIFY(f.call("b", "ledger_details").value("historical").toBool());
    QCOMPARE(f.call("b", "expense_list").value("expenses").toArray().size(), 1);
    QVERIFY(!f.call("b", "expense_detail", {{"expense_id", record.value("id")}})
                 .value("can_edit")
                 .toBool());
    const int logs = f.count("operation_logs");
    QVERIFY(f.call("b", "exit_ledger", request).value("reused").toBool());
    QCOMPARE(f.count("operation_logs"), logs);
    auto overview = f.call("a", "settlement_overview");
    f.call("a", "settlement_repay",
           {{"payer_id", "b"},
            {"recipient_id", "a"},
            {"amount", "20"},
            {"paid_at", f.now},
            {"request_key", "former-repay"},
            {"balance_token", overview.value("balance_token")}});
    QCOMPARE(f.call("a", "settlement_overview").value("total_repayments").toString(),
             QStringLiteral("20.00"));
}
void MembershipTests::noHistoryExitRevokesEveryReadAndWrite() {
    Fixture f;
    const auto record = f.seed();
    auto request = f.request("b", false);
    request.remove("settlement_deadline");
    f.call("b", "exit_ledger", request);
    QCOMPARE(f.call("b", "list_ledgers").value("ledgers").toArray().size(), 0);
    for (const auto& method : QStringList{
             "ledger_details", "expense_list", "expense_detail", "expense_history", "finance_info",
             "finance_event", "expense_submit", "expense_save_draft", "expense_review",
             "expense_withdraw", "expense_lock_acquire", "expense_notifications",
             "expense_notice_read", "settlement_overview", "settlement_create", "settlement_detail",
             "settlement_repay", "invite", "change_role"}) {
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", method,
                                                              {{"expense_id", record.value("id")},
                                                               {"source_id", record.value("id")}}));
    }
    QCOMPARE(f.call("a", "expense_list").value("expenses").toArray().size(), 1);
}
void MembershipTests::laterBackdatedRecordsAndDraftPublicationAreDenied() {
    Fixture f;
    f.seed();
    const auto draft = f.call("a", "expense_save_draft", f.form("Private draft", "10"));
    f.call("b", "exit_ledger", f.request("b"));
    auto late = f.form("Backdated new entry", "20");
    late["date"] = "2020-01-01";
    late["splits"] = QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}}};
    const auto record = f.call("a", "expense_submit", late).value("expense").toObject();
    auto publish = late;
    publish["description"] = "Private draft";
    publish["expense_id"] = draft.value("id");
    publish["version"] = draft.value("version");
    publish["lock_token"] = draft.value("lock_token");
    f.call("a", "expense_submit", publish);
    QCOMPARE(f.call("b", "expense_list").value("expenses").toArray().size(), 1);
    for (const auto& id : QJsonArray{record.value("id"), draft.value("id")}) {
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 f.call("b", "expense_detail", {{"expense_id", id}}));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 f.call("b", "finance_info", {{"source_id", id}}));
    }
    for (const auto& method : QStringList{"expense_submit", "expense_save_draft", "expense_review",
                                          "expense_lock_acquire", "finance_refund",
                                          "finance_adjustment", "settlement_overview"}) {
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", method, late));
    }
}
void MembershipTests::laterRefundAndRevisionDoNotLeakIntoHistory() {
    Fixture f;
    const auto record = f.seed();
    const auto pending =
        f.call("a", "expense_submit", f.form("Pending", "10")).value("expense").toObject();
    f.call("b", "exit_ledger", f.request("b"));
    const auto info = f.call("a", "finance_info", {{"source_id", record.value("id")}});
    const auto refund = f.call("a", "finance_refund",
                               {{"source_id", record.value("id")},
                                {"amount", "30"},
                                {"receiver_id", "a"},
                                {"occurred_at", f.now},
                                {"reason", "Return"},
                                {"expected_token", info.value("token")},
                                {"request_key", "refund"}});
    const auto saved = f.call("b", "finance_info", {{"source_id", record.value("id")}});
    QCOMPARE(saved.value("net_amount").toString(), QStringLiteral("90.00"));
    QVERIFY(saved.value("events").toArray().isEmpty());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("b", "finance_event", {{"event_id", refund.value("id")}}));
    const auto lock = f.call("a", "expense_lock_acquire", {{"expense_id", pending.value("id")}});
    auto edit = f.form("Pending changed", "15");
    edit["splits"] = QJsonArray{QJsonObject{{"user_id", "a"}, {"value", ""}}};
    edit["expense_id"] = pending.value("id");
    edit["version"] = pending.value("version");
    edit["lock_token"] = lock.value("lock_token");
    edit["change_reason"] = "Correct receipt";
    f.call("a", "expense_submit", edit);
    QCOMPARE(f.call("b", "expense_detail", {{"expense_id", pending.value("id")}})
                 .value("form")
                 .toObject()
                 .value("amount")
                 .toString(),
             QStringLiteral("10"));
    QVERIFY(f.call("b", "expense_history", {{"expense_id", pending.value("id")}})
                .value("revisions")
                .toArray()
                .isEmpty());
}
void MembershipTests::adminTransferAndExitAreAtomic() {
    Fixture f;
    f.seed();
    const auto request = f.request("a", true, "b");
    f.call("a", "exit_ledger", request);
    QCOMPARE(f.call("b", "ledger_details").value("role").toString(), QStringLiteral("admin"));
    QVERIFY(f.call("a", "ledger_details").value("historical").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "change_role", {{"user_id", "c"}, {"role", "member"}}));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "invite", {{"username", "outside"}, {"role", "member"}}));
    f.call("b", "invite", {{"username", "outside"}, {"role", "member"}});
    QCOMPARE(f.count("membership_events"), 1);
    QVERIFY(f.call("a", "exit_ledger", request).value("reused").toBool());
}
void MembershipTests::standaloneTransferPromotesCurrentReadOnlyMember() {
    Fixture f;
    auto request = f.request("a", false, "c");
    request["settlement_deadline"] = "";
    f.call("a", "transfer_admin", request);
    QCOMPARE(f.call("c", "ledger_details").value("role").toString(), QStringLiteral("admin"));
    QCOMPARE(f.call("a", "ledger_details").value("role").toString(), QStringLiteral("member"));
    QVERIFY(f.call("a", "transfer_admin", request).value("reused").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "transfer_admin", f.request("a", false, "b")));
}
void MembershipTests::invalidSuccessorStalePreviewAndConfirmationAreRejected() {
    Fixture f;
    auto request = f.request("a", true, "");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "exit_ledger", request));
    request = f.request("a", true, "outside");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "exit_ledger", request));
    auto stale = f.request("b");
    f.call("a", "change_role", {{"user_id", "c"}, {"role", "member"}});
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", "exit_ledger", stale));
    request = f.request("b");
    request["confirmed"] = false;
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", "exit_ledger", request));
    request = f.request("b");
    request["settlement_deadline"] = "2020-01-01";
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", "exit_ledger", request));
    f.call("c", "exit_ledger", f.request("c", false));
    f.call("b", "exit_ledger", f.request("b", false));
    QVERIFY(!f.call("a", "membership_exit_preview").value("can_exit").toBool());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "exit_ledger", f.request("a")));
}
void MembershipTests::failuresRollBackTransferExitAndSnapshots() {
    Fixture f;
    f.seed();
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_exit BEFORE INSERT ON operation_logs WHEN "
                 "NEW.action='exit_ledger' BEGIN SELECT RAISE(ABORT,'Injected'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                             f.call("a", "exit_ledger", f.request("a", true, "b")));
    QCOMPARE(f.call("a", "ledger_details").value("role").toString(), QStringLiteral("admin"));
    QCOMPARE(f.call("b", "ledger_details").value("role").toString(), QStringLiteral("member"));
    QCOMPARE(f.count("membership_history"), 0);
    QCOMPARE(f.count("membership_events"), 0);
    QVERIFY(sql.exec("DROP TRIGGER fail_exit"));
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_snapshot BEFORE INSERT ON membership_history BEGIN SELECT "
                 "RAISE(ABORT,'Injected'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.call("b", "exit_ledger", f.request("b")));
    QCOMPARE(f.call("b", "ledger_details").value("role").toString(), QStringLiteral("member"));
}
void MembershipTests::rejoinThenExitReplacesAuthorizedScope() {
    Fixture f;
    f.seed();
    f.call("b", "exit_ledger", f.request("b"));
    f.join("b");
    QCOMPARE(f.count("membership_history"), 0);
    QCOMPARE(f.call("b", "ledger_details").value("role").toString(), QStringLiteral("member"));
    const auto fresh = f.call("a", "expense_submit", f.form("New authorized entry", "12"))
                           .value("expense")
                           .toObject();
    auto request = f.request("b");
    request["request_key"] = "second-exit";
    f.call("b", "exit_ledger", request);
    QCOMPARE(f.call("b", "expense_list").value("expenses").toArray().size(), 2);
    QVERIFY(!f.call("b", "expense_detail", {{"expense_id", fresh.value("id")}}).isEmpty());
}
void MembershipTests::archiveRestoreAndDeletionRespectHistoricalScope() {
    Fixture f;
    f.seed();
    f.call("b", "exit_ledger", f.request("b"));
    f.call("c", "exit_ledger", f.request("c", false));
    auto preview = f.call("a", "ledger_close_preview");
    const QJsonObject archive{{"preview_token", preview.value("preview_token")},
                              {"mode", "archive"},
                              {"confirmed", true},
                              {"confirm_name", "Membership book"}};
    f.call("a", "ledger_close", archive);
    QCOMPARE(f.call("b", "expense_list").value("expenses").toArray().size(), 1);
    f.call("a", "restore_ledger",
           {{"close_token", preview.value("preview_token")}, {"confirmed", true}});
    preview = f.call("a", "ledger_close_preview");
    f.call("a", "ledger_close",
           {{"preview_token", preview.value("preview_token")},
            {"mode", "delete"},
            {"confirmed", true},
            {"confirm_name", "Membership book"}});
    QCOMPARE(f.count("membership_history"), 0);
    QCOMPARE(f.count("membership_events"), 0);
    QVERIFY(f.count("operation_logs") > 0);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", "expense_list"));
}
QTEST_GUILESS_MAIN(MembershipTests)
#include "membership_tests.moc"
