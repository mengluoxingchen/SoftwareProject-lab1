#include "domain/split.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <numeric>

#include "domain/service_error.h"

namespace ledger {
namespace {
[[noreturn]] void invalid(const QString& message, const QString& field) {
    throw ServiceError(QStringLiteral("validation"), message, field);
}
// Exact (a*b)/divisor without floating point or an overflowing intermediate product.
// Here b<=divisor and divisor<=1000*9999999999, so the remainder arithmetic is bounded.
QPair<qint64, qint64> multiplyDivide(qint64 a, qint64 b, qint64 divisor) {
    qint64 quotient = 0;
    qint64 remainder = 0;
    for (int bit = 62; bit >= 0; --bit) {
        quotient *= 2;
        remainder = remainder * 2 + (((static_cast<quint64>(a) >> bit) & 1U) ? b : 0);
        quotient += remainder / divisor;
        remainder %= divisor;
    }
    return {quotient, remainder};
}
}  // namespace

AmountInCents parseMoney(const QString& text, const QString& field) {
    static const QRegularExpression expression(QStringLiteral("^[0-9]{1,10}(?:\\.[0-9]{1,2})?$"));
    const QString value = text.trimmed();
    if (!expression.match(value).hasMatch()) {
        invalid(QStringLiteral("金额须为非负数字，最多10位整数和2位小数。"), field);
    }
    const auto pieces = value.split('.');
    const qint64 yuan = pieces[0].toLongLong();
    const qint64 cents = pieces.size() == 2 ? pieces[1].leftJustified(2, '0').toLongLong() : 0;
    return yuan * 100 + cents;
}

QString formatMoney(AmountInCents amount) {
    const quint64 magnitude =
        amount < 0 ? static_cast<quint64>(-(amount + 1)) + 1 : static_cast<quint64>(amount);
    return (amount < 0 ? QStringLiteral("-") : QString()) + QString::number(magnitude / 100) + '.' +
           QString::number(magnitude % 100).rightJustified(2, '0');
}

qint64 parseRatio(const QString& text) {
    static const QRegularExpression expression(QStringLiteral("^[0-9]{1,6}(?:\\.[0-9]{1,4})?$"));
    const QString value = text.trimmed();
    if (!expression.match(value).hasMatch()) {
        invalid(QStringLiteral("比例须为正数，最多6位整数和4位小数，如1、2或1.5。"),
                QStringLiteral("splits"));
    }
    const auto pieces = value.split('.');
    const qint64 units = pieces[0].toLongLong() * 10000 +
                         (pieces.size() == 2 ? pieces[1].leftJustified(4, '0').toLongLong() : 0);
    if (units <= 0) {
        invalid(QStringLiteral("每位参与成员的比例必须大于0。"), QStringLiteral("splits"));
    }
    return units;
}

QVector<ShareAllocation> calculateSplit(AmountInCents total, const QString& method,
                                        const QVector<SplitPart>& parts) {
    if (total <= 0) {
        invalid(QStringLiteral("消费金额必须大于0。"), QStringLiteral("amount"));
    }
    if (parts.isEmpty() || parts.size() > 1000) {
        invalid(QStringLiteral("请选择1至1000位分摊成员。"), QStringLiteral("splits"));
    }
    if (method != "equal" && method != "ratio" && method != "fixed") {
        invalid(QStringLiteral("请选择有效的分摊方式。"), QStringLiteral("split_method"));
    }
    QSet<QString> seen;
    QVector<ShareAllocation> result;
    QVector<qint64> weights;
    qint64 sum = 0;
    for (const auto& part : parts) {
        if (part.user_id.isEmpty() || seen.contains(part.user_id)) {
            invalid(QStringLiteral("分摊成员不能为空或重复。"), QStringLiteral("splits"));
        }
        seen.insert(part.user_id);
        const qint64 value = method == "fixed"   ? parseMoney(part.value, "splits")
                             : method == "ratio" ? parseRatio(part.value)
                                                 : 1;
        weights.append(value);
        sum += value;
        result.append({part.user_id, method == "fixed" ? value : 0});
    }
    if (method == "fixed") {
        if (sum != total) {
            invalid(QStringLiteral("分摊合计%1元，与消费金额相差%2元，请调整后提交。")
                        .arg(formatMoney(sum), formatMoney(qAbs(sum - total))),
                    QStringLiteral("splits"));
        }
        return result;
    }
    QVector<qint64> remainders;
    qint64 assigned = 0;
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const auto allocation = multiplyDivide(total, weights[index], sum);
        result[index].amount = allocation.first;
        remainders.append(allocation.second);
        assigned += allocation.first;
    }
    QVector<int> order(parts.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        if (remainders[left] != remainders[right]) {
            return remainders[left] > remainders[right];
        }
        return parts[left].user_id < parts[right].user_id;
    });
    for (qint64 index = 0; index < total - assigned; ++index) {
        ++result[order[static_cast<int>(index)]].amount;
    }
    return result;
}

QVector<ShareAllocation> allocateRefund(AmountInCents total,
                                        const QVector<ShareAllocation>& remaining) {
    if (total < 0 || remaining.isEmpty() || remaining.size() > 1000) {
        invalid(QStringLiteral("退款分摊数据不正确。"), "amount");
    }
    QSet<QString> seen;
    qint64 sum = 0;
    constexpr qint64 limit = 999999999999LL;
    for (const auto& part : remaining) {
        if (part.user_id.isEmpty() || seen.contains(part.user_id) || part.amount < 0 ||
            part.amount > limit - sum) {
            invalid(QStringLiteral("剩余承担明细不一致，无法分配退款。"), "amount");
        }
        seen.insert(part.user_id);
        sum += part.amount;
    }
    if (total > sum) {
        invalid(QStringLiteral("退款金额不能超过剩余可退金额。"), "amount");
    }
    QVector<ShareAllocation> result;
    QVector<qint64> remainders;
    qint64 assigned = 0;
    for (const auto& part : remaining) {
        const auto allocation =
            sum == 0 ? QPair<qint64, qint64>{0, 0} : multiplyDivide(total, part.amount, sum);
        result.append({part.user_id, allocation.first});
        remainders.append(allocation.second);
        assigned += allocation.first;
    }
    QVector<int> order(remaining.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return remainders[a] != remainders[b] ? remainders[a] > remainders[b]
                                              : remaining[a].user_id < remaining[b].user_id;
    });
    for (qint64 index = 0; index < total - assigned; ++index) {
        ++result[order[static_cast<int>(index)]].amount;
    }
    return result;
}
}  // namespace ledger
