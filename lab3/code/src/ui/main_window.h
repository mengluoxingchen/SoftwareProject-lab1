#pragma once

#include <QJsonObject>
#include <QMainWindow>
#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTabBar;
class QCloseEvent;

namespace ledger {
class ApiClient;
class ExpensePage;
class BrowsePage;
class SettlementPage;
class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(ApiClient& api, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* createLoginPage();
    QWidget* createWorkspace();
    QWidget* createLedgerPage();
    QWidget* createMembershipControls(QWidget* parent);
    void openMembershipDialog(bool exiting);
    QWidget* createLedgerCloseControls(QWidget* parent);
    QWidget* createClosedLedgerSection(QWidget* parent);
    void closeCurrentLedger();
    void renderClosedLedgers();
    void showClosedLedgerLogs(const QString& id);
    void submitAuthentication();
    void openAccountSecurity();
    void openPasswordReset();
    void showRecoveryCode(const QString& code, QWidget* parent = nullptr);
    void refreshWorkspace(const QString& preferred_ledger = {});
    void loadLedger();
    void renderMembers(const QJsonObject& ledger);
    void renderInvitations(const QJsonObject& result);
    void selectPage(int index);
    void signOut();
    void clearSession();
    void runAction(const std::function<void()>& action);
    void notify(const QString& text, bool error = false);
    ApiClient& api_;
    QJsonObject user_;
    QStackedWidget* screens_ = nullptr;
    QTabBar* auth_mode_ = nullptr;
    QLineEdit* username_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* confirmation_ = nullptr;
    QLabel* auth_error_ = nullptr;
    QLabel* password_hint_ = nullptr;
    QPushButton* auth_submit_ = nullptr;
    QListWidget* navigation_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLabel* page_title_ = nullptr;
    QLabel* page_description_ = nullptr;
    QLabel* account_ = nullptr;
    QLabel* notice_ = nullptr;
    QComboBox* ledgers_ = nullptr;
    QLineEdit* new_ledger_name_ = nullptr;
    QLabel* ledger_summary_ = nullptr;
    QWidget* close_ledger_panel_ = nullptr;
    QPushButton* close_ledger_button_ = nullptr;
    QLabel* close_ledger_hint_ = nullptr;
    QTableWidget* closed_ledgers_ = nullptr;
    QLabel* closed_empty_ = nullptr;
    QTableWidget* members_ = nullptr;
    QWidget* departed_panel_ = nullptr;
    QTableWidget* departed_members_ = nullptr;
    QTableWidget* invitations_ = nullptr;
    QWidget* invitation_form_ = nullptr;
    QLineEdit* invite_username_ = nullptr;
    QComboBox* invite_role_ = nullptr;
    ExpensePage* expense_page_ = nullptr;
    BrowsePage* query_expenses_ = nullptr;
    BrowsePage* audit_page_ = nullptr;
    ExpensePage* review_expenses_ = nullptr;
    SettlementPage* settlement_page_ = nullptr;
    QPushButton* exit_ledger_button_ = nullptr;
    QPushButton* transfer_admin_button_ = nullptr;
    bool historical_ = false;
    QString active_ledger_id_;
};
}  // namespace ledger
