#pragma once

#include <QJsonObject>
#include <QWidget>
#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;

namespace ledger {
class ApiClient;
class SettlementPage final : public QWidget {
    Q_OBJECT
public:
    explicit SettlementPage(ApiClient& api, QWidget* parent = nullptr);
    void setContext(const QJsonObject& book, const QJsonObject& user);
    void clearContext();
    void refresh();
signals:
    void sessionExpired(const QString& message);

private:
    void render();
    void renderPersonal();
    void generate();
    void recordRepayment();
    void showSnapshot(const QString& id);
    void action(const std::function<void()>& operation);
    void message(const QString& text, bool error = false);
    ApiClient& api_;
    QJsonObject book_, user_, overview_;
    QLabel* notice_ = nullptr;
    QLabel* pending_ = nullptr;
    QLabel* total_ = nullptr;
    QLabel* repaid_ = nullptr;
    QLabel* refunds_ = nullptr;
    QLabel* outstanding_ = nullptr;
    QLabel* suggestions_ = nullptr;
    QLabel *my_status_ = nullptr, *my_scope_ = nullptr, *my_paid_ = nullptr, *my_owed_ = nullptr,
           *my_repaid_ = nullptr, *my_received_ = nullptr, *my_directions_empty_ = nullptr,
           *my_history_empty_ = nullptr;
    QTableWidget *my_directions_ = nullptr, *my_history_ = nullptr;
    QLabel* history_empty_ = nullptr;
    QLabel* repayment_empty_ = nullptr;
    QPushButton* generate_ = nullptr;
    QPushButton* repay_ = nullptr;
    QTableWidget* balances_ = nullptr;
    QTableWidget* history_ = nullptr;
    QTableWidget* repayments_ = nullptr;
    QTableWidget* financial_history_ = nullptr;
};
}  // namespace ledger
