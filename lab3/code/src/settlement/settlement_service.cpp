#include "settlement/settlement_service.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QMap>
#include <QSqlError>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
#include <utility>

#include "domain/service_error.h"
#include "domain/split.h"
#include "finance/finance_projection.h"
#include "infrastructure/audit_store.h"
#include "infrastructure/database.h"

namespace ledger {
namespace {
QString text(const QJsonObject& object, const QString& key, int maximum = 128) {
    if (!object.value(key).isString() || object.value(key).toString().size() > maximum) {
        throw ServiceError("validation", QStringLiteral("请填写有效的字段内容。"), key);
    }
    return object.value(key).toString().trimmed();
}
QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString json(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
QString hash(const QJsonObject& object) {
    return QString::fromLatin1(
        QCryptographicHash::hash(json(object).toUtf8(), QCryptographicHash::Sha256).toHex());
}
qint64 cents(QString value) {
    // Only called with formatMoney output; cumulative totals can exceed one input amount.
    value.remove('.');
    bool ok = false;
    const qint64 result = value.toLongLong(&ok);
    if (!ok) {
        throw std::runtime_error("Invalid stored settlement amount.");
    }
    return result;
}
qint64 add(qint64 a, qint64 b) {
    constexpr qint64 limit = 9000000000000000LL;
    if (b > limit || b < -limit || a > limit || a < -limit || (b > 0 && a > limit - b) ||
        (b < 0 && a < -limit - b)) {
        throw ServiceError("amount_limit", QStringLiteral("账本累计金额超出可计算范围。"));
    }
    return a + b;
}
class Transaction final {
public:
    explicit Transaction(QSqlDatabase database) : database_(std::move(database)) {
        if (!database_.transaction()) {
            throw std::runtime_error("Cannot begin settlement transaction.");
        }
    }
    ~Transaction() {
        if (!committed_) {
            database_.rollback();
        }
    }
    void commit() {
        if (!database_.commit()) {
            throw std::runtime_error("Cannot commit settlement transaction.");
        }
        committed_ = true;
    }

private:
    QSqlDatabase database_;
    bool committed_ = false;
};

}  // namespace

SettlementService::SettlementService(Database& database, Clock clock)
    : database_(database.connection()), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] { return QDateTime::currentSecsSinceEpoch(); };
    }
}

