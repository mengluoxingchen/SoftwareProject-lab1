#include <QApplication>
#include <QComboBox>
#include <QDateTime>
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
QString balance(QTableWidget* table, const QString& id) {
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toString() == id) {
            return table->item(row, 5)->text();
        }
    }
    return {};
}
bool screenshot(QWidget& window, const QString& filename) {
    const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
    if (output.isEmpty()) {
        return true;
    }
    window.repaint();
    for (auto* child : window.findChildren<QWidget*>()) {
        if (child->isVisible()) {
            child->repaint();
        }
    }
    QCoreApplication::processEvents();
    QTest::qWait(100);
    static_cast<void>(window.grab());
    QCoreApplication::processEvents();
    return QDir().mkpath(output) && window.grab().save(QDir(output).filePath(filename));
}
void answer(QMessageBox::StandardButton choice) {
    QTimer::singleShot(0, qApp, [choice] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            dialog->button(choice)->click();
        }
    });
}
}  // namespace

class UiSettlementTests final : public QObject {
    Q_OBJECT
private slots:
    void readOnlyMemberSeesFortyFivePartiallyRepaidAndCleared();
    void generatePartiallyRepayAndViewUnchangedHistory();
};

void UiSettlementTests::generatePartiallyRepayAndViewUnchangedHistory() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient owner, member, third;
        owner.connectToService(directory.path(), false);
        member.connectToService(directory.path(), false);
        third.connectToService(directory.path(), false);
        owner.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        member.call("register", {{"username", "member"}, {"password", "Password-42"}});
        third.call("register", {{"username", "third"}, {"password", "Password-42"}});
        const auto a = owner.login("owner", "Password-42");
        const auto b = member.login("member", "Password-42");
        const auto c = third.login("third", "Password-42");
        const auto book =
            owner.call("create_ledger", {{"name", QStringLiteral("三人结算验收账本")}});
        for (auto* client : {&member, &third}) {
            const QString username = client == &member ? "member" : "third";
            const auto invitation = owner.call(
                "invite",
                {{"ledger_id", book.value("id")}, {"username", username}, {"role", "member"}});
            client->call("accept_invitation", {{"invitation_id", invitation.value("id")}});
        }
        const auto record =
            member
                .call("expense_submit",
                      {{"ledger_id", book.value("id")},
                       {"amount", "90.00"},
                       {"date", "2026-11-09"},
                       {"payer_id", a.value("id")},
                       {"description", QStringLiteral("三人办公采购")},
                       {"split_method", "equal"},
                       {"splits",
                        QJsonArray{QJsonObject{{"user_id", a.value("id")}, {"value", ""}},
                                   QJsonObject{{"user_id", b.value("id")}, {"value", ""}},
                                   QJsonObject{{"user_id", c.value("id")}, {"value", ""}}}}})
                .value("expense")
                .toObject();
        owner.call("expense_review", {{"ledger_id", book.value("id")},
                                      {"expense_id", record.value("id")},
                                      {"version", record.value("version")},
                                      {"decision", "approve"}});
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        QFile stylesheet(":/resources/styles.qss");
        QVERIFY(stylesheet.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        ledger::MainWindow window(api);
        window.resize(1440, 1000);
        window.show();
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        const auto login = [&](const QString& username) {
            window.findChild<QLineEdit*>("usernameInput")->setText(username);
            window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
            window.findChild<QPushButton*>("authSubmit")->click();
        };
        login("owner");
        auto* navigation = window.findChild<QListWidget*>("navigation");
        navigation->setCurrentRow(3);
        auto* page = window.findChild<QWidget*>("settlementPage");
        QVERIFY(page);
        auto* generate = page->findChild<QPushButton*>("generateSettlement");
        auto* balances = page->findChild<QTableWidget*>("memberBalances");
        auto* history = page->findChild<QTableWidget*>("settlementHistory");
        auto* repayments = page->findChild<QTableWidget*>("repaymentHistory");
        auto* repay = page->findChild<QPushButton*>("recordRepayment");
        QVERIFY(generate && balances && history && repayments && repay);
        QVERIFY(generate->isEnabled());
        QVERIFY(!repay->isEnabled());
        answer(QMessageBox::No);
        generate->click();
        QCOMPARE(history->rowCount(), 0);
        answer(QMessageBox::Yes);
        generate->click();
        QCOMPARE(history->rowCount(), 1);
        QVERIFY(!generate->isEnabled());
        QVERIFY(repay->isEnabled());
        QCOMPARE(balance(balances, a.value("id").toString()), QStringLiteral("60.00"));
        QCOMPARE(balance(balances, b.value("id").toString()), QStringLiteral("-30.00"));
        QCOMPARE(balance(balances, c.value("id").toString()), QStringLiteral("-30.00"));
        QVERIFY(screenshot(window, "stage5-settlement.png"));
        bool dialog_checked = false, dialog_capture = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* payer = dialog->findChild<QComboBox*>("repaymentPayer");
            auto* recipient = dialog->findChild<QComboBox*>("repaymentRecipient");
            auto* amount = dialog->findChild<QLineEdit*>("repaymentAmount");
            auto* save = dialog->findChild<QPushButton*>("confirmRepayment");
            if (!payer || !recipient || !amount || !save) {
                dialog->reject();
                return;
            }
            payer->setCurrentIndex(payer->findData(b.value("id").toString()));
            recipient->setCurrentIndex(recipient->findData(a.value("id").toString()));
            amount->setText("abc");
            save->click();
            const bool invalid = !dialog->findChild<QLabel*>("repaymentError")->text().isEmpty();
            amount->clear();
            QTest::keyClicks(amount, "20.00");
            dialog_capture = screenshot(*dialog, "stage5-record-repayment.png");
            dialog_checked = invalid && amount->text() == "20.00";
            save->click();
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        repay->click();
        QVERIFY(dialog_checked && dialog_capture);
        QCOMPARE(repayments->rowCount(), 1);
        QCOMPARE(balance(balances, a.value("id").toString()), QStringLiteral("40.00"));
        QCOMPARE(balance(balances, b.value("id").toString()), QStringLiteral("-10.00"));
        QCOMPARE(page->findChild<QLabel*>("settledTotal")->text(), QStringLiteral("90.00 元"));
        QVERIFY(screenshot(window, "stage5-partial-repayment.png"));
        bool snapshot_checked = false, snapshot_capture = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            if (auto* table = dialog->findChild<QTableWidget*>("snapshotBalances")) {
                snapshot_checked = balance(table, a.value("id").toString()) == "60.00" &&
                                   balance(table, b.value("id").toString()) == "-30.00";
                snapshot_capture = screenshot(*dialog, "stage5-immutable-snapshot.png");
            }
            dialog->reject();
        });
        auto* snapshot_button =
            history->cellWidget(0, 4)->findChild<QPushButton*>("viewSettlementSnapshot");
        QVERIFY(snapshot_button);
        snapshot_button->click();
        QVERIFY(snapshot_checked && snapshot_capture);
        bool overpayment_cancelled = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            auto* payer = dialog->findChild<QComboBox*>("repaymentPayer");
            auto* recipient = dialog->findChild<QComboBox*>("repaymentRecipient");
            if (!payer || !recipient) {
                dialog->reject();
                return;
            }
            payer->setCurrentIndex(payer->findData(b.value("id").toString()));
            recipient->setCurrentIndex(recipient->findData(a.value("id").toString()));
            dialog->findChild<QLineEdit*>("repaymentAmount")->setText("15.00");
            answer(QMessageBox::No);
            dialog->findChild<QPushButton*>("confirmRepayment")->click();
            overpayment_cancelled = dialog->isVisible();
            dialog->reject();
        });
        repay->click();
        QVERIFY(overpayment_cancelled);
        QCOMPARE(repayments->rowCount(), 1);
        window.findChild<QPushButton*>("logoutButton")->click();
        login("member");
        navigation->setCurrentRow(3);
        bool own_payer_only = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            if (auto* payer = dialog->findChild<QComboBox*>("repaymentPayer")) {
                own_payer_only = payer->count() == 1 &&
                                 payer->currentData().toString() == b.value("id").toString();
            }
            dialog->reject();
        });
        repay->click();
        QVERIFY(own_payer_only);
        owner.call(
            "change_role",
            {{"ledger_id", book.value("id")}, {"user_id", b.value("id")}, {"role", "read_only"}});
        window.findChild<QPushButton*>("refreshWorkspace")->click();
        QVERIFY(!navigation->item(3)->isHidden());
        QVERIFY(!generate->isVisible());
        QVERIFY(!repay->isVisible());
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 api.call("settlement_create", {{"ledger_id", book.value("id")},
                                                                {"preview_token", "forged"}}));
        window.findChild<QPushButton*>("logoutButton")->click();
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void UiSettlementTests::readOnlyMemberSeesFortyFivePartiallyRepaidAndCleared() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient owner, member, api;
        for (auto* client : {&owner, &member, &api}) {
            client->connectToService(directory.path(), false);
        }
        owner.call("register", {{"username", "xmx"}, {"password", "Password-42"}});
        member.call("register", {{"username", QStringLiteral("甲")}, {"password", "Password-42"}});
        const auto a = owner.login("xmx", "Password-42");
        const auto b = member.login(QStringLiteral("甲"), "Password-42");
        const auto book = owner.call("create_ledger", {{"name", QStringLiteral("电费验收")}});
        const auto id = book.value("id");
        const auto invite = owner.call(
            "invite",
            {{"ledger_id", id}, {"username", QStringLiteral("甲")}, {"role", "read_only"}});
        member.call("accept_invitation", {{"invitation_id", invite.value("id")}});
        const auto expense =
            owner
                .call("expense_submit",
                      {{"ledger_id", id},
                       {"amount", "90.00"},
                       {"date", QDate::currentDate().toString(Qt::ISODate)},
                       {"payer_id", a.value("id")},
                       {"description", QStringLiteral("宿舍电费")},
                       {"split_method", "equal"},
                       {"splits",
                        QJsonArray{QJsonObject{{"user_id", a.value("id")}, {"value", ""}},
                                   QJsonObject{{"user_id", b.value("id")}, {"value", ""}}}}})
                .value("expense")
                .toObject();
        owner.call("expense_review", {{"ledger_id", id},
                                      {"expense_id", expense.value("id")},
                                      {"version", expense.value("version")},
                                      {"decision", "approve"}});
        QFile css(":/resources/styles.qss");
        QVERIFY(css.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(css.readAll()));
        ledger::MainWindow window(api);
        window.resize(1440, 1000);
        window.show();
        window.findChild<QLineEdit*>("usernameInput")->setText(QStringLiteral("甲"));
        window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
        window.findChild<QPushButton*>("authSubmit")->click();
        auto* navigation = window.findChild<QListWidget*>("navigation");
        QVERIFY(!navigation->item(3)->isHidden());
        navigation->setCurrentRow(4);
        auto* query = window.findChild<QWidget*>("queryExpensePage");
        auto* shortcut = query->findChild<QPushButton*>("openMySettlement");
        QVERIFY(shortcut && shortcut->isVisible());
        shortcut->click();
        QCOMPARE(navigation->currentRow(), 3);
        auto* page = window.findChild<QWidget*>("settlementPage");
        auto* status = page->findChild<QLabel*>("mySettlementStatus");
        auto* repaid_amount = page->findChild<QLabel*>("myRepaidAmount");
        auto* history = page->findChild<QTableWidget*>("myRepaymentHistory");
        auto* refresh = page->findChild<QPushButton*>("refreshSettlement");
        QVERIFY(status->text().contains(QStringLiteral("待生成结算")));
        QVERIFY(!page->findChild<QPushButton*>("generateSettlement")->isVisible());
        QVERIFY(!page->findChild<QPushButton*>("recordRepayment")->isVisible());
        auto overview = owner.call("settlement_overview", {{"ledger_id", id}});
        owner.call("settlement_create",
                   {{"ledger_id", id}, {"preview_token", overview.value("preview_token")}});
        refresh->click();
        QVERIFY(status->text().contains(QStringLiteral("我还需还款：45.00")));
        QVERIFY(!page->findChild<QTableWidget*>("mySettlementDirections")
                     ->findItems("xmx", Qt::MatchExactly)
                     .isEmpty());
        QVERIFY(screenshot(window, "my-repayment-unpaid-45.png"));
        const auto pay = [&](const QString& amount, const QString& key) {
            const auto current = owner.call("settlement_overview", {{"ledger_id", id}});
            owner.call("settlement_repay", {{"ledger_id", id},
                                            {"payer_id", b.value("id")},
                                            {"recipient_id", a.value("id")},
                                            {"amount", amount},
                                            {"paid_at", QDateTime::currentSecsSinceEpoch()},
                                            {"request_key", key},
                                            {"balance_token", current.value("balance_token")}});
            refresh->click();
        };
        const auto valid_token =
            owner.call("settlement_overview", {{"ledger_id", id}}).value("balance_token");
        bool forbidden = false;
        try {
            api.call("settlement_repay", {{"ledger_id", id},
                                          {"payer_id", b.value("id")},
                                          {"recipient_id", a.value("id")},
                                          {"amount", "45"},
                                          {"paid_at", QDateTime::currentSecsSinceEpoch()},
                                          {"request_key", "forbidden"},
                                          {"balance_token", valid_token}});
        } catch (const ledger::ServiceError& error) {
            forbidden = error.code() == "forbidden";
        }
        QVERIFY(forbidden);
        pay("20.00", "first");
        QVERIFY(status->text().contains(QStringLiteral("我还需还款：25.00")));
        QCOMPARE(repaid_amount->text(), QStringLiteral("20.00 元"));
        QCOMPARE(history->item(0, 1)->text(), QStringLiteral("还给 xmx"));
        QCOMPARE(history->item(0, 2)->text(), QStringLiteral("20.00"));
        QVERIFY(screenshot(window, "my-repayment-partial-25.png"));
        pay("25.00", "last");
        QVERIFY(status->text().contains(QStringLiteral("已结清")));
        QCOMPARE(repaid_amount->text(), QStringLiteral("45.00 元"));
        QCOMPARE(page->findChild<QTableWidget*>("repaymentHistory")->rowCount(), 2);
        QVERIFY(screenshot(window, "my-repayment-cleared.png"));
        navigation->setCurrentRow(4);
        query->findChild<QPushButton*>("refreshExpenses")->click();
        QCOMPARE(query->findChild<QTableWidget*>("expenseTable")->item(0, 6)->text(),
                 QStringLiteral("已纳入结算"));
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

QTEST_MAIN(UiSettlementTests)
#include "ui_settlement_tests.moc"
