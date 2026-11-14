#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>
#include <utility>

#include "domain/service_error.h"
#include "identity/identity_service.h"

namespace ledger {
namespace {
QString text(const QJsonObject& value, const QString& key, int maximum = 128) {
    if (!value.value(key).isString() || value.value(key).toString().size() > maximum) {
        throw ServiceError("validation", QStringLiteral("请填写有效的字段内容。"), key);
    }
    return value.value(key).toString().trimmed();
}
class Transaction final {
public:
    explicit Transaction(QSqlDatabase database) : database_(std::move(database)) {
        if (!database_.transaction()) {
            throw std::runtime_error("Cannot begin ledger transaction.");
        }
    }
    ~Transaction() {
        if (!committed_) {
            database_.rollback();
        }
    }
    void commit() {
        if (!database_.commit()) {
            throw std::runtime_error("Cannot commit ledger transaction.");
        }
        committed_ = true;
    }

private:
    QSqlDatabase database_;
    bool committed_ = false;
};
}  // namespace

QJsonObject IdentityService::closePreview(const QString& actor,
                                          const QJsonObject& parameters) const {
    const QString id = text(parameters, "ledger_id");
    requireAdmin(id, actor);
    auto ledger = query("SELECT name FROM ledgers WHERE id=?", {id});
    ledger.next();
    const auto count = [&](const QString& sql) {
        auto row = query(sql, {id});
        row.next();
        return row.value(0).toLongLong();
    };
    const qint64 others =
        count("SELECT COUNT(*) FROM memberships WHERE ledger_id=? AND status='active'") - 1;
    auto locks = query(
        "SELECT COUNT(*) FROM expense_locks l JOIN expenses e ON e.id=l.expense_id WHERE "
        "e.ledger_id=? AND l.expires_at>?",
        {id, clock_()});
    locks.next();
    const qint64 active_locks = locks.value(0).toLongLong();
    auto last = query("SELECT COALESCE(MAX(rowid),0) FROM operation_logs WHERE ledger_id=?", {id});
    last.next();
    QJsonObject result{
        {"id", id},
        {"name", ledger.value(0).toString()},
        {"other_members", others},
        {"expense_count", count("SELECT COUNT(*) FROM expenses WHERE ledger_id=?")},
        {"settlement_count", count("SELECT COUNT(*) FROM settlements WHERE ledger_id=?")},
        {"repayment_count", count("SELECT COUNT(*) FROM repayments WHERE ledger_id=?")},
        {"finance_count", count("SELECT COUNT(*) FROM expense_finance_events WHERE ledger_id=?")},
        {"pending_invitations",
         count("SELECT COUNT(*) FROM invitations WHERE ledger_id=? AND status='pending'")},
        {"active_locks", active_locks},
        {"audit_revision", last.value(0).toLongLong()}};
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    result["preview_token"] =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    result["can_close"] = others == 0 && active_locks == 0;
    result["reason"] =
        others > 0
            ? QStringLiteral("账本还有其他当前成员，不能解散；需先处理成员退出或负责人交接。")
        : active_locks > 0 ? QStringLiteral("账目正在编辑，请先保存并退出编辑，再归档或删除。")
                           : QString();
    return result;
}

QJsonObject IdentityService::closedLedgers(const QString& actor) const {
    auto rows = query(
        "SELECT id,name,status,closed_at,close_token FROM ledgers WHERE closed_by=? AND status IN "
        "('archived','deleted') ORDER BY closed_at DESC,id",
        {actor});
    QJsonArray books;
    while (rows.next()) {
        books.append(QJsonObject{{"id", rows.value(0).toString()},
                                 {"name", rows.value(1).toString()},
                                 {"status", rows.value(2).toString()},
                                 {"closed_at", rows.value(3).toLongLong()},
                                 {"close_token", rows.value(4).toString()}});
    }
    return {{"ledgers", books}};
}

QJsonObject IdentityService::closeLedger(const QString& actor, const QJsonObject& parameters) {
    const QString id = text(parameters, "ledger_id"), mode = text(parameters, "mode");
    if (mode != "archive" && mode != "delete") {
        throw ServiceError("validation", QStringLiteral("请选择归档或删除业务数据。"), "mode");
    }
    const QString token = text(parameters, "preview_token");
    if (token.isEmpty() || !parameters.value("confirmed").toBool()) {
        throw ServiceError("confirmation_required",
                           QStringLiteral("请核对账本及影响范围，确认后再操作。"));
    }
    Transaction transaction(database_);
    auto ledger = query("SELECT name,status,closed_by,close_token FROM ledgers WHERE id=?", {id});
    if (!ledger.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限操作该账本。"));
    }
    if (ledger.value(1).toString() == "active") {
        requireAdmin(id, actor);
    } else if (ledger.value(2).toString() != actor) {
        throw ServiceError("forbidden", QStringLiteral("无权限操作该账本。"));
    }
    const QString name = ledger.value(0).toString();
    if (text(parameters, "confirm_name", 80) != name) {
        throw ServiceError("validation", QStringLiteral("输入的账本名称不一致，请重新核对。"),
                           "confirm_name");
    }
    const QString status = mode == "archive" ? "archived" : "deleted";
    if (ledger.value(1).toString() != "active") {
        if (ledger.value(1).toString() == status && ledger.value(2).toString() == actor &&
            ledger.value(3).toString() == token) {
            return {{"id", id}, {"status", status}, {"reused", true}};
        }
        throw ServiceError("forbidden", QStringLiteral("该账本已关闭，或当前账号无权限处理。"));
    }
    const auto preview = closePreview(actor, {{"ledger_id", id}});
    if (!preview.value("can_close").toBool()) {
        throw ServiceError("conflict", preview.value("reason").toString());
    }
    if (preview.value("preview_token").toString() != token) {
        throw ServiceError("conflict",
                           QStringLiteral("账本内容或成员已变化，请重新打开操作窗口核对。"));
    }
    query(
        "UPDATE ledgers SET status=?,closed_at=?,closed_by=?,close_token=? WHERE id=? AND "
        "status='active'",
        {status, clock_(), actor, token, id});
    if (mode == "archive") {
        query("UPDATE invitations SET status='revoked' WHERE ledger_id=? AND status='pending'",
              {id});
    } else {
        // Keep the ledger identity, authorization basis and every audit entry.
        query("DELETE FROM membership_history WHERE ledger_id=?", {id});
        query("DELETE FROM membership_events WHERE ledger_id=?", {id});
        query("DELETE FROM expense_finance_events WHERE ledger_id=?", {id});
        for (const QString& table :
             QStringList{"expense_locks", "expense_notices", "expense_reviews", "expense_revisions",
                         "expense_withdrawals"}) {
            query("DELETE FROM " + table +
                      " WHERE expense_id IN (SELECT id FROM expenses WHERE ledger_id=?)",
                  {id});
        }
        query(
            "DELETE FROM settlement_expenses WHERE settlement_id IN (SELECT id FROM settlements "
            "WHERE ledger_id=?)",
            {id});
        query("DELETE FROM settlements WHERE ledger_id=?", {id});
        query("DELETE FROM repayments WHERE ledger_id=?", {id});
        query(
            "DELETE FROM expense_shares WHERE expense_id IN (SELECT id FROM expenses WHERE "
            "ledger_id=?)",
            {id});
        query("DELETE FROM expense_requests WHERE ledger_id=?", {id});
        query("DELETE FROM expenses WHERE ledger_id=?", {id});
        query("DELETE FROM invitations WHERE ledger_id=?", {id});
    }
    log(id, actor, mode == "archive" ? "archive_ledger" : "delete_ledger_data", id,
        QStringLiteral("%1账本“%2”：%3笔账目、%4次结算、%5条还款、%6份待接受邀请；操作日志保留。")
            .arg(mode == "archive" ? QStringLiteral("归档") : QStringLiteral("删除业务数据："),
                 name)
            .arg(preview.value("expense_count").toInteger())
            .arg(preview.value("settlement_count").toInteger())
            .arg(preview.value("repayment_count").toInteger())
            .arg(preview.value("pending_invitations").toInteger()));
    transaction.commit();
    return {{"id", id}, {"status", status}, {"reused", false}};
}

QJsonObject IdentityService::restoreLedger(const QString& actor, const QJsonObject& parameters) {
    const QString id = text(parameters, "ledger_id"), token = text(parameters, "close_token");
    if (token.isEmpty() || !parameters.value("confirmed").toBool()) {
        throw ServiceError("confirmation_required", QStringLiteral("请确认恢复该归档账本。"));
    }
    Transaction transaction(database_);
    auto book = query(
        "SELECT l.name,l.status FROM ledgers l JOIN memberships m ON m.ledger_id=l.id "
        "WHERE l.id=? AND l.closed_by=? AND l.close_token=? AND m.user_id=? AND m.role='admin' AND "
        "m.status='active'",
        {id, actor, token, actor});
    if (!book.next() || book.value(1).toString() == "deleted") {
        throw ServiceError("forbidden", QStringLiteral("无权限恢复，或账本业务数据已删除。"));
    }
    if (book.value(1).toString() == "active") {
        return {{"id", id}, {"reused", true}};
    }
    query("UPDATE ledgers SET status='active' WHERE id=? AND status='archived'", {id});
    log(id, actor, "restore_ledger", id,
        QStringLiteral("恢复归档账本“%1”；已取消的邀请不会自动恢复。")
            .arg(book.value(0).toString()));
    transaction.commit();
    return {{"id", id}, {"reused", false}};
}

QJsonObject IdentityService::closedLedgerLogs(const QString& actor,
                                              const QJsonObject& parameters) const {
    const QString id = text(parameters, "ledger_id");
    auto book = query(
        "SELECT name,status FROM ledgers WHERE id=? AND closed_by=? AND status IN "
        "('archived','deleted')",
        {id, actor});
    if (!book.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看此已关闭账本的日志。"));
    }
    QJsonArray logs;
    auto rows = query(
        "SELECT o.id,o.created_at,COALESCE(u.username,o.actor_id),o.action,o.target_id,o.detail "
        "FROM operation_logs o "
        "LEFT JOIN users u ON u.id=o.actor_id WHERE o.ledger_id=? AND "
        "(o.action<>'save_expense_draft' OR o.actor_id=?) ORDER BY o.created_at "
        "DESC,o.rowid DESC LIMIT 1000",
        {id, actor});
    while (rows.next()) {
        logs.append(QJsonObject{{"id", rows.value(0).toString()},
                                {"created_at", rows.value(1).toLongLong()},
                                {"actor", rows.value(2).toString()},
                                {"action", rows.value(3).toString()},
                                {"target", rows.value(4).toString()},
                                {"detail", rows.value(5).toString()}});
    }
    return {{"ledger_id", id},
            {"name", book.value(0).toString()},
            {"status", book.value(1).toString()},
            {"logs", logs}};
}
}  // namespace ledger
