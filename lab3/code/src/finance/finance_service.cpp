#include "finance/finance_service.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <stdexcept>
#include <utility>

#include "domain/service_error.h"
#include "finance/finance_projection.h"
#include "infrastructure/audit_store.h"
#include "infrastructure/database.h"

namespace ledger {
namespace {
QString text(const QJsonObject& object, const QString& key, int maximum = 128) {
    const auto value = object.value(key);
    if ((!value.isUndefined() && !value.isString()) || value.toString().size() > maximum) {
        throw ServiceError("validation", QStringLiteral("字段格式或长度不正确。"), key);
    }
    const auto result = value.toString().trimmed();
    return result.isNull() ? QStringLiteral("") : result;
}
QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString requestKey(const QJsonObject& parameters) {
    const auto value = text(parameters, "request_key");
    return value.isEmpty() ? QString() : QStringLiteral("request:") + value;
}
void requireReason(const QString& reason) {
    if (reason.isEmpty()) {
        throw ServiceError("validation", QStringLiteral("请填写退款或调整原因。"), "reason");
    }
}
QJsonObject content(const QJsonObject& parameters, bool refund) {
    QJsonObject result{{"kind", refund ? QStringLiteral("refund") : text(parameters, "kind")},
                       {"source_id", text(parameters, "source_id")},
                       {"amount", formatMoney(parseMoney(text(parameters, "amount", 32)))},
                       {"reason", text(parameters, "reason", 500)}};
    if (refund) {
        result["receiver_id"] = text(parameters, "receiver_id");
        result["occurred_at"] = parameters.value("occurred_at");
    } else {
        for (const QString& key :
             QStringList{"date", "payer_id", "description", "category", "split_method"}) {
            result[key] = text(parameters, key,
                               key == "description" ? 500
                               : key == "category"  ? 80
                                                    : 128);
        }
        result["splits"] = parameters.value("splits");
    }
    return result;
}
class Transaction final {
public:
    explicit Transaction(QSqlDatabase database) : database_(std::move(database)) {
        if (!database_.transaction()) {
            throw std::runtime_error("Cannot begin financial transaction.");
        }
    }
    ~Transaction() {
        if (!committed_) {
            database_.rollback();
        }
    }
    void commit() {
        if (!database_.commit()) {
            throw std::runtime_error("Cannot commit financial transaction.");
        }
        committed_ = true;
    }

private:
    QSqlDatabase database_;
    bool committed_ = false;
};
QString similarKey(const QString& ledger_id, const QJsonObject& form) {
    const auto bytes =
        QJsonDocument(QJsonArray{ledger_id, form.value("date"),
                                 QString::number(parseMoney(form.value("amount").toString())),
                                 form.value("payer_id"), form.value("description")})
            .toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
}  // namespace

FinanceService::FinanceService(Database& database, Clock clock)
    : database_(database.connection()), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] { return QDateTime::currentSecsSinceEpoch(); };
    }
}
QString FinanceService::requireMember(const QString& actor, const QString& ledger_id,
                                      bool admin) const {
    auto row = finance_detail::query(
        database_,
        "SELECT m.role FROM memberships m JOIN ledgers l ON l.id=m.ledger_id WHERE m.ledger_id=? "
        "AND m.user_id=? AND m.status='active' AND l.status='active'",
        {ledger_id, actor});
    if (!row.next() || (admin && row.value(0).toString() != "admin")) {
        throw ServiceError("forbidden",
                           QStringLiteral("无权限。退款和结算后调整须由当前账本管理员核对登记。"));
    }
    return row.value(0).toString();
}
QJsonObject FinanceService::dispatch(const QString& actor, const QString& method,
                                     const QJsonObject& parameters) {
    if (method == "finance_write_status") {
        const QString ledger = text(parameters, "ledger_id");
        requireMember(actor, ledger);
        auto row = finance_detail::query(
            database_,
            "SELECT id,new_expense_id FROM expense_finance_events WHERE ledger_id=? "
            "AND actor_id=? AND request_key=?",
            {ledger, actor, requestKey(parameters)});
        if (!row.next()) return {{"found", false}};
        return {{"found", true},
                {"event_id", row.value(0).toString()},
                {"expense_id", row.value(1).toString()}};
    }
    if (method == "finance_info") {
        return info(actor, parameters);
    }
    if (method == "finance_event") {
        return event(actor, parameters);
    }
    if (method == "finance_refund") {
        return refund(actor, parameters);
    }
    if (method == "finance_adjustment_preview") {
        return adjustmentPreview(actor, parameters);
    }
    if (method == "finance_adjustment") {
        return adjustment(actor, parameters);
    }
    throw ServiceError("unknown_method", QStringLiteral("不支持的财务操作。"));
}
QJsonObject FinanceService::info(const QString& actor, const QJsonObject& parameters) const {
    const QString ledger_id = text(parameters, "ledger_id");
    const QString role = requireMember(actor, ledger_id);
    auto state = finance_detail::projection(database_, ledger_id, text(parameters, "source_id"));
    if (state.value("status").toString() == "draft" &&
        state.value("creator_id").toString() != actor) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看他人的私有草稿。"));
    }
    state["can_refund"] =
        role == "admin" && !state.value("superseded").toBool() &&
        (state.value("status") == "approved" || state.value("status") == "settled") &&
        finance_detail::cents(state.value("net_amount").toString()) > 0;
    state["can_correct"] = role == "admin" && !state.value("superseded").toBool() &&
                           state.value("status") == "settled";
    QJsonArray history;
    auto events = finance_detail::query(
        database_,
        "SELECT "
        "f.id,f.kind,f.amount_cents,f.delta_cents,f.occurred_at,f.reason,u.username,f.source_id,f."
        "new_expense_id FROM expense_finance_events f JOIN users u ON u.id=f.actor_id WHERE "
        "f.root_id=? ORDER BY f.revision DESC LIMIT 100",
        {state.value("root_id").toString()});
    while (events.next()) {
        history.append(QJsonObject{{"id", events.value(0).toString()},
                                   {"kind", events.value(1).toString()},
                                   {"amount", formatMoney(events.value(2).toLongLong())},
                                   {"delta", formatMoney(events.value(3).toLongLong())},
                                   {"occurred_at", events.value(4).toLongLong()},
                                   {"reason", events.value(5).toString()},
                                   {"actor", events.value(6).toString()},
                                   {"source_id", events.value(7).toString()},
                                   {"new_expense_id", events.value(8).toString()}});
    }
    state["events"] = history;
    QJsonArray receivers;
    auto members =
        finance_detail::query(database_,
                              "SELECT u.id,u.username,m.status FROM memberships m JOIN users u ON "
                              "u.id=m.user_id WHERE m.ledger_id=? ORDER BY u.username,u.id",
                              {ledger_id});
    while (members.next()) {
        receivers.append(QJsonObject{{"id", members.value(0).toString()},
                                     {"username", members.value(1).toString()},
                                     {"status", members.value(2).toString()}});
    }
    state["receivers"] = receivers;
    return state;
}
QJsonObject FinanceService::event(const QString& actor, const QJsonObject& parameters) const {
    const QString ledger_id = text(parameters, "ledger_id");
    requireMember(actor, ledger_id);
    auto row = finance_detail::query(
        database_,
        "SELECT "
        "f.id,f.kind,f.amount_cents,f.delta_cents,f.occurred_at,f.reason,u.username,f.before_state,"
        "f.after_state,COALESCE(r.username,'') FROM expense_finance_events f JOIN users u ON "
        "u.id=f.actor_id LEFT JOIN users r ON r.id=f.receiver_id WHERE f.id=? AND f.ledger_id=?",
        {text(parameters, "event_id"), ledger_id});
    if (!row.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看该退款或调整记录。"));
    }
    return {{"id", row.value(0).toString()},
            {"kind", row.value(1).toString()},
            {"amount", formatMoney(row.value(2).toLongLong())},
            {"delta", formatMoney(row.value(3).toLongLong())},
            {"occurred_at", row.value(4).toLongLong()},
            {"reason", row.value(5).toString()},
            {"actor", row.value(6).toString()},
            {"before", QJsonDocument::fromJson(row.value(7).toString().toUtf8()).object()},
            {"after", QJsonDocument::fromJson(row.value(8).toString().toUtf8()).object()},
            {"receiver", row.value(9).toString()}};
}
void FinanceService::appendEvent(const QString& actor, const QString& ledger_id, const QString& id,
                                 const QString& kind, const QString& source,
                                 const QString& new_expense, const QString& receiver, qint64 amount,
                                 qint64 delta, qint64 occurred_at, const QString& reason,
                                 const QString& key, const QString& fingerprint,
                                 const QJsonObject& before, const QJsonObject& after) {
    using namespace finance_detail;
    query(database_,
          "INSERT INTO expense_finance_events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {id, ledger_id, after.value("root_id").toString(),
           source.isEmpty() ? QVariant() : QVariant(source),
           new_expense.isEmpty() ? QVariant() : QVariant(new_expense), actor, kind,
           receiver.isEmpty() ? QVariant() : QVariant(receiver), amount, delta, occurred_at,
           clock_(), reason, after.value("revision").toInteger(), key, fingerprint,
           json(storedState(before)), json(storedState(after))});
    const QString action = kind == "refund"       ? "record_refund"
                           : kind == "supplement" ? "record_supplement"
                                                  : "correct_settled_expense";
    const auto log_id = identifier();
    query(database_, "INSERT INTO operation_logs VALUES(?,?,?,?,?,?,?)",
          {log_id, ledger_id, actor, action, id,
           QStringLiteral("关联支出%1；原净额%2元，调整后净额%3元，变动%4元；原因：%5")
               .arg(source.isEmpty() ? new_expense : source,
                    before.value("net_amount").toString("0.00"),
                    after.value("net_amount").toString(), formatMoney(delta), reason),
           clock_()});
    appendAuditEvidence(database_, log_id);
}

