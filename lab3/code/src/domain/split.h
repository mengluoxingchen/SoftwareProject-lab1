#pragma once

#include <QString>
#include <QVector>

#include "domain/types.h"

namespace ledger {
struct SplitPart {
    QString user_id;
    QString value;
};
struct ShareAllocation {
    QString user_id;
    AmountInCents amount = 0;
};

[[nodiscard]] AmountInCents parseMoney(const QString& text,
                                       const QString& field = QStringLiteral("amount"));
[[nodiscard]] QString formatMoney(AmountInCents amount);
[[nodiscard]] qint64 parseRatio(const QString& text);
[[nodiscard]] QVector<ShareAllocation> calculateSplit(AmountInCents total, const QString& method,
                                                      const QVector<SplitPart>& parts);
// Allocates a refund against the remaining exact-cent shares, including zero shares.
[[nodiscard]] QVector<ShareAllocation> allocateRefund(AmountInCents total,
                                                      const QVector<ShareAllocation>& remaining);
}  // namespace ledger
