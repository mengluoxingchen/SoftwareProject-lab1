#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "transport/api_client.h"
#include "ui/main_window.h"

class UiWorkflowTests final : public QObject {
    Q_OBJECT
private slots:
    void reviewReviseLockWithdrawAndDetectDuplicate();
};

void UiWorkflowTests::reviewReviseLockWithdrawAndDetectDuplicate() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient owner, member;
        owner.connectToService(directory.path(), false);
        member.connectToService(directory.path(), false);
        owner.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        member.call("register", {{"username", "member"}, {"password", "Password-42"}});
        const auto admin_user = owner.login("owner", "Password-42");
        const auto member_user = member.login("member", "Password-42");
        const auto book =
            owner.call("create_ledger", {{"name", QStringLiteral("审核流程验收账本")}});
        const QString ledger_id = book.value("id").toString();
        const auto invitation = owner.call(
            "invite", {{"ledger_id", ledger_id}, {"username", "member"}, {"role", "member"}});
        member.call("accept_invitation", {{"invitation_id", invitation.value("id")}});
        const auto form = [&](const QString& description) {
            return QJsonObject{
                {"ledger_id", ledger_id},
                {"amount", "90.00"},
                {"date", QDate::currentDate().toString(Qt::ISODate)},
                {"payer_id", admin_user.value("id")},
                {"description", description},
                {"split_method", "equal"},
                {"splits",
                 QJsonArray{QJsonObject{{"user_id", admin_user.value("id")}, {"value", ""}},
                            QJsonObject{{"user_id", member_user.value("id")}, {"value", ""}}}}};
        };
        auto record = member.call("expense_submit", form("办公采购")).value("expense").toObject();
        const QString id = record.value("id").toString();
        ledger::ApiClient admin_api, member_api;
        admin_api.connectToService(directory.path(), false);
        member_api.connectToService(directory.path(), false);
        QFile stylesheet(":/resources/styles.qss");
        QVERIFY(stylesheet.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        ledger::MainWindow admin_window(admin_api), member_window(member_api);
        admin_window.show();
        member_window.show();
        QCoreApplication::processEvents();
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        const auto login = [](ledger::MainWindow& window, const QString& username) {
            window.findChild<QLineEdit*>("usernameInput")->setText(username);
            window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
            window.findChild<QPushButton*>("authSubmit")->click();
        };
        login(admin_window, "owner");
        login(member_window, "member");
        QVERIFY(admin_api.isAuthenticated() && member_api.isAuthenticated());
        auto* admin_nav = admin_window.findChild<QListWidget*>("navigation");
        auto* member_nav = member_window.findChild<QListWidget*>("navigation");
        auto* queue = admin_window.findChild<QWidget*>("reviewExpensePage");
        auto* admin_manage = admin_window.findChild<QWidget*>("expensePage");
        auto* manage = member_window.findChild<QWidget*>("expensePage");
        auto* queue_table = queue->findChild<QTableWidget*>("expenseTable");
        auto* records = manage->findChild<QTableWidget*>("expenseTable");
        const auto screenshot = [&](QWidget* target, const QString& filename) {
            target->repaint();
            for (auto* child : target->findChildren<QWidget*>()) {
                if (child->isVisible()) {
                    child->repaint();
                }
            }
            QCoreApplication::processEvents();
            QTest::qWait(100);
            static_cast<void>(target->grab());
            const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
            return output.isEmpty() ||
                   (QDir().mkpath(output) && target->grab().save(QDir(output).filePath(filename)));
        };
        const auto rowFor = [](QTableWidget* table, const QString& expense_id) {
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->item(row, 0)->data(Qt::UserRole).toString() == expense_id) {
                    return row;
                }
            }
            return -1;
        };
        const auto buttonFor = [&](QTableWidget* table, const QString& expense_id,
                                   const char* name) -> QPushButton* {
            const int row = rowFor(table, expense_id);
            return row < 0 ? nullptr : table->cellWidget(row, 7)->findChild<QPushButton*>(name);
        };
        const auto current = [&] {
            return member.call("expense_detail", {{"ledger_id", ledger_id}, {"expense_id", id}});
        };
        const auto refresh = [&](QWidget* page) {
            page->findChild<QPushButton*>("refreshExpenses")->click();
        };
        bool modal_seen = false, modal_ok = false;
        const auto inDialog = [&](const std::function<bool(QDialog*)>& operation) {
            modal_seen = false;
            modal_ok = false;
            QTimer::singleShot(0, &admin_window, [&, operation] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog) {
                    modal_seen = true;
                    modal_ok = operation(dialog);
                    if (dialog->isVisible()) {
                        dialog->reject();
                    }
                }
            });
        };
        admin_nav->setCurrentRow(2);
        QCOMPARE(queue_table->rowCount(), 1);
        QVERIFY(screenshot(&admin_window, "stage4-review-queue.png"));
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("rejectExpense")->click();
            const bool required =
                dialog->findChild<QLabel*>("reviewError")->text().contains("填写原因");
            dialog->findChild<QLineEdit*>("reviewReason")->setText("发票信息需要补充");
            const bool captured = screenshot(dialog, "stage4-review-reject.png");
            dialog->findChild<QPushButton*>("rejectExpense")->click();
            return required && captured;
        });
        QVERIFY(buttonFor(queue_table, id, "viewExpense"));
        buttonFor(queue_table, id, "viewExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        QCOMPARE(current().value("status").toString(), QStringLiteral("rejected"));
        QCOMPARE(queue_table->rowCount(), 0);
        member_nav->setCurrentRow(1);
        refresh(manage);
        auto* notices = manage->findChild<QTableWidget*>("expenseNotices");
        QCOMPARE(notices->rowCount(), 1);
        QVERIFY(notices->item(0, 1)->text().contains("发票信息需要补充"));
        QVERIFY(screenshot(&member_window, "stage4-rejection-notice.png"));
        QVERIFY(buttonFor(records, id, "editExpense"));
        buttonFor(records, id, "editExpense")->click();
        manage->findChild<QLineEdit*>("expenseAmount")->setText("120.00");
        manage->findChild<QPushButton*>("submitExpense")->click();
        QVERIFY(manage->findChild<QLabel*>("expenseNotice")->text().contains("修改原因"));
        manage->findChild<QLineEdit*>("expenseChangeReason")->setText("补充发票并更正金额");
        admin_nav->setCurrentRow(1);
        refresh(admin_manage);
        auto* admin_records = admin_manage->findChild<QTableWidget*>("expenseTable");
        QVERIFY(buttonFor(admin_records, id, "editExpense"));
        buttonFor(admin_records, id, "editExpense")->click();
        QVERIFY(admin_manage->findChild<QLabel*>("expenseNotice")->text().contains("正在被修改"));
        manage->findChild<QPushButton*>("submitExpense")->click();
        QCOMPARE(current().value("status").toString(), QStringLiteral("pending"));
        QVERIFY(current().value("corrected").toBool());
        buttonFor(records, id, "editExpense")->click();
        admin_nav->setCurrentRow(2);
        refresh(queue);
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("approveExpense")->click();
            const bool blocked =
                dialog->findChild<QLabel*>("reviewError")->text().contains("正在被修改");
            return blocked && screenshot(dialog, "stage4-locked-review.png");
        });
        buttonFor(queue_table, id, "viewExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        manage->findChild<QPushButton*>("backToExpenseList")->click();
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("approveExpense")->click();
            return true;
        });
        buttonFor(queue_table, id, "viewExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        QCOMPARE(current().value("status").toString(), QStringLiteral("approved"));
        refresh(manage);
        buttonFor(records, id, "editExpense")->click();
        manage->findChild<QLineEdit*>("expenseAmount")->setText("150.00");
        manage->findChild<QLineEdit*>("expenseChangeReason")->setText("已审核账目补正收据金额");
        QVERIFY(screenshot(&member_window, "stage4-edit-approved.png"));
        manage->findChild<QPushButton*>("submitExpense")->click();
        QCOMPARE(current().value("status").toString(), QStringLiteral("pending"));
        refresh(queue);
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("approveExpense")->click();
            return true;
        });
        buttonFor(queue_table, id, "viewExpense")->click();
        QCOMPARE(current().value("status").toString(), QStringLiteral("approved"));
        refresh(manage);
        inDialog([&](QDialog* dialog) {
            auto* revisions = dialog->findChild<QTableWidget*>("expenseRevisionHistory");
            const bool two = revisions && revisions->rowCount() == 2;
            auto* text = dialog->findChild<QPlainTextEdit*>("expenseRevisionDetails");
            const bool preserved =
                text->toPlainText().contains("120.00") && text->toPlainText().contains("150.00");
            dialog->findChild<QScrollArea*>()->ensureWidgetVisible(revisions);
            return two && preserved && screenshot(dialog, "stage4-revision-history.png");
        });
        buttonFor(records, id, "viewExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("confirmWithdrawExpense")->click();
            const bool required =
                dialog->findChild<QLabel*>("withdrawError")->text().contains("原因");
            dialog->findChild<QLineEdit*>("withdrawReason")->setText("误录，保留证据后撤回");
            const bool captured = screenshot(dialog, "stage4-withdraw-confirm.png");
            dialog->findChild<QPushButton*>("confirmWithdrawExpense")->click();
            return required && captured;
        });
        buttonFor(records, id, "withdrawExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        QCOMPARE(current().value("status").toString(), QStringLiteral("withdrawn"));
        QCOMPARE(records->rowCount(), 0);
        manage->findChild<QComboBox*>("expenseStatusFilter")
            ->setCurrentIndex(
                manage->findChild<QComboBox*>("expenseStatusFilter")->findData("withdrawn"));
        QCOMPARE(records->rowCount(), 1);
        const auto first =
            member.call("expense_submit", form("重复电费")).value("expense").toObject();
        auto duplicate = form("重复电费");
        duplicate["confirm_similar"] = true;
        const auto second = owner.call("expense_submit", duplicate).value("expense").toObject();
        refresh(queue);
        QCOMPARE(queue_table->rowCount(), 2);
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QPushButton*>("approveExpense")->click();
            const bool required =
                dialog->findChild<QLabel*>("reviewError")->text().contains("核对");
            dialog->findChild<QCheckBox*>("reviewSimilarConfirmed")->setChecked(true);
            dialog->findChild<QPushButton*>("approveExpense")->click();
            return required;
        });
        buttonFor(queue_table, first.value("id").toString(), "viewExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        inDialog([&](QDialog* dialog) {
            dialog->findChild<QCheckBox*>("reviewSimilarConfirmed")->setChecked(true);
            dialog->findChild<QPushButton*>("approveExpense")->click();
            const bool blocked =
                dialog->findChild<QLabel*>("reviewError")->text().contains("不能重复通过");
            const bool captured = screenshot(dialog, "stage4-duplicate-review.png");
            dialog->findChild<QLineEdit*>("reviewReason")->setText("与已通过账目为同一笔支出");
            dialog->findChild<QPushButton*>("rejectExpense")->click();
            return blocked && captured;
        });
        buttonFor(queue_table, second.value("id").toString(), "viewExpense")->click();
        QVERIFY(modal_seen && modal_ok);
        QCOMPARE(queue_table->rowCount(), 0);
        admin_window.findChild<QPushButton*>("logoutButton")->click();
        member_window.findChild<QPushButton*>("logoutButton")->click();
        admin_window.close();
        member_window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}
QTEST_MAIN(UiWorkflowTests)
#include "ui_workflow_tests.moc"