QJsonObject FinanceService::refund(const QString& actor, const QJsonObject& parameters) {
    using namespace finance_detail;
    const QString ledger_id = text(parameters, "ledger_id");
    requireMember(actor, ledger_id, true);
    const auto values = content(parameters, true);
    const QString fingerprint = digest(values);
    const QString key = requestKey(parameters);
    if (key.isEmpty()) {
        throw ServiceError("validation", QStringLiteral("缺少请求编号，请重新打开退款窗口。"));
    }
    const qint64 amount = parseMoney(values.value("amount").toString());
    if (amount <= 0) {
        throw ServiceError("validation", QStringLiteral("退款金额须大于0。"), "amount");
    }
    const QString reason = values.value("reason").toString();
    requireReason(reason);
    const auto time_value = values.value("occurred_at");
    const qint64 time = time_value.toInteger(-1);
    if (!time_value.isDouble() || time_value.toDouble() != static_cast<double>(time) || time <= 0 ||
        time > clock_() + 300) {
        throw ServiceError("validation", QStringLiteral("请填写已实际发生的退款时间。"),
                           "occurred_at");
    }
    Transaction transaction(database_);
    auto previous = query(database_,
                          "SELECT id,actor_id,fingerprint FROM expense_finance_events WHERE "
                          "ledger_id=? AND request_key=?",
                          {ledger_id, key});
    if (previous.next()) {
        if (previous.value(1).toString() != actor || previous.value(2).toString() != fingerprint) {
            throw ServiceError("conflict",
                               QStringLiteral("该请求已登记，请核对原结果，不能覆盖为其他内容。"));
        }
        auto result =
            event(actor, {{"ledger_id", ledger_id}, {"event_id", previous.value(0).toString()}});
        result["reused"] = true;
        return result;
    }
    const QString source = values.value("source_id").toString(),
                  receiver = values.value("receiver_id").toString();
    auto before = projection(database_, ledger_id, source);
    if (before.value("superseded").toBool() ||
        (before.value("status") != "approved" && before.value("status") != "settled")) {
        throw ServiceError(
            "conflict",
            QStringLiteral(
                "仅可为当前有效的已通过或已结算支出登记退款，请先审核或查看最新调整记录。"));
    }
    if (text(parameters, "expected_token") != before.value("token").toString()) {
        throw ServiceError("conflict", QStringLiteral("支出或退款已变化，请刷新详情后重新核对。"));
    }
    auto lock = query(database_, "SELECT 1 FROM expense_locks WHERE expense_id=? AND expires_at>?",
                      {source, clock_()});
    if (lock.next()) {
        throw ServiceError("locked", QStringLiteral("该账目正在编辑，请等编辑完成后登记退款。"));
    }
    auto member = query(database_, "SELECT 1 FROM memberships WHERE ledger_id=? AND user_id=?",
                        {ledger_id, receiver});
    if (!member.next()) {
        throw ServiceError("validation", QStringLiteral("退款收款人须属于该账本的当前或历史成员。"),
                           "receiver_id");
    }
    const qint64 remaining = cents(before.value("net_amount").toString());
    if (amount > remaining) {
        throw ServiceError(
            "validation",
            QStringLiteral("退款不能超过剩余可退金额%1元。").arg(formatMoney(remaining)), "amount");
    }
    auto parts = shares(before.value("effective_shares").toArray());
    qint64 sum = 0;
    for (const auto& part : parts) {
        sum += part.amount;
    }
    if (sum != remaining) {
        throw ServiceError("integrity", QStringLiteral("承担明细不一致，退款未登记。"));
    }
    const auto returned = allocateRefund(amount, parts);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        parts[index].amount -= returned[index].amount;
    }
    auto after = storedState(before);
    after["net_amount"] = formatMoney(remaining - amount);
    after["refunded_amount"] =
        formatMoney(cents(before.value("refunded_amount").toString()) + amount);
    after["effective_shares"] = shareJson(parts, before.value("effective_shares").toArray());
    after["revision"] = before.value("root_revision").toInteger() + 1;
    const QString id = identifier();
    appendEvent(actor, ledger_id, id, "refund", source, {}, receiver, amount, -amount, time, reason,
                key, fingerprint, before, after);
    static_cast<void>(ledgerBalances(database_, ledger_id));
    transaction.commit();
    return event(actor, {{"ledger_id", ledger_id}, {"event_id", id}});
}

