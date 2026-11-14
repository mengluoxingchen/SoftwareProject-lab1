#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTableWidget;
namespace ledger {
class ApiClient;
class BrowsePage final : public QWidget {
    Q_OBJECT
public:
    BrowsePage(ApiClient& api, bool audit, QWidget* parent = nullptr);
    void setContext(const QJsonObject& book, const QJsonObject& user);
    void clearContext();
    void refresh();
signals:
    void sessionExpired(const QString& message);
    void viewSettlementRequested();

private:
    void reset();
    void configureScope();
    void loadOptions();
    void render(const QJsonObject& response);
    void renderGroups();
    void showDetail(const QString& id);
    void action(const std::function<void()>& operation);
    QJsonObject parameters() const;
    ApiClient& api_;
    bool audit_ = false, loading_ = false;
    QJsonObject book_, user_, response_;
    int page_ = 1, pages_ = 1;
    QLineEdit *from_, *to_, *minimum_ = nullptr, *maximum_ = nullptr, *keyword_;
    QComboBox *ledger_ = nullptr, *scope_ = nullptr, *payer_ = nullptr, *category_ = nullptr,
              *state_ = nullptr, *actor_ = nullptr, *action_ = nullptr, *size_, *group_ = nullptr;
    QLabel *notice_, *summary_, *empty_, *page_label_;
    QPushButton *previous_, *next_;
    QPushButton* settlement_button_ = nullptr;
    QTableWidget *table_, *groups_ = nullptr;
};
}  // namespace ledger
