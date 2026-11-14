#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
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
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/main_window.h"

namespace {
void answer(QMessageBox::StandardButton choice) {
    QTimer::singleShot(0, qApp, [choice] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            box->button(choice)->click();
        }
    });
}
bool screenshot(QWidget& widget, const QString& name) {
    const auto path = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
    if (path.isEmpty()) {
        return true;
    }
    widget.repaint();
    QCoreApplication::processEvents();
    QTest::qWait(80);
    return QDir().mkpath(path) && widget.grab().save(QDir(path).filePath(name));
}
}  // namespace
class UiMembershipTests final : public QObject {
    Q_OBJECT
private slots:
    void soleMemberExitDoesNotOpenLedgerClosure();
    void exitHistoryHandoverRejoinAndRevoke();
};
void UiMembershipTests::soleMemberExitDoesNotOpenLedgerClosure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient admin, api;
        admin.connectToService(directory.path(), false);
        api.connectToService(directory.path(), false);
        admin.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        admin.login("owner", "Password-42");
        const auto book = admin.call("create_ledger", {{"name", QStringLiteral("单人账本")}});
        const auto id = book.value("id");
        QFile css(":/resources/styles.qss");
        QVERIFY(css.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(css.readAll()));
        ledger::MainWindow window(api);
        window.resize(1480, 1050);
        window.show();
        window.findChild<QLineEdit*>("usernameInput")->setText("owner");
        window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
        window.findChild<QPushButton*>("authSubmit")->click();
        bool explained = false, captured = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* notice = qobject_cast<QMessageBox*>(dialog);
            explained = notice && notice->objectName() == "soleMemberExitNotice" &&
                        notice->text().contains(QStringLiteral("只剩您一位成员")) &&
                        !dialog->findChild<QPushButton*>("confirmCloseLedger");
            captured = screenshot(*dialog, "sole-member-exit-notice.png");
            dialog->reject();
        });
        window.findChild<QPushButton*>("exitLedgerButton")->click();
        QVERIFY(explained && captured);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        QCOMPARE(admin.call("ledger_details", {{"ledger_id", id}}).value("role").toString(),
                 QStringLiteral("admin"));
        QCOMPARE(admin.call("closed_ledgers").value("ledgers").toArray().size(), 0);
        bool closure = false;
        QTimer::singleShot(0, qApp, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                closure = dialog->objectName() == "closeLedgerDialog" &&
                          dialog->findChild<QPushButton*>("confirmCloseLedger");
                screenshot(*dialog, "sole-member-archive-dialog.png");
                dialog->reject();
            }
        });
        window.findChild<QPushButton*>("closeLedgerButton")->click();
        QVERIFY(closure);
        QCOMPARE(admin.call("list_ledgers").value("ledgers").toArray().size(), 1);
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}
void UiMembershipTests::exitHistoryHandoverRejoinAndRevoke() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient admin, member, api;
        for (auto* client : {&admin, &member, &api}) {
            client->connectToService(directory.path(), false);
        }
        admin.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        member.call("register", {{"username", "member"}, {"password", "Password-42"}});
        const auto a = admin.login("owner", "Password-42"),
                   b = member.login("member", "Password-42");
        const auto book = admin.call("create_ledger", {{"name", QStringLiteral("退出交接验收")}});
        const auto id = book.value("id");
        const auto join = [&](ledger::ApiClient& inviter, ledger::ApiClient& receiver,
                              const QString& name) {
            const auto invite =
                inviter.call("invite", {{"ledger_id", id}, {"username", name}, {"role", "member"}});
            receiver.call("accept_invitation", {{"invitation_id", invite.value("id")}});
        };
        join(admin, member, "member");
        auto form = QJsonObject{
            {"ledger_id", id},
            {"amount", "90"},
            {"date", QDate::currentDate().toString(Qt::ISODate)},
            {"payer_id", a.value("id")},
            {"description", QStringLiteral("两人办公采购")},
            {"split_method", "equal"},
            {"splits", QJsonArray{QJsonObject{{"user_id", a.value("id")}, {"value", ""}},
                                  QJsonObject{{"user_id", b.value("id")}, {"value", ""}}}}};
        const auto record = member.call("expense_submit", form).value("expense").toObject();
        admin.call("expense_review", {{"ledger_id", id},
                                      {"expense_id", record.value("id")},
                                      {"version", record.value("version")},
                                      {"decision", "approve"}});
        auto overview = admin.call("settlement_overview", {{"ledger_id", id}});
        admin.call("settlement_create",
                   {{"ledger_id", id}, {"preview_token", overview.value("preview_token")}});
        QFile css(":/resources/styles.qss");
        QVERIFY(css.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(css.readAll()));
        ledger::MainWindow window(api);
        window.resize(1480, 1050);
        window.show();
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        const auto login = [&](const QString& name) {
            window.findChild<QLineEdit*>("usernameInput")->setText(name);
            window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
            window.findChild<QPushButton*>("authSubmit")->click();
        };
        login("member");
        bool seen = false, captured = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            seen = dialog->objectName() == "membershipChangeDialog";
            dialog->findChild<QCheckBox*>("membershipSetDeadline")->setChecked(true);
            dialog->findChild<QDateEdit*>("membershipDeadline")
                ->setDate(QDate::currentDate().addDays(7));
            captured = screenshot(*dialog, "stage7-exit-debt.png");
            answer(QMessageBox::Yes);
            dialog->findChild<QPushButton*>("confirmMembershipChange")->click();
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        window.findChild<QPushButton*>("exitLedgerButton")->click();
        QVERIFY(seen && captured);
        QVERIFY(member.call("ledger_details", {{"ledger_id", id}}).value("historical").toBool());
        auto* navigation = window.findChild<QListWidget*>("navigation");
        QVERIFY(navigation->item(1)->isHidden());
        QVERIFY(navigation->item(2)->isHidden());
        QVERIFY(navigation->item(3)->isHidden());
        auto* query = window.findChild<QWidget*>("queryExpensePage");
        QCOMPARE(query->findChild<QTableWidget*>("expenseTable")->rowCount(), 1);
        QVERIFY(!window.findChild<QPushButton*>("exitLedgerButton")->isVisible());
        QVERIFY(screenshot(window, "stage7-historical-view.png"));
        form["amount"] = "12";
        form["description"] = "Backdated after exit";
        form["date"] = "2020-01-01";
        form["splits"] = QJsonArray{QJsonObject{{"user_id", a.value("id")}, {"value", ""}}};
        const auto late = admin.call("expense_submit", form).value("expense").toObject();
        window.findChild<QPushButton*>("refreshWorkspace")->click();
        QCOMPARE(query->findChild<QTableWidget*>("expenseTable")->rowCount(), 1);
        QVERIFY_THROWS_EXCEPTION(
            ledger::ServiceError,
            member.call("expense_detail", {{"ledger_id", id}, {"expense_id", late.value("id")}}));
        join(admin, member, "member");
        window.findChild<QPushButton*>("refreshWorkspace")->click();
        navigation->setCurrentRow(0);
        QVERIFY(window.findChild<QPushButton*>("exitLedgerButton")->isVisible());
        window.findChild<QPushButton*>("logoutButton")->click();
        login("owner");
        bool missing_successor = false, cancelled = false, transferred = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* save = dialog->findChild<QPushButton*>("confirmMembershipChange");
            save->click();
            missing_successor =
                !dialog->findChild<QLabel*>("membershipChangeError")->text().isEmpty();
            auto* successor = dialog->findChild<QComboBox*>("membershipSuccessor");
            successor->setCurrentIndex(successor->findData(b.value("id").toString()));
            answer(QMessageBox::No);
            save->click();
            cancelled = admin.call("ledger_details", {{"ledger_id", id}}).value("role") == "admin";
            screenshot(*dialog, "stage7-transfer-exit.png");
            answer(QMessageBox::Yes);
            save->click();
            transferred = dialog->result() == QDialog::Accepted;
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        window.findChild<QPushButton*>("exitLedgerButton")->click();
        QVERIFY(missing_successor && cancelled && transferred);
        QCOMPARE(member.call("ledger_details", {{"ledger_id", id}}).value("role").toString(),
                 QStringLiteral("admin"));
        QVERIFY(admin.call("ledger_details", {{"ledger_id", id}}).value("historical").toBool());
        join(member, admin, "owner");
        window.findChild<QPushButton*>("refreshWorkspace")->click();
        navigation->setCurrentRow(0);
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            dialog->findChild<QCheckBox*>("membershipKeepHistory")->setChecked(false);
            answer(QMessageBox::Yes);
            dialog->findChild<QPushButton*>("confirmMembershipChange")->click();
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        window.findChild<QPushButton*>("exitLedgerButton")->click();
        QCOMPARE(admin.call("list_ledgers").value("ledgers").toArray().size(), 0);
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 admin.call("expense_list", {{"ledger_id", id}}));
        QCOMPARE(window.findChild<QComboBox*>("ledgerSelector")->count(), 0);
        window.findChild<QPushButton*>("logoutButton")->click();
        login("member");
        auto* departed = window.findChild<QTableWidget*>("departedMembersTable");
        QCOMPARE(departed->rowCount(), 1);
        QCOMPARE(departed->item(0, 0)->text(), QStringLiteral("owner"));
        QCOMPARE(departed->item(0, 2)->text(), QStringLiteral("不保留"));
        QVERIFY(departed->showGrid());
        QCOMPARE(departed->item(0, 0)->textAlignment(), Qt::AlignCenter);
        QVERIFY(screenshot(window, "stage7-departed-members.png"));
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}
QTEST_MAIN(UiMembershipTests)
#include "ui_membership_tests.moc"