QJsonObject FinanceService::validateForm(const QString& ledger_id,
                                         const QJsonObject& parameters) const {
    using namespace finance_detail;
    const qint64 amount = parseMoney(text(parameters, "amount", 32));
    const QString kind = text(parameters, "kind");
    if (amount == 0 && kind != "correction") {
        throw ServiceError("validation", QStringLiteral("补记金额须大于0。"), "amount");
    }
    QJsonObject form{{"amount", formatMoney(amount)},
                     {"date", text(parameters, "date", 16)},
                     {"payer_id", text(parameters, "payer_id")},
                     {"description", text(parameters, "description", 500)},
                     {"category", text(parameters, "category", 80)},
                     {"split_method", text(parameters, "split_method", 32)}};
    if (amount == 0) {
        form["splits"] = QJsonArray{};
        return {{"form", form}, {"shares", QJsonArray{}}};
    }
    const QString date = form.value("date").toString();
    if (!QDate::fromString(date, Qt::ISODate).isValid() ||
        QDate::fromString(date, Qt::ISODate).toString(Qt::ISODate) != date) {
        throw ServiceError("validation", QStringLiteral("请选择有效消费日期。"), "date");
    }
    if (form.value("description").toString().isEmpty()) {
        throw ServiceError("validation", QStringLiteral("请填写消费说明。"), "description");
    }
    QHash<QString, QString> names;
    auto people = query(database_,
                        "SELECT m.user_id,u.username FROM memberships m JOIN users u ON "
                        "u.id=m.user_id WHERE m.ledger_id=? AND m.status='active'",
                        {ledger_id});
    while (people.next()) {
        names.insert(people.value(0).toString(), people.value(1).toString());
    }
    if (!names.contains(form.value("payer_id").toString())) {
        throw ServiceError("validation", QStringLiteral("请选择当前账本的实际付款人。"),
                           "payer_id");
    }
    if (!parameters.value("splits").isArray() ||
        parameters.value("splits").toArray().size() > 1000) {
        throw ServiceError("validation", QStringLiteral("费用承担人格式不正确。"), "splits");
    }
    QVector<SplitPart> parts;
    QJsonArray split_form;
    for (const auto& item : parameters.value("splits").toArray()) {
        const auto part = item.toObject();
        const QString id = text(part, "user_id"), value = text(part, "value", 32);
        if (!item.isObject() || !names.contains(id)) {
            throw ServiceError("validation", QStringLiteral("承担人须属于当前账本。"), "splits");
        }
        parts.append({id, value});
        split_form.append(QJsonObject{{"user_id", id}, {"value", value}});
    }
    const auto allocations = calculateSplit(amount, form.value("split_method").toString(), parts);
    auto result = shareJson(allocations);
    for (qsizetype index = 0; index < result.size(); ++index) {
        auto row = result[index].toObject();
        row["username"] = names.value(row.value("user_id").toString());
        result[index] = row;
    }
    form["splits"] = split_form;
    return {{"form", form}, {"shares", result}};
}

