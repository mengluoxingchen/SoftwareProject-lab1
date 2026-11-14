#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "domain/password_policy.h"
#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/expense_page.h"
#include "ui/main_window.h"

namespace ledger {
namespace {
QLabel* note(const QString& text, QWidget* parent, const char* name = "") {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setObjectName(QString::fromLatin1(name));
    return label;
}
QLineEdit* entry(QVBoxLayout* layout, const QString& title, const char* name, QWidget* parent,
                 bool secret = true) {
    layout->addWidget(note(title, parent));
    auto* edit = new QLineEdit(parent);
    edit->setObjectName(QString::fromLatin1(name));
    edit->setMaxLength(128);
    if (secret) edit->setEchoMode(QLineEdit::Password);
    layout->addWidget(edit);
    return edit;
}
QPushButton* action(const QString& title, const char* name, QWidget* parent) {
    auto* button = new QPushButton(title, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}
QVBoxLayout* form(QDialog& dialog, const QString& title) {
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(26, 24, 26, 24);
    layout->setSpacing(12);
    layout->addWidget(note(title, &dialog, "emptyTitle"));
    return layout;
}
const QString policy =
    QStringLiteral("新密码为8–32位，须同时包含英文字母和数字，可用英文符号，不含空格。");
}  // namespace

void MainWindow::showRecoveryCode(const QString& code, QWidget* parent) {
    QDialog dialog(parent ? parent : this);
    dialog.setObjectName("recoveryCodeDialog");
    auto* layout = form(dialog, QStringLiteral("请保存账号恢复码"));
    layout->addWidget(
        note(QStringLiteral("恢复码用于忘记密码时重置账号，只在本次显示。请妥善保存，不要发送给其他"
                            "成员。重新生成或重置密码后，旧恢复码立即失效。"),
             &dialog));
    auto* output = new QLineEdit(code, &dialog);
    output->setObjectName("recoveryCodeOutput");
    output->setReadOnly(true);
    output->setAlignment(Qt::AlignCenter);
    output->setMinimumWidth(500);
    layout->addWidget(output);
    auto* copy = action(QStringLiteral("复制恢复码"), "copyRecoveryCode", &dialog);
    auto* done = action(QStringLiteral("我已保存，继续"), "recoveryCodeDone", &dialog);
    layout->addWidget(copy);
    layout->addWidget(
        note(QStringLiteral("未保存也可在能够登录时，使用当前密码重新生成。若密码和恢复码都遗失，将"
                            "无法自助重置。复制内容会在60秒后清除，请及时保存。"),
             &dialog));
    layout->addWidget(done);
    connect(copy, &QPushButton::clicked, &dialog, [code, copy] {
        QApplication::clipboard()->setText(code);
        copy->setText(QStringLiteral("已复制，请及时保存"));
        QTimer::singleShot(60000, qApp, [code] {
            if (QApplication::clipboard()->text() == code) QApplication::clipboard()->clear();
        });
    });
    connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void MainWindow::openAccountSecurity() {
    QJsonObject status;
    try {
        status = api_.call("account_security");
    } catch (const ServiceError& error) {
        if (error.code() == "unauthenticated") {
            clearSession();
            auth_error_->setStyleSheet({});
            auth_error_->setText(error.message());
        } else {
            notify(error.message(), true);
        }
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("accountSecurityDialog");
    auto* layout = form(dialog, QStringLiteral("账号安全"));
    layout->addWidget(
        note(QStringLiteral("当前账号：") + user_.value("username").toString(), &dialog));
    auto* status_label =
        note(status.value("has_recovery_code").toBool()
                 ? QStringLiteral("已设置恢复码。重新生成会使旧码失效，必须保存新码。")
                 : QStringLiteral("尚未设置恢复码，建议现在生成。忘记密码前需提前保存。"),
             &dialog, "recoveryStatus");
    layout->addWidget(status_label);
    auto* current = entry(layout, QStringLiteral("当前密码（修改密码和生成恢复码均需验证）"),
                          "securityCurrentPassword", &dialog);
    auto* replacement = entry(layout, QStringLiteral("新密码"), "securityNewPassword", &dialog);
    auto* confirmation =
        entry(layout, QStringLiteral("确认新密码"), "securityConfirmPassword", &dialog);
    layout->addWidget(note(policy, &dialog));
    auto* error_label = note({}, &dialog, "accountSecurityError");
    error_label->setStyleSheet("color:#b42318;");
    layout->addWidget(error_label);
    auto* buttons = new QHBoxLayout();
    auto* generate =
        action(status.value("has_recovery_code").toBool() ? QStringLiteral("重新生成恢复码")
                                                          : QStringLiteral("生成恢复码"),
               "generateRecoveryCode", &dialog);
    auto* change = action(QStringLiteral("修改密码"), "changePasswordSubmit", &dialog);
    auto* close = action(QStringLiteral("关闭"), "accountSecurityClose", &dialog);
    buttons->addWidget(generate);
    buttons->addWidget(change);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    bool changed = false;
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(generate, &QPushButton::clicked, &dialog, [&] {
        try {
            const auto result =
                api_.call("recovery_generate", {{"current_password", current->text()}});
            current->clear();
            error_label->clear();
            status_label->setText(
                QStringLiteral("已设置恢复码。重新生成会使旧码失效，必须保存新码。"));
            generate->setText(QStringLiteral("重新生成恢复码"));
            showRecoveryCode(result.value("recovery_code").toString(), &dialog);
        } catch (const ServiceError& error) {
            if (error.code() == "unauthenticated") dialog.reject();
            error_label->setText(error.message());
        }
    });
    connect(change, &QPushButton::clicked, &dialog, [&] {
        const QString error = PasswordPolicy::registrationError(replacement->text());
        if (!error.isEmpty()) {
            error_label->setText(error);
            return;
        }
        if (replacement->text() != confirmation->text()) {
            error_label->setText(QStringLiteral("两次输入的新密码不一致。"));
            return;
        }
        if (!expense_page_->confirmLeave()) return;
        try {
            api_.call("password_change", {{"current_password", current->text()},
                                          {"new_password", replacement->text()},
                                          {"confirm_password", confirmation->text()}});
            changed = true;
            dialog.accept();
        } catch (const ServiceError& failure) {
            if (failure.code() == "unauthenticated") dialog.reject();
            error_label->setText(
                failure.code() == "connection"
                    ? QStringLiteral("操作结果尚未确认，请先尝试新密码登录，勿反复提交。")
                    : failure.message());
        }
    });
    dialog.exec();
    if (changed || !api_.isAuthenticated()) {
        try {
            api_.logout();
        } catch (const ServiceError&) {
        }
        clearSession();
        auth_error_->setStyleSheet("color:#25765b;");
        auth_error_->setText(changed ? QStringLiteral("密码已修改，旧登录状态已失效，请重新登录。")
                                     : QStringLiteral("登录已失效，请重新登录。"));
    }
}

void MainWindow::openPasswordReset() {
    QDialog dialog(this);
    dialog.setObjectName("passwordResetDialog");
    auto* layout = form(dialog, QStringLiteral("通过恢复码重置密码"));
    layout->addWidget(
        note(QStringLiteral("请输入本人此前保存的恢复码。未设置恢复码或同时遗失密码与恢复码时，无法"
                            "自助重置；账本管理员也不能代改账号密码。账目和成员身份会保留。"),
             &dialog));
    auto* name = entry(layout, QStringLiteral("用户名"), "resetUsername", &dialog, false);
    name->setText(username_->text());
    auto* recovery = entry(layout, QStringLiteral("账号恢复码"), "resetRecoveryCode", &dialog);
    auto* replacement = entry(layout, QStringLiteral("新密码"), "resetNewPassword", &dialog);
    auto* confirmation =
        entry(layout, QStringLiteral("确认新密码"), "resetConfirmPassword", &dialog);
    layout->addWidget(note(policy, &dialog));
    auto* error = note({}, &dialog, "passwordResetError");
    error->setStyleSheet("color:#b42318;");
    layout->addWidget(error);
    auto* submit = action(QStringLiteral("重置密码"), "passwordResetSubmit", &dialog);
    auto* cancel = action(QStringLiteral("取消"), "passwordResetCancel", &dialog);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(submit);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    QString next_code;
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(submit, &QPushButton::clicked, &dialog, [&] {
        try {
            next_code = api_.call("password_reset", {{"username", name->text()},
                                                     {"recovery_code", recovery->text()},
                                                     {"new_password", replacement->text()},
                                                     {"confirm_password", confirmation->text()}})
                            .value("recovery_code")
                            .toString();
            dialog.accept();
        } catch (const ServiceError& failure) {
            error->setText(
                failure.code() == "connection"
                    ? QStringLiteral(
                          "操作结果尚未确认，请先尝试新密码登录，登录后可重新生成恢复码。")
                    : failure.message());
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        const QString account_name = name->text();
        clearSession();
        username_->setText(account_name);
        auth_error_->setStyleSheet("color:#25765b;");
        auth_error_->setText(QStringLiteral("密码已重置，请保存新的恢复码，再使用新密码登录。"));
        showRecoveryCode(next_code);
    }
}
}  // namespace ledger
