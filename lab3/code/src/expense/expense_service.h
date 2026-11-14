#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <functional>

#include "finance/finance_service.h"

namespace ledger {
class Database;
class ExpenseService final {
public:
    using Clock = std::function<qint64()>;
    explicit ExpenseService(Database& database, Clock clock = {});
    QJsonObject dispatch(const QString& actor, const QString& method, const QJsonObject& parameters,
                         const QString& session = {});

private:
    QSqlQuery query(const QString& sql, const QVariantList& values = {}) const;
    QString requireMember(const QString& actor, const QString& ledger_id, bool write) const;
    QJsonObject list(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject detail(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject save(const QString& actor, const QJsonObject& parameters, bool submit,
                     const QString& session);
    QJsonObject lock(const QString& actor, const QString& session, const QString& operation,
                     const QJsonObject& parameters);
    void requireEditLock(const QString& actor, const QString& session,
                         const QJsonObject& parameters) const;
    void requireUnlocked(const QString& expense_id) const;
    QJsonObject review(const QString& actor, const QJsonObject& parameters);
    QJsonObject withdraw(const QString& actor, const QJsonObject& parameters);
    QJsonObject history(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject notifications(const QString& actor, const QJsonObject& parameters, bool mark_read);
    QJsonArray similarRecords(const QString& expense_id, const QString& ledger_id,
                              const QString& key) const;
    void appendLog(const QString& ledger_id, const QString& actor, const QString& action,
                   const QString& expense_id, const QString& reason,
                   const QJsonObject& amounts = {});
    void notifyCreator(const QJsonObject& record, const QString& ledger_id, const QString& kind,
                       const QString& message);
    QSqlDatabase database_;
    Clock clock_;
    FinanceService finances_;
};
}  // namespace ledger
