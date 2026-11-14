#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
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
        for (const auto& user : QStringList{"a", "b", "c", "outsider"}) {
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
        id = call("a", "create_ledger", {{"name", "Query audit book"}}).value("id").toString();
        for (const auto& user : QStringList{"b", "c"}) {
            const auto invitation =
                call("a", "invite",
                     {{"username", user}, {"role", user == "c" ? "read_only" : "member"}});
            call(user, "accept_invitation", {{"invitation_id", invitation.value("id")}});
        }
    }
    QJsonObject call(const QString& actor, const QString& method, QJsonObject parameters = {}) {
        if (!parameters.contains("ledger_id")) {
            parameters["ledger_id"] = id;
        }
        return service->dispatch(method, parameters, actor.leftJustified(64, '0'));
    }
    QJsonObject form(const QString& description = "Shared", const QString& amount = "90",
                     const QString& bearer = "b") {
        return {{"amount", amount},
                {"date", "2026-11-10"},
                {"description", description},
                {"category", "办公"},
                {"payer_id", "a"},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", bearer}, {"value", ""}}}}};
    }
    QJsonObject submit(QString description = "Shared", QString amount = "90",
                       QString bearer = "b") {
        return call("a", "expense_submit", form(description, amount, bearer))
            .value("expense")
            .toObject();
    }
    void approve(const QJsonObject& row) {
        call("a", "expense_review",
             {{"expense_id", row.value("id")},
              {"version", row.value("version")},
              {"decision", "approve"},
              {"reason", "Checked"}});
    }
    void exit(const QString& actor, bool keep) {
        const auto preview = call(actor, "membership_exit_preview");
        call(actor, "exit_ledger",
             {{"preview_token", preview.value("preview_token")},
              {"confirmed", true},
              {"keep_history", keep},
              {"request_key", actor + "-exit"}});
    }
    int count(const QString& table) {
        QSqlQuery sql(database.connection());
        if (!sql.exec("SELECT COUNT(*) FROM " + table) || !sql.next()) {
            throw std::runtime_error("Count");
        }
        return sql.value(0).toInt();
    }
    void raw(int number, const QString& status = "approved") {
        QSqlQuery sql(database.connection());
        const auto payload = form(QStringLiteral("Item %1").arg(number), "10.01");
        sql.prepare(
            "INSERT INTO "
            "expenses(id,ledger_id,creator_id,payer_id,amount_cents,expense_date,description,"
            "category,legacy_expense_type,split_method,status,payload,similar_key,version,created_"
            "at,updated_at) "
            "VALUES(?,?,'a','a',1001,'2026-11-10',?,'办公','','equal',?,?,'',1,?,?)");
        const QString record = QStringLiteral("raw-%1").arg(number, 5, 10, QLatin1Char('0'));
        sql.addBindValue(record);
        sql.addBindValue(id);
        sql.addBindValue(payload.value("description").toString());
        sql.addBindValue(status);
        sql.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        sql.addBindValue(now + number);
        sql.addBindValue(now + number);
        if (!sql.exec()) {
            throw std::runtime_error("Seed expense");
        }
        sql.prepare("INSERT INTO expense_shares VALUES(?,\'b\',1001)");
        sql.addBindValue(record);
        if (!sql.exec()) {
            throw std::runtime_error("Seed shares");
        }
    }
};
QJsonObject logFor(const QJsonObject& response, const QString& action) {
    for (const auto& value : response.value("logs").toArray()) {
        const auto item = value.toObject();
        if (item.value("action").toString() == action) {
            return item;
        }
    }
    return {};
}
}  // namespace
class QueryAuditTests final : public QObject {
    Q_OBJECT
private slots:
    void combineInclusiveFiltersAndLiteralKeyword();
    void rejectInvalidRangesAndPagination();
    void auditFiltersAndPaginationAreCombined();
    void paginationSummaryIncludesAllMatches();
    void privateDraftAndHistoricalOptionsStayScoped();
    void statsExcludeWithdrawnAndReplacedAndDeductRefunds();
    void relatedAudienceIncludesRemovedBearerAndExactBeforeAfter();
    void draftLogsRemainPrivateEvenFromOtherAdmin();
    void nonAdminScopeObjectAndLedgerForgeryAreRejected();
    void relatedLogsAndScopeSurviveDeletion();
    void rawLogsEvidenceAndAudienceAreImmutable();
    void evidenceFailureRollsBackExpenseAndRawLog();
    void migrationRetainsEvidenceAndExactExitCutoff();
    void thousandRecordQueryThreeRunBenchmark();
};
void QueryAuditTests::combineInclusiveFiltersAndLiteralKeyword() {
    Fixture f;
    const auto record = f.submit("100% office_item", "90");
    f.approve(record);
    f.submit("Other", "91", "c");
    const auto result = f.call("b", "query_expenses",
                               {{"date_from", "2026-11-10"},
                                {"date_to", "2026-11-10"},
                                {"amount_min", "90"},
                                {"amount_max", "90.00"},
                                {"category", "办公"},
                                {"payer_id", "a"},
                                {"description", "% OFFICE_"},
                                {"status", "approved"}});
    QCOMPARE(result.value("total").toInt(), 1);
    QCOMPARE(result.value("expenses").toArray()[0].toObject().value("id"), record.value("id"));
    QCOMPARE(result.value("summary").toObject().value("net_amount").toString(),
             QStringLiteral("90.00"));
    QCOMPARE(
        f.call("b", "query_expenses", {{"description", "%' OR 1=1 --"}}).value("total").toInt(), 0);
}
void QueryAuditTests::rejectInvalidRangesAndPagination() {
    Fixture f;
    for (const auto& condition :
         QList<QJsonObject>{{{"date_from", "2026-02-30"}},
                            {{"date_from", "2026-12-01"}, {"date_to", "2026-01-01"}},
                            {{"amount_min", "10"}, {"amount_max", "1"}},
                            {{"amount_min", "-1"}},
                            {{"page", 0}},
                            {{"page_size", 201}},
                            {{"page", 1.5}},
                            {{"description", true}},
                            {{"status", "normal"}}}) {
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "query_expenses", condition));
    }
}
void QueryAuditTests::paginationSummaryIncludesAllMatches() {
    Fixture f;
    for (int n = 0; n < 75; ++n) {
        f.raw(n);
    }
    const auto result = f.call("b", "query_expenses", {{"page", 2}, {"page_size", 20}});
    QCOMPARE(result.value("expenses").toArray().size(), 20);
    QCOMPARE(result.value("total").toInt(), 75);
    QCOMPARE(result.value("pages").toInt(), 4);
    QCOMPARE(result.value("summary").toObject().value("net_amount").toString(),
             QStringLiteral("750.75"));
    QCOMPARE(result.value("categories").toArray()[0].toObject().value("count").toInt(), 75);
    QCOMPARE(f.call("b", "query_expenses", {{"page", 5}, {"page_size", 20}})
                 .value("expenses")
                 .toArray()
                 .size(),
             0);
}
void QueryAuditTests::privateDraftAndHistoricalOptionsStayScoped() {
    Fixture f;
    f.submit();
    auto secret = f.form("Secret", "12", "a");
    secret["category"] = "private-category";
    f.call("a", "expense_save_draft", secret);
    QCOMPARE(f.call("b", "query_expenses").value("total").toInt(), 1);
    QVERIFY(
        !f.call("b", "query_options").value("categories").toArray().contains("private-category"));
    f.exit("b", true);
    auto later = f.form("Backdated", "20", "a");
    later["date"] = "2020-01-01";
    f.call("a", "expense_submit", later);
    QCOMPARE(f.call("b", "query_expenses", {{"date_from", "2010-01-01"}}).value("total").toInt(),
             1);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("outsider", "query_expenses"));
}
void QueryAuditTests::statsExcludeWithdrawnAndReplacedAndDeductRefunds() {
    Fixture f;
    const auto record = f.submit();
    f.approve(record);
    auto overview = f.call("a", "settlement_overview");
    f.call("a", "settlement_create", {{"preview_token", overview.value("preview_token")}});
    const auto info = f.call("a", "finance_info", {{"source_id", record.value("id")}});
    f.call("a", "finance_refund",
           {{"source_id", record.value("id")},
            {"receiver_id", "a"},
            {"amount", "30"},
            {"occurred_at", f.now},
            {"reason", "Return"},
            {"request_key", "refund"},
            {"expected_token", info.value("token")}});
    auto correction = f.form("Corrected", "120");
    correction["kind"] = "correction";
    correction["source_id"] = record.value("id");
    correction["expected_token"] =
        f.call("a", "finance_info", {{"source_id", record.value("id")}}).value("token");
    correction["reason"] = "Recheck receipt";
    correction["request_key"] = "correct";
    const auto preview = f.call("a", "finance_adjustment_preview", correction);
    correction["confirmed"] = true;
    correction["confirmation_token"] = preview.value("confirmation_token");
    f.call("a", "finance_adjustment", correction);
    const auto withdrawn = f.submit("Cancelled", "10");
    f.call("a", "expense_withdraw",
           {{"expense_id", withdrawn.value("id")},
            {"version", withdrawn.value("version")},
            {"confirmed", true},
            {"reason", "Cancel"}});
    const auto result = f.call("b", "query_expenses");
    QCOMPARE(result.value("total").toInt(), 2);
    for (const auto& value : result.value("expenses").toArray()) {
        const auto original = value.toObject();
        if (original.value("id") == record.value("id")) {
            QCOMPARE(original.value("net_amount").toString(), QStringLiteral("60.00"));
            QCOMPARE(original.value("refunded_amount").toString(), QStringLiteral("30.00"));
        }
    }
    const auto stats = result.value("summary").toObject();
    QCOMPARE(stats.value("count").toInt(), 1);
    QCOMPARE(stats.value("gross_amount").toString(), QStringLiteral("120.00"));
    QCOMPARE(stats.value("net_amount").toString(), QStringLiteral("90.00"));
    QCOMPARE(stats.value("refund_amount").toString(), QStringLiteral("30.00"));
    const auto voids = f.call("b", "query_expenses", {{"status", "withdrawn"}});
    QCOMPARE(voids.value("total").toInt(), 1);
    QCOMPARE(voids.value("summary").toObject().value("count").toInt(), 0);
}
void QueryAuditTests::relatedAudienceIncludesRemovedBearerAndExactBeforeAfter() {
    Fixture f;
    const auto record = f.submit();
    QVERIFY(!logFor(f.call("b", "audit_list"), "submit_expense").isEmpty());
    QVERIFY(logFor(f.call("c", "audit_list"), "submit_expense").isEmpty());
    const auto lock = f.call("a", "expense_lock_acquire", {{"expense_id", record.value("id")}});
    auto edit = f.form("Changed", "120", "c");
    edit["expense_id"] = record.value("id");
    edit["version"] = record.value("version");
    edit["lock_token"] = lock.value("lock_token");
    edit["change_reason"] = "Correct bearer";
    f.call("a", "expense_submit", edit);
    const auto b = logFor(f.call("b", "audit_list"), "revise_expense"),
               c = logFor(f.call("c", "audit_list"), "revise_expense");
    QVERIFY(!b.isEmpty() && !c.isEmpty());
    QCOMPARE(b.value("before_amount").toString(), QStringLiteral("90"));
    QCOMPARE(b.value("after_amount").toString(), QStringLiteral("120"));
    QCOMPARE(b.value("reason").toString(), QStringLiteral("Correct bearer"));
    QVERIFY(logFor(f.call("c", "audit_list"), "submit_expense").isEmpty());
    f.approve(f.call("a", "expense_detail", {{"expense_id", record.value("id")}}));
    QVERIFY(!logFor(f.call("c", "audit_list"), "approve_expense").isEmpty());
}
void QueryAuditTests::draftLogsRemainPrivateEvenFromOtherAdmin() {
    Fixture f;
    const auto draft = f.call("b", "expense_save_draft", f.form("Private draft", "25"));
    auto edited = f.form("Private draft", "30");
    edited["expense_id"] = draft.value("id");
    edited["version"] = draft.value("version");
    edited["lock_token"] = draft.value("lock_token");
    f.call("b", "expense_save_draft", edited);
    const auto change = logFor(f.call("b", "audit_list"), "save_expense_draft");
    QCOMPARE(change.value("before_amount").toString(), QStringLiteral("25"));
    QCOMPARE(change.value("after_amount").toString(), QStringLiteral("30"));
    QVERIFY(!logFor(f.call("b", "audit_list"), "save_expense_draft").isEmpty());
    QVERIFY(logFor(f.call("a", "audit_list", {{"scope", "all"}}), "save_expense_draft").isEmpty());
    const auto log = logFor(f.call("b", "audit_list"), "save_expense_draft");
    QVERIFY_THROWS_EXCEPTION(
        ledger::ServiceError,
        f.call("a", "audit_detail", {{"scope", "all"}, {"log_id", log.value("id")}}));
    f.exit("b", false);
    f.exit("c", false);
    const auto close = f.call("a", "ledger_close_preview");
    f.call("a", "ledger_close",
           {{"mode", "delete"},
            {"confirmed", true},
            {"confirm_name", "Query audit book"},
            {"preview_token", close.value("preview_token")}});
    QVERIFY(logFor(f.call("a", "closed_ledger_logs"), "save_expense_draft").isEmpty());
}
void QueryAuditTests::nonAdminScopeObjectAndLedgerForgeryAreRejected() {
    Fixture f;
    const auto unrelated = f.submit("Only C", "10", "c");
    Q_UNUSED(unrelated);
    const auto hidden = logFor(f.call("a", "audit_list"), "submit_expense");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("b", "audit_list", {{"scope", "all"}}));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("b", "audit_detail", {{"log_id", hidden.value("id")}}));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("outsider", "audit_list"));
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("a", "audit_list", {{"ledger_id", "other"}}));
    const auto management = logFor(f.call("a", "audit_list"), "create_ledger");
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             f.call("b", "audit_detail", {{"log_id", management.value("id")}}));
}
void QueryAuditTests::relatedLogsAndScopeSurviveDeletion() {
    Fixture f;
    const auto record = f.submit();
    f.approve(record);
    const auto before = f.call("b", "audit_list");
    const int related = before.value("total").toInt();
    QVERIFY(related >= 2);
    f.exit("b", true);
    const auto same = f.call("a", "expense_detail", {{"expense_id", record.value("id")}});
    f.call("a", "expense_withdraw",
           {{"expense_id", record.value("id")},
            {"version", same.value("version")},
            {"confirmed", true},
            {"reason", "After exit"}});
    QCOMPARE(f.call("b", "audit_list").value("total").toInt(), related);
    f.exit("c", false);
    const auto preview = f.call("a", "ledger_close_preview");
    f.call("a", "ledger_close",
           {{"mode", "delete"},
            {"confirmed", true},
            {"confirm_name", "Query audit book"},
            {"preview_token", preview.value("preview_token")}});
    QCOMPARE(f.count("expenses"), 0);
    QVERIFY(f.count("audit_details") > 0);
    QCOMPARE(f.call("b", "audit_list").value("total").toInt(), related);
    QVERIFY(!f.call("b", "audit_ledgers").value("ledgers").toArray().isEmpty());
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("c", "audit_list"));
    QVERIFY(!logFor(f.call("a", "audit_list"), "delete_ledger_data").isEmpty());
}
void QueryAuditTests::rawLogsEvidenceAndAudienceAreImmutable() {
    Fixture f;
    f.submit();
    QSqlQuery sql(f.database.connection());
    for (const auto& table : QStringList{"operation_logs", "audit_details", "audit_audience"}) {
        QVERIFY(!sql.exec("DELETE FROM " + table));
        QVERIFY(!sql.exec("UPDATE " + table + " SET " +
                          (table == "operation_logs"  ? "detail='changed'"
                           : table == "audit_details" ? "payload='{}'"
                                                      : "user_id='a'")));
    }
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, f.call("a", "audit_delete"));
}
void QueryAuditTests::evidenceFailureRollsBackExpenseAndRawLog() {
    Fixture f;
    const int logs = f.count("operation_logs");
    QSqlQuery sql(f.database.connection());
    QVERIFY(
        sql.exec("CREATE TRIGGER fail_evidence BEFORE INSERT ON audit_details BEGIN SELECT "
                 "RAISE(ABORT,'Injected'); END"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, f.submit());
    QCOMPARE(f.count("expenses"), 0);
    QCOMPARE(f.count("operation_logs"), logs);
}
void QueryAuditTests::thousandRecordQueryThreeRunBenchmark() {
    Fixture f;
    QVERIFY(f.database.connection().transaction());
    for (int n = 0; n < 1000; ++n) {
        f.raw(n);
    }
    QVERIFY(f.database.connection().commit());
    qint64 elapsed = 0;
    for (int run = 0; run < 3; ++run) {
        QElapsedTimer timer;
        timer.start();
        const auto result = f.call("b", "query_expenses",
                                   {{"date_from", "2026-01-01"},
                                    {"amount_min", "10"},
                                    {"category", "办公"},
                                    {"page_size", 200}});
        elapsed += timer.elapsed();
        QCOMPARE(result.value("total").toInt(), 1000);
        QCOMPARE(result.value("expenses").toArray().size(), 200);
        QCOMPARE(result.value("summary").toObject().value("net_amount").toString(),
                 QStringLiteral("10010.00"));
    }
    qInfo() << "1000 record service query, three-run mean (ms):" << elapsed / 3;
    QVERIFY2(elapsed / 3 <= 2000, "1000-row service query must average at most two seconds.");
}
void QueryAuditTests::migrationRetainsEvidenceAndExactExitCutoff() {
    QTemporaryDir directory;
    const auto path = directory.filePath("old-audit.sqlite3");
    {
        ledger::Database database(path);
        database.open();
        QSqlQuery sql(database.connection());
        for (const auto& user : QStringList{"a", "b"}) {
            sql.prepare("INSERT INTO users VALUES(?,?,?,X'01',X'02',600000,1)");
            sql.addBindValue(user);
            sql.addBindValue(user);
            sql.addBindValue(user);
            QVERIFY(sql.exec());
        }
        QVERIFY(sql.exec(
            "INSERT INTO ledgers(id,name,created_by,created_at) VALUES('book','Old book','a',1)"));
        QVERIFY(
            sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                     "VALUES('book','a','admin',1)"));
        QVERIFY(
            sql.exec("INSERT INTO "
                     "memberships(ledger_id,user_id,role,joined_at,status,keep_history,exited_at) "
                     "VALUES('book','b','member',1,'exited',1,2)"));
        const auto hash = QCryptographicHash::hash(QString("b").leftJustified(64, '0').toUtf8(),
                                                   QCryptographicHash::Sha256);
        sql.prepare("INSERT INTO sessions VALUES(?,'b',1000)");
        sql.addBindValue(hash);
        QVERIFY(sql.exec());
        sql.prepare(
            "INSERT INTO "
            "expenses(id,ledger_id,creator_id,payer_id,amount_cents,expense_date,description,"
            "category,legacy_expense_type,split_method,status,payload,similar_key,version,created_"
            "at,updated_at) "
            "VALUES('e','book','a','a',9000,'2026-11-10','Old','','','equal','pending',?,'',1,1,"
            "1)");
        sql.addBindValue(QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{"amount", "90"},
                            {"payer_id", "a"},
                            {"description", "Old"},
                            {"splits", QJsonArray{QJsonObject{{"user_id", "b"}, {"value", ""}}}}})
                .toJson(QJsonDocument::Compact)));
        QVERIFY(sql.exec());
        QVERIFY(
            sql.exec("INSERT INTO operation_logs "
                     "VALUES('before','book','a','submit_expense','e','Original evidence',1)"));
        QVERIFY(sql.exec(
            "INSERT INTO operation_logs VALUES('exit','book','b','exit_ledger','b','Exit',2)"));
        QVERIFY(sql.exec(
            "INSERT INTO operation_logs VALUES('after','book','a','approve_expense','e','After "
            "exit same second',2)"));
        for (const auto& statement : QStringList{
                 "DROP TABLE account_recovery", "DROP TABLE account_security_events",
                 "DROP TABLE expense_requests", "DROP TABLE audit_audience",
                 "DROP TABLE audit_details", "DROP INDEX logs_query", "DROP INDEX expenses_query",
                 "ALTER TABLE memberships DROP COLUMN audit_cutoff", "PRAGMA user_version=9"}) {
            QVERIFY(sql.exec(statement));
        }
    }
    ledger::Database database(path);
    database.open();
    QCOMPARE(database.schemaVersion(), 12);
    ledger::IdentityService service(database, [] { return qint64(10); });
    const auto result = service.dispatch("audit_list", {{"ledger_id", "book"}},
                                         QString("b").leftJustified(64, '0'));
    QCOMPARE(result.value("total").toInt(), 1);
    QCOMPARE(result.value("logs").toArray()[0].toObject().value("detail").toString(),
             QStringLiteral("Original evidence"));
    QSqlQuery sql(database.connection());
    QVERIFY(sql.exec("SELECT COUNT(*) FROM operation_logs"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 3);
    QVERIFY(!sql.exec("DELETE FROM audit_details"));
    QVERIFY(!sql.exec("DELETE FROM operation_logs"));
}
void QueryAuditTests::auditFiltersAndPaginationAreCombined() {
    Fixture f;
    const auto record = f.submit();
    f.approve(record);
    const auto day = QDateTime::fromSecsSinceEpoch(f.now).date().toString(Qt::ISODate);
    QJsonObject condition{{"date_from", day},
                          {"date_to", day},
                          {"actor_id", "a"},
                          {"action", "approve_expense"},
                          {"keyword", "checked"}};
    const auto result = f.call("a", "audit_list", condition);
    QCOMPARE(result.value("total").toInt(), 1);
    condition["actor_id"] = "b";
    QCOMPARE(f.call("a", "audit_list", condition).value("total").toInt(), 0);
    const auto first = f.call("a", "audit_list", {{"page_size", 1}}),
               second = f.call("a", "audit_list", {{"page_size", 1}, {"page", 2}});
    QCOMPARE(first.value("total"), second.value("total"));
    QCOMPARE(first.value("logs").toArray().size(), 1);
    QVERIFY(first.value("logs").toArray()[0].toObject().value("id") !=
            second.value("logs").toArray()[0].toObject().value("id"));
}
QTEST_GUILESS_MAIN(QueryAuditTests)
#include "query_audit_tests.moc"
