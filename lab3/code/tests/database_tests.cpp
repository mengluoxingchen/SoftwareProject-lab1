#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <stdexcept>

#include "infrastructure/database.h"

namespace {
bool removeAuditSchema(QSqlQuery& query) {
    if (!query.exec("DROP TABLE account_recovery") ||
        !query.exec("DROP TABLE account_security_events"))
        return false;
    return query.exec("DROP TABLE expense_requests") && query.exec("DROP TABLE audit_audience") &&
           query.exec("DROP TABLE audit_details") && query.exec("DROP INDEX logs_query") &&
           query.exec("DROP INDEX expenses_query") &&
           query.exec("ALTER TABLE memberships DROP COLUMN audit_cutoff");
}
bool removeMembershipSchema(QSqlQuery& query) {
    if (!removeAuditSchema(query)) {
        return false;
    }
    return query.exec("DROP TABLE membership_history") &&
           query.exec("DROP TABLE membership_events") &&
           query.exec("ALTER TABLE memberships DROP COLUMN settlement_deadline") &&
           query.exec("ALTER TABLE memberships DROP COLUMN history_context");
}
bool removeFinanceSchema(QSqlQuery& query) {
    if (!removeMembershipSchema(query)) {
        return false;
    }
    return query.exec("DROP TABLE expense_finance_events") &&
           query.exec("DROP INDEX expenses_financial_root") &&
           query.exec("DROP INDEX unique_submitted_expense") &&
           query.exec("DROP INDEX one_approved_similar_expense") &&
           query.exec("ALTER TABLE expenses DROP COLUMN adjustment_reason") &&
           query.exec("ALTER TABLE expenses DROP COLUMN financial_root_id") &&
           query.exec("ALTER TABLE expenses DROP COLUMN is_adjustment") &&
           query.exec("ALTER TABLE expenses DROP COLUMN superseded") &&
           query.exec(
               "CREATE UNIQUE INDEX unique_submitted_expense ON "
               "expenses(ledger_id,creator_id,expense_date,amount_cents,payer_id,description) "
               "WHERE status<>'draft'") &&
           query.exec(
               "CREATE UNIQUE INDEX one_approved_similar_expense ON "
               "expenses(ledger_id,similar_key) WHERE status IN ('approved','settled') AND "
               "similar_key<>''");
}
bool removeLedgerClosureColumns(QSqlQuery& query) {
    if (!removeFinanceSchema(query)) {
        return false;
    }
    return query.exec("ALTER TABLE ledgers DROP COLUMN close_token") &&
           query.exec("ALTER TABLE ledgers DROP COLUMN closed_by") &&
           query.exec("ALTER TABLE ledgers DROP COLUMN closed_at");
}
bool removeSettlementTables(QSqlQuery& query) {
    if (!removeLedgerClosureColumns(query)) {
        return false;
    }
    for (const QString& table : QStringList{"settlement_expenses", "settlements", "repayments"}) {
        if (!query.exec("DROP TABLE " + table)) {
            return false;
        }
    }
    return true;
}
bool removeWorkflowTables(QSqlQuery& query) {
    if (!removeSettlementTables(query)) {
        return false;
    }
    for (const QString& table : QStringList{"expense_locks", "expense_notices", "expense_reviews",
                                            "expense_revisions", "expense_withdrawals"}) {
        if (!query.exec("DROP TABLE " + table)) {
            return false;
        }
    }
    return query.exec("DROP INDEX one_approved_similar_expense");
}
}  // namespace

class DatabaseTests final : public QObject {
    Q_OBJECT

private slots:
    void upgradesVersionTenPreservingAudit();
    void upgradesVersionEightRetainsMembershipAndFinancialTables();
    void createsAndReopensPersistentDatabase();
    void preservesUnicodeAndTreatsSqlAsData();
    void doesNotEraseExistingMetadata();
    void rejectsNewerDatabaseSchema();
    void upgradesVersionOneWithoutLosingMetadata();
    void upgradesVersionTwoWithoutLosingMemberships();
    void upgradesVersionThreeWithoutChangingExpenseAmounts();
    void upgradesVersionFourWithWorkflowTables();
    void upgradesVersionFiveWithoutChangingExistingHistory();
    void upgradesVersionSixPreservingSettlementAndAudit();
    void upgradesVersionSevenWithoutRewritingExpenses();
};

