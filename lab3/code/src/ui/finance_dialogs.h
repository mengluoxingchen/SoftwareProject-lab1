#pragma once

#include <QJsonObject>
#include <QString>
class QWidget;
namespace ledger {
class ApiClient;
QString financeKind(const QString& kind, bool voided = false);
bool recordRefundDialog(ApiClient& api, const QJsonObject& book, const QJsonObject& record,
                        QWidget* parent);
void showFinanceEventDialog(ApiClient& api, const QJsonObject& book, const QString& event_id,
                            QWidget* parent);
}  // namespace ledger
