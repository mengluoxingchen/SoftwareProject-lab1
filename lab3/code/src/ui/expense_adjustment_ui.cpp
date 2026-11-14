#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QUuid>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/expense_page.h"

namespace ledger {
void ExpensePage::startSupplement() {
    if (!can_write_ || !confirmLeave()) {
        return;
    }
    resetForm();
    financial_mode_ = "supplement";
    financial_request_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    reason_label_->setText(QStringLiteral("补记原因 *（请说明此前漏记的原因）"));
    reason_panel_->show();
    draft_button_->hide();
    const bool admin = book_.value("role") == "admin";
    form_title_->setText(admin ? QStringLiteral("结算后补记（核对后生效）")
                               : QStringLiteral("结算后补记（提交审核）"));
    findChild<QPushButton*>("reacquireExpenseLock")->hide();
    submit_button_->setText(admin ? QStringLiteral("核对并确认补记")
                                  : QStringLiteral("提交补记审核"));
    stack_->setCurrentIndex(1);
    notice_->clear();
}

void ExpensePage::startCorrection(const QString& id) {
    if (!confirmLeave()) {
        return;
    }
    const auto record =
        api_.call("expense_detail", {{"ledger_id", book_.value("id")}, {"expense_id", id}});
    if (book_.value("role") != "admin" || record.value("status") != "settled" ||
        record.value("superseded").toBool()) {
        throw ServiceError("forbidden", QStringLiteral("仅管理员可纠正当前有效的已结算账目。"));
    }
    fillForm(record);
    editing_id_.clear();
    editing_status_.clear();
    version_ = 0;
    lock_token_.clear();
    financial_mode_ = "correction";
    financial_source_id_ = id;
    financial_token_ = record.value("financial").toObject().value("token").toString();
    financial_request_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    reason_label_->setText(
        QStringLiteral("纠错原因 *（原记录和原结算快照保留；填 0 可冲销未退款支出）"));
    reason_panel_->show();
    draft_button_->hide();
    form_title_->setText(QStringLiteral("结算后纠错（保留原记录）"));
    findChild<QPushButton*>("reacquireExpenseLock")->hide();
    submit_button_->setText(QStringLiteral("核对并确认纠错"));
    stack_->setCurrentIndex(1);
    notice_->clear();
    preview();
}

bool ExpensePage::submitAdjustment() {
    bool saved = false;
    action([&] {
        markField({});
        if (reconcileWrite()) {
            saved = true;
            return;
        }
        auto parameters = formData();
        parameters["kind"] = financial_mode_;
        parameters["source_id"] = financial_source_id_;
        parameters["expected_token"] = financial_token_;
        parameters["request_key"] = financial_request_key_;
        parameters["reason"] = change_reason_->text();
        const auto preview = api_.call("finance_adjustment_preview", parameters);
        QMessageBox question(QMessageBox::Question, QStringLiteral("核对结算后调整"),
                             preview.value("confirm_text").toString(),
                             QMessageBox::Yes | QMessageBox::No, this);
        question.setTextFormat(Qt::PlainText);
        question.setDefaultButton(QMessageBox::No);
        question.button(QMessageBox::Yes)->setText(QStringLiteral("确认登记"));
        question.button(QMessageBox::No)->setText(QStringLiteral("返回修改"));
        if (question.exec() != QMessageBox::Yes) {
            return;
        }
        parameters["confirmed"] = true;
        parameters["confirmation_token"] = preview.value("confirmation_token");
        parameters = prepareWrite("finance_adjustment", parameters);
        api_.call("finance_adjustment", parameters);
        removeRecovery();
        dirty_ = false;
        editing_id_.clear();
        financial_mode_.clear();
        stack_->setCurrentIndex(0);
        reloadList();
        saved = true;
        message(QStringLiteral("调整已登记并计入当前余额，原记录与旧结算快照已保留。"));
    });
    return saved;
}
}  // namespace ledger
