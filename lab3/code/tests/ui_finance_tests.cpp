#include <QApplication>
#include <QDate>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "transport/api_client.h"
#include "ui/expense_details_dialog.h"
#include "ui/expense_page.h"
#include "ui/settlement_page.h"

namespace {
void answer(QMessageBox::StandardButton choice) {
    QTimer::singleShot(0, qApp, [choice] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            dialog->button(choice)->click();
        }
    });
}
bool screenshot(QWidget& window, const QString& name) {
    const auto output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
    if (output.isEmpty()) {
        return true;
    }
    window.repaint();
    QCoreApplication::processEvents();
    QTest::qWait(80);
    return QDir().mkpath(output) && window.grab().save(QDir(output).filePath(name));
}
QString balance(const QJsonObject& result, const QJsonValue& id) {
    for (const auto& value : result.value("members").toArray()) {
        const auto row = value.toObject();
        if (row.value("user_id") == id) {
            return row.value("balance").toString();
        }
    }
    return {};
}
}  // namespace

class UiFinanceTests final : public QObject {
    Q_OBJECT
private slots:
    void refundCorrectSupplementAndInspectHistory();
};

void UiFinanceTests::refundCorrectSupplementAndInspectHistory() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient admin, member, third;
        for (auto* client : {&admin, &member, &third}) {
            client->connectToService(directory.path(), false);
        }
        admin.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        member.call("register", {{"username", "member"}, {"password", "Password-42"}});
        third.call("register", {{"username", "third"}, {"password", "Password-42"}});
        const auto a = admin.login("owner", "Password-42"),
                   b = member.login("member", "Password-42"),
                   c = third.login("third", "Password-42");
        const auto created =
            admin.call("create_ledger", {{"name", QStringLiteral("退款与补记验收")}});
        const auto id = created.value("id");
        for (auto* client : {&member, &third}) {
            const auto invitation =
                admin.call("invite", {{"ledger_id", id},
                                      {"username", client == &member ? "member" : "third"},
                                      {"role", "member"}});
            client->call("accept_invitation", {{"invitation_id", invitation.value("id")}});
        }
        QJsonObject form{
            {"ledger_id", id},
            {"amount", "90"},
            {"date", QDate::currentDate().toString(Qt::ISODate)},
            {"payer_id", a.value("id")},
            {"description", QStringLiteral("办公用品采购")},
            {"split_method", "equal"},
            {"splits", QJsonArray{QJsonObject{{"user_id", a.value("id")}, {"value", ""}},
                                  QJsonObject{{"user_id", b.value("id")}, {"value", ""}},
                                  QJsonObject{{"user_id", c.value("id")}, {"value", ""}}}}};
        const auto record = member.call("expense_submit", form).value("expense").toObject();
        admin.call("expense_review", {{"ledger_id", id},
                                      {"expense_id", record.value("id")},
                                      {"version", record.value("version")},
                                      {"decision", "approve"}});
        auto overview = admin.call("settlement_overview", {{"ledger_id", id}});
        const auto snapshot =
            admin.call("settlement_create",
                       {{"ledger_id", id}, {"preview_token", overview.value("preview_token")}});
        const auto book = admin.call("ledger_details", {{"ledger_id", id}});
        QFile css(":/resources/styles.qss");
        QVERIFY(css.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(css.readAll()));
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        ledger::ExpenseDetailsDialog details(admin, book, record.value("id").toString(), false);
        details.show();
        QTest::qWait(80);
        bool refund_seen = false, invalid_seen = false, refund_screenshot = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != "refundDialog") {
                if (dialog) {
                    dialog->reject();
                }
                return;
            }
            refund_seen = true;
            auto* amount = dialog->findChild<QLineEdit*>("refundAmount");
            auto* reason = dialog->findChild<QLineEdit*>("refundReason");
            auto* save = dialog->findChild<QPushButton*>("confirmRefund");
            amount->setText("100");
            reason->setText(QStringLiteral("退还部分未使用用品"));
            save->click();
            invalid_seen = !dialog->findChild<QLabel*>("refundError")->text().isEmpty() &&
                           amount->text() == "100";
            amount->setText("30");
            refund_screenshot = screenshot(*dialog, "stage6-refund.png");
            save->click();
            if (dialog->isVisible()) {
                dialog->reject();
            }
        });
        details.findChild<QPushButton*>("recordExpenseRefund")->click();
        QVERIFY(refund_seen && invalid_seen && refund_screenshot);
        QCOMPARE(details.findChild<QTableWidget*>("expenseFinanceHistory")->rowCount(), 1);
        QCOMPARE(details.findChild<QTableWidget*>("expenseDetailShares")->item(0, 1)->text(),
                 QStringLiteral("30.00"));
        QCOMPARE(details.findChild<QTableWidget*>("expenseNetShares")->item(0, 1)->text(),
                 QStringLiteral("20.00"));
        overview = admin.call("settlement_overview", {{"ledger_id", id}});
        QCOMPARE(balance(overview, a.value("id")), QStringLiteral("40.00"));
        details.close();

        ledger::ExpensePage page(admin, false);
        page.resize(1320, 1000);
        page.setContext(book, a);
        page.show();
        auto* records = page.findChild<QTableWidget*>("expenseTable");
        QVERIFY(records);
        auto* correct = records->cellWidget(0, 7)->findChild<QPushButton*>("correctSettledExpense");
        QVERIFY(correct);
        correct->click();
        auto* amount = page.findChild<QLineEdit*>("expenseAmount");
        auto* reason = page.findChild<QLineEdit*>("expenseChangeReason");
        amount->setText("120");
        reason->setText(QStringLiteral("核对发票，原金额少记30元"));
        QVERIFY(screenshot(page, "stage6-correction-form.png"));
        answer(QMessageBox::No);
        page.findChild<QPushButton*>("submitExpense")->click();
        QCOMPARE(admin.call("expense_list", {{"ledger_id", id}}).value("expenses").toArray().size(),
                 1);
        answer(QMessageBox::Yes);
        page.findChild<QPushButton*>("submitExpense")->click();
        QCOMPARE(records->rowCount(), 2);
        overview = admin.call("settlement_overview", {{"ledger_id", id}});
        QCOMPARE(overview.value("total_expenses").toString(), QStringLiteral("90.00"));
        QCOMPARE(balance(overview, a.value("id")), QStringLiteral("60.00"));
        page.close();

        ledger::ExpensePage member_page(member, false);
        member_page.resize(1320, 1000);
        member_page.setContext(member.call("ledger_details", {{"ledger_id", id}}), b);
        member_page.show();
        auto* supplement = member_page.findChild<QPushButton*>("supplementExpenseButton");
        QVERIFY(supplement && supplement->isEnabled());
        supplement->click();
        member_page.findChild<QLineEdit*>("expenseAmount")->setText("60");
        member_page.findChild<QLineEdit*>("expenseDescription")
            ->setText(QStringLiteral("补记资料打印费"));
        member_page.findChild<QLineEdit*>("expenseChangeReason")
            ->setText(QStringLiteral("结算时遗漏发票"));
        answer(QMessageBox::Yes);
        member_page.findChild<QPushButton*>("submitExpense")->click();
        overview = admin.call("settlement_overview", {{"ledger_id", id}});
        QCOMPARE(overview.value("total_expenses").toString(), QStringLiteral("90.00"));
        const auto queue =
            admin.call("expense_review_queue", {{"ledger_id", id}}).value("expenses").toArray();
        QCOMPARE(queue.size(), 1);
        member_page.close();
        ledger::ExpenseDetailsDialog review(admin, book, queue[0].toObject().value("id").toString(),
                                            true);
        review.show();
        review.findChild<QLineEdit*>("reviewReason")->setText(QStringLiteral("已核对补记凭证"));
        review.findChild<QPushButton*>("approveExpense")->click();
        overview = admin.call("settlement_overview", {{"ledger_id", id}});
        QCOMPARE(overview.value("total_expenses").toString(), QStringLiteral("150.00"));
        QCOMPARE(balance(overview, a.value("id")), QStringLiteral("40.00"));
        // Correction net90 owed30 each; supplement60 paid B owed20 each => A40 B10 C-50.
        QCOMPARE(balance(overview, b.value("id")), QStringLiteral("10.00"));
        const auto old = admin.call("settlement_detail",
                                    {{"ledger_id", id}, {"settlement_id", snapshot.value("id")}});
        QCOMPARE(balance(old, a.value("id")), QStringLiteral("60.00"));
        QCOMPARE(balance(old, b.value("id")), QStringLiteral("-30.00"));

        ledger::SettlementPage settlements(admin);
        settlements.resize(1320, 1050);
        settlements.setContext(book, a);
        settlements.show();
        QCOMPARE(settlements.findChild<QLabel*>("refundTotal")->text(), QStringLiteral("30.00 元"));
        auto* finance = settlements.findChild<QTableWidget*>("ledgerFinanceHistory");
        QCOMPARE(finance->rowCount(), 3);
        for (auto* table : settlements.findChildren<QTableWidget*>()) {
            QVERIFY(table->showGrid());
            for (int row = 0; row < table->rowCount(); ++row) {
                for (int column = 0; column < table->columnCount(); ++column) {
                    if (const auto* item = table->item(row, column)) {
                        QCOMPARE(item->textAlignment(), Qt::AlignCenter);
                    }
                }
            }
        }
        QVERIFY(screenshot(settlements, "stage6-current-balances.png"));
        bool event_seen = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                event_seen =
                    dialog->objectName() == "financeEventDialog" &&
                    dialog->findChild<QTableWidget*>("financeEventShares")->rowCount() == 3;
                screenshot(*dialog, "stage6-finance-history.png");
                dialog->reject();
            }
        });
        emit finance->cellDoubleClicked(0, 0);
        QVERIFY(event_seen);
        settlements.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}
QTEST_MAIN(UiFinanceTests)
#include "ui_finance_tests.moc"
