#include "infrastructure/database.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <stdexcept>
#include <utility>

#include "infrastructure/audit_store.h"

namespace ledger {
namespace {

template <typename SqlObject>
void requireSuccess(bool success, const QString& context, const SqlObject& source) {
    if (!success) {
        throw std::runtime_error(
            (context + QStringLiteral(": ") + source.lastError().text()).toStdString());
    }
}

}  // namespace

Database::Database(QString path)
    : path_(std::move(path)),
      connection_name_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}

Database::~Database() {
    if (connection_.isValid()) {
        connection_.close();
        connection_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_name_);
    }
}

void Database::open() {
    if (isOpen()) {
        return;
    }
    if (path_ != QStringLiteral(":memory:")) {
        const QString directory = QFileInfo(path_).absolutePath();
        if (!QDir().mkpath(directory)) {
            throw std::runtime_error("Cannot create the local data directory.");
        }
    }
    if (!connection_.isValid()) {
        connection_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name_);
    }
    connection_.setDatabaseName(path_);
    connection_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    requireSuccess(connection_.open(), QStringLiteral("Cannot open SQLite database"), connection_);
    try {
        execute(QStringLiteral("PRAGMA foreign_keys = ON"));
        execute(QStringLiteral("PRAGMA journal_mode = WAL"));
        migrate();
    } catch (...) {
        connection_.close();
        throw;
    }
}

bool Database::isOpen() const { return connection_.isValid() && connection_.isOpen(); }

int Database::schemaVersion() const {
    QSqlQuery query(connection_);
    requireSuccess(query.exec(QStringLiteral("PRAGMA user_version")),
                   QStringLiteral("Cannot read schema version"), query);
    if (!query.next()) {
        throw std::runtime_error("SQLite returned no schema version.");
    }
    return query.value(0).toInt();
}

QString Database::metadata(const QString& key) const {
    QSqlQuery query(connection_);
    query.prepare(QStringLiteral("SELECT value FROM app_metadata WHERE key = :key"));
    query.bindValue(QStringLiteral(":key"), key);
    requireSuccess(query.exec(), QStringLiteral("Cannot read application metadata"), query);
    return query.next() ? query.value(0).toString() : QString();
}

