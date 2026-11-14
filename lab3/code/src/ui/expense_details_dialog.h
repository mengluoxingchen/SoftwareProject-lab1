#pragma once
#include <QDialog>
#include <QJsonObject>
class QLabel;
class QPushButton;
class QLineEdit;
class QCheckBox;
class QFormLayout;
class QTableWidget;
class QPlainTextEdit;
namespace ledger {
class ApiClient;
class ExpenseDetailsDialog final : public QDialog {
    Q_OBJECT
public:
    ExpenseDetailsDialog(ApiClient& api, const QJsonObject& book, const QString& expense_id,
                         bool review_mode, QWidget* parent = nullptr);
signals:
    void sessionExpired(QString message);

private:
    void reload();
    void decide(const QString& decision);
    void showRevision();
    ApiClient& api_;
    QJsonObject book_, record_, history_;
    QString expense_id_;
    bool review_mode_;
    QFormLayout* summary_;
    QTableWidget *shares_, *similar_, *reviews_, *revisions_, *withdrawals_;
    QPlainTextEdit* revision_text_;
    QLabel *finance_summary_, *payment_explanation_;
    QTableWidget *net_shares_, *finance_history_;
    QPushButton *refund_button_, *replacement_button_;
    QLabel *error_, *lock_;
    QWidget* review_form_;
    QLineEdit* reason_;
    QCheckBox* confirmed_;
};
}  // namespace ledger
