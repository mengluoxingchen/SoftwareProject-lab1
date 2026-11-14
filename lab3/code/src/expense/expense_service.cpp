#include "expense/expense_service.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QUuid>

#include "domain/service_error.h"
#include "domain/split.h"
#include "domain/write_request.h"
#include "expense/expense_support.h"
#include "finance/finance_projection.h"
#include "infrastructure/database.h"

namespace ledger {
using namespace expense_detail;

ExpenseService::ExpenseService(Database& database, Clock clock)
    : database_(database.connection()), clock_(std::move(clock)), finances_(database, clock_) {
    if (!clock_) {
        clock_ = [] { return QDateTime::currentSecsSinceEpoch(); };
    }
}

QSqlQuery ExpenseService::query(const QString& sql, const QVariantList& values) const {
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

QString ExpenseService::requireMember(const QString& actor, const QString& ledger_id,
                                      bool write) const {
    auto member = query(
        QStringLiteral(
            "SELECT m.role FROM memberships m JOIN ledgers l ON l.id=m.ledger_id "
            "WHERE m.user_id=? AND m.ledger_id=? AND m.status='active' AND l.status='active'"),
        {actor, ledger_id});
    if (!member.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限访问该账本。"));
    }
    const QString role = member.value(0).toString();
    if (write && role == "read_only") {
        throw ServiceError("forbidden", QStringLiteral("只读用户不能新增、修改或提交账目。"));
    }
    return role;
}

QJsonObject ExpenseService::dispatch(const QString& actor, const QString& method,
                                     const QJsonObject& parameters, const QString& session) {
    const QString session_key = session.isEmpty() ? actor : session;
    if (method == "expense_write_status") {
        const QString ledger = text(parameters, "ledger_id", 128);
        requireMember(actor, ledger, false);
        auto receipt = query(
            "SELECT method,fingerprint,expense_id,version,status FROM "
            "expense_requests WHERE ledger_id=? AND user_id=? AND request_key=?",
            {ledger, actor, text(parameters, "request_key", 128)});
        if (!receipt.next()) return {{"found", false}};
        return {{"found", true},
                {"method", receipt.value(0).toString()},
                {"fingerprint", receipt.value(1).toString()},
                {"expense_id", receipt.value(2).toString()},
                {"version", receipt.value(3).toInt()},
                {"status", receipt.value(4).toString()}};
    }
    if (method == "expense_list") {
        return list(actor, parameters);
    }
    if (method == "expense_detail") {
        return detail(actor, parameters);
    }
    if (method == "expense_save_draft") {
        return save(actor, parameters, false, session_key);
    }
    if (method == "expense_submit") {
        return save(actor, parameters, true, session_key);
    }
    if (method.startsWith("expense_lock_")) {
        return lock(actor, session_key, method, parameters);
    }
    if (method == "expense_review_queue") {
        if (requireMember(actor, text(parameters, "ledger_id", 128), false) != "admin") {
            throw ServiceError("forbidden", QStringLiteral("只有管理员可以审核账目。"));
        }
        auto filter = parameters;
        filter["status"] = "pending";
        return list(actor, filter);
    }
    if (method == "expense_review") {
        return review(actor, parameters);
    }
    if (method == "expense_withdraw") {
        return withdraw(actor, parameters);
    }
    if (method == "expense_history") {
        return history(actor, parameters);
    }
    if (method == "expense_notifications") {
        return notifications(actor, parameters, false);
    }
    if (method == "expense_notice_read") {
        return notifications(actor, parameters, true);
    }
    throw ServiceError("unknown_method", QStringLiteral("不支持的账目操作。"));
}

QJsonObject ExpenseService::list(const QString& actor, const QJsonObject& parameters) const {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    const QString role = requireMember(actor, ledger_id, false);
    const QString status = text(parameters, "status", 32);
    const QStringList states{"draft", "pending", "approved", "rejected", "withdrawn", "settled"};
    if (!status.isEmpty() && !states.contains(status)) {
        invalid(QStringLiteral("账目状态不正确。"), "status");
    }
    auto rows =
        query(QStringLiteral(
                  "SELECT "
                  "e.id,e.expense_date,e.description,e.amount_cents,"
                  "e.status,e.creator_id,c.username,"
                  "COALESCE(p.username,''),e.version,e.payload,EXISTS(SELECT 1 FROM "
                  "expense_revisions r WHERE r.expense_id=e.id),e.superseded,e.is_adjustment,"
                  "EXISTS(SELECT 1 FROM expense_finance_events f WHERE f.root_id=CASE WHEN "
                  "e.financial_root_id='' THEN e.id ELSE e.financial_root_id END) FROM expenses e "
                  "JOIN users c ON c.id=e.creator_id "
                  "LEFT JOIN users p ON p.id=e.payer_id WHERE e.ledger_id=? AND "
                  "(e.status<>'draft' OR e.creator_id=?) "
                  "AND ((?='' AND e.status<>'withdrawn') OR e.status=?) ORDER "
                  "BY e.created_at DESC,e.id LIMIT 1000"),
              {ledger_id, actor, status, status});
    QHash<QString, QString> names;
    auto people = query(QStringLiteral("SELECT u.id,u.username FROM memberships m JOIN users u ON "
                                       "u.id=m.user_id WHERE m.ledger_id=?"),
                        {ledger_id});
    while (people.next()) {
        names.insert(people.value(0).toString(), people.value(1).toString());
    }
    QJsonArray records;
    while (rows.next()) {
        const auto form = QJsonDocument::fromJson(rows.value(9).toString().toUtf8()).object();
        QStringList participants;
        for (const auto& value : form.value("splits").toArray()) {
            const QString id = value.toObject().value("user_id").toString();
            participants.append(names.value(id, id));
        }
        records.append(QJsonObject{
            {"id", rows.value(0).toString()},
            {"date", rows.value(1).toString()},
            {"description", rows.value(2).toString()},
            {"amount",
             rows.value(3).isNull() ? QString() : formatMoney(rows.value(3).toLongLong())},
            {"status", rows.value(4).toString()},
            {"creator_id", rows.value(5).toString()},
            {"creator", rows.value(6).toString()},
            {"payer", rows.value(7).toString()},
            {"version", rows.value(8).toInt()},
            {"participants", participants.join(QStringLiteral("、"))},
            {"corrected", rows.value(10).toBool()},
            {"superseded", rows.value(11).toBool()},
            {"is_adjustment", rows.value(12).toBool()},
            {"has_finance", rows.value(13).toBool()},
            {"can_edit",
             !rows.value(13).toBool() && !rows.value(11).toBool() &&
                 editable(role, rows.value(5).toString(), actor, rows.value(4).toString())},
            {"can_withdraw",
             !rows.value(13).toBool() && !rows.value(11).toBool() &&
                 editable(role, rows.value(5).toString(), actor, rows.value(4).toString()) &&
                 rows.value(4).toString() != "draft"}});
    }
    auto history = query("SELECT EXISTS(SELECT 1 FROM settlements WHERE ledger_id=?)", {ledger_id});
    history.next();
    return {{"expenses", records},
            {"can_adjust", role == "admin" && history.value(0).toBool()},
            {"can_supplement", role != "read_only" && history.value(0).toBool()}};
}

QJsonObject ExpenseService::detail(const QString& actor, const QJsonObject& parameters) const {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    const QString role = requireMember(actor, ledger_id, false);
    auto row = query(QStringLiteral("SELECT "
                                    "e.id,e.creator_id,e.status,e.version,e.payload,e.created_at,e."
                                    "submitted_at,c.username,COALESCE(p.username,'') "
                                    "FROM expenses e JOIN users c ON c.id=e.creator_id LEFT JOIN "
                                    "users p ON p.id=e.payer_id WHERE e.id=? AND e.ledger_id=?"),
                     {text(parameters, "expense_id", 128), ledger_id});
    if (!row.next() || (row.value(2).toString() == "draft" && row.value(1).toString() != actor)) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看该账目。"));
    }
    QJsonArray shares;
    auto allocations = query(
        QStringLiteral("SELECT s.user_id,u.username,s.amount_cents FROM expense_shares s JOIN "
                       "users u ON u.id=s.user_id WHERE s.expense_id=? ORDER BY s.user_id"),
        {row.value(0)});
    while (allocations.next()) {
        shares.append(QJsonObject{{"user_id", allocations.value(0).toString()},
                                  {"username", allocations.value(1).toString()},
                                  {"amount", formatMoney(allocations.value(2).toLongLong())}});
    }
    auto form = QJsonDocument::fromJson(row.value(4).toString().toUtf8()).object();
    form.remove("expense_type");
    auto held = query(
        "SELECT u.username,l.expires_at FROM expense_locks l JOIN users u ON u.id=l.holder_id "
        "WHERE l.expense_id=? AND l.expires_at>?",
        {row.value(0), clock_()});
    const bool locked = held.next();
    const auto financial =
        finance_detail::projection(database_, ledger_id, row.value(0).toString());
    const bool has_finance = financial.value("root_revision").toInteger() > 0;
    auto changed =
        query("SELECT 1 FROM expense_revisions WHERE expense_id=? LIMIT 1", {row.value(0)});
    auto key = query("SELECT similar_key FROM expenses WHERE id=?", {row.value(0)});
    key.next();
    return {
        {"id", row.value(0).toString()},
        {"creator_id", row.value(1).toString()},
        {"creator", row.value(7).toString()},
        {"payer", row.value(8).toString()},
        {"status", row.value(2).toString()},
        {"version", row.value(3).toInt()},
        {"form", form},
        {"shares", shares},
        {"financial", financial},
        {"has_finance", has_finance},
        {"is_adjustment", financial.value("is_adjustment")},
        {"superseded", financial.value("superseded")},
        {"can_refund",
         role == "admin" && !financial.value("superseded").toBool() &&
             (row.value(2).toString() == "approved" || row.value(2).toString() == "settled") &&
             finance_detail::cents(financial.value("net_amount").toString()) > 0},
        {"corrected", changed.next()},
        {"locked", locked},
        {"lock_holder", locked ? held.value(0).toString() : QString()},
        {"lock_expires_at", locked ? held.value(1).toLongLong() : 0},
        {"similar_records",
         row.value(2).toString() == "draft"
             ? QJsonArray{}
             : similarRecords(row.value(0).toString(), ledger_id, key.value(0).toString())},
        {"can_review", role == "admin" && row.value(2).toString() == "pending"},
        {"can_edit", !has_finance && !financial.value("superseded").toBool() &&
                         editable(role, row.value(1).toString(), actor, row.value(2).toString())}};
}

QJsonObject ExpenseService::save(const QString& actor, const QJsonObject& parameters, bool submit,
                                 const QString& session) {
    const QString ledger_id = text(parameters, "ledger_id", 128);
    requireMember(actor, ledger_id, true);
    const QString request_key = text(parameters, "request_key", 128);
    const QString request_method =
        submit ? QStringLiteral("expense_submit") : QStringLiteral("expense_save_draft");
    const QString fingerprint = expenseRequestFingerprint(request_method, parameters);
    if (!request_key.isEmpty()) {
        auto receipt = dispatch(actor, "expense_write_status",
                                {{"ledger_id", ledger_id}, {"request_key", request_key}}, session);
        if (receipt.value("found").toBool()) {
            if (receipt.value("method") != request_method ||
                receipt.value("fingerprint") != fingerprint)
                throw ServiceError(
                    "request_conflict",
                    QStringLiteral("此操作已保存，输入与原操作不同，请先核对恢复副本。"));
            receipt["reused"] = true;
            return receipt;
        }
    }
    const QString existing_id = text(parameters, "expense_id", 128);
    const int version = parameters.value("version").toInt();
    QString creator = actor;
    QJsonObject before;
    QString previous_status = "draft";
    QString change_reason = QStringLiteral("");
    if (!existing_id.isEmpty()) {
        before = detail(actor, parameters);
        creator = before.value("creator_id").toString();
        previous_status = before.value("status").toString();
        if (!before.value("can_edit").toBool()) {
            throw ServiceError("forbidden",
                               QStringLiteral("无权限修改该账目，或账目已撤回/结算。"));
        }
        if (!submit && previous_status != "draft") {
            throw ServiceError(
                "conflict", QStringLiteral("已提交账目不能退回草稿，请保存修改并重新提交审核。"));
        }
        if (before.value("version").toInt() != version) {
            throw ServiceError("conflict", QStringLiteral("账目已被更新，请重新打开后再修改。"));
        }
        requireEditLock(actor, session, parameters);
        if (previous_status != "draft") {
            change_reason = text(parameters, "change_reason");
            if (change_reason.isEmpty()) {
                invalid(QStringLiteral("请填写修改原因。"), "change_reason");
            }
        }
    }
    const auto old_form = before.value("form").toObject();
    const bool post_settlement = parameters.value("post_settlement_adjustment").toBool() ||
                                 old_form.value("post_settlement_adjustment").toBool();
    const QString adjustment_reason = parameters.contains("adjustment_reason")
                                          ? text(parameters, "adjustment_reason")
                                          : old_form.value("adjustment_reason").toString();
    if (post_settlement) {
        if (!submit) {
            invalid(QStringLiteral("结算后补记请提交审核，不保存为普通草稿。"),
                    "adjustment_reason");
        }
        if (adjustment_reason.trimmed().isEmpty()) {
            invalid(QStringLiteral("请填写结算后补记原因。"), "adjustment_reason");
        }
        auto history = query("SELECT 1 FROM settlements WHERE ledger_id=? LIMIT 1", {ledger_id});
        if (!history.next()) {
            throw ServiceError("conflict",
                               QStringLiteral("账本尚未生成结算，请先使用普通新增账目。"));
        }
    }
    QSet<QString> members;
    auto people = query(
        QStringLiteral("SELECT user_id FROM memberships WHERE ledger_id=? AND status='active'"),
        {ledger_id});
    while (people.next()) {
        members.insert(people.value(0).toString());
    }
    const QString amount_text = text(parameters, "amount", 32);
    const QString date = text(parameters, "date", 16);
    const QString payer = text(parameters, "payer_id", 128);
    const QString description = text(parameters, "description");
    const QString category = text(parameters, "category", 80);
    const QString method = text(parameters, "split_method", 32);
    QVariant amount;
    if (!amount_text.isEmpty()) {
        amount = parseMoney(amount_text);
        if (amount.toLongLong() <= 0) {
            invalid(QStringLiteral("消费金额必须大于0。"), "amount");
        }
    } else if (submit) {
        invalid(QStringLiteral("请填写消费金额。"), "amount");
    }
    if ((!date.isEmpty() && (!QDate::fromString(date, Qt::ISODate).isValid() ||
                             QDate::fromString(date, Qt::ISODate).toString(Qt::ISODate) != date)) ||
        (submit && date.isEmpty())) {
        invalid(QStringLiteral("请选择有效的消费日期。"), "date");
    }
    if ((!payer.isEmpty() && !members.contains(payer)) || (submit && payer.isEmpty())) {
        invalid(QStringLiteral("请选择当前账本的实际付款人。"), "payer_id");
    }
    if (submit && description.isEmpty()) {
        invalid(QStringLiteral("请填写消费说明。"), "description");
    }
    if (method != "equal" && method != "ratio" && method != "fixed") {
        invalid(QStringLiteral("请选择分摊方式。"), "split_method");
    }
    if (!parameters.value("splits").isArray() ||
        parameters.value("splits").toArray().size() > 1000) {
        invalid(QStringLiteral("分摊成员格式不正确。"), "splits");
    }
    QVector<SplitPart> parts;
    QJsonArray split_payload;
    QSet<QString> selected;
    for (const auto& value : parameters.value("splits").toArray()) {
        if (!value.isObject()) {
            invalid(QStringLiteral("分摊明细格式不正确。"), "splits");
        }
        const auto part = value.toObject();
        const QString id = text(part, "user_id", 128);
        const QString split_value = text(part, "value", 32);
        if (!members.contains(id) || selected.contains(id)) {
            invalid(QStringLiteral("分摊成员必须属于当前账本，且不能重复。"), "splits");
        }
        selected.insert(id);
        if (!split_value.isEmpty() && method == "fixed") {
            static_cast<void>(parseMoney(split_value, "splits"));
        }
        if (!split_value.isEmpty() && method == "ratio") {
            static_cast<void>(parseRatio(split_value));
        }
        parts.append({id, split_value});
        split_payload.append(QJsonObject{{"user_id", id}, {"value", split_value}});
    }
    const auto allocations =
        submit ? calculateSplit(amount.toLongLong(), method, parts) : QVector<ShareAllocation>{};
    QJsonObject form{{"amount", amount_text},      {"date", date},         {"payer_id", payer},
                     {"description", description}, {"category", category}, {"split_method", method},
                     {"splits", split_payload}};
    if (post_settlement) {
        form["post_settlement_adjustment"] = true;
        form["adjustment_reason"] = adjustment_reason;
    }
    const QString payload = QString::fromUtf8(QJsonDocument(form).toJson(QJsonDocument::Compact));
    QString similar_key = QStringLiteral("");
    if (submit) {
        const QByteArray key =
            QJsonDocument(QJsonArray{ledger_id, date, QString::number(amount.toLongLong()), payer,
                                     description})
                .toJson(QJsonDocument::Compact);
        similar_key =
            QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
        auto own = query(
            QStringLiteral("SELECT 1 FROM expenses WHERE ledger_id=? AND creator_id=? "
                           "AND similar_key=? AND status<>'draft' AND superseded=0 AND id<>?"),
            {ledger_id, creator, similar_key, existing_id});
        if (own.next()) {
            throw ServiceError(
                "duplicate",
                QStringLiteral("你已提交过日期、金额、付款人和说明完全相同的账目，请勿重复提交。"));
        }
        auto similar = query(
            QStringLiteral(
                "SELECT e.id,u.username FROM expenses e JOIN users u ON "
                "u.id=e.creator_id WHERE e.ledger_id=? AND e.creator_id<>? AND "
                "e.similar_key=? AND e.status<>'draft' AND e.superseded=0 AND e.id<>? LIMIT 20"),
            {ledger_id, creator, similar_key, existing_id});
        QJsonArray records;
        while (similar.next()) {
            records.append(QJsonObject{{"id", similar.value(0).toString()},
                                       {"creator", similar.value(1).toString()}});
        }
        if (!records.isEmpty() && !parameters.value("confirm_similar").toBool()) {
            return {{"confirmation_required", true}, {"similar_records", records}};
        }
    }
    const QString id = existing_id.isEmpty() ? identifier() : existing_id;
    const QString status = submit ? QStringLiteral("pending") : QStringLiteral("draft");
    const QVariant payer_value = payer.isEmpty() ? QVariant() : QVariant(payer);
    const QVariant submitted_at = submit ? QVariant(clock_()) : QVariant();
    Transaction transaction(database_);
    if (existing_id.isEmpty()) {
        query(QStringLiteral(
                  "INSERT INTO "
                  "expenses(id,ledger_id,creator_id,payer_id,amount_cents,expense_date,description,"
                  "category,legacy_expense_type,split_method,status,payload,similar_key,version,"
                  "created_at,updated_at,submitted_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"),
              {id, ledger_id, actor, payer_value, amount, date, description, category,
               QStringLiteral(""), method, status, payload, similar_key, 1, clock_(), clock_(),
               submitted_at});
    } else {
        auto updated = query(
            QStringLiteral("UPDATE expenses SET "
                           "payer_id=?,amount_cents=?,expense_date=?,description=?,category=?,"
                           "split_method=?,status=?,payload=?,similar_key=?,version=version+1,"
                           "updated_at=?,submitted_at=? WHERE id=? AND version=? AND status=?"),
            {payer_value, amount, date, description, category, method, status, payload, similar_key,
             clock_(), submitted_at, id, version, previous_status});
        if (updated.numRowsAffected() != 1) {
            throw ServiceError("conflict", QStringLiteral("草稿发生冲突，请重新打开后再试。"));
        }
    }
    if (post_settlement) {
        query(
            "UPDATE expenses SET is_adjustment=1,financial_root_id=?,adjustment_reason=? WHERE "
            "id=?",
            {id, adjustment_reason, id});
    }
    query(QStringLiteral("DELETE FROM expense_shares WHERE expense_id=?"), {id});
    for (const auto& share : allocations) {
        query(QStringLiteral("INSERT INTO expense_shares VALUES(?,?,?)"),
              {id, share.user_id, share.amount});
    }
    if (!existing_id.isEmpty() && previous_status != "draft") {
        QJsonArray after_shares;
        for (const auto& share : allocations) {
            after_shares.append(
                QJsonObject{{"user_id", share.user_id}, {"amount", formatMoney(share.amount)}});
        }
        query("INSERT INTO expense_revisions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
              {identifier(), id, actor, change_reason, json(before.value("form").toObject()),
               payload, previous_status, status,
               QString::fromUtf8(
                   QJsonDocument(before.value("shares").toArray()).toJson(QJsonDocument::Compact)),
               QString::fromUtf8(QJsonDocument(after_shares).toJson(QJsonDocument::Compact)),
               version, version + 1, clock_()});
    }
    QString edit_token = QStringLiteral("");
    if (submit) {
        query("DELETE FROM expense_locks WHERE expense_id=?", {id});
    } else {
        edit_token = existing_id.isEmpty() ? identifier() : text(parameters, "lock_token", 128);
        if (existing_id.isEmpty()) {
            query("INSERT INTO expense_locks VALUES(?,?,?,?,?)",
                  {id, actor, session, edit_token, clock_() + 120});
        } else {
            query("UPDATE expense_locks SET expires_at=? WHERE expense_id=?", {clock_() + 120, id});
        }
    }
    appendLog(ledger_id, actor,
              previous_status != "draft" ? "revise_expense"
              : submit                   ? "submit_expense"
                                         : "save_expense_draft",
              id, previous_status != "draft" ? change_reason : description,
              {{"before_amount", before.value("form").toObject().value("amount").toString()},
               {"after_amount", form.value("amount").toString()}});
    if (!request_key.isEmpty()) {
        query("INSERT INTO expense_requests VALUES(?,?,?,?,?,?,?,?,?)",
              {ledger_id, actor, request_key, request_method, fingerprint, id,
               existing_id.isEmpty() ? 1 : version + 1, status, clock_()});
    }
    transaction.commit();
    const auto result = detail(actor, {{"ledger_id", ledger_id}, {"expense_id", id}});
    if (submit) {
        return {{"confirmation_required", false}, {"expense", result}};
    }
    auto saved = result;
    saved["lock_token"] = edit_token;
    saved["lock_expires_at"] = clock_() + 120;
    return saved;
}

}  // namespace ledger
