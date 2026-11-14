#pragma once
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

namespace ledger {
// Adds immutable evidence and audiences in the caller's business transaction.
void appendAuditEvidence(QSqlDatabase database, const QString& log_id, bool legacy = false,
                         const QJsonObject& amounts = {});
}  // namespace ledger
