#pragma once
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
namespace ledger {
// Only business input participates: leases and confirmation UI may change on retry.
inline QString expenseRequestFingerprint(const QString& method, const QJsonObject& parameters) {
    QJsonObject content{{"method", method}};
    for (const auto& key :
         QStringList{"ledger_id", "expense_id", "version", "amount", "date", "payer_id",
                     "description", "category", "split_method", "splits", "change_reason",
                     "post_settlement_adjustment", "adjustment_reason"}) {
        if (parameters.contains(key)) content.insert(key, parameters.value(key));
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(content).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
}  // namespace ledger