void Database::setMetadata(const QString& key, const QString& value) {
    QSqlQuery query(connection_);
    query.prepare(
        QStringLiteral("INSERT INTO app_metadata (key, value) VALUES (:key, :value) "
                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.bindValue(QStringLiteral(":key"), key);
    query.bindValue(QStringLiteral(":value"), value);
    requireSuccess(query.exec(), QStringLiteral("Cannot save application metadata"), query);
}

QSqlDatabase Database::connection() const { return connection_; }

void Database::migrate() {
    const int version = schemaVersion();
    if (version > 12) {
        throw std::runtime_error("The database was created by a newer application version.");
    }
    if (version == 12) {
        return;
    }
    requireSuccess(connection_.transaction(), QStringLiteral("Cannot start database migration"),
                   connection_);
    try {
        if (version == 0) {
            execute(QStringLiteral(
                "CREATE TABLE app_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"));
        }
        if (version < 2) {
            execute(
                QStringLiteral("CREATE TABLE users (id TEXT PRIMARY KEY, username TEXT NOT NULL, "
                               "username_key TEXT NOT NULL UNIQUE, password_salt BLOB NOT NULL, "
                               "password_hash BLOB NOT NULL, password_iterations INTEGER NOT NULL, "
                               "created_at INTEGER NOT NULL)"));
            execute(QStringLiteral(
                "CREATE TABLE ledgers (id TEXT PRIMARY KEY, name TEXT NOT NULL, "
                "created_by TEXT NOT NULL REFERENCES users(id), created_at INTEGER NOT NULL, "
                "status TEXT NOT NULL DEFAULT 'active' CHECK(status IN "
                "('active','archived','deleted')))"));
            execute(QStringLiteral(
                "CREATE TABLE memberships (ledger_id TEXT NOT NULL REFERENCES ledgers(id), "
                "user_id TEXT NOT NULL REFERENCES users(id), role TEXT NOT NULL "
                "CHECK(role IN ('admin','member','read_only')), status TEXT NOT NULL DEFAULT "
                "'active' "
                "CHECK(status IN ('active','exited')), joined_at INTEGER NOT NULL, "
                "exited_at INTEGER, keep_history INTEGER NOT NULL DEFAULT 0, "
                "PRIMARY KEY(ledger_id,user_id))"));
            execute(
                QStringLiteral("CREATE UNIQUE INDEX one_active_admin ON memberships(ledger_id) "
                               "WHERE role='admin' AND status='active'"));
            execute(QStringLiteral(
                "CREATE TABLE invitations (id TEXT PRIMARY KEY, ledger_id TEXT NOT NULL REFERENCES "
                "ledgers(id), target_user_id TEXT NOT NULL REFERENCES users(id), "
                "inviter_id TEXT NOT NULL REFERENCES users(id), role TEXT NOT NULL "
                "CHECK(role IN ('member','read_only')), status TEXT NOT NULL DEFAULT 'pending' "
                "CHECK(status IN ('pending','accepted','revoked')), created_at INTEGER NOT NULL, "
                "accepted_at INTEGER)"));
            execute(
                QStringLiteral("CREATE UNIQUE INDEX one_pending_invitation ON "
                               "invitations(ledger_id,target_user_id) "
                               "WHERE status='pending'"));
            execute(QStringLiteral(
                "CREATE TABLE sessions (token_hash BLOB PRIMARY KEY, user_id TEXT NOT NULL "
                "REFERENCES users(id), expires_at INTEGER NOT NULL)"));
            execute(QStringLiteral(
                "CREATE TABLE operation_logs (id TEXT PRIMARY KEY, ledger_id TEXT NOT NULL, "
                "actor_id TEXT NOT NULL, action TEXT NOT NULL, target_id TEXT NOT NULL, "
                "detail TEXT NOT NULL, created_at INTEGER NOT NULL)"));
            execute(QStringLiteral(
                "CREATE TRIGGER logs_no_update BEFORE UPDATE ON operation_logs BEGIN "
                "SELECT RAISE(ABORT,'Operation logs are immutable'); END"));
            execute(QStringLiteral(
                "CREATE TRIGGER logs_no_delete BEFORE DELETE ON operation_logs BEGIN "
                "SELECT RAISE(ABORT,'Operation logs are immutable'); END"));
        }
        if (version < 3) {
            execute(QStringLiteral(
                "CREATE TABLE expenses(id TEXT PRIMARY KEY, ledger_id TEXT NOT NULL REFERENCES "
                "ledgers(id), "
                "creator_id TEXT NOT NULL REFERENCES users(id), payer_id TEXT REFERENCES "
                "users(id), "
                "amount_cents INTEGER CHECK(amount_cents>0), expense_date TEXT NOT NULL, "
                "description "
                "TEXT NOT NULL, "
                "category TEXT NOT NULL, expense_type TEXT NOT NULL CHECK(expense_type IN "
                "('','public','advance')), "
                "split_method TEXT NOT NULL CHECK(split_method IN ('equal','ratio','fixed')), "
                "status TEXT NOT NULL CHECK(status IN "
                "('draft','pending','approved','rejected','withdrawn','settled')), "
                "payload TEXT NOT NULL, similar_key TEXT NOT NULL, version INTEGER NOT NULL "
                "DEFAULT 1, "
                "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, submitted_at INTEGER)"));
            execute(QStringLiteral(
                "CREATE TABLE expense_shares(expense_id TEXT NOT NULL REFERENCES "
                "expenses(id) ON DELETE CASCADE, "
                "user_id TEXT NOT NULL REFERENCES users(id), amount_cents INTEGER NOT "
                "NULL CHECK(amount_cents>=0), "
                "PRIMARY KEY(expense_id,user_id))"));
            execute(
                QStringLiteral("CREATE UNIQUE INDEX unique_submitted_expense ON "
                               "expenses(ledger_id,creator_id,expense_date,amount_cents,payer_id,"
                               "description) WHERE status<>'draft'"));
            execute(QStringLiteral(
                "CREATE INDEX expenses_ledger_status ON expenses(ledger_id,status,created_at)"));
            execute(
                QStringLiteral("CREATE INDEX expenses_similar ON expenses(ledger_id,similar_key)"));
        }
        // Keep old labels for compatibility; active expense rules use payer and shares only.
        if (version < 4) {
            execute(QStringLiteral(
                "ALTER TABLE expenses RENAME COLUMN expense_type TO legacy_expense_type"));
        }
        if (version < 5) {
            execute(QStringLiteral(
                "CREATE TABLE expense_locks(expense_id TEXT PRIMARY KEY REFERENCES "
                "expenses(id),holder_id TEXT NOT NULL REFERENCES users(id),session_key TEXT NOT "
                "NULL,token TEXT NOT NULL UNIQUE,expires_at INTEGER NOT NULL)"));
            execute(
                QStringLiteral("CREATE TABLE expense_reviews(id TEXT PRIMARY KEY,expense_id TEXT "
                               "NOT NULL REFERENCES "
                               "expenses(id),reviewer_id TEXT NOT NULL REFERENCES "
                               "users(id),decision TEXT NOT NULL "
                               "CHECK(decision IN ('approve','reject')),reason TEXT NOT "
                               "NULL,reviewed_version INTEGER "
                               "NOT NULL,created_at INTEGER NOT NULL)"));
            execute(QStringLiteral(
                "CREATE TABLE expense_revisions(id TEXT PRIMARY KEY,expense_id TEXT NOT NULL "
                "REFERENCES expenses(id),actor_id TEXT NOT NULL REFERENCES users(id),reason TEXT "
                "NOT "
                "NULL,previous_form TEXT NOT NULL,new_form TEXT NOT NULL,previous_status TEXT NOT "
                "NULL,new_status TEXT NOT NULL,previous_shares TEXT NOT NULL,new_shares TEXT NOT "
                "NULL,previous_version INTEGER NOT NULL,new_version INTEGER NOT NULL,created_at "
                "INTEGER NOT NULL)"));
            execute(QStringLiteral(
                "CREATE TABLE expense_withdrawals(id TEXT PRIMARY KEY,expense_id TEXT NOT NULL "
                "UNIQUE "
                "REFERENCES expenses(id),actor_id TEXT NOT NULL REFERENCES users(id),reason TEXT "
                "NOT "
                "NULL,previous_version INTEGER NOT NULL,created_at INTEGER NOT NULL)"));
            execute(QStringLiteral(
                "CREATE TABLE expense_notices(id TEXT PRIMARY KEY,user_id TEXT NOT NULL REFERENCES "
                "users(id),ledger_id TEXT NOT NULL REFERENCES ledgers(id),expense_id TEXT NOT NULL "
                "REFERENCES expenses(id),kind TEXT NOT NULL,message TEXT NOT NULL,created_at "
                "INTEGER "
                "NOT NULL,read_at INTEGER)"));
            execute(QStringLiteral(
                "CREATE INDEX notices_recipient ON expense_notices(user_id,ledger_id,created_at)"));
            execute(QStringLiteral(
                "CREATE INDEX revisions_expense ON expense_revisions(expense_id,created_at)"));
            execute(QStringLiteral(
                "CREATE INDEX reviews_expense ON expense_reviews(expense_id,created_at)"));
            execute(
                QStringLiteral("CREATE UNIQUE INDEX one_approved_similar_expense ON "
                               "expenses(ledger_id,similar_key) "
                               "WHERE status IN ('approved','settled') AND similar_key<>''"));
            for (const QString& table :
                 QStringList{"expense_reviews", "expense_revisions", "expense_withdrawals"}) {
                execute(
                    QStringLiteral("CREATE TRIGGER %1_no_update BEFORE UPDATE ON %1 BEGIN SELECT "
                                   "RAISE(ABORT,'History is immutable'); END")
                        .arg(table));
                execute(
                    QStringLiteral("CREATE TRIGGER %1_no_delete BEFORE DELETE ON %1 BEGIN SELECT "
                                   "RAISE(ABORT,'History is immutable'); END")
                        .arg(table));
            }
        }
        if (version < 6) {
            execute(QStringLiteral(
                "CREATE TABLE settlements(id TEXT PRIMARY KEY,ledger_id TEXT NOT NULL REFERENCES "
                "ledgers(id),"
                "actor_id TEXT NOT NULL REFERENCES users(id),created_at INTEGER NOT NULL,"
                "preview_token TEXT NOT NULL,expense_count INTEGER NOT NULL CHECK(expense_count>0),"
                "amount_cents INTEGER NOT NULL CHECK(amount_cents>0),snapshot TEXT NOT NULL,"
                "UNIQUE(ledger_id,preview_token))"));
            execute(QStringLiteral(
                "CREATE TABLE settlement_expenses(settlement_id TEXT NOT NULL REFERENCES "
                "settlements(id),"
                "expense_id TEXT NOT NULL UNIQUE REFERENCES expenses(id),version INTEGER NOT NULL,"
                "form TEXT NOT NULL,shares TEXT NOT NULL,PRIMARY KEY(settlement_id,expense_id))"));
            execute(QStringLiteral(
                "CREATE TABLE repayments(id TEXT PRIMARY KEY,ledger_id TEXT NOT NULL REFERENCES "
                "ledgers(id),"
                "actor_id TEXT NOT NULL REFERENCES users(id),payer_id TEXT NOT NULL REFERENCES "
                "users(id),"
                "recipient_id TEXT NOT NULL REFERENCES users(id),amount_cents INTEGER NOT NULL "
                "CHECK(amount_cents>0),"
                "paid_at INTEGER NOT NULL,created_at INTEGER NOT NULL,note TEXT NOT NULL,"
                "request_key TEXT NOT NULL,fingerprint TEXT NOT NULL,confirmed_overpayment INTEGER "
                "NOT "
                "NULL,"
                "CHECK(payer_id<>recipient_id),UNIQUE(ledger_id,request_key))"));
            execute(QStringLiteral(
                "CREATE INDEX settlements_ledger ON settlements(ledger_id,created_at,id)"));
            execute(QStringLiteral(
                "CREATE INDEX repayments_ledger ON repayments(ledger_id,created_at,id)"));
            for (const QString& table :
                 QStringList{"settlements", "settlement_expenses", "repayments"}) {
                execute(
                    QStringLiteral("CREATE TRIGGER %1_no_update BEFORE UPDATE ON %1 BEGIN SELECT "
                                   "RAISE(ABORT,'Settlement history is immutable'); END")
                        .arg(table));
                execute(
                    QStringLiteral("CREATE TRIGGER %1_no_delete BEFORE DELETE ON %1 BEGIN SELECT "
                                   "RAISE(ABORT,'Settlement history is immutable'); END")
                        .arg(table));
            }
        }
        if (version < 7) {
            execute(QStringLiteral("ALTER TABLE ledgers ADD COLUMN closed_at INTEGER"));
            execute(QStringLiteral(
                "ALTER TABLE ledgers ADD COLUMN closed_by TEXT REFERENCES users(id)"));
            execute(QStringLiteral(
                "ALTER TABLE ledgers ADD COLUMN close_token TEXT NOT NULL DEFAULT ''"));
            // Explicit whole-ledger deletion is the only exception to business-history deletion.
            // Operation-log triggers remain unconditional and are never relaxed.
            const QMap<QString, QString> scopes{
                {"expense_reviews",
                 "SELECT 1 FROM expenses e JOIN ledgers l ON l.id=e.ledger_id WHERE "
                 "e.id=OLD.expense_id AND l.status='deleted'"},
                {"expense_revisions",
                 "SELECT 1 FROM expenses e JOIN ledgers l ON l.id=e.ledger_id WHERE "
                 "e.id=OLD.expense_id AND l.status='deleted'"},
                {"expense_withdrawals",
                 "SELECT 1 FROM expenses e JOIN ledgers l ON l.id=e.ledger_id WHERE "
                 "e.id=OLD.expense_id AND l.status='deleted'"},
                {"settlement_expenses",
                 "SELECT 1 FROM settlements s JOIN ledgers l ON l.id=s.ledger_id WHERE "
                 "s.id=OLD.settlement_id AND l.status='deleted'"},
                {"settlements",
                 "SELECT 1 FROM ledgers l WHERE l.id=OLD.ledger_id AND l.status='deleted'"},
                {"repayments",
                 "SELECT 1 FROM ledgers l WHERE l.id=OLD.ledger_id AND l.status='deleted'"}};
            for (auto it = scopes.begin(); it != scopes.end(); ++it) {
                execute(QStringLiteral("DROP TRIGGER %1_no_delete").arg(it.key()));
                execute(QStringLiteral(
                            "CREATE TRIGGER %1_no_delete BEFORE DELETE ON %1 WHEN NOT EXISTS(%2) "
                            "BEGIN SELECT RAISE(ABORT,'History is immutable'); END")
                            .arg(it.key(), it.value()));
            }
        }
        if (version < 8) {
            execute(QStringLiteral(
                "ALTER TABLE expenses ADD COLUMN superseded INTEGER NOT NULL DEFAULT 0 "
                "CHECK(superseded IN (0,1))"));
            execute(QStringLiteral(
                "ALTER TABLE expenses ADD COLUMN is_adjustment INTEGER NOT NULL DEFAULT "
                "0 CHECK(is_adjustment IN (0,1))"));
            execute(QStringLiteral(
                "ALTER TABLE expenses ADD COLUMN financial_root_id TEXT NOT NULL DEFAULT ''"));
            execute(QStringLiteral(
                "ALTER TABLE expenses ADD COLUMN adjustment_reason TEXT NOT NULL DEFAULT ''"));
            execute(QStringLiteral(
                "CREATE TABLE expense_finance_events(id TEXT PRIMARY KEY,ledger_id TEXT "
                "NOT NULL REFERENCES ledgers(id),"
                "root_id TEXT NOT NULL REFERENCES expenses(id),source_id TEXT "
                "REFERENCES expenses(id),new_expense_id TEXT REFERENCES expenses(id),"
                "actor_id TEXT NOT NULL REFERENCES users(id),kind TEXT NOT NULL "
                "CHECK(kind IN ('refund','supplement','correction')),"
                "receiver_id TEXT REFERENCES users(id),amount_cents INTEGER NOT NULL "
                "CHECK(amount_cents>=0),delta_cents INTEGER NOT NULL,"
                "occurred_at INTEGER NOT NULL,created_at INTEGER NOT NULL,reason TEXT "
                "NOT NULL,revision INTEGER NOT NULL CHECK(revision>0),"
                "request_key TEXT NOT NULL,fingerprint TEXT NOT NULL,before_state TEXT "
                "NOT NULL,after_state TEXT NOT NULL,"
                "UNIQUE(root_id,revision),UNIQUE(ledger_id,request_key))"));
            execute(
                QStringLiteral("CREATE UNIQUE INDEX one_correction_per_source ON "
                               "expense_finance_events(source_id) WHERE kind='correction'"));
            execute(QStringLiteral(
                "CREATE INDEX finance_ledger ON expense_finance_events(ledger_id,created_at)"));
            execute(QStringLiteral(
                "CREATE INDEX expenses_financial_root ON expenses(ledger_id,financial_root_id)"));
            execute(
                QStringLiteral("CREATE TRIGGER finance_no_update BEFORE UPDATE ON "
                               "expense_finance_events BEGIN SELECT "
                               "RAISE(ABORT,'Financial history is immutable'); END"));
            execute(QStringLiteral(
                "CREATE TRIGGER finance_no_delete BEFORE DELETE ON expense_finance_events WHEN NOT "
                "EXISTS(SELECT 1 FROM ledgers WHERE id=OLD.ledger_id AND status='deleted') BEGIN "
                "SELECT RAISE(ABORT,'Financial history is immutable'); END"));
            execute(QStringLiteral("DROP INDEX unique_submitted_expense"));
            execute(
                QStringLiteral("CREATE UNIQUE INDEX unique_submitted_expense ON "
                               "expenses(ledger_id,creator_id,expense_date,amount_cents,payer_id,"
                               "description) WHERE status<>'draft' AND superseded=0"));
            execute(QStringLiteral("DROP INDEX one_approved_similar_expense"));
            execute(QStringLiteral(
                "CREATE UNIQUE INDEX one_approved_similar_expense ON "
                "expenses(ledger_id,similar_key) "
                "WHERE status IN ('approved','settled') AND similar_key<>'' AND superseded=0"));
        }
        if (version < 9) {
            execute(
                "ALTER TABLE memberships ADD COLUMN settlement_deadline TEXT NOT NULL DEFAULT ''");
            execute(
                "ALTER TABLE memberships ADD COLUMN history_context TEXT NOT NULL DEFAULT '{}'");
            execute(
                "CREATE TABLE membership_history(ledger_id TEXT NOT NULL REFERENCES "
                "ledgers(id),user_id TEXT NOT NULL REFERENCES users(id),kind TEXT NOT NULL "
                "CHECK(kind "
                "IN ('expense','finance_event')),object_id TEXT NOT NULL,snapshot TEXT NOT "
                "NULL,PRIMARY KEY(ledger_id,user_id,kind,object_id))");
            execute(
                "CREATE TABLE membership_events(id TEXT PRIMARY KEY,ledger_id TEXT NOT NULL "
                "REFERENCES "
                "ledgers(id),actor_id TEXT NOT NULL REFERENCES users(id),kind TEXT NOT NULL "
                "CHECK(kind "
                "IN ('exit','transfer')),target_id TEXT NOT NULL REFERENCES users(id),request_key "
                "TEXT "
                "NOT NULL,fingerprint TEXT NOT NULL,detail TEXT NOT NULL,created_at INTEGER NOT "
                "NULL,UNIQUE(ledger_id,request_key))");
            execute(
                "CREATE TRIGGER membership_event_no_update BEFORE UPDATE ON membership_events "
                "BEGIN "
                "SELECT RAISE(ABORT,'Membership history is immutable'); END");
            execute(
                "CREATE TRIGGER membership_event_no_delete BEFORE DELETE ON membership_events WHEN "
                "NOT "
                "EXISTS(SELECT 1 FROM ledgers WHERE id=OLD.ledger_id AND status='deleted') BEGIN "
                "SELECT RAISE(ABORT,'Membership history is immutable'); END");
        }
        if (version < 10) {
            execute("ALTER TABLE memberships ADD COLUMN audit_cutoff INTEGER NOT NULL DEFAULT 0");
            execute(
                "CREATE TABLE audit_details(log_id TEXT PRIMARY KEY REFERENCES "
                "operation_logs(id),ledger_id TEXT NOT NULL REFERENCES ledgers(id),kind TEXT NOT "
                "NULL,expense_id TEXT NOT NULL,private_owner TEXT NOT NULL,payload TEXT NOT NULL)");
            execute(
                "CREATE TABLE audit_audience(log_id TEXT NOT NULL REFERENCES "
                "operation_logs(id),user_id TEXT NOT NULL REFERENCES users(id),PRIMARY "
                "KEY(log_id,user_id))");
            execute("CREATE INDEX audit_for_user ON audit_audience(user_id,log_id)");
            execute("CREATE INDEX audit_kind_ledger ON audit_details(ledger_id,kind,log_id)");
            execute(
                "CREATE INDEX logs_query ON operation_logs(ledger_id,created_at,action,actor_id)");
            execute("CREATE INDEX expenses_query ON expenses(ledger_id,expense_date,amount_cents)");
            for (const QString& table : QStringList{"audit_details", "audit_audience"}) {
                execute(
                    QStringLiteral("CREATE TRIGGER %1_no_update BEFORE UPDATE ON %1 BEGIN SELECT "
                                   "RAISE(ABORT,'Audit evidence is immutable'); END")
                        .arg(table));
                execute(
                    QStringLiteral("CREATE TRIGGER %1_no_delete BEFORE DELETE ON %1 BEGIN SELECT "
                                   "RAISE(ABORT,'Audit evidence is immutable'); END")
                        .arg(table));
            }
            execute(
                "UPDATE memberships SET audit_cutoff=COALESCE((SELECT MAX(o.rowid) FROM "
                "operation_logs "
                "o WHERE o.ledger_id=memberships.ledger_id AND o.actor_id=memberships.user_id AND "
                "o.action='exit_ledger'),0) WHERE status='exited'");
            QSqlQuery old_logs(connection_);
            requireSuccess(old_logs.exec("SELECT id FROM operation_logs ORDER BY rowid"),
                           QStringLiteral("Cannot read audit history"), old_logs);
            QStringList audit_ids;
            while (old_logs.next()) {
                audit_ids.append(old_logs.value(0).toString());
            }
            old_logs.finish();
            for (const auto& log_id : audit_ids) {
                appendAuditEvidence(connection_, log_id, true);
            }
        }
        if (version < 11) {
            execute(
                "CREATE TABLE expense_requests(ledger_id TEXT NOT NULL REFERENCES ledgers(id),"
                "user_id TEXT NOT NULL REFERENCES users(id),request_key TEXT NOT NULL,"
                "method TEXT NOT NULL,fingerprint TEXT NOT NULL,expense_id TEXT NOT NULL "
                "REFERENCES expenses(id),version INTEGER NOT NULL,status TEXT NOT NULL,"
                "created_at INTEGER NOT NULL,PRIMARY KEY(ledger_id,user_id,request_key))");
            execute(
                "CREATE TRIGGER expense_request_no_update BEFORE UPDATE ON expense_requests "
                "BEGIN SELECT RAISE(ABORT,'Write receipts are immutable'); END");
            execute(
                "CREATE TRIGGER expense_request_no_delete BEFORE DELETE ON expense_requests WHEN "
                "NOT "
                "EXISTS(SELECT 1 FROM ledgers WHERE id=OLD.ledger_id AND status='deleted') "
                "BEGIN SELECT RAISE(ABORT,'Write receipts are immutable'); END");
        }
        execute(
            "CREATE TABLE account_recovery(user_id TEXT PRIMARY KEY REFERENCES users(id),"
            "salt BLOB NOT NULL CHECK(length(salt)=16),hash BLOB NOT NULL CHECK(length(hash)=32),"
            "iterations INTEGER NOT NULL,created_at INTEGER NOT NULL)");
        execute(
            "CREATE TABLE account_security_events(id TEXT PRIMARY KEY,user_id TEXT NOT NULL "
            "REFERENCES users(id),action TEXT NOT NULL CHECK(action IN "
            "('password_changed','recovery_generated','password_reset')),created_at INTEGER NOT "
            "NULL)");
        execute(
            "CREATE TRIGGER account_security_no_update BEFORE UPDATE ON account_security_events "
            "BEGIN SELECT RAISE(ABORT,'Account history is immutable'); END");
        execute(
            "CREATE TRIGGER account_security_no_delete BEFORE DELETE ON account_security_events "
            "BEGIN SELECT RAISE(ABORT,'Account history is immutable'); END");
        execute(QStringLiteral("PRAGMA user_version = 12"));
        requireSuccess(connection_.commit(), QStringLiteral("Cannot commit database migration"),
                       connection_);
    } catch (...) {
        connection_.rollback();
        throw;
    }
}

void Database::execute(const QString& statement) {
    QSqlQuery query(connection_);
    requireSuccess(query.exec(statement), QStringLiteral("SQLite operation failed"), query);
}

}  // namespace ledger