QJsonObject FinanceService::adjustmentPreview(const QString& actor,
                                              const QJsonObject& parameters) const {
    using namespace finance_detail;
    const QString ledger_id = text(parameters, "ledger_id");
    requireMember(actor, ledger_id, true);
    auto settled =
        query(database_, "SELECT 1 FROM settlements WHERE ledger_id=? LIMIT 1", {ledger_id});
    if (!settled.next()) {
        throw ServiceError("conflict",
                           QStringLiteral("账本尚未生成过结算，漏记支出请使用普通新增账目。"));
    }
    const QString kind = text(parameters, "kind"), reason = text(parameters, "reason", 500);
    requireReason(reason);
    if (kind != "supplement" && kind != "correction") {
        throw ServiceError("validation", QStringLiteral("请选择补记或结算后纠错。"));
    }
    const auto validated = validateForm(ledger_id, parameters);
    const auto form = validated.value("form").toObject();
    QJsonObject before;
    if (kind == "correction") {
        before = projection(database_, ledger_id, text(parameters, "source_id"));
        if (before.value("status") != "settled" || before.value("superseded").toBool()) {
            throw ServiceError(
                "conflict", QStringLiteral("仅可纠正当前有效的已结算账目，请查看最新调整记录。"));
        }
        if (text(parameters, "expected_token") != before.value("token").toString()) {
            throw ServiceError("conflict",
                               QStringLiteral("退款或调整已变化，请重新打开纠错表单。"));
        }
        auto held =
            query(database_, "SELECT 1 FROM expense_locks WHERE expense_id=? AND expires_at>?",
                  {text(parameters, "source_id"), clock_()});
        if (held.next()) {
            throw ServiceError("locked", QStringLiteral("账目正在编辑，请等待编辑完成。"));
        }
    }
    const qint64 gross = parseMoney(form.value("amount").toString());
    const qint64 refunded =
        before.isEmpty() ? 0 : cents(before.value("refunded_amount").toString());
    if (gross < refunded) {
        throw ServiceError(
            "validation",
            QStringLiteral("纠正后的原始金额不能低于已实际退款%1元。").arg(formatMoney(refunded)),
            "amount");
    }
    const QString source =
        kind == "correction" ? text(parameters, "source_id") : QStringLiteral("");
    if (gross > 0) {
        auto duplicate =
            query(database_,
                  "SELECT 1 FROM expenses WHERE ledger_id=? AND superseded=0 AND id<>? AND "
                  "similar_key=? AND status IN ('approved','settled') LIMIT 1",
                  {ledger_id, source, similarKey(ledger_id, form)});
        if (duplicate.next()) {
            throw ServiceError("duplicate_approved",
                               QStringLiteral("已有相同日期、金额、付款人与说明的有效账目，请先核对"
                                              "，真实不同支出需补充可区分说明。"));
        }
        auto own = query(database_,
                         "SELECT 1 FROM expenses WHERE ledger_id=? AND creator_id=? AND "
                         "superseded=0 AND id<>? AND similar_key=? AND status<>'draft' LIMIT 1",
                         {ledger_id, actor, source, similarKey(ledger_id, form)});
        if (own.next()) {
            throw ServiceError("duplicate", QStringLiteral("你已提交相同账目，请先核对原结果。"));
        }
    }
    const auto fingerprint = digest(content(parameters, false));
    const QString token = digest(
        {{"content", fingerprint}, {"before", before}, {"actor", actor}, {"ledger", ledger_id}});
    return {{"form", form},
            {"shares", validated.value("shares")},
            {"before", before},
            {"confirmation_token", token},
            {"net_amount", formatMoney(gross - refunded)},
            {"confirm_text",
             kind == "supplement"
                 ? QStringLiteral(
                       "确认补记原始支出%"
                       "1元？核对后立即计入当前余额，并保留本次管理员审核记录；旧结算快照不变。")
                       .arg(formatMoney(gross))
                 : QStringLiteral("原始金额%1元 → "
                                  "%2元，累计退款%3元，调整后净额%"
                                  "4元。原记录保留并标为已被替代，旧结算快照不变。确认已核对？")
                       .arg(before.value("gross_amount").toString(), formatMoney(gross),
                            formatMoney(refunded), formatMoney(gross - refunded))}};
}

