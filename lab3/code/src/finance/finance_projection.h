#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>

#include "domain/split.h"

namespace ledger::finance_detail {
QSqlQuery query(QSqlDatabase database, const QString& sql, const QVariantList& values = {});
QString json(const QJsonObject& object);
QString digest(const QJsonObject& object);
qint64 cents(QString value);
QVector<ShareAllocation> shares(const QJsonArray& values);
QJsonArray shareJson(const QVector<ShareAllocation>& shares, const QJsonArray& names = {});
QJsonObject projection(QSqlDatabase database, const QString& ledger_id, const QString& expense_id);
QJsonObject storedState(QJsonObject state);
QJsonObject ledgerBalances(QSqlDatabase database, const QString& ledger_id);
}  // namespace ledger::finance_detail
