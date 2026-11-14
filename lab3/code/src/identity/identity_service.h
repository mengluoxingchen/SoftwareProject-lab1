#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <functional>
#include <optional>

#include "expense/expense_service.h"
#include "finance/finance_service.h"
#include "query/query_service.h"
#include "settlement/settlement_service.h"

namespace ledger {

class Database;

class IdentityService final {
public:
    using Clock = std::function<qint64()>;
    explicit IdentityService(Database& database, Clock clock = {});
    QJsonObject dispatch(const QString& method, const QJsonObject& parameters,
                         const QString& token);

private:
    struct LoginAttempt {
        int failures = 0;
        qint64 blocked_until = 0;
    };
    QSqlQuery query(const QString& sql, const QVariantList& parameters = {}) const;
    QString authenticate(const QString& token) const;
    QJsonObject user(const QString& id) const;
    QString requireMember(const QString& ledger_id, const QString& user_id) const;
    void requireAdmin(const QString& ledger_id, const QString& user_id) const;
    void log(const QString& ledger_id, const QString& actor, const QString& action,
             const QString& target, const QString& detail);
    QJsonObject registerUser(const QJsonObject& parameters);
    QJsonObject login(const QJsonObject& parameters);
    QJsonObject accountSecurity(const QString& actor) const;
    QJsonObject changePassword(const QString& actor, const QJsonObject& parameters);
    QJsonObject generateRecoveryCode(const QString& actor, const QJsonObject& parameters);
    QJsonObject resetPassword(const QJsonObject& parameters);
    void verifyCurrentPassword(const QString& actor, const QString& password);
    QString storeRecoveryCode(const QString& actor);
    void recordSecurityEvent(const QString& actor, const QString& action);
    QJsonObject listLedgers(const QString& actor) const;
    QJsonObject createLedger(const QString& actor, const QJsonObject& parameters);
    QJsonObject ledgerDetails(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject invite(const QString& actor, const QJsonObject& parameters);
    QJsonObject invitations(const QString& actor) const;
    QJsonObject acceptInvitation(const QString& actor, const QJsonObject& parameters);
    QJsonObject changeRole(const QString& actor, const QJsonObject& parameters);
    QJsonObject membershipPreview(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject changeMembership(const QString& actor, const QJsonObject& parameters, bool exiting);
    std::optional<QJsonObject> historicalRead(const QString& actor, const QString& method,
                                              const QJsonObject& parameters) const;
    QJsonObject closePreview(const QString& actor, const QJsonObject& parameters) const;
    QJsonObject closeLedger(const QString& actor, const QJsonObject& parameters);
    QJsonObject closedLedgers(const QString& actor) const;
    QJsonObject restoreLedger(const QString& actor, const QJsonObject& parameters);
    QJsonObject closedLedgerLogs(const QString& actor, const QJsonObject& parameters) const;

    QSqlDatabase database_;
    Clock clock_;
    QHash<QString, LoginAttempt> login_attempts_;
    QHash<QString, LoginAttempt> security_attempts_;
    ExpenseService expenses_;
    SettlementService settlements_;
    FinanceService finances_;
    QueryService queries_;
};

}  // namespace ledger