QJsonObject FinanceService::adjustment(const QString& actor, const QJsonObject& parameters) {
    using namespace finance_detail;
    const QString ledger_id = text(parameters, "ledger_id");
    requireMember(actor, ledger_id, true);
    const QString key = requestKey(parameters);
    if (key.isEmpty() || !parameters.value("confirmed").toBool()) {
        throw ServiceError("confirmation_required", QStringLiteral("请核对并确认此次财务调整。"));
    }
    const auto fingerprint = digest(content(parameters, false));
    Transaction transaction(database_);
    auto previous = query(database_,
                          "SELECT id,actor_id,fingerprint FROM expense_finance_events WHERE "
                          "ledger_id=? AND request_key=?",
                          {ledger_id, key});
    if (previous.next()) {
        if (previous.value(1).toString() != actor || previous.value(2).toString() != fingerprint) {
            throw ServiceError(
                "conflict", QStringLiteral("该请求已登记，请先核对原结果，不能覆盖为不同内容。"));
        }
        auto result =
            event(actor, {{"ledger_id", ledger_id}, {"event_id", previous.value(0).toString()}});
        result["reused"] = true;
        return result;
    }
    const auto preview = adjustmentPreview(actor, parameters);
    if (text(parameters, "confirmation_token") != preview.value("confirmation_token").toString()) {
        throw ServiceError("conflict", QStringLiteral("确认内容已变化，请重新核对调整。"));
    }
    const QString kind = text(parameters, "kind"), reason = text(parameters, "reason", 500);
    const auto before = preview.value("before").toObject(), form = preview.value("form").toObject();
    const qint64 gross = parseMoney(form.value("amount").toString());
    const qint64 old_gross = before.isEmpty() ? 0 : cents(before.value("gross_amount").toString());
    const qint64 refunded =
        before.isEmpty() ? 0 : cents(before.value("refunded_amount").toString());
    const QString source =
        kind == "correction" ? text(parameters, "source_id") : QStringLiteral("");
    const QString expense_id = gross == 0 ? QStringLiteral("") : identifier();
    const QString root = kind == "correction" ? before.value("root_id").toString() : expense_id;
    if (kind == "correction") {
        auto updated = query(database_,
                             "UPDATE expenses SET superseded=1 WHERE id=? AND ledger_id=? AND "
                             "status='settled' AND superseded=0",
                             {source, ledger_id});
        if (updated.numRowsAffected() != 1) {
            throw ServiceError("conflict", QStringLiteral("原记录已调整，请刷新。"));
        }
    }
    const auto original = preview.value("shares").toArray();
    if (gross > 0) {
        query(database_,
              "INSERT INTO "
              "expenses(id,ledger_id,creator_id,payer_id,amount_cents,expense_date,description,"
              "category,legacy_expense_type,split_method,status,payload,similar_key,version,"
              "created_at,updated_at,submitted_at,is_adjustment,financial_root_id) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,'pending',?,?,1,?,?,?,1,?)",
              {expense_id, ledger_id, actor, form.value("payer_id").toString(), gross,
               form.value("date").toString(), form.value("description").toString(),
               form.value("category").toString(), QStringLiteral(""),
               form.value("split_method").toString(), json(form), similarKey(ledger_id, form),
               clock_(), clock_(), clock_(), root});
        for (const auto& value : shares(original)) {
            query(database_, "INSERT INTO expense_shares VALUES(?,?,?)",
                  {expense_id, value.user_id, value.amount});
        }
        query(database_, "INSERT INTO expense_reviews VALUES(?,?,?,?,?,?,?)",
              {identifier(), expense_id, actor, QStringLiteral("approve"),
               QStringLiteral("管理员核对结算后调整：") + reason, 1, clock_()});
        query(database_, "UPDATE expenses SET status='approved',version=2 WHERE id=?",
              {expense_id});
        query(database_, "UPDATE expenses SET status='settled',version=3 WHERE id=?", {expense_id});
        query(database_, "UPDATE expenses SET adjustment_reason=? WHERE id=?",
              {reason, expense_id});
    }
    auto effective = shares(original);
    if (gross > 0) {
        const auto allocations = allocateRefund(refunded, effective);
        for (qsizetype index = 0; index < effective.size(); ++index) {
            effective[index].amount -= allocations[index].amount;
        }
    }
    QJsonObject after{
        {"root_id", root},
        {"expense_id", expense_id},
        {"gross_amount", formatMoney(gross)},
        {"net_amount", formatMoney(gross - refunded)},
        {"refunded_amount", formatMoney(refunded)},
        {"payer_id", form.value("payer_id")},
        {"form", form},
        {"original_shares", original},
        {"effective_shares", shareJson(effective, original)},
        {"revision", before.isEmpty() ? 1 : before.value("root_revision").toInteger() + 1}};
    const QString id = identifier();
    appendEvent(actor, ledger_id, id, kind, source, expense_id, {}, gross, gross - old_gross,
                clock_(), reason, key, fingerprint, before, after);
    static_cast<void>(ledgerBalances(database_, ledger_id));
    transaction.commit();
    return event(actor, {{"ledger_id", ledger_id}, {"event_id", id}});
}

