#include <QJsonDocument>

#include "expense/expense_service.h"
#include "expense/expense_support.h"
#include "infrastructure/audit_store.h"

namespace ledger {
using namespace expense_detail;

void ExpenseService::appendLog(const QString& ledger_id, const QString& actor,
                               const QString& action, const QString& expense_id,
                               const QString& reason, const QJsonObject& amounts) {
    const auto log_id = identifier();
    query("INSERT INTO operation_logs VALUES(?,?,?,?,?,?,?)",
          {log_id, ledger_id, actor, action, expense_id, reason, clock_()});
    appendAuditEvidence(database_, log_id, false, amounts);
}

void ExpenseService::requireEditLock(const QString& actor, const QString& session,
                                     const QJsonObject& parameters) const {
    auto held = query(
        "SELECT 1 FROM expense_locks WHERE expense_id=? AND holder_id=? AND session_key=? AND "
        "token=? AND expires_at>?",
        {text(parameters, "expense_id", 128), actor, session, text(parameters, "lock_token", 128),
         clock_()});
    if (!held.next()) {
        throw ServiceError(
            "lock_lost",
            QStringLiteral("编辑锁已失效或属于其他会话，当前输入保留，请重新获取编辑锁后再试。"));
    }
}

void ExpenseService::requireUnlocked(const QString& expense_id) const {
    auto held = query("SELECT 1 FROM expense_locks WHERE expense_id=? AND expires_at>?",
                      {expense_id, clock_()});
    if (held.next()) {
        throw ServiceError("locked",
                           QStringLiteral("该记录正在被修改，请等修改完成后再审核或撤回。"));
    }
}

QJsonObject ExpenseService::lock(const QString& actor, const QString& session,
                                 const QString& operation, const QJsonObject& parameters) {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    const QString id = text(parameters, "expense_id", 128);
    const auto record = detail(actor, parameters);
    if (operation == "expense_lock_release") {
        auto released = query(
            "DELETE FROM expense_locks WHERE expense_id=? AND holder_id=? AND session_key=? AND "
            "token=?",
            {id, actor, session, text(parameters, "lock_token", 128)});
        return {{"released", released.numRowsAffected() == 1}};
    }
    if (!record.value("can_edit").toBool()) {
        throw ServiceError("forbidden", QStringLiteral("无权限修改该账目，或账目已撤回/结算。"));
    }
    if (operation == "expense_lock_renew") {
        requireEditLock(actor, session, parameters);
        query("UPDATE expense_locks SET expires_at=? WHERE expense_id=?", {clock_() + 120, id});
        return {{"expires_at", clock_() + 120}};
    }
    if (operation != "expense_lock_acquire") {
        throw ServiceError("unknown_method", QStringLiteral("不支持的编辑锁操作。"));
    }
    if (parameters.contains("version") &&
        parameters.value("version").toInt() != record.value("version").toInt()) {
        throw ServiceError(
            "conflict",
            QStringLiteral("账目内容已更新，不能覆盖新版本；请先保留当前输入，再重新打开账目。"));
    }
    Transaction transaction(database_);
    query("DELETE FROM expense_locks WHERE expense_id=? AND expires_at<=?", {id, clock_()});
    auto held =
        query("SELECT holder_id,session_key,token FROM expense_locks WHERE expense_id=?", {id});
    QString token;
    if (held.next()) {
        if (held.value(0).toString() != actor || held.value(1).toString() != session) {
            throw ServiceError("locked", QStringLiteral("该记录正在被修改，请稍后再试。"));
        }
        token = held.value(2).toString();
        query("UPDATE expense_locks SET expires_at=? WHERE expense_id=?", {clock_() + 120, id});
    } else {
        token = identifier();
        query("INSERT INTO expense_locks VALUES(?,?,?,?,?)",
              {id, actor, session, token, clock_() + 120});
    }
    transaction.commit();
    return {{"expense", detail(actor, parameters)},
            {"lock_token", token},
            {"expires_at", clock_() + 120}};
}

QJsonArray ExpenseService::similarRecords(const QString& expense_id, const QString& ledger_id,
                                          const QString& key) const {
    QJsonArray records;
    if (key.isEmpty()) {
        return records;
    }
    auto rows = query(
        "SELECT e.id,u.username,e.status,e.version FROM expenses e JOIN users u ON "
        "u.id=e.creator_id WHERE e.ledger_id=? AND e.similar_key=? AND e.id<>? AND e.superseded=0 "
        "AND e.status NOT IN "
        "('draft','withdrawn') ORDER BY e.created_at,e.id LIMIT 100",
        {ledger_id, key, expense_id});
    while (rows.next()) {
        records.append(QJsonObject{{"id", rows.value(0).toString()},
                                   {"creator", rows.value(1).toString()},
                                   {"status", rows.value(2).toString()},
                                   {"version", rows.value(3).toInt()}});
    }
    return records;
}

void ExpenseService::notifyCreator(const QJsonObject& record, const QString& ledger_id,
                                   const QString& kind, const QString& message) {
    query("INSERT INTO expense_notices VALUES(?,?,?,?,?,?,?,NULL)",
          {identifier(), record.value("creator_id").toString(), ledger_id,
           record.value("id").toString(), kind, message, clock_()});
}

QJsonObject ExpenseService::review(const QString& actor, const QJsonObject& parameters) {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    if (requireMember(actor, ledger_id, false) != "admin") {
        throw ServiceError("forbidden", QStringLiteral("只有管理员可以审核账目。"));
    }
    const auto record = detail(actor, parameters);
    const QString id = record.value("id").toString();
    const int version = record.value("version").toInt();
    if (parameters.value("version").toInt() != version ||
        record.value("status").toString() != "pending") {
        throw ServiceError("conflict", QStringLiteral("账目状态或版本已变化，请刷新后再审核。"));
    }
    const QString decision = text(parameters, "decision", 32);
    const QString reason = text(parameters, "reason");
    if (decision != "approve" && decision != "reject") {
        invalid(QStringLiteral("请选择通过或驳回。"), "decision");
    }
    if (decision == "reject" && reason.isEmpty()) {
        invalid(QStringLiteral("驳回时必须填写原因。"), "reason");
    }
    Transaction transaction(database_);
    requireUnlocked(id);
    if (decision == "approve") {
        auto key = query("SELECT similar_key FROM expenses WHERE id=?", {id});
        key.next();
        auto accepted = query(
            "SELECT id FROM expenses WHERE ledger_id=? AND similar_key=? AND id<>? AND "
            "superseded=0 AND status IN "
            "('approved','settled') LIMIT 1",
            {ledger_id, key.value(0), id});
        if (accepted.next()) {
            throw ServiceError("duplicate_approved",
                               QStringLiteral("相似账目已有一条通过或结算，不能重复通过。若确为不同"
                                              "支出，请先补充可区分的消费说明再重新提交。"));
        }
        if (!record.value("similar_records").toArray().isEmpty() &&
            !parameters.value("confirm_similar").toBool()) {
            invalid(QStringLiteral("请先核对相似账目，并勾选已完成核对。"), "similar");
        }
    }
    const QString status = decision == "approve" ? "approved" : "rejected";
    auto updated = query(
        "UPDATE expenses SET status=?,version=version+1,updated_at=? WHERE id=? AND version=? AND "
        "status='pending'",
        {status, clock_(), id, version});
    if (updated.numRowsAffected() != 1) {
        throw ServiceError("conflict", QStringLiteral("审核发生冲突，请刷新后再试。"));
    }
    query("INSERT INTO expense_reviews VALUES(?,?,?,?,?,?,?)",
          {identifier(), id, actor, decision, reason, version, clock_()});
    appendLog(ledger_id, actor, decision == "approve" ? "approve_expense" : "reject_expense", id,
              reason);
    if (decision == "approve" && record.value("is_adjustment").toBool()) {
        finances_.approveSupplement(actor, ledger_id, id);
    }
    notifyCreator(
        record, ledger_id, status,
        QStringLiteral("账目“%1”%2%3")
            .arg(record.value("form").toObject().value("description").toString(),
                 decision == "approve" ? (record.value("is_adjustment").toBool()
                                              ? QStringLiteral("补记已通过审核并计入当前余额。")
                                              : QStringLiteral("已通过审核。"))
                                       : QStringLiteral("已被驳回，原因："),
                 reason));
    transaction.commit();
    return detail(actor, parameters);
}

QJsonObject ExpenseService::withdraw(const QString& actor, const QJsonObject& parameters) {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    const auto record = detail(actor, parameters);
    const QString id = record.value("id").toString();
    if (!record.value("can_edit").toBool() || record.value("status").toString() == "draft") {
        throw ServiceError("forbidden",
                           QStringLiteral("只能撤回有权限管理且尚未结算的已提交账目。"));
    }
    const int version = record.value("version").toInt();
    if (parameters.value("version").toInt() != version) {
        throw ServiceError("conflict", QStringLiteral("账目已更新，请刷新后再撤回。"));
    }
    const QString reason = text(parameters, "reason");
    if (reason.isEmpty()) {
        invalid(QStringLiteral("撤回时必须填写原因。"), "reason");
    }
    Transaction transaction(database_);
    requireUnlocked(id);
    auto updated = query(
        "UPDATE expenses SET status='withdrawn',version=version+1,updated_at=? WHERE id=? AND "
        "version=? AND status IN ('pending','approved','rejected')",
        {clock_(), id, version});
    if (updated.numRowsAffected() != 1) {
        throw ServiceError("conflict", QStringLiteral("账目状态已变化，不能撤回。"));
    }
    query("INSERT INTO expense_withdrawals VALUES(?,?,?,?,?,?)",
          {identifier(), id, actor, reason, version, clock_()});
    appendLog(ledger_id, actor, "withdraw_expense", id, reason);
    notifyCreator(
        record, ledger_id, "withdrawn",
        QStringLiteral("账目“%1”已撤回，原因：%2")
            .arg(record.value("form").toObject().value("description").toString(), reason));
    transaction.commit();
    return detail(actor, parameters);
}

QJsonObject ExpenseService::history(const QString& actor, const QJsonObject& parameters) const {
    const auto record = detail(actor, parameters);
    const QString id = record.value("id").toString();
    QJsonArray reviews, revisions, withdrawals;
    auto rows = query(
        "SELECT u.username,r.decision,r.reason,r.reviewed_version,r.created_at FROM "
        "expense_reviews r JOIN users u ON u.id=r.reviewer_id WHERE r.expense_id=? ORDER BY "
        "r.created_at,r.rowid",
        {id});
    while (rows.next()) {
        reviews.append(QJsonObject{{"actor", rows.value(0).toString()},
                                   {"decision", rows.value(1).toString()},
                                   {"reason", rows.value(2).toString()},
                                   {"version", rows.value(3).toInt()},
                                   {"time", rows.value(4).toLongLong()}});
    }
    rows = query(
        "SELECT "
        "u.username,r.reason,r.previous_form,r.new_form,r.previous_status,r.new_status,r.previous_"
        "shares,r.new_shares,r.previous_version,r.new_version,r.created_at FROM expense_revisions "
        "r JOIN users u ON u.id=r.actor_id WHERE r.expense_id=? ORDER BY r.created_at,r.rowid",
        {id});
    while (rows.next()) {
        auto previous = QJsonDocument::fromJson(rows.value(2).toString().toUtf8()).object();
        previous.remove("expense_type");
        revisions.append(QJsonObject{
            {"actor", rows.value(0).toString()},
            {"reason", rows.value(1).toString()},
            {"before", previous},
            {"after", QJsonDocument::fromJson(rows.value(3).toString().toUtf8()).object()},
            {"before_status", rows.value(4).toString()},
            {"after_status", rows.value(5).toString()},
            {"before_shares", QJsonDocument::fromJson(rows.value(6).toString().toUtf8()).array()},
            {"after_shares", QJsonDocument::fromJson(rows.value(7).toString().toUtf8()).array()},
            {"before_version", rows.value(8).toInt()},
            {"after_version", rows.value(9).toInt()},
            {"time", rows.value(10).toLongLong()}});
    }
    rows = query(
        "SELECT u.username,w.reason,w.created_at FROM expense_withdrawals w JOIN users u ON "
        "u.id=w.actor_id WHERE w.expense_id=?",
        {id});
    while (rows.next()) {
        withdrawals.append(QJsonObject{{"actor", rows.value(0).toString()},
                                       {"reason", rows.value(1).toString()},
                                       {"time", rows.value(2).toLongLong()}});
    }
    QJsonObject names;
    rows = query(
        "SELECT u.id,u.username FROM memberships m JOIN users u ON u.id=m.user_id WHERE "
        "m.ledger_id=?",
        {text(parameters, "ledger_id", 128)});
    while (rows.next()) {
        names[rows.value(0).toString()] = rows.value(1).toString();
    }
    return {{"reviews", reviews},
            {"revisions", revisions},
            {"withdrawals", withdrawals},
            {"names", names}};
}

QJsonObject ExpenseService::notifications(const QString& actor, const QJsonObject& parameters,
                                          bool mark_read) {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    requireMember(actor, ledger_id, false);
    if (mark_read) {
        auto marked = query(
            "UPDATE expense_notices SET read_at=COALESCE(read_at,?) WHERE id=? AND user_id=? AND "
            "ledger_id=?",
            {clock_(), text(parameters, "notice_id", 128), actor, ledger_id});
        if (marked.numRowsAffected() != 1) {
            throw ServiceError("forbidden", QStringLiteral("无权限处理该通知。"));
        }
    }
    QJsonArray notices;
    auto rows = query(
        "SELECT id,expense_id,kind,message,created_at,read_at FROM expense_notices WHERE user_id=? "
        "AND ledger_id=? ORDER BY created_at DESC,rowid DESC LIMIT 100",
        {actor, ledger_id});
    while (rows.next()) {
        notices.append(QJsonObject{{"id", rows.value(0).toString()},
                                   {"expense_id", rows.value(1).toString()},
                                   {"kind", rows.value(2).toString()},
                                   {"message", rows.value(3).toString()},
                                   {"time", rows.value(4).toLongLong()},
                                   {"read", !rows.value(5).isNull()}});
    }
    return {{"notices", notices}};
}
}  // namespace ledger
