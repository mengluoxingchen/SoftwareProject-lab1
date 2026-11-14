#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
#include <QVector>

namespace ledger {
class Database;
class QueryService final {
public:
    explicit QueryService(Database& database);
    QJsonObject dispatch(const QString& actor, const QString& method,
                         const QJsonObject& parameters) const;

private:
    QVector<QJsonObject> records(const QString& actor, const QString& ledger_id) const;
    QJsonObject expenses(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject options(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject auditAccess(const QString& actor, const QString& ledger_id) const;
    QJsonObject audit(const QString& actor, const QString& method,
                      const QJsonObject& parameters) const;
    QSqlDatabase database_;
};
}  // namespace ledger
