#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <functional>

namespace ledger {
class Database;
class FinanceService final {
public:
    using Clock = std::function<qint64()>;
    explicit FinanceService(Database& database, Clock clock = {});
    QJsonObject dispatch(const QString& actor, const QString& method,
                         const QJsonObject& parameters);
    // Internal approval hook; the caller owns the review transaction.
    void approveSupplement(const QString& actor, const QString& ledger_id,
                           const QString& expense_id);

private:
    QString requireMember(const QString& actor, const QString& ledger_id, bool admin = false) const;
    QJsonObject info(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject event(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject refund(const QString& actor, const QJsonObject& parameters);
    QJsonObject adjustmentPreview(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject adjustment(const QString& actor, const QJsonObject& parameters);
    QJsonObject validateForm(const QString& ledger_id, const QJsonObject& parameters) const;
    void appendEvent(const QString& actor, const QString& ledger_id, const QString& id,
                     const QString& kind, const QString& source, const QString& new_expense,
                     const QString& receiver, qint64 amount, qint64 delta, qint64 occurred_at,
                     const QString& reason, const QString& key, const QString& fingerprint,
                     const QJsonObject& before, const QJsonObject& after);
    QSqlDatabase database_;
    Clock clock_;
};
}  // namespace ledger
