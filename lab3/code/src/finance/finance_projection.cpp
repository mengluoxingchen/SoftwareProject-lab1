#include "finance/finance_projection.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QMap>
#include <QSqlError>
#include <algorithm>
#include <stdexcept>

#include "domain/service_error.h"

namespace ledger::finance_detail {
QSqlQuery query(QSqlDatabase database, const QString& sql, const QVariantList& values) {
    QSqlQuery result(database);
    if (!result.prepare(sql)) {
        throw std::runtime_error(result.lastError().text().toStdString());
    }
    for (const auto& value : values) {
        result.addBindValue(value);
    }
    if (!result.exec()) {
        throw std::runtime_error(result.lastError().text().toStdString());
    }
    return result;
}
QString json(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
QString digest(const QJsonObject& object) {
    return QString::fromLatin1(
        QCryptographicHash::hash(json(object).toUtf8(), QCryptographicHash::Sha256).toHex());
}
qint64 cents(QString value) {
    value.remove('.');
    bool ok = false;
    const qint64 amount = value.toLongLong(&ok);
    if (!ok) {
        throw std::runtime_error("Invalid internal financial amount.");
    }
    return amount;
}
QVector<ShareAllocation> shares(const QJsonArray& values) {
    QVector<ShareAllocation> result;
    for (const auto& value : values) {
        const auto item = value.toObject();
        result.append({item.value("user_id").toString(), cents(item.value("amount").toString())});
    }
    return result;
}
QJsonArray shareJson(const QVector<ShareAllocation>& values, const QJsonArray& names) {
    QHash<QString, QString> labels;
    for (const auto& value : names) {
        labels.insert(value.toObject().value("user_id").toString(),
                      value.toObject().value("username").toString());
    }
    QJsonArray result;
    for (const auto& value : values) {
        result.append(QJsonObject{{"user_id", value.user_id},
                                  {"username", labels.value(value.user_id, value.user_id)},
                                  {"amount", formatMoney(value.amount)}});
    }
    return result;
}
QJsonObject storedState(QJsonObject state) {
    for (const QString& key :
         QStringList{"token", "current_expense_id", "root_revision", "superseded", "status",
                     "version", "is_adjustment", "creator_id"}) {
        state.remove(key);
    }
    return state;
}
QJsonObject projection(QSqlDatabase database, const QString& ledger_id, const QString& expense_id) {
    auto row = query(database,
                     "SELECT "
                     "id,payer_id,amount_cents,payload,status,version,financial_root_id,superseded,"
                     "is_adjustment,creator_id FROM expenses WHERE ledger_id=? AND id=?",
                     {ledger_id, expense_id});
    if (!row.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看该支出的财务记录。"));
    }
    const QString root = row.value(6).toString().isEmpty() ? expense_id : row.value(6).toString();
    const qint64 gross = row.value(2).toLongLong();
    QJsonArray original;
    auto parts = query(database,
                       "SELECT s.user_id,u.username,s.amount_cents FROM expense_shares s JOIN "
                       "users u ON u.id=s.user_id WHERE s.expense_id=? ORDER BY s.user_id",
                       {expense_id});
    while (parts.next()) {
        original.append(QJsonObject{{"user_id", parts.value(0).toString()},
                                    {"username", parts.value(1).toString()},
                                    {"amount", formatMoney(parts.value(2).toLongLong())}});
    }
    QJsonObject state{{"root_id", root},
                      {"expense_id", expense_id},
                      {"gross_amount", formatMoney(gross)},
                      {"net_amount", formatMoney(gross)},
                      {"refunded_amount", "0.00"},
                      {"payer_id", row.value(1).toString()},
                      {"original_shares", original},
                      {"effective_shares", original},
                      {"form", QJsonDocument::fromJson(row.value(3).toString().toUtf8()).object()},
                      {"revision", 0}};
    auto latest = query(database,
                        "SELECT revision,after_state FROM expense_finance_events WHERE root_id=? "
                        "ORDER BY revision DESC LIMIT 1",
                        {root});
    const bool has_events = latest.next();
    const qint64 revision = has_events ? latest.value(0).toLongLong() : 0;
    const auto current =
        has_events ? QJsonDocument::fromJson(latest.value(1).toString().toUtf8()).object() : state;
    if (current.value("expense_id").toString() == expense_id) {
        state = current;
    } else {
        auto replaced = query(database,
                              "SELECT before_state FROM expense_finance_events WHERE source_id=? "
                              "AND kind='correction'",
                              {expense_id});
        if (!replaced.next()) {
            throw ServiceError("integrity", QStringLiteral("调整关联不一致，请核对财务历史。"));
        }
        state = QJsonDocument::fromJson(replaced.value(0).toString().toUtf8()).object();
    }
    state["current_expense_id"] = current.value("expense_id");
    state["root_revision"] = revision;
    state["superseded"] = row.value(7).toBool();
    state["is_adjustment"] = row.value(8).toBool();
    state["status"] = row.value(4).toString();
    state["version"] = row.value(5).toInt();
    state["creator_id"] = row.value(9).toString();
    state["token"] = digest(state);
    return state;
}
QJsonObject ledgerBalances(QSqlDatabase database, const QString& ledger_id) {
    const auto add = [](qint64 a, qint64 b) {
        constexpr qint64 limit = 9000000000000000LL;
        if (a > limit || a < -limit || b > limit || b < -limit || (b > 0 && a > limit - b) ||
            (b < 0 && a < -limit - b)) {
            throw ServiceError("amount_limit", QStringLiteral("账本累计金额超出可计算范围。"));
        }
        return a + b;
    };
    struct Balance {
        QString name;
        qint64 paid = 0, owed = 0, repaid = 0, received = 0, gross_paid = 0, refund_received = 0;
    };
    QMap<QString, Balance> members;
    auto users = query(database,
                       "SELECT u.id,u.username FROM users u WHERE u.id IN (SELECT user_id FROM "
                       "memberships WHERE ledger_id=?) ORDER BY u.id",
                       {ledger_id});
    while (users.next()) {
        members[users.value(0).toString()].name = users.value(1).toString();
    }
    qint64 total = 0, gross_total = 0, refunded = 0, owed_total = 0, repayment_total = 0;
    int count = 0, refund_count = 0, repayment_count = 0;
    auto expenses = query(database,
                          "SELECT id,payer_id,amount_cents FROM expenses WHERE ledger_id=? AND "
                          "status='settled' AND superseded=0",
                          {ledger_id});
    while (expenses.next()) {
        const auto state = projection(database, ledger_id, expenses.value(0).toString());
        const qint64 gross = expenses.value(2).toLongLong(),
                     net = cents(state.value("net_amount").toString());
        auto& payer = members[expenses.value(1).toString()];
        payer.paid = add(payer.paid, gross);
        payer.gross_paid = add(payer.gross_paid, gross);
        gross_total = add(gross_total, gross);
        total = add(total, net);
        ++count;
        qint64 share_total = 0;
        for (const auto& share : shares(state.value("effective_shares").toArray())) {
            auto& member = members[share.user_id];
            member.owed = add(member.owed, share.amount);
            share_total = add(share_total, share.amount);
        }
        if (share_total != net || gross - cents(state.value("refunded_amount").toString()) != net) {
            throw ServiceError("integrity", QStringLiteral("当前有效支出与承担明细不一致。"));
        }
        owed_total = add(owed_total, share_total);
    }
    auto refunds = query(
        database,
        "SELECT f.receiver_id,f.amount_cents FROM expense_finance_events f JOIN expenses root ON "
        "root.id=f.root_id WHERE f.ledger_id=? AND f.kind='refund' AND root.status='settled'",
        {ledger_id});
    while (refunds.next()) {
        const qint64 amount = refunds.value(1).toLongLong();
        auto& receiver = members[refunds.value(0).toString()];
        receiver.paid = add(receiver.paid, -amount);
        receiver.refund_received = add(receiver.refund_received, amount);
        refunded = add(refunded, amount);
        ++refund_count;
    }
    if (gross_total - refunded != total || owed_total != total) {
        throw ServiceError("integrity", QStringLiteral("退款或调整汇总不一致，请先核对数据。"));
    }
    auto repayments = query(
        database, "SELECT payer_id,recipient_id,amount_cents FROM repayments WHERE ledger_id=?",
        {ledger_id});
    while (repayments.next()) {
        const qint64 amount = repayments.value(2).toLongLong();
        auto& payer = members[repayments.value(0).toString()];
        auto& receiver = members[repayments.value(1).toString()];
        payer.repaid = add(payer.repaid, amount);
        receiver.received = add(receiver.received, amount);
        repayment_total = add(repayment_total, amount);
        ++repayment_count;
    }
    QJsonArray rows, suggestions;
    QVector<QPair<QString, qint64>> debtors, creditors;
    qint64 outstanding = 0, net_total = 0;
    for (auto it = members.begin(); it != members.end(); ++it) {
        const auto& member = it.value();
        const qint64 net =
            add(add(member.paid, -member.owed), add(member.repaid, -member.received));
        net_total = add(net_total, net);
        rows.append(QJsonObject{{"user_id", it.key()},
                                {"username", member.name.isEmpty() ? it.key() : member.name},
                                {"paid", formatMoney(member.paid)},
                                {"owed", formatMoney(member.owed)},
                                {"repaid", formatMoney(member.repaid)},
                                {"received", formatMoney(member.received)},
                                {"balance", formatMoney(net)},
                                {"gross_paid", formatMoney(member.gross_paid)},
                                {"refund_received", formatMoney(member.refund_received)}});
        if (net > 0) {
            creditors.append({it.key(), net});
            outstanding = add(outstanding, net);
        }
        if (net < 0) {
            debtors.append({it.key(), -net});
        }
    }
    if (net_total != 0) {
        throw ServiceError("integrity", QStringLiteral("成员余额不平衡。"));
    }
    qsizetype i = 0, j = 0;
    while (i < debtors.size() && j < creditors.size()) {
        const qint64 amount = std::min(debtors[i].second, creditors[j].second);
        suggestions.append(QJsonObject{{"payer_id", debtors[i].first},
                                       {"payer", members.value(debtors[i].first).name},
                                       {"recipient_id", creditors[j].first},
                                       {"recipient", members.value(creditors[j].first).name},
                                       {"amount", formatMoney(amount)}});
        debtors[i].second -= amount;
        creditors[j].second -= amount;
        if (debtors[i].second == 0) {
            ++i;
        }
        if (creditors[j].second == 0) {
            ++j;
        }
    }
    auto history =
        query(database, "SELECT EXISTS(SELECT 1 FROM settlements WHERE ledger_id=?)", {ledger_id});
    history.next();
    auto adjustments =
        query(database,
              "SELECT COUNT(*) FROM expense_finance_events WHERE ledger_id=? AND kind<>'refund'",
              {ledger_id});
    adjustments.next();
    QJsonObject result{{"members", rows},
                       {"total_expenses", formatMoney(total)},
                       {"gross_expenses", formatMoney(gross_total)},
                       {"total_refunds", formatMoney(refunded)},
                       {"refund_count", refund_count},
                       {"expense_count", count},
                       {"total_repayments", formatMoney(repayment_total)},
                       {"repayment_count", repayment_count},
                       {"outstanding", formatMoney(outstanding)},
                       {"suggestions", suggestions},
                       {"has_settlements", history.value(0).toBool()},
                       {"adjustment_count", adjustments.value(0).toInt()}};
    result["balance_token"] = digest(result);
    return result;
}
}  // namespace ledger::finance_detail
