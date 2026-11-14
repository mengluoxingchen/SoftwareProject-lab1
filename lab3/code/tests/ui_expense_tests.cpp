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
#include <QScrollArea>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "domain/split.h"
#include "recovery/recovery_store.h"
#include "transport/api_client.h"
#include "ui/main_window.h"

class UiExpenseTests final : public QObject {
    Q_OBJECT
private slots:
    void draftSplitSubmitDuplicateAndReadOnly();
    void refreshMembersPreservesDraftAndEnablesSelectedInputs();
};

void UiExpenseTests::draftSplitSubmitDuplicateAndReadOnly() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient administrator;
        ledger::ApiClient member;
        administrator.connectToService(directory.path(), false);
        member.connectToService(directory.path(), false);
        administrator.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        member.call("register", {{"username", "member"}, {"password", "Password-42"}});
        const auto owner = administrator.login("owner", "Password-42");
        const auto peer = member.login("member", "Password-42");
        const auto book =
            administrator.call("create_ledger", {{"name", QStringLiteral("团队共享账本")}});
        const QString book_id = book.value("id").toString();
        const auto invitation = administrator.call(
            "invite", {{"ledger_id", book_id}, {"username", "member"}, {"role", "member"}});
        member.call("accept_invitation", {{"invitation_id", invitation.value("id")}});
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        QFile stylesheet(QStringLiteral(":/resources/styles.qss"));
        QVERIFY(stylesheet.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        ledger::MainWindow window(api);
        window.show();
        QCoreApplication::processEvents();
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
        const auto screenshot = [&](const QString& filename) {
            // Hidden Windows test processes need a paint pass before capture.
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
            const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
            return output.isEmpty() ||
                   (QDir().mkpath(output) && window.grab().save(QDir(output).filePath(filename)));
        };
        const auto login = [&](const QString& username) {
            window.findChild<QLineEdit*>("usernameInput")->setText(username);
            window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
            window.findChild<QPushButton*>("authSubmit")->click();
        };
        login("owner");
        QVERIFY(api.isAuthenticated());
        auto* navigation = window.findChild<QListWidget*>("navigation");
        navigation->setCurrentRow(1);
        auto* page = window.findChild<QWidget*>("expensePage");
        QVERIFY(page);
        auto* amount = page->findChild<QLineEdit*>("expenseAmount");
        auto* description = page->findChild<QLineEdit*>("expenseDescription");
        auto* method = page->findChild<QComboBox*>("expenseSplitMethod");
        auto* splits = page->findChild<QTableWidget*>("expenseSplits");
        auto* records = page->findChild<QTableWidget*>("expenseTable");
        auto* notice = page->findChild<QLabel*>("expenseNotice");
        auto* submit = page->findChild<QPushButton*>("submitExpense");
        auto* create = page->findChild<QPushButton*>("newExpenseButton");
        auto* back = page->findChild<QPushButton*>("backToExpenseList");
        QVERIFY(amount && description && method && splits && records && notice && submit &&
                create && back);
        create->click();
        QCOMPARE(splits->rowCount(), 2);
        auto* payer = page->findChild<QComboBox*>("expensePayer");
        auto* preview = page->findChild<QLabel*>("expensePreview");
        QVERIFY(payer && preview);
        const int owner_row =
            splits->item(0, 0)->data(Qt::UserRole).toString() == owner.value("id").toString() ? 0
                                                                                              : 1;
        const int other_row = 1 - owner_row;
        amount->setText("90.00");
        QCOMPARE(splits->item(owner_row, 3)->text(), QStringLiteral("45.00"));
        QCOMPARE(splits->item(owner_row, 4)->text(), QStringLiteral("90.00"));
        QCOMPARE(splits->item(owner_row, 5)->text(), QStringLiteral("应收 45.00"));
        QCOMPARE(splits->item(other_row, 5)->text(), QStringLiteral("应还 45.00"));
        QVERIFY(preview->text().contains(QStringLiteral("member需还给owner 45.00元")));
        QVERIFY(screenshot("expense-ninety-shared.png"));
        splits->item(owner_row, 0)->setCheckState(Qt::Unchecked);
        QCOMPARE(splits->item(owner_row, 3)->text(), QStringLiteral("0.00"));
        QCOMPARE(splits->item(owner_row, 5)->text(), QStringLiteral("应收 90.00"));
        QCOMPARE(splits->item(other_row, 3)->text(), QStringLiteral("90.00"));
        QVERIFY(preview->text().contains(QStringLiteral("当前仅1人承担")));
        QVERIFY(screenshot("expense-ninety-other-only.png"));
        payer->setCurrentIndex(payer->findData(peer.value("id").toString()));
        QCOMPARE(splits->item(other_row, 4)->text(), QStringLiteral("90.00"));
        QCOMPARE(splits->item(other_row, 5)->text(), QStringLiteral("无需收还"));
        payer->setCurrentIndex(payer->findData(owner.value("id").toString()));
        splits->item(owner_row, 0)->setCheckState(Qt::Checked);
        amount->setText("100.00");
        submit->click();
        QVERIFY(notice->text().contains(QStringLiteral("消费说明")));
        QVERIFY(description->property("invalid").toBool());
        QCOMPARE(amount->text(), QStringLiteral("100.00"));
        description->setText(QStringLiteral("办公用品"));
        method->setCurrentIndex(method->findData("fixed"));
        const auto setPart = [&](int row, const QString& value) {
            qobject_cast<QLineEdit*>(splits->cellWidget(row, 2))->setText(value);
        };
        setPart(0, "30.00");
        setPart(1, "60.00");
        submit->click();
        QVERIFY(notice->text().contains(QStringLiteral("合计")));
        QVERIFY(splits->property("invalid").toBool());
        QCOMPARE(records->rowCount(), 0);
        setPart(1, "70.00");
        QCOMPARE(splits->item(0, 3)->text(), QStringLiteral("30.00"));
        QCOMPARE(splits->item(1, 3)->text(), QStringLiteral("70.00"));
        page->findChild<QPushButton*>("saveExpenseDraft")->click();
        QVERIFY(notice->text().contains(QStringLiteral("草稿已保存")));
        QVERIFY(screenshot("stage3-expense-form.png"));
        back->click();
        QCOMPARE(records->rowCount(), 1);
        QCOMPARE(records->item(0, 6)->text(), QStringLiteral("草稿"));
        auto* edit = records->cellWidget(0, 7)->findChild<QPushButton*>("editExpenseDraft");
        QVERIFY(edit);
        edit->click();
        QCOMPARE(amount->text(), QStringLiteral("100.00"));
        QCOMPARE(method->currentData().toString(), QStringLiteral("fixed"));
        QCOMPARE(qobject_cast<QLineEdit*>(splits->cellWidget(1, 2))->text(),
                 QStringLiteral("70.00"));
        submit->click();
        QCOMPARE(records->rowCount(), 1);
        QCOMPARE(records->item(0, 6)->text(), QStringLiteral("待审核"));
        QVERIFY(!records->cellWidget(0, 7)->findChild<QPushButton*>("editExpenseDraft"));

        // Exact repeats from the same creator are blocked without clearing the form.
        create->click();
        amount->setText("100.0");
        description->setText(QStringLiteral("办公用品"));
        submit->click();
        QVERIFY(notice->text().contains(QStringLiteral("请勿重复提交")));
        QCOMPARE(amount->text(), QStringLiteral("100.0"));
        QCOMPARE(records->rowCount(), 1);
        // Reuse the retained form for a distinct ratio expense.
        amount->setText("90.00");
        description->setText(QStringLiteral("团队餐费"));
        method->setCurrentIndex(method->findData("ratio"));
        setPart(0, "1");
        setPart(1, "2");
        QCOMPARE(splits->item(0, 3)->text(), QStringLiteral("30.00"));
        QCOMPARE(splits->item(1, 3)->text(), QStringLiteral("60.00"));
        QVERIFY(screenshot("stage3-expense-ratio.png"));
        submit->click();
        QCOMPARE(records->rowCount(), 2);
        create->click();
        amount->setText("100.01");
        description->setText(QStringLiteral("交通代付"));
        QCOMPARE(ledger::parseMoney(splits->item(0, 3)->text()) +
                     ledger::parseMoney(splits->item(1, 3)->text()),
                 10001);
        submit->click();
        QCOMPARE(records->rowCount(), 3);

        const QJsonArray participants{QJsonObject{{"user_id", owner.value("id")}, {"value", ""}},
                                      QJsonObject{{"user_id", peer.value("id")}, {"value", ""}}};
        const QJsonObject similar{{"ledger_id", book_id},
                                  {"amount", "30.00"},
                                  {"date", QDate::currentDate().toString(Qt::ISODate)},
                                  {"payer_id", owner.value("id")},
                                  {"description", QStringLiteral("共同代付")},
                                  {"split_method", "equal"},
                                  {"splits", participants}};
        QVERIFY(!member.call("expense_submit", similar).value("confirmation_required").toBool());
        create->click();
        amount->setText("30.00");
        description->setText(QStringLiteral("共同代付"));
        bool dialog_seen = false;
        const auto answerNextDialog = [&](QMessageBox::StandardButton answer) {
            QTimer::singleShot(0, &window, [&, answer] {
                if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    dialog_seen = true;
                    dialog->button(answer)->click();
                }
            });
        };
        answerNextDialog(QMessageBox::No);
        submit->click();
        QVERIFY(dialog_seen);
        QVERIFY(notice->text().contains(QStringLiteral("已取消提交")));
        QCOMPARE(administrator.call("expense_list", {{"ledger_id", book_id}})
                     .value("expenses")
                     .toArray()
                     .size(),
                 4);
        QCOMPARE(description->text(), QStringLiteral("共同代付"));
        dialog_seen = false;
        answerNextDialog(QMessageBox::Yes);
        submit->click();
        QVERIFY(dialog_seen);
        QCOMPARE(records->rowCount(), 5);
        QVERIFY(screenshot("stage3-expense-list.png"));
        bool detail_seen = false;
        bool detail_valid = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto* detail = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                detail_seen = true;
                auto* shares = detail->findChild<QTableWidget*>();
                detail_valid = shares && shares->rowCount() == 2;
                const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
                if (!output.isEmpty()) {
                    detail_valid =
                        detail_valid &&
                        detail->grab().save(QDir(output).filePath("stage3-expense-detail.png"));
                }
                detail->reject();
            }
        });
        records->cellWidget(0, 7)->findChild<QPushButton*>("viewExpense")->click();
        QVERIFY(detail_seen && detail_valid);

        // Canceling navigation preserves input; choosing Save creates a resumable draft.
        create->click();
        description->setText(QStringLiteral("稍后补充金额"));
        answerNextDialog(QMessageBox::Cancel);
        back->click();
        QVERIFY(description->isVisible());
        QCOMPARE(description->text(), QStringLiteral("稍后补充金额"));
        answerNextDialog(QMessageBox::Save);
        back->click();
        QCOMPARE(records->rowCount(), 6);
        // One member may pay while another member bears the entire expense.
        create->click();
        amount->setText("90.00");
        description->setText(QStringLiteral("替member购买用品"));
        for (int row = 0; row < splits->rowCount(); ++row) {
            if (splits->item(row, 0)->data(Qt::UserRole).toString() ==
                owner.value("id").toString()) {
                splits->item(row, 0)->setCheckState(Qt::Unchecked);
            }
        }
        for (int row = 0; row < splits->rowCount(); ++row) {
            if (splits->item(row, 0)->checkState() == Qt::Checked) {
                QCOMPARE(splits->item(row, 3)->text(), QStringLiteral("90.00"));
            }
        }
        QVERIFY(screenshot("stage3-payer-excluded.png"));
        submit->click();
        QCOMPARE(records->rowCount(), 7);
        administrator.call(
            "change_role",
            {{"ledger_id", book_id}, {"user_id", peer.value("id")}, {"role", "read_only"}});
        window.findChild<QPushButton*>("logoutButton")->click();
        login("member");
        QVERIFY(api.isAuthenticated());
        QVERIFY(navigation->item(1)->isHidden());
        QVERIFY(!navigation->item(4)->isHidden());
        navigation->setCurrentRow(4);
        auto* query = window.findChild<QWidget*>("queryExpensePage");
        QCOMPARE(query->findChild<QTableWidget*>("expenseTable")->rowCount(), 6);
        const auto* readonly_create = query->findChild<QPushButton*>("newExpenseButton");
        QVERIFY(!readonly_create || !readonly_create->isVisible());
        QVERIFY(screenshot("stage3-expense-readonly.png"));
        window.findChild<QPushButton*>("logoutButton")->click();
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void UiExpenseTests::refreshMembersPreservesDraftAndEnablesSelectedInputs() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient administrator;
        ledger::ApiClient peer;
        ledger::ApiClient newcomer;
        for (auto* client : {&administrator, &peer, &newcomer}) {
            client->connectToService(directory.path(), false);
        }
        administrator.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        peer.call("register", {{"username", "member"}, {"password", "Password-42"}});
        newcomer.call("register", {{"username", "newcomer"}, {"password", "Password-42"}});
        administrator.login("owner", "Password-42");
        peer.login("member", "Password-42");
        newcomer.login("newcomer", "Password-42");
        const auto book =
            administrator.call("create_ledger", {{"name", QStringLiteral("分摊交互验收")}});
        const auto invite = [&](const QString& username) {
            return administrator.call(
                "invite",
                {{"ledger_id", book.value("id")}, {"username", username}, {"role", "member"}});
        };
        peer.call("accept_invitation", {{"invitation_id", invite("member").value("id")}});
        const auto pending = invite("newcomer");
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        ledger::MainWindow window(api);
        window.show();
        window.findChild<QLineEdit*>("usernameInput")->setText("owner");
        window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
        window.findChild<QPushButton*>("authSubmit")->click();
        auto* navigation = window.findChild<QListWidget*>("navigation");
        navigation->setCurrentRow(1);
        auto* page = window.findChild<QWidget*>("expensePage");
        page->findChild<QPushButton*>("newExpenseButton")->click();
        auto* amount = page->findChild<QLineEdit*>("expenseAmount");
        auto* description = page->findChild<QLineEdit*>("expenseDescription");
        auto* payer = page->findChild<QComboBox*>("expensePayer");
        auto* method = page->findChild<QComboBox*>("expenseSplitMethod");
        auto* splits = page->findChild<QTableWidget*>("expenseSplits");
        auto* refresh = page->findChild<QPushButton*>("refreshExpenseMembers");
        auto* save = page->findChild<QPushButton*>("saveExpenseDraft");
        auto* notice = page->findChild<QLabel*>("expenseNotice");
        const auto input = [&](int row) {
            return qobject_cast<QLineEdit*>(splits->cellWidget(row, 2));
        };
        QCOMPARE(splits->rowCount(), 2);
        amount->setText("100.00");
        description->setText(QStringLiteral("保留分摊与草稿编辑状态"));
        const auto payer_id = payer->currentData();
        QVERIFY(!input(0)->isEnabled());
        method->setCurrentIndex(method->findData("ratio"));
        QVERIFY(input(0)->isEnabled());
        input(0)->setFocus();
        input(0)->selectAll();
        QTest::keyClicks(input(0), "2");
        QCOMPARE(input(0)->text(), QStringLiteral("2"));
        method->setCurrentIndex(method->findData("fixed"));
        input(0)->setText("30.00");
        input(1)->setText("70.00");
        save->click();
        QVERIFY(notice->text().contains(QStringLiteral("草稿已保存")));
        // Invitations do not become selectable members before acceptance.
        refresh->click();
        QCOMPARE(splits->rowCount(), 2);
        newcomer.call("accept_invitation", {{"invitation_id", pending.value("id")}});
        refresh->click();
        QCOMPARE(splits->rowCount(), 3);
        QCOMPARE(amount->text(), QStringLiteral("100.00"));
        QCOMPARE(description->text(), QStringLiteral("保留分摊与草稿编辑状态"));
        QCOMPARE(payer->currentData(), payer_id);
        QCOMPARE(method->currentData().toString(), QStringLiteral("fixed"));
        QCOMPARE(input(0)->text(), QStringLiteral("30.00"));
        QCOMPARE(input(1)->text(), QStringLiteral("70.00"));
        QCOMPARE(splits->item(2, 0)->checkState(), Qt::Unchecked);
        QVERIFY(!input(2)->isEnabled());
        window.findChild<QPushButton*>("refreshWorkspace")->click();
        QCOMPARE(splits->rowCount(), 3);
        QCOMPARE(input(1)->text(), QStringLiteral("70.00"));
        // The centered checkbox must respond to a real click, not just model writes.
        splits->scrollToItem(splits->item(2, 0));
        QCoreApplication::processEvents();
        const QPoint checkbox = splits->visualItemRect(splits->item(2, 0)).center();
        QTest::mouseClick(splits->viewport(), Qt::LeftButton, Qt::NoModifier, checkbox);
        QCOMPARE(splits->item(2, 0)->checkState(), Qt::Checked);
        QVERIFY(input(2)->isEnabled());
        input(2)->setFocus();
        QTest::keyClicks(input(2), "20.00");
        QCOMPARE(input(2)->text(), QStringLiteral("20.00"));
        input(1)->setText("50.00");
        QCOMPARE(splits->item(2, 3)->text(), QStringLiteral("20.00"));
        const QString output = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
        if (!output.isEmpty()) {
            page->findChild<QScrollArea*>()->ensureWidgetVisible(splits);
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
            QVERIFY(QDir().mkpath(output));
            QVERIFY(window.grab().save(QDir(output).filePath("expense-members-refreshed.png")));
        }
        save->click();
        QVERIFY(notice->text().contains(QStringLiteral("草稿已保存")));
        const auto rows =
            api.call("expense_list", {{"ledger_id", book.value("id")}}).value("expenses").toArray();
        QCOMPARE(rows.size(), 1);
        // A departed bearer must stay explicit in both a live form and a reopened draft.
        const auto exit_preview =
            peer.call("membership_exit_preview", {{"ledger_id", book.value("id")}});
        peer.call("exit_ledger", {{"ledger_id", book.value("id")},
                                  {"preview_token", exit_preview.value("preview_token")},
                                  {"request_key", "exit-during-expense-edit"},
                                  {"confirmed", true},
                                  {"keep_history", true}});
        refresh->click();
        const auto rowFor = [&](const QString& id) {
            for (int row = 0; row < splits->rowCount(); ++row) {
                if (splits->item(row, 0)->data(Qt::UserRole).toString() == id) return row;
            }
            return -1;
        };
        int departed = rowFor(peer.currentUserId());
        QVERIFY(departed >= 0);
        QVERIFY(splits->item(departed, 1)->text().contains(QStringLiteral("已退出")));
        QCOMPARE(splits->item(departed, 0)->checkState(), Qt::Checked);
        QCOMPARE(input(departed)->text(), QStringLiteral("50.00"));
        QVERIFY(!input(departed)->isEnabled());
        QVERIFY(
            page->findChild<QLabel*>("expensePreview")->text().contains(QStringLiteral("已退出")));
        if (!output.isEmpty()) {
            page->findChild<QScrollArea*>()->ensureWidgetVisible(splits);
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(QDir(output).filePath("expense-member-departed.png")));
        }
        save->click();
        QVERIFY(!notice->text().contains(QStringLiteral("草稿已保存")));
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (dialog) dialog->button(QMessageBox::Discard)->click();
        });
        page->findChild<QPushButton*>("backToExpenseList")->click();
        page->findChild<QPushButton*>("editExpenseDraft")->click();
        departed = rowFor(peer.currentUserId());
        QVERIFY(departed >= 0);
        QVERIFY(splits->item(departed, 1)->text().contains(QStringLiteral("已退出")));
        QCOMPARE(splits->item(departed, 0)->checkState(), Qt::Checked);
        QCOMPARE(input(departed)->text(), QStringLiteral("50.00"));
        splits->item(departed, 0)->setCheckState(Qt::Unchecked);
        input(rowFor(newcomer.currentUserId()))->setText("70.00");
        QTest::qWait(500);
        ledger::RecoveryStore recovery(directory.path(), api.currentUserId(),
                                       book.value("id").toString());
        QVERIFY(!recovery.entries().isEmpty());
        const auto recovery_rows = recovery.read(recovery.entries().first())
                                       .value("snapshot")
                                       .toObject()
                                       .value("rows")
                                       .toArray();
        QCOMPARE(recovery_rows.size(), 2);
        for (const auto& row : recovery_rows)
            QVERIFY(row.toObject().value("id").toString() != peer.currentUserId());
        save->click();
        QVERIFY(notice->text().contains(QStringLiteral("草稿已保存")));
        const auto saved = api.call(
            "expense_detail",
            {{"ledger_id", book.value("id")}, {"expense_id", rows[0].toObject().value("id")}});
        QCOMPARE(saved.value("form").toObject().value("splits").toArray().size(), 2);
        peer.call("accept_invitation", {{"invitation_id", invite("member").value("id")}});
        refresh->click();
        departed = rowFor(peer.currentUserId());
        QCOMPARE(splits->item(departed, 1)->text(), QStringLiteral("member"));
        QCOMPARE(splits->item(departed, 0)->checkState(), Qt::Unchecked);
        // Returning via the member shortcut must preserve the saved draft.
        page->findChild<QPushButton*>("manageExpenseMembers")->click();
        QCOMPARE(navigation->currentRow(), 0);
        QCOMPARE(window.findChild<QLineEdit*>("inviteUsername")->isVisible(), true);
        window.findChild<QPushButton*>("logoutButton")->click();
        window.close();
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

QTEST_MAIN(UiExpenseTests)
#include "ui_expense_tests.moc"
