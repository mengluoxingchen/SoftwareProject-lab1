#include "query/query_service.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>

#include "domain/service_error.h"
#include "domain/split.h"
#include "finance/finance_projection.h"
#include "infrastructure/database.h"

namespace ledger {
namespace {
using finance_detail::query;
QString text(const QJsonObject& object, const QString& key, int maximum = 128) {
    const auto value = object.value(key);
    if ((!value.isUndefined() && !value.isString()) || value.toString().size() > maximum) {
        throw ServiceError("validation", QStringLiteral("筛选条件格式或长度不正确。"), key);
    }
    const auto result = value.toString().trimmed();
    return result.isNull() ? QStringLiteral("") : result;
}
QString date(const QJsonObject& object, const QString& key) {
    const auto value = text(object, key, 10);
    if (!value.isEmpty() &&
        (!QDate::fromString(value, Qt::ISODate).isValid() ||
         QDate::fromString(value, Qt::ISODate).toString(Qt::ISODate) != value)) {
        throw ServiceError("validation", QStringLiteral("日期请填写为YYYY-MM-DD。"), key);
    }
    return value;
}
int integer(const QJsonObject& object, const QString& key, int fallback, int maximum) {
    const auto value = object.value(key);
    if (value.isUndefined()) {
        return fallback;
    }
    if (!value.isDouble() || value.toDouble() != value.toInt() || value.toInt() < 1 ||
        value.toInt() > maximum) {
        throw ServiceError("validation", QStringLiteral("分页参数不正确。"), key);
    }
    return value.toInt();
}
QJsonObject object(const QVariant& value) {
    return QJsonDocument::fromJson(value.toString().toUtf8()).object();
}
const QStringList states{"draft", "pending", "approved", "rejected", "withdrawn", "settled"};
struct Filter {
    QString from, to, category, payer, description, status;
    bool uncategorized = false;
    qint64 minimum = -1, maximum = -1;
    int page = 1, size = 50;
    explicit Filter(const QJsonObject& parameters) {
        from = date(parameters, "date_from");
        to = date(parameters, "date_to");
        category = text(parameters, "category", 80);
        payer = text(parameters, "payer_id");
        description = text(parameters, "description", 500);
        status = text(parameters, "status", 32);
        uncategorized = parameters.value("uncategorized").toBool();
        if (!from.isEmpty() && !to.isEmpty() && from > to) {
            throw ServiceError("validation", QStringLiteral("开始日期不能晚于结束日期。"),
                               "date_from");
        }
        if (!status.isEmpty() && !states.contains(status)) {
            throw ServiceError("validation", QStringLiteral("账目状态不正确。"), "status");
        }
        const auto low = text(parameters, "amount_min", 32),
                   high = text(parameters, "amount_max", 32);
        if (!low.isEmpty()) {
            minimum = parseMoney(low);
        }
        if (!high.isEmpty()) {
            maximum = parseMoney(high);
        }
        if (minimum >= 0 && maximum >= 0 && minimum > maximum) {
            throw ServiceError("validation", QStringLiteral("最小金额不能大于最大金额。"),
                               "amount_min");
        }
        page = integer(parameters, "page", 1, 1000000);
        size = integer(parameters, "page_size", 50, 200);
    }
    bool matches(const QJsonObject& row) const {
        const QString value = row.value("date").toString(), state = row.value("status").toString();
        if ((!from.isEmpty() && value < from) || (!to.isEmpty() && value > to) ||
            (status.isEmpty() ? state == "withdrawn" : state != status)) {
            return false;
        }
        if ((!category.isEmpty() && row.value("category").toString() != category) ||
            (uncategorized && !row.value("category").toString().isEmpty()) ||
            (!payer.isEmpty() && row.value("payer_id").toString() != payer) ||
            (!description.isEmpty() &&
             !row.value("description").toString().contains(description, Qt::CaseInsensitive))) {
            return false;
        }
        const QString amount = row.value("amount").toString();
        if ((minimum >= 0 || maximum >= 0) && amount.isEmpty()) {
            return false;
        }
        const qint64 cents = amount.isEmpty() ? 0 : parseMoney(amount);
        return (minimum < 0 || cents >= minimum) && (maximum < 0 || cents <= maximum);
    }
};
struct Summary {
    int count = 0;
    qint64 gross = 0, refunds = 0, net = 0;
    void add(qint64 original, qint64 refunded, qint64 effective) {
        constexpr qint64 limit = 9000000000000000LL;
        if (original < 0 || refunded < 0 || effective < 0 || original - refunded != effective ||
            gross > limit - original || refunds > limit - refunded || net > limit - effective) {
            throw ServiceError("amount_limit", QStringLiteral("统计金额超出范围或明细不一致。"));
        }
        ++count;
        gross += original;
        refunds += refunded;
        net += effective;
    }
    QJsonObject json() const {
        return {{"count", count},
                {"gross_amount", formatMoney(gross)},
                {"refund_amount", formatMoney(refunds)},
                {"net_amount", formatMoney(net)}};
    }
};
QJsonArray groups(const QMap<QString, Summary>& values) {
    QJsonArray result;
    for (auto it = values.begin(); it != values.end(); ++it) {
        auto item = it.value().json();
        item["name"] = it.key();
        result.append(item);
    }
    return result;
}
}  // namespace

QueryService::QueryService(Database& database) : database_(database.connection()) {}
QJsonObject QueryService::dispatch(const QString& actor, const QString& method,
                                   const QJsonObject& parameters) const {
    if (method == "query_expenses") {
        return expenses(actor, parameters);
    }
    if (method == "query_options") {
        return options(actor, parameters);
    }
    if (method == "audit_list" || method == "audit_detail" || method == "audit_ledgers") {
        return audit(actor, method, parameters);
    }
    throw ServiceError("unknown_method", QStringLiteral("不支持的查询操作。"));
}
QVector<QJsonObject> QueryService::records(const QString& actor, const QString& ledger_id) const {
    auto membership = query(database_,
                            "SELECT m.status,m.keep_history,l.status FROM memberships m JOIN "
                            "ledgers l ON l.id=m.ledger_id WHERE m.ledger_id=? AND m.user_id=?",
                            {ledger_id, actor});
    if (!membership.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限查询该账本。"));
    }
    const bool historical = membership.value(0).toString() == "exited";
    if (historical ? (!membership.value(1).toBool() || membership.value(2).toString() == "deleted")
                   : membership.value(2).toString() != "active") {
        throw ServiceError("forbidden", QStringLiteral("无权限查询该账本。"));
    }
    QVector<QJsonObject> result;
    if (historical) {
        auto saved = query(
            database_,
            "SELECT snapshot FROM membership_history WHERE ledger_id=? AND user_id=? AND "
            "kind='expense' ORDER BY json_extract(snapshot,'$.detail.created_at') DESC,object_id",
            {ledger_id, actor});
        while (saved.next()) {
            const auto snapshot = object(saved.value(0)),
                       detail = snapshot.value("detail").toObject(),
                       form = detail.value("form").toObject(),
                       finance = snapshot.value("finance").toObject();
            QStringList bearers;
            for (const auto& value : detail.value("shares").toArray()) {
                bearers.append(value.toObject().value("username").toString());
            }
            result.append({{"id", detail.value("id")},
                           {"date", form.value("date")},
                           {"description", form.value("description")},
                           {"amount", form.value("amount")},
                           {"category", form.value("category")},
                           {"payer_id", form.value("payer_id")},
                           {"payer", detail.value("payer")},
                           {"creator", detail.value("creator")},
                           {"participants", bearers.join("、")},
                           {"status", detail.value("status")},
                           {"corrected", detail.value("corrected")},
                           {"superseded", detail.value("superseded")},
                           {"is_adjustment", detail.value("is_adjustment")},
                           {"net_amount", finance.value("net_amount")},
                           {"refunded_amount", finance.value("refunded_amount")}});
        }
        return result;
    }
    QMap<QString, QString> names;
    auto people = query(database_,
                        "SELECT u.id,u.username FROM memberships m JOIN users u ON u.id=m.user_id "
                        "WHERE m.ledger_id=?",
                        {ledger_id});
    while (people.next()) {
        names[people.value(0).toString()] = people.value(1).toString();
    }
    auto rows = query(
        database_,
        "SELECT "
        "e.id,e.expense_date,e.description,e.amount_cents,e.category,e.payer_id,COALESCE(p."
        "username,''),c.username,e.status,e.superseded,e.is_adjustment,e.payload,EXISTS(SELECT 1 "
        "FROM expense_revisions r WHERE r.expense_id=e.id),CASE WHEN e.superseded=1 THEN "
        "(SELECT before_state FROM expense_finance_events WHERE source_id=e.id AND "
        "kind='correction' LIMIT 1) ELSE (SELECT f.after_state FROM "
        "expense_finance_events f WHERE f.root_id=CASE WHEN e.financial_root_id='' THEN e.id ELSE "
        "e.financial_root_id END ORDER BY f.revision DESC LIMIT 1) END FROM expenses e JOIN users "
        "c ON "
        "c.id=e.creator_id LEFT JOIN users p ON p.id=e.payer_id WHERE e.ledger_id=? AND "
        "(e.status<>'draft' OR e.creator_id=?) ORDER BY e.created_at DESC,e.id",
        {ledger_id, actor});
    while (rows.next()) {
        const auto form = object(rows.value(11)), finance = object(rows.value(13));
        QStringList bearers;
        for (const auto& value : form.value("splits").toArray()) {
            const auto id = value.toObject().value("user_id").toString();
            bearers.append(names.value(id, id));
        }
        const QString amount =
            rows.value(3).isNull() ? QString() : formatMoney(rows.value(3).toLongLong());
        result.append(
            {{"id", rows.value(0).toString()},
             {"date", rows.value(1).toString()},
             {"description", rows.value(2).toString()},
             {"amount", amount},
             {"category", rows.value(4).toString()},
             {"payer_id", rows.value(5).toString()},
             {"payer", rows.value(6).toString()},
             {"creator", rows.value(7).toString()},
             {"status", rows.value(8).toString()},
             {"superseded", rows.value(9).toBool()},
             {"is_adjustment", rows.value(10).toBool()},
             {"participants", bearers.join("、")},
             {"corrected", rows.value(12).toBool()},
             {"net_amount", finance.isEmpty() ? amount : finance.value("net_amount").toString()},
             {"refunded_amount", finance.value("refunded_amount").toString("0.00")}});
    }
    return result;
}
QJsonObject QueryService::options(const QString& actor, const QJsonObject& parameters) const {
    QSet<QString> categories;
    QMap<QString, QString> payers;
    for (const auto& row : records(actor, text(parameters, "ledger_id"))) {
        if (!row.value("category").toString().isEmpty()) {
            categories.insert(row.value("category").toString());
        }
        if (!row.value("payer_id").toString().isEmpty()) {
            payers[row.value("payer_id").toString()] = row.value("payer").toString();
        }
    }
    auto sorted = categories.values();
    sorted.sort();
    QJsonArray category_values, payer_values;
    for (const auto& value : sorted) {
        category_values.append(value);
    }
    for (auto it = payers.begin(); it != payers.end(); ++it) {
        payer_values.append(QJsonObject{{"id", it.key()}, {"username", it.value()}});
    }
    return {{"categories", category_values}, {"payers", payer_values}};
}
QJsonObject QueryService::expenses(const QString& actor, const QJsonObject& parameters) const {
    const Filter filter(parameters);
    const auto all = records(actor, text(parameters, "ledger_id"));
    QJsonArray page;
    Summary summary;
    QMap<QString, Summary> categories, months;
    QJsonObject counts;
    int matched = 0;
    const qint64 offset = static_cast<qint64>(filter.page - 1) * filter.size;
    for (const auto& row : all) {
        if (!filter.matches(row)) {
            continue;
        }
        if (matched >= offset && page.size() < filter.size) {
            page.append(row);
        }
        ++matched;
        const QString state = row.value("status").toString();
        counts[state] = counts.value(state).toInt() + 1;
        if ((state == "approved" || state == "settled") && !row.value("superseded").toBool()) {
            const auto gross = parseMoney(row.value("amount").toString()),
                       refund = parseMoney(row.value("refunded_amount").toString("0.00")),
                       net = parseMoney(row.value("net_amount").toString());
            summary.add(gross, refund, net);
            const QString category = row.value("category").toString().isEmpty()
                                         ? QStringLiteral("未分类")
                                         : row.value("category").toString();
            categories[category].add(gross, refund, net);
            months[row.value("date").toString().left(7)].add(gross, refund, net);
        }
    }
    auto stats = summary.json();
    stats["matched_count"] = matched;
    stats["statuses"] = counts;
    return {{"expenses", page},
            {"total", matched},
            {"page", filter.page},
            {"page_size", filter.size},
            {"pages", qMax(1, (matched + filter.size - 1) / filter.size)},
            {"summary", stats},
            {"categories", groups(categories)},
            {"months", groups(months)}};
}

QJsonObject QueryService::auditAccess(const QString& actor, const QString& ledger_id) const {
    auto row =
        query(database_,
              "SELECT l.name,l.status,l.closed_by,m.status,m.role,m.keep_history,m.audit_cutoff "
              "FROM ledgers l JOIN memberships m ON m.ledger_id=l.id WHERE l.id=? AND m.user_id=?",
              {ledger_id, actor});
    if (!row.next()) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看该账本日志。"));
    }
    const bool exited = row.value(3).toString() == "exited",
               closed = row.value(1).toString() != "active";
    const bool admin = !exited && row.value(4).toString() == "admin" &&
                       (!closed || row.value(2).toString() == actor);
    if (exited ? !row.value(5).toBool() : (closed && !admin)) {
        throw ServiceError("forbidden", QStringLiteral("无权限查看该账本日志。"));
    }
    return {{"id", ledger_id},
            {"name", row.value(0).toString()},
            {"status", row.value(1).toString()},
            {"can_view_all", admin},
            {"historical", exited},
            {"cutoff", exited ? row.value(6).toLongLong() : qint64(0)}};
}
QJsonObject QueryService::audit(const QString& actor, const QString& method,
                                const QJsonObject& parameters) const {
    if (method == "audit_ledgers") {
        QJsonArray books;
        auto ids =
            query(database_, "SELECT ledger_id FROM memberships WHERE user_id=? ORDER BY ledger_id",
                  {actor});
        while (ids.next()) {
            try {
                books.append(auditAccess(actor, ids.value(0).toString()));
            } catch (const ServiceError& failure) {
                if (failure.code() != "forbidden") {
                    throw;
                }
            }
        }
        return {{"ledgers", books}};
    }
    const QString ledger_id = text(parameters, "ledger_id");
    const auto access = auditAccess(actor, ledger_id);
    QString scope = text(parameters, "scope", 16);
    if (scope.isEmpty()) {
        scope = access.value("can_view_all").toBool() ? "all" : "related";
    }
    if (scope != "all" && scope != "related") {
        throw ServiceError("validation", QStringLiteral("日志范围不正确。"));
    }
    if (scope == "all" && !access.value("can_view_all").toBool()) {
        throw ServiceError("forbidden", QStringLiteral("只有当前管理员可查看全部管理日志。"));
    }
    const QString from = date(parameters, "date_from"), to = date(parameters, "date_to"),
                  action = text(parameters, "action"), by = text(parameters, "actor_id"),
                  keyword = text(parameters, "keyword", 500), wanted = text(parameters, "log_id");
    if (!from.isEmpty() && !to.isEmpty() && from > to) {
        throw ServiceError("validation", QStringLiteral("开始日期不能晚于结束日期。"));
    }
    const int page_number = integer(parameters, "page", 1, 1000000),
              size = integer(parameters, "page_size", 50, 200);
    const qint64 offset = static_cast<qint64>(page_number - 1) * size;
    auto rows =
        query(database_,
              "SELECT "
              "o.id,o.rowid,o.created_at,o.actor_id,COALESCE(u.username,o.actor_id),o.action,o."
              "target_id,o.detail,COALESCE(d.private_owner,''),COALESCE(d.kind,'management'),d."
              "payload,EXISTS(SELECT 1 FROM audit_audience a WHERE a.log_id=o.id AND a.user_id=?) "
              "FROM operation_logs o LEFT JOIN users u ON u.id=o.actor_id LEFT JOIN audit_details "
              "d ON d.log_id=o.id WHERE o.ledger_id=? ORDER BY o.created_at DESC,o.rowid DESC",
              {actor, ledger_id});
    QJsonArray result, actors;
    QMap<QString, QString> visible_actors;
    int total = 0;
    while (rows.next()) {
        if ((!rows.value(8).toString().isEmpty() && rows.value(8).toString() != actor) ||
            (access.value("historical").toBool() &&
             rows.value(1).toLongLong() > access.value("cutoff").toInteger()) ||
            (scope == "related" &&
             (!rows.value(11).toBool() || rows.value(9).toString() == "management" ||
              rows.value(9).toString() == "settlement"))) {
            continue;
        }
        const QString id = rows.value(0).toString();
        auto item = object(rows.value(10));
        item["id"] = id;
        item["created_at"] = rows.value(2).toLongLong();
        item["actor_id"] = rows.value(3).toString();
        item["actor"] = rows.value(4).toString();
        item["action"] = rows.value(5).toString();
        item["target"] = rows.value(6).toString();
        item["detail"] = rows.value(7).toString();
        if (method == "audit_detail") {
            if (id == wanted) {
                return item;
            }
            continue;
        }
        visible_actors[rows.value(3).toString()] = rows.value(4).toString();
        const QString day =
            QDateTime::fromSecsSinceEpoch(rows.value(2).toLongLong()).date().toString(Qt::ISODate);
        if ((!from.isEmpty() && day < from) || (!to.isEmpty() && day > to) ||
            (!action.isEmpty() && rows.value(5).toString() != action) ||
            (!by.isEmpty() && rows.value(3).toString() != by) ||
            (!keyword.isEmpty() && !(rows.value(7).toString() + item.value("reason").toString() +
                                     item.value("expense_description").toString())
                                        .contains(keyword, Qt::CaseInsensitive))) {
            continue;
        }
        if (total >= offset && result.size() < size) {
            result.append(item);
        }
        ++total;
    }
    if (method == "audit_detail") {
        throw ServiceError("forbidden", QStringLiteral("无权限查看此日志，或日志不在历史范围内。"));
    }
    for (auto it = visible_actors.begin(); it != visible_actors.end(); ++it) {
        actors.append(QJsonObject{{"id", it.key()}, {"username", it.value()}});
    }
    auto response = access;
    response["logs"] = result;
    response["total"] = total;
    response["page"] = page_number;
    response["pages"] = qMax(1, (total + size - 1) / size);
    response["page_size"] = size;
    response["scope"] = scope;
    response["actors"] = actors;
    return response;
}
}  // namespace ledger
