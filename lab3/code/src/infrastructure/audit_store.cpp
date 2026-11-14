#include "infrastructure/audit_store.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariantList>

namespace ledger {
namespace {
QSqlQuery query(QSqlDatabase database, const QString& sql, const QVariantList& values = {}) {
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
QJsonObject object(const QVariant& value) {
    return QJsonDocument::fromJson(value.toString().toUtf8()).object();
}
QString money(qint64 value) {
    const bool negative = value < 0;
    const auto whole = value / 100, fraction = value % 100;
    return (negative ? QStringLiteral("-") : QString()) +
           QString::number(whole < 0 ? -whole : whole) + "." +
           QString::number(fraction < 0 ? -fraction : fraction).rightJustified(2, '0');
}
void participants(const QJsonObject& form, QSet<QString>& users) {
    users.insert(form.value("payer_id").toString());
    for (const auto& part : form.value("splits").toArray()) {
        users.insert(part.toObject().value("user_id").toString());
    }
}
void financialParticipants(const QJsonObject& state, QSet<QString>& users) {
    users.insert(state.value("payer_id").toString());
    for (const auto& value : state.value("original_shares").toArray()) {
        users.insert(value.toObject().value("user_id").toString());
    }
    for (const auto& value : state.value("effective_shares").toArray()) {
        users.insert(value.toObject().value("user_id").toString());
    }
}
}  // namespace

void appendAuditEvidence(QSqlDatabase database, const QString& log_id, bool legacy,
                         const QJsonObject& amounts) {
    auto log = query(
        database,
        "SELECT rowid,ledger_id,actor_id,action,target_id,detail FROM operation_logs WHERE id=?",
        {log_id});
    if (!log.next()) {
        throw std::runtime_error("Audit log does not exist.");
    }
    const auto ordinal = log.value(0).toLongLong();
    const QString ledger_id = log.value(1).toString(), actor = log.value(2).toString(),
                  action = log.value(3).toString(), target = log.value(4).toString();
    QString kind = "management", expense_id = QStringLiteral(""),
            private_owner = QStringLiteral("");
    QJsonObject evidence{{"before_amount", ""},
                         {"after_amount", ""},
                         {"reason", log.value(5).toString()},
                         {"result", "已完成"},
                         {"legacy", legacy}};
    QSet<QString> audience;
    const QStringList expense_actions{"save_expense_draft", "submit_expense", "revise_expense",
                                      "approve_expense",    "reject_expense", "withdraw_expense"};
    if (expense_actions.contains(action)) {
        kind = "expense";
        expense_id = target;
        audience.insert(actor);
        if (action == "save_expense_draft") {
            private_owner = actor;
        }
        auto record =
            query(database, "SELECT creator_id,payload FROM expenses WHERE id=? AND ledger_id=?",
                  {target, ledger_id});
        if (record.next()) {
            const QString creator = record.value(0).toString();
            audience.insert(creator);
            auto form = object(record.value(1));
            // Old revisions can be matched to the append order of revision logs. Never
            // invent original draft values when the old schema did not preserve them.
            qint64 revision_count = 0;
            if (legacy) {
                auto count = query(database,
                                   "SELECT COUNT(*) FROM operation_logs WHERE ledger_id=? AND "
                                   "target_id=? AND action='revise_expense' AND rowid<=?",
                                   {ledger_id, target, ordinal});
                count.next();
                revision_count = count.value(0).toLongLong();
                auto revision =
                    query(database,
                          revision_count > 0 ? "SELECT new_form FROM expense_revisions WHERE "
                                               "expense_id=? ORDER BY rowid LIMIT 1 OFFSET ?"
                                             : "SELECT previous_form FROM expense_revisions WHERE "
                                               "expense_id=? ORDER BY rowid LIMIT 1",
                          revision_count > 0 ? QVariantList{target, revision_count - 1}
                                             : QVariantList{target});
                if (revision.next()) {
                    form = object(revision.value(0));
                }
            }
            if (!(legacy && action == "save_expense_draft")) {
                participants(form, audience);
                evidence["after_amount"] = form.value("amount");
                evidence["expense_description"] = form.value("description");
            }
            if (action == "revise_expense") {
                auto revision = query(
                    database,
                    legacy ? "SELECT previous_form,new_form,reason,previous_status,new_status FROM "
                             "expense_revisions WHERE expense_id=? ORDER BY rowid LIMIT 1 OFFSET ?"
                           : "SELECT previous_form,new_form,reason,previous_status,new_status FROM "
                             "expense_revisions WHERE expense_id=? ORDER BY rowid DESC LIMIT 1",
                    legacy ? QVariantList{target, qMax<qint64>(0, revision_count - 1)}
                           : QVariantList{target});
                if (revision.next()) {
                    const auto before = object(revision.value(0)),
                               after = object(revision.value(1));
                    participants(before, audience);
                    participants(after, audience);
                    evidence["before_amount"] = before.value("amount");
                    evidence["after_amount"] = after.value("amount");
                    evidence["reason"] = revision.value(2).toString();
                    evidence["before_status"] = revision.value(3).toString();
                    evidence["after_status"] = revision.value(4).toString();
                    evidence["result"] = "修改后重新待审核";
                }
            }
        }
        if (action == "approve_expense") {
            evidence["result"] = "审核通过";
        }
        if (action == "reject_expense") {
            evidence["result"] = "审核驳回";
        }
        if (action == "withdraw_expense") {
            evidence["result"] = "已撤回";
        }
        if (action == "submit_expense") {
            evidence["result"] = "已提交待审核";
        }
        if (action == "save_expense_draft") {
            audience = QSet<QString>{actor};
            evidence["result"] = "私有草稿已保存";
        }
    } else if (action == "record_refund" || action == "record_supplement" ||
               action == "correct_settled_expense") {
        kind = "finance";
        audience.insert(actor);
        auto event = query(database,
                           "SELECT "
                           "root_id,COALESCE(source_id,''),COALESCE(new_expense_id,''),COALESCE("
                           "receiver_id,''),reason,before_state,after_state FROM "
                           "expense_finance_events WHERE id=? AND ledger_id=?",
                           {target, ledger_id});
        if (event.next()) {
            const auto before = object(event.value(5)), after = object(event.value(6));
            financialParticipants(before, audience);
            financialParticipants(after, audience);
            audience.insert(event.value(3).toString());
            expense_id = event.value(1).toString().isEmpty() ? event.value(2).toString()
                                                             : event.value(1).toString();
            evidence["before_amount"] = before.value("net_amount").toString("0.00");
            evidence["after_amount"] = after.value("net_amount").toString();
            evidence["amount_basis"] = "净支出";
            evidence["reason"] = event.value(4).toString();
            evidence["result"] = "财务调整已生效";
            auto creators = query(database, "SELECT creator_id FROM expenses WHERE id IN (?,?,?)",
                                  {event.value(0), event.value(1), event.value(2)});
            while (creators.next()) {
                audience.insert(creators.value(0).toString());
            }
        }
    } else if (action == "record_repayment") {
        kind = "repayment";
        audience.insert(actor);
        auto repayment = query(database,
                               "SELECT payer_id,recipient_id,amount_cents,note FROM repayments "
                               "WHERE id=? AND ledger_id=?",
                               {target, ledger_id});
        if (repayment.next()) {
            audience.insert(repayment.value(0).toString());
            audience.insert(repayment.value(1).toString());
            evidence["after_amount"] = money(repayment.value(2).toLongLong());
            evidence["reason"] = repayment.value(3).toString();
            evidence["result"] = "实际还款已登记";
        }
    } else if (action == "create_settlement") {
        kind = "settlement";
        evidence["result"] = "结算批次已生成";
    }
    evidence["kind"] = kind;
    evidence["expense_id"] = expense_id;
    for (const auto& key : {"before_amount", "after_amount"}) {
        if (amounts.contains(key)) {
            evidence[key] = amounts.value(key);
        }
    }
    const auto encoded = QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact));
    query(database, "INSERT INTO audit_details VALUES(?,?,?,?,?,?)",
          {log_id, ledger_id, kind, expense_id, private_owner, encoded});
    for (const auto& user : audience) {
        if (user.isEmpty()) {
            continue;
        }
        query(database,
              "INSERT INTO audit_audience(log_id,user_id) SELECT ?,id FROM users WHERE id=?",
              {log_id, user});
    }
}
}  // namespace ledger