void FinanceService::approveSupplement(const QString& actor, const QString& ledger_id,
                                       const QString& expense_id) {
    using namespace finance_detail;
    requireMember(actor, ledger_id, true);
    auto row = query(database_,
                     "SELECT adjustment_reason FROM expenses WHERE id=? AND ledger_id=? AND "
                     "status='approved' AND is_adjustment=1 AND superseded=0",
                     {expense_id, ledger_id});
    if (!row.next()) {
        throw ServiceError("conflict", QStringLiteral("补记状态已变化，请刷新。"));
    }
    auto history =
        query(database_, "SELECT 1 FROM settlements WHERE ledger_id=? LIMIT 1", {ledger_id});
    if (!history.next()) {
        throw ServiceError("conflict", QStringLiteral("结算历史不存在，不能登记结算后补记。"));
    }
    const QString reason = row.value(0).toString();
    requireReason(reason);
    auto state = projection(database_, ledger_id, expense_id);
    const qint64 gross = cents(state.value("gross_amount").toString());
    qint64 total = 0;
    for (const auto& part : shares(state.value("effective_shares").toArray())) {
        total += part.amount;
    }
    if (gross <= 0 || total != gross || state.value("root_revision").toInteger() != 0) {
        throw ServiceError("integrity", QStringLiteral("补记承担明细或财务关联不一致。"));
    }
    query(database_,
          "UPDATE expenses SET status='settled',version=version+1,financial_root_id=? WHERE id=?",
          {expense_id, expense_id});
    state["revision"] = 1;
    appendEvent(actor, ledger_id, identifier(), "supplement", {}, expense_id, {}, gross, gross,
                clock_(), reason, QStringLiteral("review:") + expense_id,
                digest({{"expense", expense_id}}), {}, state);
    static_cast<void>(ledgerBalances(database_, ledger_id));
}
}  // namespace ledger
