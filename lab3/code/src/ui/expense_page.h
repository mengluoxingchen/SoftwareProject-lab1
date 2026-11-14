#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QWidget>
#include <functional>

#include "recovery/recovery_store.h"

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace ledger {
class ApiClient;
class ExpensePage final : public QWidget {
    Q_OBJECT
public:
    ExpensePage(ApiClient& api, bool view_only, QWidget* parent = nullptr,
                bool review_mode = false);
    void setContext(const QJsonObject& book, const QJsonObject& user);
    void clearContext();
    void refresh();
    bool confirmLeave();
signals:
    void sessionExpired(const QString& message);
    void manageMembersRequested();

private:
    QWidget* createList();
    QWidget* createForm();
    void startNew();
    void startSupplement();
    void startCorrection(const QString& id);
    bool submitAdjustment();
    void resetForm();
    void addBearerRow(const QJsonObject& member, bool selected);
    void syncMembers();
    void refreshMembers();
    void fillForm(const QJsonObject& record);
    QJsonObject formData() const;
    void preview();
    void reloadList();
    bool saveDraft();
    bool submit();
    void openDraft(const QString& id);
    void showDetail(const QString& id);
    void action(const std::function<void()>& operation);
    void message(const QString& text, bool error = false);
    void markField(const QString& field);
    void releaseLock();
    void renewLock();
    void reacquireLock();
    void withdrawRecord(const QJsonObject& record);
    void reloadNotices();
    void queueRecovery();
    bool persistRecovery();
    void releaseRecovery();
    void removeRecovery();
    QJsonObject recoverySnapshot() const;
    void showRecoveryCopies();
    void restoreRecovery(const QString& id);
    void viewRecovery(const QJsonObject& envelope);
    QJsonObject prepareWrite(const QString& method, QJsonObject parameters);
    bool reconcileWrite();
    void updateRecoveryButton();

    ApiClient& api_;
    QTimer* recovery_timer_ = nullptr;
    QPushButton* recovery_button_ = nullptr;
    QLabel* recovery_status_ = nullptr;
    QString recovery_id_;
    std::unique_ptr<RecoveryLease> recovery_lease_;
    QJsonObject pending_write_;
    bool view_only_ = false;
    bool review_mode_ = false;
    bool can_write_ = false;
    bool loading_ = false;
    bool dirty_ = false;
    QJsonObject book_;
    QJsonObject user_;
    QString financial_mode_, financial_source_id_, financial_token_, financial_request_key_;
    bool editing_post_adjustment_ = false;
    QString record_adjustment_reason_;
    QString editing_id_;
    QString editing_status_;
    QString lock_token_;
    QElapsedTimer last_activity_;
    QTimer* lock_timer_ = nullptr;
    int version_ = 0;
    QStackedWidget* stack_ = nullptr;
    QLabel* notice_ = nullptr;
    QPushButton* new_button_ = nullptr;
    QPushButton* supplement_button_ = nullptr;
    QLabel* reason_label_ = nullptr;
    QComboBox* filter_ = nullptr;
    QLabel* empty_ = nullptr;
    QTableWidget* records_ = nullptr;
    QWidget* notices_panel_ = nullptr;
    QTableWidget* notices_ = nullptr;
    QLabel* form_title_ = nullptr;
    QLineEdit* amount_ = nullptr;
    QDateEdit* date_ = nullptr;
    QComboBox* payer_ = nullptr;
    QLineEdit* description_ = nullptr;
    QLineEdit* category_ = nullptr;
    QLineEdit* change_reason_ = nullptr;
    QWidget* reason_panel_ = nullptr;
    QComboBox* method_ = nullptr;
    QPushButton* manage_members_button_ = nullptr;
    QLabel* member_help_ = nullptr;
    QLabel* split_hint_ = nullptr;
    QTableWidget* splits_ = nullptr;
    QLabel* preview_ = nullptr;
    QPushButton* draft_button_ = nullptr;
    QPushButton* submit_button_ = nullptr;
};
}  // namespace ledger
