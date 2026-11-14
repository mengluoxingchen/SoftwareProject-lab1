#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include "domain/service_error.h"
#include "finance/finance_projection.h"
#include "identity/identity_service.h"

namespace ledger {
namespace {
QString text(const QJsonObject& object, const QString& key, int maximum = 128) {
    const auto value = object.value(key);
    if ((!value.isUndefined() && !value.isString()) || value.toString().size() > maximum) {
        throw ServiceError("validation", QStringLiteral("输入格式或长度不正确。"), key);
    }
    const auto result = value.toString().trimmed();
    return result.isNull() ? QStringLiteral("") : result;
}
QJsonObject object(const QVariant& value) {
    return QJsonDocument::fromJson(value.toString().toUtf8()).object();
}
QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
class Transaction final {
public:
    explicit Transaction(QSqlDatabase database) : database_(database) {
        if (!database_.transaction()) {
            throw std::runtime_error("Cannot begin membership transaction.");
        }
    }
    ~Transaction() {
        if (!committed_) {
            database_.rollback();
        }
    }
    void commit() {
        if (!database_.commit()) {
            throw std::runtime_error("Cannot commit membership transaction.");
        }
        committed_ = true;
    }

private:
    QSqlDatabase database_;
    bool committed_ = false;
};
}  // namespace

QJsonObject IdentityService::membershipPreview(const QString& actor,
                                               const QJsonObject& parameters) const {
    const QString id = text(parameters, "ledger_id"), role = requireMember(id, actor);
    auto result = ledgerDetails(actor, {{"ledger_id", id}});
    const auto balances = finance_detail::ledgerBalances(database_, id);
    QString balance = "0.00";
    for (const auto& value : balances.value("members").toArray()) {
        const auto member = value.toObject();
        if (member.value("user_id").toString() == actor) {
            balance = member.value("balance").toString();
        }
    }
    auto pending = query(
        "SELECT COUNT(*) FROM expenses WHERE ledger_id=? AND status IN ('pending','approved') AND "
        "superseded=0",
        {id});
    pending.next();
    auto revision =
        query("SELECT COALESCE(MAX(rowid),0) FROM operation_logs WHERE ledger_id=?", {id});
    revision.next();
    result["balance"] = balance;
    result["pending_count"] = pending.value(0).toLongLong();
    result["balance_token"] = balances.value("balance_token");
    result["revision"] = revision.value(0).toLongLong();
    result["can_exit"] = role != "admin" || result.value("members").toArray().size() > 1;
    result["preview_token"] = finance_detail::digest(result);
    return result;
}

QJsonObject IdentityService::changeMembership(const QString& actor, const QJsonObject& parameters,
                                              bool exiting) {
    const QString id = text(parameters, "ledger_id"), key = text(parameters, "request_key");
    if (key.isEmpty() || !parameters.value("confirmed").toBool()) {
        throw ServiceError("validation", QStringLiteral("请核对退出或交接内容后确认。"));
    }
    if (exiting && !parameters.value("keep_history").isBool()) {
        throw ServiceError("validation", QStringLiteral("请选择是否保留历史只读权限。"));
    }
    const bool keep = exiting && parameters.value("keep_history").toBool();
    const QString successor = text(parameters, "successor_id"),
                  deadline = text(parameters, "settlement_deadline", 10);
    const QString request_key = (exiting ? "exit:" : "transfer:") + key;
    const QString fingerprint = finance_detail::digest({{"actor", actor},
                                                        {"successor", successor},
                                                        {"keep_history", keep},
                                                        {"deadline", deadline}});
    Transaction transaction(database_);
    auto previous = query(
        "SELECT actor_id,fingerprint,detail FROM membership_events WHERE ledger_id=? AND "
        "request_key=?",
        {id, request_key});
    if (previous.next()) {
        if (previous.value(0).toString() != actor || previous.value(1).toString() != fingerprint) {
            throw ServiceError("conflict", QStringLiteral("该请求已经处理，不能改为其他内容。"));
        }
        auto result = object(previous.value(2));
        result["reused"] = true;
        return result;
    }
    const QString role = requireMember(id, actor);
    const auto preview = membershipPreview(actor, parameters);
    if (text(parameters, "preview_token") != preview.value("preview_token").toString()) {
        throw ServiceError("conflict",
                           QStringLiteral("成员、账目或余额已变化，请重新打开退出/交接窗口核对。"));
    }
    if (!deadline.isEmpty()) {
        const auto date = QDate::fromString(deadline, Qt::ISODate);
        if (!exiting || !date.isValid() || date.toString(Qt::ISODate) != deadline ||
            date < QDateTime::fromSecsSinceEpoch(clock_()).date()) {
            throw ServiceError("validation",
                               QStringLiteral("结清截止日期须为今天或之后的有效日期。"),
                               "settlement_deadline");
        }
    }
    if (!exiting && role != "admin") {
        throw ServiceError("forbidden", QStringLiteral("只有当前管理员可以交接负责人。"));
    }
    if (role == "admin") {
        if (preview.value("members").toArray().size() <= 1) {
            throw ServiceError("conflict",
                               QStringLiteral("已无其他当前成员，请使用归档 / 删除账本。"));
        }
        if (successor.isEmpty() || successor == actor || requireMember(id, successor) == "admin") {
            throw ServiceError("validation", QStringLiteral("请选择另一位当前成员作为新管理员。"),
                               "successor_id");
        }
    } else if (!successor.isEmpty()) {
        throw ServiceError("forbidden", QStringLiteral("普通成员退出不能指定或更换管理员。"));
    }

    // Freeze authorized object identifiers AND readable values. Later edits, backdated
    // insertions, new refunds, and draft publication cannot expand the departed user's scope.
    QJsonObject context;
    query("DELETE FROM membership_history WHERE ledger_id=? AND user_id=?", {id, actor});
    if (exiting && keep) {
        context = ledgerDetails(actor, {{"ledger_id", id}});
        context.remove("financial_members");
        auto scope = query(
            "SELECT id,created_at FROM expenses WHERE ledger_id=? AND (status<>'draft' OR "
            "creator_id=?) ORDER BY created_at,id",
            {id, actor});
        QStringList ids;
        QHash<QString, qint64> created;
        while (scope.next()) {
            ids.append(scope.value(0).toString());
            created[scope.value(0).toString()] = scope.value(1).toLongLong();
        }
        QSet<QString> allowed(ids.begin(), ids.end()), event_ids;
        for (const auto& expense : ids) {
            const QJsonObject reference{{"ledger_id", id}, {"expense_id", expense}};
            auto detail = expenses_.dispatch(actor, "expense_detail", reference);
            const auto history = expenses_.dispatch(actor, "expense_history", reference);
            auto finance = finances_.dispatch(actor, "finance_info",
                                              {{"ledger_id", id}, {"source_id", expense}});
            detail["created_at"] = created.value(expense);
            detail["can_edit"] = false;
            detail["can_review"] = false;
            detail["can_refund"] = false;
            detail["locked"] = false;
            detail["lock_holder"] = "";
            detail["lock_expires_at"] = 0;
            QJsonArray similar;
            for (const auto& value : detail.value("similar_records").toArray()) {
                if (allowed.contains(value.toObject().value("id").toString())) {
                    similar.append(value);
                }
            }
            detail["similar_records"] = similar;
            finance["can_refund"] = false;
            finance["can_correct"] = false;
            finance["receivers"] = QJsonArray{};
            for (const auto& value : finance.value("events").toArray()) {
                event_ids.insert(value.toObject().value("id").toString());
            }
            query("INSERT INTO membership_history VALUES(?,?,?,?,?)",
                  {id, actor, "expense", expense,
                   finance_detail::json(
                       {{"detail", detail}, {"history", history}, {"finance", finance}})});
        }
        for (const auto& event : event_ids) {
            const auto value = finances_.dispatch(actor, "finance_event",
                                                  {{"ledger_id", id}, {"event_id", event}});
            query("INSERT INTO membership_history VALUES(?,?,?,?,?)",
                  {id, actor, "finance_event", event, finance_detail::json(value)});
        }
    }
    if (role == "admin") {
        query(
            "UPDATE memberships SET role='member' WHERE ledger_id=? AND user_id=? AND "
            "status='active' AND role='admin'",
            {id, actor});
        query(
            "UPDATE memberships SET role='admin' WHERE ledger_id=? AND user_id=? AND "
            "status='active'",
            {id, successor});
        // Revoke active edit leases: an old administrator session must not retain elevated work.
        query(
            "DELETE FROM expense_locks WHERE holder_id IN (?,?) AND expense_id IN (SELECT id FROM "
            "expenses WHERE ledger_id=?)",
            {actor, successor, id});
        log(id, actor, "transfer_admin", successor,
            QStringLiteral("原管理员%1交接给%2").arg(actor, successor));
    }
    QJsonObject result{{"ledger_id", id},
                       {"exited", exiting},
                       {"historical", keep},
                       {"balance", preview.value("balance")},
                       {"settlement_deadline", deadline},
                       {"successor_id", successor}};
    if (exiting) {
        query(
            "UPDATE memberships SET "
            "status='exited',exited_at=?,keep_history=?,settlement_deadline=?,history_context=? "
            "WHERE ledger_id=? AND user_id=?",
            {clock_(), keep ? 1 : 0, deadline, finance_detail::json(context), id, actor});
        query(
            "DELETE FROM expense_locks WHERE holder_id=? AND expense_id IN (SELECT id FROM "
            "expenses WHERE ledger_id=?)",
            {actor, id});
        log(id, actor, "exit_ledger", actor,
            QStringLiteral("退出；历史只读%1；当时余额%2元；截止日期%3")
                .arg(keep ? "保留" : "不保留", preview.value("balance").toString(),
                     deadline.isEmpty() ? QStringLiteral("未设置") : deadline));
    }
    if (exiting) {
        query(
            "UPDATE memberships SET audit_cutoff=(SELECT COALESCE(MAX(rowid),0) FROM "
            "operation_logs WHERE ledger_id=?) WHERE ledger_id=? AND user_id=?",
            {id, id, actor});
    }
    query("INSERT INTO membership_events VALUES(?,?,?,?,?,?,?,?,?)",
          {identifier(), id, actor, exiting ? "exit" : "transfer",
           role == "admin" ? successor : actor, request_key, fingerprint,
           finance_detail::json(result), clock_()});
    transaction.commit();
    return result;
}

std::optional<QJsonObject> IdentityService::historicalRead(const QString& actor,
                                                           const QString& method,
                                                           const QJsonObject& parameters) const {
    const QString id = text(parameters, "ledger_id");
    auto membership = query(
        "SELECT "
        "m.status,m.keep_history,m.exited_at,m.settlement_deadline,m.history_context,l.status FROM "
        "memberships m JOIN ledgers l ON l.id=m.ledger_id WHERE m.ledger_id=? AND m.user_id=?",
        {id, actor});
    if (!membership.next() || membership.value(0).toString() != "exited") {
        return std::nullopt;
    }
    if (!membership.value(1).toBool() || membership.value(5).toString() == "deleted") {
        throw ServiceError("forbidden",
                           QStringLiteral("退出后没有该账本的访问权限，或业务数据已经删除。"));
    }
    if (method == "history_context") {
        auto context = object(membership.value(4));
        context["id"] = id;
        context["role"] = "history_read_only";
        context["historical"] = true;
        context["exited_at"] = membership.value(2).toLongLong();
        context["settlement_deadline"] = membership.value(3).toString();
        context["capabilities"] = QJsonObject{{"manage_members", false},
                                              {"review", false},
                                              {"write_expense", false},
                                              {"settle", false}};
        return context;
    }
    if (method == "expense_list") {
        const QString status = text(parameters, "status", 32);
        if (!status.isEmpty() &&
            !QStringList{"draft", "pending", "approved", "rejected", "withdrawn", "settled"}
                 .contains(status)) {
            throw ServiceError("validation", QStringLiteral("账目状态不正确。"));
        }
        auto rows = query(
            "SELECT snapshot FROM membership_history WHERE ledger_id=? AND user_id=? AND "
            "kind='expense' ORDER BY json_extract(snapshot,'$.detail.created_at') DESC,object_id",
            {id, actor});
        QJsonArray result;
        while (rows.next()) {
            const auto detail = object(rows.value(0)).value("detail").toObject(),
                       form = detail.value("form").toObject();
            const QString state = detail.value("status").toString();
            if (status.isEmpty() ? state == "withdrawn" : state != status) {
                continue;
            }
            QStringList bearers;
            for (const auto& value : detail.value("shares").toArray()) {
                bearers.append(value.toObject().value("username").toString());
            }
            result.append(QJsonObject{{"id", detail.value("id")},
                                      {"date", form.value("date")},
                                      {"description", form.value("description")},
                                      {"amount", form.value("amount")},
                                      {"status", state},
                                      {"payer", detail.value("payer")},
                                      {"creator", detail.value("creator")},
                                      {"participants", bearers.join("、")},
                                      {"corrected", detail.value("corrected")},
                                      {"superseded", detail.value("superseded")},
                                      {"is_adjustment", detail.value("is_adjustment")},
                                      {"can_edit", false},
                                      {"can_withdraw", false}});
            if (result.size() == 1000) {
                break;
            }
        }
        return QJsonObject{{"expenses", result},
                           {"can_adjust", false},
                           {"can_supplement", false},
                           {"historical", true}};
    }
    const bool event = method == "finance_event";
    if (method != "expense_detail" && method != "expense_history" && method != "finance_info" &&
        !event) {
        throw ServiceError(
            "forbidden",
            QStringLiteral(
                "历史只读权限只允许查看退出时保存的账目与明细，不能写入或查看当前余额。"));
    }
    const QString object_id = text(parameters, event                      ? "event_id"
                                               : method == "finance_info" ? "source_id"
                                                                          : "expense_id");
    auto saved = query(
        "SELECT snapshot FROM membership_history WHERE ledger_id=? AND user_id=? AND kind=? AND "
        "object_id=?",
        {id, actor, event ? "finance_event" : "expense", object_id});
    if (!saved.next()) {
        throw ServiceError("forbidden", QStringLiteral("该记录不在退出时保存的历史授权范围内。"));
    }
    const auto snapshot = object(saved.value(0));
    return event ? snapshot
                 : snapshot
                       .value(method == "expense_detail"    ? "detail"
                              : method == "expense_history" ? "history"
                                                            : "finance")
                       .toObject();
}
}  // namespace ledger
