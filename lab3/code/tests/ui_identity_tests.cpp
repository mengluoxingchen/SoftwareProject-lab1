#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "transport/api_client.h"
#include "ui/main_window.h"

class UiIdentityTests final : public QObject {
    Q_OBJECT
private slots:
    void registerCreateInviteSwitchRolesAndLogout();
    void archiveRestoreAndDeleteWithRetainedLogs();
};

void UiIdentityTests::registerCreateInviteSwitchRolesAndLogout() {
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
        QFile stylesheet(QStringLiteral(":/resources/styles.qss"));
        QVERIFY(stylesheet.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        ledger::MainWindow window(api);
        window.show();
        QCoreApplication::processEvents();
        QVERIFY(window.isVisible());
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        const auto screenshot = [&](const QString& name, QWidget* subject = nullptr) {
            QCoreApplication::processEvents();
            QTest::qWait(100);
            const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
            return output.isEmpty() ||
                   (QDir().mkpath(output) &&
                    (subject ? subject : &window)->grab().save(QDir(output).filePath(name)));
        };
        QVERIFY(screenshot(QStringLiteral("stage2-login.png")));
        auto* modes = window.findChild<QTabBar*>(QStringLiteral("authMode"));
        auto* username = window.findChild<QLineEdit*>(QStringLiteral("usernameInput"));
        auto* password = window.findChild<QLineEdit*>(QStringLiteral("passwordInput"));
        auto* confirmation = window.findChild<QLineEdit*>(QStringLiteral("confirmationInput"));
        auto* submit = window.findChild<QPushButton*>(QStringLiteral("authSubmit"));
        QVERIFY(modes && username && password && confirmation && submit);
        username->setText(QStringLiteral("甲"));
        password->setText(QString(40, '7'));
        QCOMPARE(password->text().size(), 40);
        QTest::mouseClick(submit, Qt::LeftButton);
        QVERIFY(!api.isAuthenticated());
        QCOMPARE(window.findChild<QLabel*>(QStringLiteral("authError"))->text(),
                 QStringLiteral("用户名或密码错误。"));
        QTest::mouseClick(modes, Qt::LeftButton, Qt::NoModifier, modes->tabRect(1).center());
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("passwordHint"))->isVisible());
        QVERIFY(screenshot(QStringLiteral("stage2-register.png")));
        const QStringList invalid_passwords{
            QStringLiteral("12345678"), QStringLiteral("abcdefgh"), QStringLiteral("Abc1234"),
            QStringLiteral("Aa") + QString(31, '1'), QStringLiteral("Abc 12345")};
        for (const QString& candidate : invalid_passwords) {
            password->setText(candidate);
            confirmation->setText(candidate);
            QCOMPARE(password->text(), candidate);
            QTest::mouseClick(submit, Qt::LeftButton);
            QVERIFY(!api.isAuthenticated());
            QVERIFY(window.findChild<QLabel*>(QStringLiteral("authError"))
                        ->text()
                        .contains(QStringLiteral("密码")));
        }
        password->setText(QStringLiteral("Password-42"));
        confirmation->setText(QStringLiteral("Password-42"));
        username->setText(QString(33, QChar(0x4e59)));
        QCOMPARE(username->text().size(), 33);
        QTest::mouseClick(submit, Qt::LeftButton);
        QVERIFY(!api.isAuthenticated());
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("authError"))
                    ->text()
                    .contains(QStringLiteral("长度")));
        username->setText(QStringLiteral("甲"));
        QTest::mouseClick(submit, Qt::LeftButton);
        QVERIFY(api.isAuthenticated());
        QVERIFY(password->text().isEmpty());
        auto* name = window.findChild<QLineEdit*>(QStringLiteral("newLedgerName"));
        name->setText(QStringLiteral("团队共享账本"));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("createLedgerButton")),
                          Qt::LeftButton);
        auto* selector = window.findChild<QComboBox*>(QStringLiteral("ledgerSelector"));
        QCOMPARE(selector->count(), 1);
        QCOMPARE(window.findChild<QLabel*>("ledgerSummary")->textFormat(), Qt::PlainText);
        const QString ledger_id = selector->currentData().toString();
        QVERIFY(!ledger_id.isEmpty());
        auto* members = window.findChild<QTableWidget*>(QStringLiteral("membersTable"));
        QCOMPARE(members->rowCount(), 1);
        QCOMPARE(members->item(0, 0)->text(), QStringLiteral("甲"));
        QCOMPARE(members->item(0, 1)->text(), QStringLiteral("管理员"));
        ledger::ApiClient peer;
        peer.connectToService(directory.path(), false);
        peer.call("register", {{"username", QStringLiteral("乙")}, {"password", "Password-42"}});
        peer.login(QStringLiteral("乙"), "Password-42");
        window.findChild<QLineEdit*>(QStringLiteral("inviteUsername"))
            ->setText(QStringLiteral("乙"));
        QTest::mouseClick(window.findChild<QPushButton*>(QStringLiteral("inviteButton")),
                          Qt::LeftButton);
        const auto pending = peer.call("invitations").value("invitations").toArray();
        QCOMPARE(pending.size(), 1);
        auto* refresh = window.findChild<QPushButton*>(QStringLiteral("refreshWorkspace"));
        auto* logout = window.findChild<QPushButton*>(QStringLiteral("logoutButton"));
        QTest::mouseClick(logout, Qt::LeftButton);
        QVERIFY(!api.isAuthenticated());
        QCOMPARE(selector->count(), 0);
        QCOMPARE(members->rowCount(), 0);
        username->setText(QStringLiteral("乙"));
        password->setText(QStringLiteral("Password-42"));
        QTest::mouseClick(submit, Qt::LeftButton);
        QVERIFY(api.isAuthenticated());
        QCOMPARE(selector->count(), 0);
        auto* received = window.findChild<QTableWidget*>(QStringLiteral("invitationsTable"));
        QCOMPARE(received->rowCount(), 1);
        auto* accept = window.findChild<QPushButton*>(QStringLiteral("acceptInvitationButton"));
        QVERIFY(accept);
        window.findChild<QScrollArea*>()->ensureWidgetVisible(accept);
        QCoreApplication::processEvents();
        QVERIFY(screenshot(QStringLiteral("stage2-invitation-pending.png")));
        QTest::mouseClick(accept, Qt::LeftButton);
        QCOMPARE(selector->count(), 1);
        QCOMPARE(members->rowCount(), 2);
        QVERIFY(!window.findChild<QPushButton*>(QStringLiteral("inviteButton"))->isVisible());
        auto* navigation = window.findChild<QListWidget*>(QStringLiteral("navigation"));
        QVERIFY(navigation->item(2)->isHidden());
        QVERIFY(!navigation->item(1)->isHidden());
        QVERIFY(screenshot(QStringLiteral("stage2-member.png")));
        ledger::ApiClient administrator;
        administrator.connectToService(directory.path(), false);
        ledger::MainWindow admin_window(administrator);
        admin_window.show();
        QCoreApplication::processEvents();
        QVERIFY(admin_window.isVisible());
        admin_window.findChild<QLineEdit*>(QStringLiteral("usernameInput"))
            ->setText(QStringLiteral("甲"));
        admin_window.findChild<QLineEdit*>(QStringLiteral("passwordInput"))
            ->setText(QStringLiteral("Password-42"));
        QTest::mouseClick(admin_window.findChild<QPushButton*>(QStringLiteral("authSubmit")),
                          Qt::LeftButton);
        QCOMPARE(admin_window.findChild<QTableWidget*>(QStringLiteral("membersTable"))->rowCount(),
                 2);
        QVERIFY(screenshot(QStringLiteral("stage2-admin.png"), &admin_window));
        // Inspect every workspace page at the supported minimum desktop size.
        auto* admin_navigation = admin_window.findChild<QListWidget*>("navigation");
        admin_window.resize(1050, 700);
        for (int page = 0; page < admin_navigation->count(); ++page) {
            admin_navigation->setCurrentRow(page);
            QCoreApplication::processEvents();
            QVERIFY(screenshot(QStringLiteral("compact-page-%1.png").arg(page), &admin_window));
        }
        admin_window.resize(1440, 900);
        admin_navigation->setCurrentRow(0);
        auto* change_role =
            admin_window.findChild<QPushButton*>(QStringLiteral("changeRoleButton"));
        QVERIFY(change_role);
        QTest::mouseClick(change_role, Qt::LeftButton);
        admin_window.close();
        QTest::mouseClick(refresh, Qt::LeftButton);
        QVERIFY(navigation->item(1)->isHidden());
        QVERIFY(navigation->item(2)->isHidden());
        QVERIFY(!navigation->item(3)->isHidden());
        QVERIFY(!navigation->item(4)->isHidden());
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("ledgerSummary"))
                    ->text()
                    .contains(QStringLiteral("只读用户")));
        QVERIFY(screenshot(QStringLiteral("stage2-readonly.png")));
        QTest::mouseClick(logout, Qt::LeftButton);
        QVERIFY(username->text().isEmpty());
        QVERIFY(password->text().isEmpty());
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void UiIdentityTests::archiveRestoreAndDeleteWithRetainedLogs() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient administrator;
        administrator.connectToService(directory.path(), false);
        administrator.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        const auto user = administrator.login("owner", "Password-42");
        const auto book =
            administrator.call("create_ledger", {{"name", QStringLiteral("临时账本-关闭验收")}});
        const auto expense =
            administrator
                .call("expense_submit",
                      {{"ledger_id", book.value("id")},
                       {"amount", "90"},
                       {"date", "2026-11-09"},
                       {"payer_id", user.value("id")},
                       {"description", QStringLiteral("用于验证归档后保留")},
                       {"split_method", "equal"},
                       {"splits",
                        QJsonArray{QJsonObject{{"user_id", user.value("id")}, {"value", ""}}}}})
                .value("expense")
                .toObject();
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        QFile stylesheet(":/resources/styles.qss");
        QVERIFY(stylesheet.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        ledger::MainWindow window(api);
        window.show();
        window.findChild<QLineEdit*>("usernameInput")->setText("owner");
        window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
        window.findChild<QPushButton*>("authSubmit")->click();
        QVERIFY(api.isAuthenticated());
        auto* close = window.findChild<QPushButton*>("closeLedgerButton");
        auto* closed = window.findChild<QTableWidget*>("closedLedgersTable");
        QVERIFY(close && closed);
        QVERIFY(close->isEnabled());
        const auto screenshot = [&](QWidget& subject, const QString& name) {
            const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
            if (output.isEmpty()) {
                return true;
            }
            subject.repaint();
            for (auto* child : subject.findChildren<QWidget*>()) {
                if (child->isVisible()) {
                    child->repaint();
                }
            }
            QCoreApplication::processEvents();
            QTest::qWait(100);
            static_cast<void>(subject.grab());
            QCoreApplication::processEvents();
            return QDir().mkpath(output) && subject.grab().save(QDir(output).filePath(name));
        };
        const auto answer = [](QMessageBox::StandardButton choice) {
            QTimer::singleShot(0, qApp, [choice] {
                if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    dialog->button(choice)->click();
                }
            });
        };
        QVERIFY(screenshot(window, "ledger-close-entry.png"));
        bool cancelled = false;
        QTimer::singleShot(0, qApp, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                cancelled = true;
                dialog->reject();
            }
        });
        close->click();
        QVERIFY(cancelled);
        QCOMPARE(closed->rowCount(), 0);
        bool archived = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* proceed = dialog->findChild<QPushButton*>("confirmCloseLedger");
            if (!proceed) {
                dialog->reject();
                return;
            }
            answer(QMessageBox::Yes);
            proceed->click();
            archived = !dialog->isVisible();
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        close->click();
        QVERIFY(archived);
        QCOMPARE(closed->rowCount(), 1);
        QCOMPARE(closed->item(0, 1)->text(), QStringLiteral("已归档"));
        QCOMPARE(window.findChild<QComboBox*>("ledgerSelector")->count(), 0);
        auto* restore = closed->cellWidget(0, 3)->findChild<QPushButton*>("restoreLedgerButton");
        QVERIFY(restore);
        answer(QMessageBox::No);
        restore->click();
        QCOMPARE(closed->rowCount(), 1);
        answer(QMessageBox::Yes);
        restore->click();
        QCOMPARE(closed->rowCount(), 0);
        const auto records = administrator.call("expense_list", {{"ledger_id", book.value("id")}})
                                 .value("expenses")
                                 .toArray();
        QCOMPARE(records.size(), 1);
        QCOMPARE(records[0].toObject().value("id"), expense.value("id"));
        bool deletion_checked = false, delete_capture = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* mode = dialog->findChild<QComboBox*>("closeLedgerMode");
            auto* name = dialog->findChild<QLineEdit*>("closeLedgerConfirmName");
            auto* proceed = dialog->findChild<QPushButton*>("confirmCloseLedger");
            if (!mode || !name || !proceed) {
                dialog->reject();
                return;
            }
            mode->setCurrentIndex(mode->findData("delete"));
            name->setText("Wrong ledger");
            proceed->click();
            const bool name_rejected =
                !dialog->findChild<QLabel*>("closeLedgerError")->text().isEmpty();
            name->setText(book.value("name").toString());
            answer(QMessageBox::No);
            proceed->click();
            const bool still_open = dialog->isVisible();
            delete_capture = screenshot(*dialog, "ledger-delete-confirmation.png");
            answer(QMessageBox::Yes);
            proceed->click();
            deletion_checked = name_rejected && still_open && !dialog->isVisible();
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        close->click();
        QVERIFY(deletion_checked && delete_capture);
        QCOMPARE(closed->rowCount(), 1);
        QCOMPARE(closed->item(0, 1)->text(), QStringLiteral("业务数据已删除"));
        QVERIFY(!closed->cellWidget(0, 3)->findChild<QPushButton*>("restoreLedgerButton"));
        bool logs_checked = false, logs_capture = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* logs = dialog->findChild<QTableWidget*>("closedLedgerLogsTable");
            logs_checked = logs && logs->rowCount() >= 5;
            logs_capture = screenshot(*dialog, "ledger-deleted-retained-logs.png");
            dialog->reject();
        });
        auto* logs = closed->cellWidget(0, 3)->findChild<QPushButton*>("viewClosedLedgerLogs");
        QVERIFY(logs);
        logs->click();
        QVERIFY(logs_checked && logs_capture);
        window.findChild<QScrollArea*>()->ensureWidgetVisible(closed);
        QVERIFY(screenshot(window, "ledger-deleted-list.png"));
        window.findChild<QPushButton*>("logoutButton")->click();
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

QTEST_MAIN(UiIdentityTests)
#include "ui_identity_tests.moc"
