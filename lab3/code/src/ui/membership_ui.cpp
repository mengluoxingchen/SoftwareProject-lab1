#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/expense_page.h"
#include "ui/main_window.h"

namespace ledger {
namespace {
QLabel* plain(const QString& text, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
}  // namespace
QWidget* MainWindow::createMembershipControls(QWidget* parent) {
    auto* controls = new QWidget(parent);
    auto* row = new QHBoxLayout(controls);
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch();
    transfer_admin_button_ = new QPushButton(QStringLiteral("交接管理员"), controls);
    transfer_admin_button_->setObjectName("transferAdminButton");
    exit_ledger_button_ = new QPushButton(QStringLiteral("退出账本 / 交接并退出"), controls);
    exit_ledger_button_->setObjectName("exitLedgerButton");
    row->addWidget(transfer_admin_button_);
    row->addWidget(exit_ledger_button_);
    transfer_admin_button_->hide();
    exit_ledger_button_->hide();
    connect(transfer_admin_button_, &QPushButton::clicked, this,
            [this] { openMembershipDialog(false); });
    connect(exit_ledger_button_, &QPushButton::clicked, this,
            [this] { openMembershipDialog(true); });
    return controls;
}
void MainWindow::openMembershipDialog(bool exiting) {
    if (!expense_page_->confirmLeave()) {
        return;
    }
    runAction([&] {
        const QString id = ledgers_->currentData().toString();
        const auto preview = api_.call("membership_exit_preview", {{"ledger_id", id}});
        const bool admin = preview.value("role") == "admin";
        if (exiting && !preview.value("can_exit").toBool()) {
            QMessageBox notice(
                QMessageBox::Information, QStringLiteral("暂时无法退出账本"),
                QStringLiteral("当前账本只剩您一位成员，无法交接管理员。\n\n"
                               "如果希望自己退出、由他人继续使用，请先邀请新成员，待对方接受后再交"
                               "接并退出。\n\n"
                               "如果希望结束整个账本，请关闭此提示后点击“归档 / 删除账本”。"
                               "退出只改变您的成员身份，归档或删除则会关闭整个账本。"),
                QMessageBox::Ok, this);
            notice.setObjectName("soleMemberExitNotice");
            notice.setTextFormat(Qt::PlainText);
            notice.button(QMessageBox::Ok)->setText(QStringLiteral("我知道了"));
            notice.exec();
            return;
        }
        QDialog dialog(this);
        dialog.setObjectName("membershipChangeDialog");
        dialog.setWindowTitle(exiting ? QStringLiteral("退出账本") : QStringLiteral("交接管理员"));
        dialog.setMinimumWidth(630);
        auto* body = new QVBoxLayout(&dialog);
        body->setContentsMargins(24, 22, 24, 22);
        body->setSpacing(14);
        auto* title = plain(preview.value("name").toString(), &dialog);
        title->setObjectName("sectionTitle");
        body->addWidget(title);
        const QString balance = preview.value("balance").toString();
        body->addWidget(plain(
            QStringLiteral("当前未结清：%1 "
                           "元（正数应收，负数应付）。\n退出不清除历史费用或实际还款；未结清款项不"
                           "会强制阻止退出。账本另有 %2 笔待审核或待结算记录，之后仍可能影响余额。")
                .arg(balance)
                .arg(preview.value("pending_count").toInteger()),
            &dialog));
        auto* successor = new QComboBox(&dialog);
        successor->setObjectName("membershipSuccessor");
        successor->addItem(QStringLiteral("请选择一位当前成员作为新管理员"), QString());
        for (const auto& value : preview.value("members").toArray()) {
            const auto member = value.toObject();
            if (member.value("id") != user_.value("id")) {
                successor->addItem(member.value("username").toString(),
                                   member.value("id").toString());
            }
        }
        auto* successor_label =
            plain(exiting ? QStringLiteral("新管理员 *（交接与退出一起完成）")
                          : QStringLiteral("新管理员 *（交接后您仍保留普通成员身份）"),
                  &dialog);
        body->addWidget(successor_label);
        body->addWidget(successor);
        successor_label->setVisible(admin);
        successor->setVisible(admin);
        auto* keep = new QCheckBox(QStringLiteral("保留历史只读权限"), &dialog);
        keep->setObjectName("membershipKeepHistory");
        keep->setChecked(true);
        keep->setVisible(exiting);
        body->addWidget(keep);
        auto* explanation =
            plain(QStringLiteral("保留：只读查看退出时已获授权的账目及当时明细。\n不保留：退出后无"
                                 "法再访问本账本。两种选择都不能新增、修改、审核或查看当前余额。"),
                  &dialog);
        explanation->setVisible(exiting);
        body->addWidget(explanation);
        auto* deadline_enabled = new QCheckBox(QStringLiteral("设置结清截止日期（选填）"), &dialog);
        deadline_enabled->setObjectName("membershipSetDeadline");
        deadline_enabled->setVisible(exiting);
        body->addWidget(deadline_enabled);
        auto* deadline = new QDateEdit(QDate::currentDate().addDays(7), &dialog);
        deadline->setObjectName("membershipDeadline");
        deadline->setDisplayFormat("yyyy-MM-dd");
        deadline->setCalendarPopup(true);
        deadline->setMinimumDate(QDate::currentDate());
        deadline->setVisible(exiting);
        deadline->setEnabled(false);
        body->addWidget(deadline);
        connect(deadline_enabled, &QCheckBox::toggled, deadline, &QDateEdit::setEnabled);
        auto* error = plain({}, &dialog);
        error->setObjectName("membershipChangeError");
        error->setStyleSheet("color:#b42318;");
        connect(successor, &QComboBox::currentIndexChanged, error, &QLabel::clear);
        body->addWidget(error);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        auto* save = buttons->button(QDialogButtonBox::Save);
        save->setObjectName("confirmMembershipChange");
        save->setText(exiting ? (admin ? QStringLiteral("交接并退出") : QStringLiteral("确认退出"))
                              : QStringLiteral("确认交接"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        body->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        const auto key = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bool expired = false;
        connect(save, &QPushButton::clicked, &dialog, [&] {
            if (admin && successor->currentData().toString().isEmpty()) {
                error->setText(QStringLiteral("请先选择新管理员。"));
                return;
            }
            QMessageBox question(
                QMessageBox::Question, QStringLiteral("核对成员变更"),
                exiting ? QStringLiteral("确认退出“%1”？%2%3")
                              .arg(preview.value("name").toString(),
                                   admin ? QStringLiteral("管理员将交接给 ") +
                                               successor->currentText() + QStringLiteral("。\n")
                                         : QString(),
                                   keep->isChecked() ? QStringLiteral("保留退出时的历史只读快照。")
                                                     : QStringLiteral("不保留本账本访问权限。"))
                        : QStringLiteral("确认把管理员交接给%1？您将成为普通成员。")
                              .arg(successor->currentText()),
                QMessageBox::Yes | QMessageBox::No, &dialog);
            question.setTextFormat(Qt::PlainText);
            question.setDefaultButton(QMessageBox::No);
            question.button(QMessageBox::Yes)->setText(QStringLiteral("确认"));
            question.button(QMessageBox::No)->setText(QStringLiteral("返回修改"));
            if (question.exec() != QMessageBox::Yes) {
                return;
            }
            try {
                api_.call(exiting ? "exit_ledger" : "transfer_admin",
                          {{"ledger_id", id},
                           {"preview_token", preview.value("preview_token")},
                           {"request_key", key},
                           {"confirmed", true},
                           {"successor_id",
                            admin ? successor->currentData().toString() : QStringLiteral("")},
                           {"keep_history", keep->isChecked()},
                           {"settlement_deadline", exiting && deadline_enabled->isChecked()
                                                       ? deadline->date().toString(Qt::ISODate)
                                                       : QStringLiteral("")}});
                dialog.accept();
            } catch (const ServiceError& failure) {
                error->setText(failure.message());
                if (failure.code() == "unauthenticated") {
                    expired = true;
                    dialog.reject();
                }
            }
        });
        const bool accepted = dialog.exec() == QDialog::Accepted;
        if (expired) {
            throw ServiceError("unauthenticated", QStringLiteral("登录已失效，请重新登录。"));
        }
        if (accepted) {
            refreshWorkspace(id);
            if (exiting && keep->isChecked()) {
                navigation_->setCurrentRow(4);
            }
            notify(exiting
                       ? QStringLiteral("已退出账本，费用和还款记录保留；访问权限按您的选择更新。")
                       : QStringLiteral("管理员已交接，您现在是普通成员。"));
        }
    });
}
}  // namespace ledger
