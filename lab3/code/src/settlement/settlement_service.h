#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <functional>

namespace ledger {
class Database;
class SettlementService final {
public:
    using Clock = std::function<qint64()>;
    explicit SettlementService(Database& database, Clock clock = {});
    QJsonObject dispatch(const QString& actor, const QString& method,
                         const QJsonObject& parameters);

private:
    QSqlQuery query(const QString& sql, const QVariantList& values = {}) const;
    QString requireMember(const QString& actor, const QString& ledger_id, bool write = false) const;
    QJsonObject balances(const QString& ledger_id) const;
    QJsonObject overview(const QString& actor, const QString& ledger_id) const;
    QJsonObject create(const QString& actor, const QJsonObject& parameters);
    QJsonObject detail(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject repay(const QString& actor, const QJsonObject& parameters);
    void log(const QString& actor, const QString& ledger_id, const QString& action,
             const QString& target, const QString& detail);
    QSqlDatabase database_;
    Clock clock_;
};
}  // namespace ledger