void DatabaseTests::upgradesVersionTenPreservingAudit() {
    QTemporaryDir dir;
    const auto path = dir.filePath("version10.sqlite3");
    {
        ledger::Database database(path);
        database.open();
        database.setMetadata("marker", "preserved");
        QSqlQuery sql(database.connection());
        QVERIFY(sql.exec("INSERT INTO users VALUES('a','a','a',X'01',X'02',600000,1)"));
        QVERIFY(sql.exec(
            "INSERT INTO ledgers(id,name,created_by,created_at) VALUES('b','Book','a',1)"));
        QVERIFY(sql.exec(
            "INSERT INTO operation_logs VALUES('log','b','a','create_ledger','b','Original',1)"));
        QVERIFY(
            sql.exec("INSERT INTO audit_details "
                     "VALUES('log','b','ledger','','','{\"marker\":\"original\"}')"));
        QVERIFY(sql.exec("INSERT INTO audit_audience VALUES('log','a')"));
        QVERIFY(sql.exec("DROP TABLE expense_requests"));
        QVERIFY(sql.exec("DROP TABLE account_recovery"));
        QVERIFY(sql.exec("DROP TABLE account_security_events"));
        QVERIFY(sql.exec("PRAGMA user_version=10"));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QCOMPARE(upgraded.metadata("marker"), QString("preserved"));
    QSqlQuery sql(upgraded.connection());
    QVERIFY(sql.exec("SELECT payload FROM audit_details WHERE log_id='log'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QString("{\"marker\":\"original\"}"));
    QVERIFY(sql.exec("SELECT COUNT(*) FROM audit_audience"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 1);
    QVERIFY(sql.exec("SELECT COUNT(*) FROM expense_requests"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 0);
}
void DatabaseTests::createsAndReopensPersistentDatabase() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("nested/ledger.sqlite3"));
    {
        ledger::Database database(path);
        database.open();
        QCOMPARE(database.schemaVersion(), 12);
        database.setMetadata(QStringLiteral("persisted"), QStringLiteral("yes"));
    }
    QVERIFY(QFile::exists(path));
    ledger::Database reopened(path);
    reopened.open();
    QCOMPARE(reopened.metadata(QStringLiteral("persisted")), QStringLiteral("yes"));
}

void DatabaseTests::preservesUnicodeAndTreatsSqlAsData() {
    ledger::Database database(QStringLiteral(":memory:"));
    database.open();
    const QString key = QStringLiteral("名称'; DROP TABLE app_metadata; --");
    database.setMetadata(key, QStringLiteral("合租生活账本"));
    QCOMPARE(database.metadata(key), QStringLiteral("合租生活账本"));
    database.setMetadata(QStringLiteral("safe"), QStringLiteral("表仍存在"));
    QCOMPARE(database.metadata(QStringLiteral("safe")), QStringLiteral("表仍存在"));
}

void DatabaseTests::doesNotEraseExistingMetadata() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("ledger.sqlite3"));
    {
        ledger::Database database(path);
        database.open();
        database.setMetadata(QStringLiteral("first"), QStringLiteral("original"));
        database.setMetadata(QStringLiteral("second"), QStringLiteral("before"));
        database.setMetadata(QStringLiteral("second"), QStringLiteral("after"));
        database.open();
    }
    ledger::Database database(path);
    database.open();
    QCOMPARE(database.metadata(QStringLiteral("first")), QStringLiteral("original"));
    QCOMPARE(database.metadata(QStringLiteral("second")), QStringLiteral("after"));
}

void DatabaseTests::rejectsNewerDatabaseSchema() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("future.sqlite3"));
    const QString connection_name = QStringLiteral("future_schema_fixture");
    {
        auto future = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        future.setDatabaseName(path);
        QVERIFY(future.open());
        QSqlQuery query(future);
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 13")));
    }
    QSqlDatabase::removeDatabase(connection_name);
    ledger::Database database(path);
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, database.open());
    QVERIFY(!database.isOpen());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, database.open());
}