QSqlQuery SettlementService::query(const QString& sql, const QVariantList& values) const {
    QSqlQuery result(database_);
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

QString SettlementService::requireMember(const QString& actor, const QString& ledger_id,
                                         bool write) const {
    auto rows = query(
        "SELECT m.role FROM memberships m JOIN ledgers l ON l.id=m.ledger_id "
        "WHERE m.ledger_id=? AND m.user_id=? AND m.status='active' AND l.status='active'",
        {ledger_id, actor});
    if (!rows.next() || (write && rows.value(0).toString() == "read_only")) {
        throw ServiceError("forbidden", QStringLiteral("无权限访问或变更此账本的结算。"));
    }
    return rows.value(0).toString();
}

QJsonObject SettlementService::dispatch(const QString& actor, const QString& method,
                                        const QJsonObject& parameters) {
    const QString ledger_id = text(parameters, "ledger_id");
    if (method == "settlement_overview") {
        return overview(actor, ledger_id);
    }
    if (method == "settlement_create") {
        return create(actor, parameters);
    }
    if (method == "settlement_detail") {
        return detail(actor, parameters);
    }
    if (method == "settlement_repay") {
        return repay(actor, parameters);
    }
    throw ServiceError("unknown_method", QStringLiteral("不支持的结算操作。"));
}

QJsonObject SettlementService::balances(const QString& ledger_id) const {
    return finance_detail::ledgerBalances(database_, ledger_id);
}

QJsonObject SettlementService::overview(const QString& actor, const QString& ledger_id) const {
    const QString role = requireMember(actor, ledger_id);
    auto result = balances(ledger_id);
    QJsonArray candidates;
    qint64 amount = 0, net_amount = 0;
    auto rows = query(
        "SELECT id,version,amount_cents FROM expenses WHERE ledger_id=? AND status='approved' AND "
        "superseded=0 "
        "ORDER BY id",
        {ledger_id});
    while (rows.next()) {
        candidates.append(
            QJsonObject{{"id", rows.value(0).toString()}, {"version", rows.value(1).toInt()}});
        amount = add(amount, rows.value(2).toLongLong());
        const auto financial =
            finance_detail::projection(database_, ledger_id, rows.value(0).toString());
        net_amount =
            add(net_amount, finance_detail::cents(financial.value("net_amount").toString()));
    }
    result["pending_count"] = candidates.size();
    result["pending_amount"] = formatMoney(amount);
    result["pending_net_amount"] = formatMoney(net_amount);
    result["preview_token"] = hash({{"candidates", candidates}, {"balances", result}});
    result["can_write"] = role != "read_only";
    result["can_record_for_others"] = role == "admin";
    QJsonArray history;
    auto snapshots = query(
        "SELECT s.id,s.created_at,s.expense_count,s.amount_cents,u.username FROM settlements s "
        "JOIN users u ON u.id=s.actor_id WHERE s.ledger_id=? ORDER BY s.created_at DESC,s.rowid "
        "DESC LIMIT 100",
        {ledger_id});
    while (snapshots.next()) {
        history.append(QJsonObject{{"id", snapshots.value(0).toString()},
                                   {"created_at", snapshots.value(1).toLongLong()},
                                   {"expense_count", snapshots.value(2).toInt()},
                                   {"amount", formatMoney(snapshots.value(3).toLongLong())},
                                   {"actor", snapshots.value(4).toString()}});
    }
    const QString transfer_columns =
        "SELECT "
        "r.id,p.username,t.username,r.amount_cents,r.paid_at,a.username,r.note,r.confirmed_"
        "overpayment,r.payer_id,r.recipient_id "
        "FROM repayments r JOIN users p ON p.id=r.payer_id JOIN users t ON t.id=r.recipient_id "
        "JOIN users a ON a.id=r.actor_id WHERE r.ledger_id=?";
    const auto transfers_for = [&](const QString& filter, const QVariantList& values) {
        QJsonArray repayments;
        auto transfers = query(
            transfer_columns + filter + " ORDER BY r.created_at DESC,r.rowid DESC LIMIT ?", values);
        while (transfers.next()) {
            repayments.append(QJsonObject{{"id", transfers.value(0).toString()},
                                          {"payer", transfers.value(1).toString()},
                                          {"recipient", transfers.value(2).toString()},
                                          {"amount", formatMoney(transfers.value(3).toLongLong())},
                                          {"paid_at", transfers.value(4).toLongLong()},
                                          {"actor", transfers.value(5).toString()},
                                          {"note", transfers.value(6).toString()},
                                          {"overpayment", transfers.value(7).toBool()},
                                          {"payer_id", transfers.value(8).toString()},
                                          {"recipient_id", transfers.value(9).toString()}});
        }
        return repayments;
    };
    result["history"] = history;
    result["repayments"] = transfers_for({}, {ledger_id, 100});
    result["personal_repayments"] =
        transfers_for(" AND (r.payer_id=? OR r.recipient_id=?)", {ledger_id, actor, actor, 3});
    QJsonArray financial_events;
    auto events = query(
        "SELECT "
        "f.id,f.kind,f.delta_cents,f.occurred_at,f.reason,u.username,COALESCE(f.source_id,''),"
        "COALESCE(f.new_expense_id,'') FROM expense_finance_events f JOIN users u ON "
        "u.id=f.actor_id WHERE f.ledger_id=? ORDER BY f.created_at DESC,f.rowid DESC LIMIT 100",
        {ledger_id});
    while (events.next()) {
        financial_events.append(QJsonObject{{"id", events.value(0).toString()},
                                            {"kind", events.value(1).toString()},
                                            {"delta", formatMoney(events.value(2).toLongLong())},
                                            {"occurred_at", events.value(3).toLongLong()},
                                            {"reason", events.value(4).toString()},
                                            {"actor", events.value(5).toString()},
                                            {"source_id", events.value(6).toString()},
                                            {"new_expense_id", events.value(7).toString()}});
    }
    result["financial_events"] = financial_events;
    return result;
}

void SettlementService::log(const QString& actor, const QString& ledger_id, const QString& action,
                            const QString& target, const QString& detail) {
    const auto log_id = identifier();
    query("INSERT INTO operation_logs VALUES(?,?,?,?,?,?,?)",
          {log_id, ledger_id, actor, action, target, detail, clock_()});
    appendAuditEvidence(database_, log_id);
}

QJsonObject SettlementService::detail(const QString& actor, const QJsonObject& parameters) const {
    const QString ledger_id = text(parameters, "ledger_id");
    requireMember(actor, ledger_id);
    auto row = query(
        "SELECT s.id,s.snapshot,s.created_at,u.username,s.expense_count,s.amount_cents FROM "
        "settlements s "
        "JOIN users u ON u.id=s.actor_id WHERE s.id=? AND s.ledger_id=?",
        {text(parameters, "settlement_id"), ledger_id});
    if (!row.next()) {
        throw ServiceError("not_found", QStringLiteral("找不到有权限查看的结算快照。"));
    }
    auto snapshot = QJsonDocument::fromJson(row.value(1).toString().toUtf8()).object();
    snapshot["id"] = row.value(0).toString();
    snapshot["created_at"] = row.value(2).toLongLong();
    snapshot["actor"] = row.value(3).toString();
    snapshot["new_count"] = row.value(4).toInt();
    snapshot["new_amount"] = formatMoney(row.value(5).toLongLong());
    QJsonArray expenses;
    auto rows = query(
        "SELECT expense_id,version,form FROM settlement_expenses WHERE settlement_id=? ORDER BY "
        "expense_id LIMIT 1000",
        {row.value(0)});
    while (rows.next()) {
        const auto form = QJsonDocument::fromJson(rows.value(2).toString().toUtf8()).object();
        expenses.append(QJsonObject{{"id", rows.value(0).toString()},
                                    {"version", rows.value(1).toInt()},
                                    {"date", form.value("date")},
                                    {"description", form.value("description")},
                                    {"amount", form.value("amount")}});
    }
    snapshot["expenses"] = expenses;
    return snapshot;
}

QJsonObject SettlementService::create(const QString& actor, const QJsonObject& parameters) {
    const QString ledger_id = text(parameters, "ledger_id");
    requireMember(actor, ledger_id, true);
    const QString token = text(parameters, "preview_token");
    if (token.isEmpty()) {
        throw ServiceError("validation", QStringLiteral("请先刷新并确认待结算账目。"));
    }
    Transaction transaction(database_);
    auto previous = query("SELECT id FROM settlements WHERE ledger_id=? AND preview_token=?",
                          {ledger_id, token});
    if (previous.next()) {
        auto result = detail(
            actor, {{"ledger_id", ledger_id}, {"settlement_id", previous.value(0).toString()}});
        result["reused"] = true;
        return result;
    }
    const auto preview = overview(actor, ledger_id);
    if (preview.value("preview_token").toString() != token) {
        throw ServiceError("conflict",
                           QStringLiteral("账目或余额已变化，请刷新并重新确认结算范围。"));
    }
    if (preview.value("pending_count").toInt() == 0) {
        throw ServiceError("validation", QStringLiteral("没有已通过且尚未结算的账目。"));
    }
    auto locked = query(
        "SELECT 1 FROM expenses e JOIN expense_locks l ON l.expense_id=e.id "
        "WHERE e.ledger_id=? AND e.status='approved' AND superseded=0 AND l.expires_at>? LIMIT 1",
        {ledger_id, clock_()});
    if (locked.next()) {
        throw ServiceError("locked",
                           QStringLiteral("待结算账目正在编辑，请等修改完成后再生成结算。"));
    }
    const QString id = identifier();
    // Insert the immutable header before its links; the snapshot is computed from the projected
    // state below.
    auto rows = query(
        "SELECT id,version,payload,amount_cents FROM expenses WHERE ledger_id=? AND "
        "status='approved' AND superseded=0 ORDER BY id",
        {ledger_id});
    struct Entry {
        QString id;
        int version;
        QString form;
        QString shares;
    };
    QVector<Entry> entries;
    while (rows.next()) {
        auto parts = query(
            "SELECT user_id,amount_cents FROM expense_shares WHERE expense_id=? ORDER BY user_id",
            {rows.value(0)});
        QJsonArray shares;
        qint64 total = 0;
        while (parts.next()) {
            total = add(total, parts.value(1).toLongLong());
            shares.append(QJsonObject{{"user_id", parts.value(0).toString()},
                                      {"amount", formatMoney(parts.value(1).toLongLong())}});
        }
        if (shares.isEmpty() || total != rows.value(3).toLongLong()) {
            throw ServiceError("integrity",
                               QStringLiteral("待结算账目的承担合计不一致，结算未生成。"));
        }
        entries.append({rows.value(0).toString(), rows.value(1).toInt(), rows.value(2).toString(),
                        QString::fromUtf8(QJsonDocument(shares).toJson(QJsonDocument::Compact))});
    }
    auto updated = query(
        "UPDATE expenses SET status='settled',version=version+1,updated_at=? WHERE ledger_id=? AND "
        "status='approved' AND superseded=0",
        {clock_(), ledger_id});
    if (updated.numRowsAffected() != entries.size()) {
        throw ServiceError("conflict", QStringLiteral("结算范围已变化，请刷新。"));
    }
    auto snapshot = balances(ledger_id);
    snapshot["new_net_amount"] = preview.value("pending_net_amount");
    snapshot.remove("balance_token");
    query("INSERT INTO settlements VALUES(?,?,?,?,?,?,?,?)",
          {id, ledger_id, actor, clock_(), token, entries.size(),
           cents(preview.value("pending_amount").toString()), json(snapshot)});
    for (const auto& entry : entries) {
        query("INSERT INTO settlement_expenses VALUES(?,?,?,?,?)",
              {id, entry.id, entry.version, entry.form, entry.shares});
        query("DELETE FROM expense_locks WHERE expense_id=? AND expires_at<=?",
              {entry.id, clock_()});
    }
    log(actor, ledger_id, "create_settlement", id,
        QStringLiteral("结算%1笔，共%2元")
            .arg(entries.size())
            .arg(preview.value("pending_amount").toString()));
    transaction.commit();
    return detail(actor, {{"ledger_id", ledger_id}, {"settlement_id", id}});
}

QJsonObject SettlementService::repay(const QString& actor, const QJsonObject& parameters) {
    const QString ledger_id = text(parameters, "ledger_id");
    const QString role = requireMember(actor, ledger_id, true);
    const QString payer = text(parameters, "payer_id"),
                  recipient = text(parameters, "recipient_id");
    const QString key = text(parameters, "request_key");
    if (key.isEmpty()) {
        throw ServiceError("validation", QStringLiteral("缺少还款请求编号，请重新打开还款窗口。"));
    }
    if (payer == recipient) {
        throw ServiceError("validation", QStringLiteral("付款人与收款人不能相同。"),
                           "recipient_id");
    }
    if (role != "admin" && payer != actor) {
        throw ServiceError("forbidden",
                           QStringLiteral("普通成员只能登记本人付出的还款，代登记请联系管理员。"));
    }
    for (const auto& participant : {payer, recipient}) {
        auto member = query("SELECT 1 FROM memberships WHERE ledger_id=? AND user_id=?",
                            {ledger_id, participant});
        if (!member.next()) {
            throw ServiceError("forbidden", QStringLiteral("还款双方必须为本账本当前或历史成员。"));
        }
    }
    const qint64 amount = parseMoney(text(parameters, "amount", 32));
    if (amount <= 0) {
        throw ServiceError("validation", QStringLiteral("还款金额须大于0。"), "amount");
    }
    const auto paid_at = parameters.value("paid_at");
    const qint64 time = paid_at.toInteger(-1);
    if (!paid_at.isDouble() || paid_at.toDouble() != static_cast<double>(time) || time <= 0 ||
        time > clock_() + 300) {
        throw ServiceError("validation", QStringLiteral("请填写已实际发生的还款时间。"), "paid_at");
    }
    const QString note =
        parameters.contains("note") ? text(parameters, "note", 500) : QStringLiteral("");
    const QString fingerprint = hash({{"payer", payer},
                                      {"recipient", recipient},
                                      {"amount", formatMoney(amount)},
                                      {"paid_at", time},
                                      {"note", note}});
    Transaction transaction(database_);
    auto previous =
        query("SELECT id,actor_id,fingerprint FROM repayments WHERE ledger_id=? AND request_key=?",
              {ledger_id, key});
    if (previous.next()) {
        if (previous.value(1).toString() != actor || previous.value(2).toString() != fingerprint) {
            throw ServiceError(
                "conflict",
                QStringLiteral("该请求已登记，请先刷新核对上次还款，不能用同一请求覆盖不同内容。"));
        }
        return {{"id", previous.value(0).toString()}, {"reused", true}};
    }
    const auto current = balances(ledger_id);
    if (text(parameters, "balance_token") != current.value("balance_token").toString()) {
        throw ServiceError("conflict", QStringLiteral("当前余额已变化，请刷新并重新核对还款。"));
    }
    if (!current.value("has_settlements").toBool()) {
        throw ServiceError("validation", QStringLiteral("请先生成结算，再登记该账本的还款。"));
    }
    qint64 payer_net = 0, recipient_net = 0;
    for (const auto& entry : current.value("members").toArray()) {
        const auto row = entry.toObject();
        // Signed balance strings are internal exact-cent values, not input amounts.
        const qint64 net = cents(row.value("balance").toString());
        if (row.value("user_id").toString() == payer) {
            payer_net = net;
        }
        if (row.value("user_id").toString() == recipient) {
            recipient_net = net;
        }
    }
    const qint64 available =
        std::min(std::max<qint64>(0, -payer_net), std::max<qint64>(0, recipient_net));
    const bool overpayment = amount > available;
    if (overpayment && !parameters.value("confirm_overpayment").toBool()) {
        throw ServiceError(
            "confirmation_required",
            QStringLiteral(
                "本次%1元超过双方当前可结清金额%"
                "2元。请核对方向及金额；确认实际已多还后才继续，余额可能转为反向应收/应付。")
                .arg(formatMoney(amount), formatMoney(available)),
            "amount");
    }
    const QString id = identifier();
    query("INSERT INTO repayments VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
          {id, ledger_id, actor, payer, recipient, amount, time, clock_(), note, key, fingerprint,
           overpayment});
    // Check aggregate bounds before committing either the payment or its audit record.
    static_cast<void>(balances(ledger_id));
    log(actor, ledger_id, "record_repayment", id,
        QStringLiteral("%1向%2还款%3元%4")
            .arg(payer, recipient, formatMoney(amount),
                 overpayment ? QStringLiteral("（已确认超额）") : QString()));
    transaction.commit();
    return {{"id", id}, {"reused", false}};
}
}  // namespace ledger
