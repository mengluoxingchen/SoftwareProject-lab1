#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/main_window.h"

class UiSecurityTests final : public QObject {
    Q_OBJECT
private slots:
    void changesPasswordAndResetsWithoutLosingLedger();
};

void UiSecurityTests::changesPasswordAndResetsWithoutLosingLedger() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        const QString name = QStringLiteral("账号安全验收");
        api.call("register", {{"username", name}, {"password", "Password-42"}});
        ledger::ApiClient peer;
        peer.connectToService(directory.path(), false);
        peer.login(name, "Password-42");
        QFile styles(":/resources/styles.qss");
        QVERIFY(styles.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(styles.readAll()));
        ledger::MainWindow window(api);
        window.show();
        QCoreApplication::processEvents();
        QTimer bailout;
        connect(&bailout, &QTimer::timeout, &window, [] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        bailout.start(30000);
        auto* username = window.findChild<QLineEdit*>("usernameInput");
        auto* password = window.findChild<QLineEdit*>("passwordInput");
        auto* login = window.findChild<QPushButton*>("authSubmit");
        auto* security = window.findChild<QPushButton*>("accountSecurityButton");
        QVERIFY(username && password && login && security);
        username->setText(name);
        password->setText("Password-42");
        login->click();
        QVERIFY(api.isAuthenticated());
        const auto owner = api.call("me");
        const auto book =
            api.call("create_ledger", {{"name", QStringLiteral("密码修改后保留的账本")}});
        const auto expense = api.call(
            "expense_submit",
            {{"ledger_id", book.value("id")},
             {"amount", "90"},
             {"date", "2026-11-14"},
             {"description", QStringLiteral("密码修改后保留的电费")},
             {"payer_id", owner.value("id")},
             {"split_method", "equal"},
             {"splits", QJsonArray{QJsonObject{{"user_id", owner.value("id")}, {"value", ""}}}}});
        window.findChild<QPushButton*>("refreshWorkspace")->click();
        const auto screenshot = [&](QWidget* widget, const QString& file) {
            const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
            QCoreApplication::processEvents();
            QTest::qWait(100);
            return output.isEmpty() ||
                   (QDir().mkpath(output) && widget->grab().save(QDir(output).filePath(file)));
        };
        bool changed = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("accountSecurityDialog");
            QVERIFY(dialog);
            auto* current = dialog->findChild<QLineEdit*>("securityCurrentPassword");
            auto* replacement = dialog->findChild<QLineEdit*>("securityNewPassword");
            auto* confirmation = dialog->findChild<QLineEdit*>("securityConfirmPassword");
            auto* submit = dialog->findChild<QPushButton*>("changePasswordSubmit");
            QVERIFY(current && replacement && confirmation && submit);
            current->setText("Wrong-42");
            replacement->setText("Changed-43");
            confirmation->setText("Changed-43");
            submit->click();
            QVERIFY(dialog->isVisible());
            QVERIFY(dialog->findChild<QLabel*>("accountSecurityError")
                        ->text()
                        .contains(QStringLiteral("当前密码")));
            QVERIFY(screenshot(dialog, "account-security-change.png"));
            current->setText("Password-42");
            submit->click();
            changed = true;
        });
        security->click();
        QVERIFY(changed);
        QVERIFY(!api.isAuthenticated());
        QVERIFY(
            window.findChild<QLabel*>("authError")->text().contains(QStringLiteral("密码已修改")));
        bool peer_revoked = false;
        try {
            peer.call("me");
        } catch (const ledger::ServiceError& error) {
            peer_revoked = error.code() == "unauthenticated";
        }
        QVERIFY(peer_revoked);
        username->setText(name);
        password->setText("Password-42");
        login->click();
        QVERIFY(!api.isAuthenticated());
        password->setText("Changed-43");
        login->click();
        QVERIFY(api.isAuthenticated());
        QString recovery_code;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("accountSecurityDialog");
            QVERIFY(dialog);
            dialog->findChild<QLineEdit*>("securityCurrentPassword")->setText("Changed-43");
            QTimer::singleShot(0, &window, [&] {
                auto* code_dialog = window.findChild<QDialog*>("recoveryCodeDialog");
                QVERIFY(code_dialog);
                auto* output = code_dialog->findChild<QLineEdit*>("recoveryCodeOutput");
                QVERIFY(output && output->isReadOnly());
                recovery_code = output->text();
                QCOMPARE(recovery_code.size(), 39);
                code_dialog->findChild<QPushButton*>("copyRecoveryCode")->click();
                QCOMPARE(QApplication::clipboard()->text(), recovery_code);
                QVERIFY(screenshot(code_dialog, "account-recovery-code.png"));
                code_dialog->findChild<QPushButton*>("recoveryCodeDone")->click();
            });
            dialog->findChild<QPushButton*>("generateRecoveryCode")->click();
            dialog->findChild<QPushButton*>("accountSecurityClose")->click();
        });
        security->click();
        QCOMPARE(recovery_code.size(), 39);
        window.findChild<QPushButton*>("logoutButton")->click();
        QVERIFY(!api.isAuthenticated());
        QString next_code;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("passwordResetDialog");
            QVERIFY(dialog);
            dialog->findChild<QLineEdit*>("resetUsername")->setText(name);
            auto* code = dialog->findChild<QLineEdit*>("resetRecoveryCode");
            code->setText("WrongCode");
            dialog->findChild<QLineEdit*>("resetNewPassword")->setText("Recovered-44");
            dialog->findChild<QLineEdit*>("resetConfirmPassword")->setText("Recovered-44");
            auto* submit = dialog->findChild<QPushButton*>("passwordResetSubmit");
            submit->click();
            QVERIFY(dialog->findChild<QLabel*>("passwordResetError")
                        ->text()
                        .contains(QStringLiteral("恢复码")));
            QVERIFY(screenshot(dialog, "account-password-reset.png"));
            code->setText(recovery_code);
            QTimer::singleShot(0, &window, [&] {
                auto* code_dialog = window.findChild<QDialog*>("recoveryCodeDialog");
                QVERIFY(code_dialog);
                next_code = code_dialog->findChild<QLineEdit*>("recoveryCodeOutput")->text();
                code_dialog->findChild<QPushButton*>("recoveryCodeDone")->click();
            });
            submit->click();
        });
        window.findChild<QPushButton*>("forgotPasswordButton")->click();
        QVERIFY(!api.isAuthenticated());
        QCOMPARE(next_code.size(), 39);
        QVERIFY(next_code != recovery_code);
        QCOMPARE(username->text(), name);
        password->setText("Recovered-44");
        login->click();
        QVERIFY(api.isAuthenticated());
        QCOMPARE(api.call("me").value("id"), owner.value("id"));
        QCOMPARE(window.findChild<QComboBox*>("ledgerSelector")->count(), 1);
        const auto detail = api.call(
            "expense_detail", {{"ledger_id", book.value("id")},
                               {"expense_id", expense.value("expense").toObject().value("id")}});
        QCOMPARE(detail.value("shares").toArray().first().toObject().value("amount").toString(),
                 QString("90.00"));
        bool reused = false;
        try {
            api.call("password_reset", {{"username", name},
                                        {"recovery_code", recovery_code},
                                        {"new_password", "BadReuse-45"},
                                        {"confirm_password", "BadReuse-45"}});
        } catch (const ledger::ServiceError& error) {
            reused = error.code() == "invalid_recovery";
        }
        QVERIFY(reused);
        QVERIFY(api.isAuthenticated());
        peer.login(name, "Recovered-44");
        peer.call("password_change", {{"current_password", "Recovered-44"},
                                      {"new_password", "Last-password-46"},
                                      {"confirm_password", "Last-password-46"}});
        security->click();
        QVERIFY(!api.isAuthenticated());
        QVERIFY(window.findChild<QLabel*>("authError")->text().contains(QStringLiteral("登录")));
        bailout.stop();
        QApplication::clipboard()->clear();
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

QTEST_MAIN(UiSecurityTests)
#include "ui_security_tests.moc"