void DatabaseTests::upgradesVersionOneWithoutLosingMetadata() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("old.sqlite3"));
    const QString name = QStringLiteral("old_database_fixture");
    {
        auto old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        old.setDatabaseName(path);
        QVERIFY(old.open());
        QSqlQuery query(old);
        QVERIFY(query.exec(
            QStringLiteral("CREATE TABLE app_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)")));
        QVERIFY(
            query.exec(QStringLiteral("INSERT INTO app_metadata VALUES('existing','preserved')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=1")));
    }
    QSqlDatabase::removeDatabase(name);
    ledger::Database database(path);
    database.open();
    QCOMPARE(database.schemaVersion(), 12);
    QCOMPARE(database.metadata(QStringLiteral("existing")), QStringLiteral("preserved"));
    QSqlQuery users(database.connection());
    QVERIFY(users.exec(QStringLiteral("SELECT COUNT(*) FROM users")));
    QVERIFY(users.next());
    QCOMPARE(users.value(0).toInt(), 0);
}

void DatabaseTests::upgradesVersionTwoWithoutLosingMemberships() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("v2.sqlite3"));
    {
        ledger::Database old(path);
        old.open();
        QSqlQuery sql(old.connection());
        QVERIFY(sql.exec(QStringLiteral(
            "INSERT INTO users VALUES('owner','owner','owner',X'01',X'02',600000,1)")));
        QVERIFY(
            sql.exec(QStringLiteral("INSERT INTO ledgers(id,name,created_by,created_at) "
                                    "VALUES('book','Original','owner',1)")));
        QVERIFY(
            sql.exec(QStringLiteral("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                                    "VALUES('book','owner','admin',1)")));
        QVERIFY(removeWorkflowTables(sql));
        QVERIFY(sql.exec(QStringLiteral("DROP TABLE expense_shares")));
        QVERIFY(sql.exec(QStringLiteral("DROP TABLE expenses")));
        QVERIFY(sql.exec(QStringLiteral("PRAGMA user_version=2")));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QSqlQuery sql(upgraded.connection());
    QVERIFY(sql.exec(
        QStringLiteral("SELECT role FROM memberships WHERE ledger_id='book' AND user_id='owner'")));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("admin"));
    QVERIFY(sql.exec(QStringLiteral("SELECT COUNT(*) FROM expenses")));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 0);
}

void DatabaseTests::upgradesVersionThreeWithoutChangingExpenseAmounts() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("v3.sqlite3"));
    {
        ledger::Database old(path);
        old.open();
        old.setMetadata("existing", "preserved");
        QSqlQuery sql(old.connection());
        QVERIFY(removeWorkflowTables(sql));
        QVERIFY(sql.exec("ALTER TABLE expenses RENAME COLUMN legacy_expense_type TO expense_type"));
        QVERIFY(
            sql.exec("INSERT INTO users "
                     "VALUES('alice','alice','alice',X'01',X'02',600000,1),('bob','bob','bob',X'01'"
                     ",X'02',600000,1)"));
        QVERIFY(
            sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                     "VALUES('book','Original','alice',1)"));
        QVERIFY(
            sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                     "VALUES('book','alice','admin',1),('book','bob','member',1)"));
        for (const QString& type : QStringList{"public", "advance"}) {
            const QString status = type == "public" ? "draft" : "pending";
            const QString payload = QString::fromUtf8(
                QJsonDocument(
                    QJsonObject{{"expense_type", type}, {"amount", "90.00"}, {"payer_id", "alice"}})
                    .toJson(QJsonDocument::Compact));
            sql.prepare(
                "INSERT INTO expenses "
                "VALUES(?,'book','alice','alice',9000,'2026-11-09','Original','Office',?,'equal',?,"
                "?,'',7,1,2,2)");
            sql.addBindValue(type);
            sql.addBindValue(type);
            sql.addBindValue(status);
            sql.addBindValue(payload);
            QVERIFY(sql.exec());
        }
        QVERIFY(sql.exec("INSERT INTO expense_shares VALUES('advance','bob',9000)"));
        QVERIFY(
            sql.exec("INSERT INTO operation_logs "
                     "VALUES('log','book','alice','submit_expense','advance','Original',2)"));
        QVERIFY(sql.exec("PRAGMA user_version=3"));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QCOMPARE(upgraded.metadata("existing"), QStringLiteral("preserved"));
    QSqlQuery sql(upgraded.connection());
    QVERIFY(
        sql.exec("SELECT id,legacy_expense_type,payer_id,amount_cents,status,version,payload FROM "
                 "expenses ORDER BY id"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("advance"));
    QCOMPARE(sql.value(1).toString(), QStringLiteral("advance"));
    QCOMPARE(sql.value(2).toString(), QStringLiteral("alice"));
    QCOMPARE(sql.value(3).toLongLong(), 9000LL);
    QCOMPARE(sql.value(4).toString(), QStringLiteral("pending"));
    QCOMPARE(sql.value(5).toInt(), 7);
    QCOMPARE(QJsonDocument::fromJson(sql.value(6).toString().toUtf8())
                 .object()
                 .value("amount")
                 .toString(),
             QStringLiteral("90.00"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("public"));
    QCOMPARE(sql.value(1).toString(), QStringLiteral("public"));
    QCOMPARE(sql.value(4).toString(), QStringLiteral("draft"));
    QVERIFY(!sql.next());
    QVERIFY(sql.exec("SELECT user_id,amount_cents FROM expense_shares WHERE expense_id='advance'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("bob"));
    QCOMPARE(sql.value(1).toLongLong(), 9000LL);
    QVERIFY(sql.exec("SELECT detail FROM operation_logs WHERE id='log'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("Original"));
}

void DatabaseTests::upgradesVersionFourWithWorkflowTables() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("v4.sqlite3");
    {
        ledger::Database old(path);
        old.open();
        old.setMetadata("existing", "v4");
        QSqlQuery sql(old.connection());
        QVERIFY(removeWorkflowTables(sql));
        QVERIFY(sql.exec("PRAGMA user_version=4"));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QCOMPARE(upgraded.metadata("existing"), QStringLiteral("v4"));
    QSqlQuery sql(upgraded.connection());
    QVERIFY(sql.exec("SELECT COUNT(*) FROM expense_reviews"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 0);
    QVERIFY(sql.exec("SELECT legacy_expense_type FROM expenses"));
}

void DatabaseTests::upgradesVersionFiveWithoutChangingExistingHistory() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("v5.sqlite3");
    {
        ledger::Database old(path);
        old.open();
        old.setMetadata("existing", "v5");
        QSqlQuery sql(old.connection());
        QVERIFY(sql.exec("INSERT INTO users VALUES('owner','owner','owner',X'01',X'02',600000,1)"));
        QVERIFY(
            sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                     "VALUES('book','Original','owner',1)"));
        QVERIFY(
            sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                     "VALUES('book','owner','admin',1)"));
        QVERIFY(
            sql.exec("INSERT INTO operation_logs "
                     "VALUES('old-log','book','owner','create_ledger','book','Original',1)"));
        QVERIFY(removeSettlementTables(sql));
        QVERIFY(sql.exec("PRAGMA user_version=5"));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QCOMPARE(upgraded.metadata("existing"), QStringLiteral("v5"));
    QSqlQuery sql(upgraded.connection());
    QVERIFY(sql.exec("SELECT detail FROM operation_logs WHERE id='old-log'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("Original"));
    QVERIFY(sql.exec("SELECT username FROM users WHERE id='owner'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("owner"));
    QVERIFY(sql.exec("SELECT COUNT(*) FROM settlements"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 0);
    QVERIFY(sql.exec("SELECT COUNT(*) FROM expense_reviews"));
}

void DatabaseTests::upgradesVersionSixPreservingSettlementAndAudit() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("v6.sqlite3");
    {
        ledger::Database old(path);
        old.open();
        QSqlQuery sql(old.connection());
        QVERIFY(sql.exec("INSERT INTO users VALUES('owner','owner','owner',X'01',X'02',600000,1)"));
        QVERIFY(
            sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                     "VALUES('book','Original','owner',1)"));
        QVERIFY(
            sql.exec("INSERT INTO operation_logs "
                     "VALUES('log','book','owner','create_ledger','book','Original',1)"));
        QVERIFY(sql.exec(
            "INSERT INTO settlements "
            "VALUES('snapshot','book','owner',1,'token',1,100,'{\"total_expenses\":\"1.00\"}')"));
        QVERIFY(removeLedgerClosureColumns(sql));
        QVERIFY(sql.exec("PRAGMA user_version=6"));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QSqlQuery sql(upgraded.connection());
    QVERIFY(sql.exec("SELECT snapshot FROM settlements WHERE id='snapshot'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("{\"total_expenses\":\"1.00\"}"));
    QVERIFY(sql.exec("SELECT detail FROM operation_logs WHERE id='log'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("Original"));
    QVERIFY(!sql.exec("DELETE FROM settlements"));
    QVERIFY(!sql.exec("DELETE FROM operation_logs"));
    QVERIFY(sql.exec("SELECT closed_at,closed_by,close_token FROM ledgers"));
    QVERIFY(sql.next());
    QVERIFY(sql.value(0).isNull());
    QVERIFY(sql.value(1).isNull());
}

void DatabaseTests::upgradesVersionSevenWithoutRewritingExpenses() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("v7.sqlite3");
    {
        ledger::Database old(path);
        old.open();
        QSqlQuery sql(old.connection());
        QVERIFY(sql.exec("INSERT INTO users VALUES('owner','owner','owner',X'01',X'02',600000,1)"));
        QVERIFY(
            sql.exec("INSERT INTO ledgers(id,name,created_by,created_at) "
                     "VALUES('book','Original','owner',1)"));
        QVERIFY(
            sql.exec("INSERT INTO "
                     "expenses(id,ledger_id,creator_id,payer_id,amount_cents,expense_date,"
                     "description,category,legacy_expense_type,split_method,status,payload,similar_"
                     "key,version,created_at,updated_at) "
                     "VALUES('expense','book','owner','owner',9000,'2026-11-09','Original','','','"
                     "equal','settled','{}','key',3,1,1)"));
        QVERIFY(sql.exec("INSERT INTO expense_shares VALUES('expense','owner',9000)"));
        QVERIFY(removeFinanceSchema(sql));
        QVERIFY(sql.exec("PRAGMA user_version=7"));
    }
    ledger::Database upgraded(path);
    upgraded.open();
    QCOMPARE(upgraded.schemaVersion(), 12);
    QSqlQuery sql(upgraded.connection());
    QVERIFY(
        sql.exec("SELECT amount_cents,status,version,superseded,is_adjustment FROM expenses WHERE "
                 "id='expense'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toLongLong(), 9000LL);
    QCOMPARE(sql.value(1).toString(), QStringLiteral("settled"));
    QCOMPARE(sql.value(2).toInt(), 3);
    QVERIFY(!sql.value(3).toBool());
    QVERIFY(!sql.value(4).toBool());
    QVERIFY(sql.exec("SELECT amount_cents FROM expense_shares WHERE expense_id='expense'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toLongLong(), 9000LL);
}

void DatabaseTests::upgradesVersionEightRetainsMembershipAndFinancialTables() {
    QTemporaryDir directory;
    const auto path = directory.filePath("v8.sqlite3");
    {
        ledger::Database database(path);
        database.open();
        QSqlQuery sql(database.connection());
        QVERIFY(sql.exec("INSERT INTO users VALUES('owner','owner','owner',X'01',X'02',600000,1)"));
        QVERIFY(sql.exec(
            "INSERT INTO ledgers(id,name,created_by,created_at) VALUES('book','Kept','owner',1)"));
        QVERIFY(
            sql.exec("INSERT INTO memberships(ledger_id,user_id,role,joined_at) "
                     "VALUES('book','owner','admin',1)"));
        QVERIFY(removeMembershipSchema(sql));
        QVERIFY(sql.exec("PRAGMA user_version=8"));
    }
    ledger::Database database(path);
    database.open();
    QCOMPARE(database.schemaVersion(), 12);
    QSqlQuery sql(database.connection());
    QVERIFY(sql.exec(
        "SELECT role,settlement_deadline,history_context FROM memberships WHERE user_id='owner'"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toString(), QStringLiteral("admin"));
    QCOMPARE(sql.value(1).toString(), QStringLiteral(""));
    QCOMPARE(sql.value(2).toString(), QStringLiteral("{}"));
    QVERIFY(sql.exec("SELECT COUNT(*) FROM expense_finance_events"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 0);
    QVERIFY(sql.exec("SELECT COUNT(*) FROM membership_history"));
    QVERIFY(sql.next());
    QCOMPARE(sql.value(0).toInt(), 0);
}
QTEST_GUILESS_MAIN(DatabaseTests)
#include "database_tests.moc"
