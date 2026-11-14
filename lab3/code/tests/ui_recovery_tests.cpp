#include <QApplication>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cstdlib>

#include "recovery/recovery_store.h"
#include "transport/api_client.h"
#include "ui/expense_page.h"

namespace {
void restoreFirst(ledger::ExpensePage& page, bool close_preview = false) {
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &page, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QDialog*>(widget)) {
                if (dialog->isVisible()) {
                    qWarning() << "Recovery dialog timeout:" << dialog->windowTitle();
                    dialog->reject();
                }
            }
        }
    });
    QTimer timer;
    timer.setInterval(30);
    QObject::connect(&timer, &QTimer::timeout, &page, [&] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            if (auto* table = dialog->findChild<QTableWidget*>("recoveryCopiesTable")) {
                if (table->rowCount()) {
                    const auto out = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
                    if (!out.isEmpty()) {
                        dialog->repaint();
                        dialog->grab().save(QDir(out).filePath("stage9-recovery-list.png"));
                    }
                    auto* restore =
                        table->cellWidget(0, 3)->findChild<QPushButton*>("restoreRecovery");
                    QTimer::singleShot(0, restore, &QPushButton::click);
                }
            } else if (close_preview) {
                if (auto* buttons = dialog->findChild<QDialogButtonBox*>())
                    buttons->button(QDialogButtonBox::Close)->click();
            }
        }
    });
    timer.start();
    timeout.start(10000);
    page.findChild<QPushButton*>("recoveryCopiesButton")->click();
}
struct Backend {
    QProcess process;
    QString dir;
    explicit Backend(QString directory) : dir(std::move(directory)) {
        process.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
        process.setArguments({"--data-dir", dir, "--idle-timeout", "60"});
        start();
    }
    void start() {
        process.start();
        if (!process.waitForStarted()) throw std::runtime_error("Start test backend");
    }
    void stop() {
        process.kill();
        process.waitForFinished(5000);
    }
    ~Backend() { stop(); }
};
int crashWriter(const QString& directory, const QString& marker) {
    ledger::ApiClient api;
    api.connectToService(directory, false);
    const auto user = api.login("owner", "Password-42");
    const auto books = api.call("list_ledgers").value("ledgers").toArray();
    const auto book =
        api.call("ledger_details", {{"ledger_id", books.first().toObject().value("id")}});
    ledger::ExpensePage page(api, false);
    page.resize(1400, 1000);
    page.show();
    page.setContext(book, user);
    page.findChild<QPushButton*>("newExpenseButton")->click();
    page.findChild<QLineEdit*>("expenseAmount")->setText("37.25");
    page.findChild<QLineEdit*>("expenseDescription")
        ->setText(QStringLiteral("异常退出前的采购输入"));
    page.findChild<QLineEdit*>("expenseCategory")->setText(QStringLiteral("办公"));
    page.findChild<QDateEdit*>("expenseDate")->setDate(QDate(2026, 11, 10));
    auto* method = page.findChild<QComboBox*>("expenseSplitMethod");
    method->setCurrentIndex(method->findData("ratio"));
    auto* splits = page.findChild<QTableWidget*>("expenseSplits");
    if (splits->rowCount() != 2) return 12;
    qobject_cast<QLineEdit*>(splits->cellWidget(0, 2))->setText("1");
    qobject_cast<QLineEdit*>(splits->cellWidget(1, 2))->setText("2");
    QTest::qWait(650);
    ledger::RecoveryStore store(directory, user.value("id").toString(),
                                book.value("id").toString());
    if (store.entries().size() != 1) return 13;
    QFile ready(marker);
    if (!ready.open(QIODevice::WriteOnly) || ready.write("ready") != 5) return 14;
    ready.close();
    // Test-only abrupt exit: no widget/client destructors or normal close prompts.
    std::_Exit(97);
}
}  // namespace
class UiRecoveryTests final : public QObject {
    Q_OBJECT
private slots:
    void crashRecoveryConnectionFailureAndExplicitRetry();
    void staleServerVersionKeepsLocalCopy();
};
void UiRecoveryTests::crashRecoveryConnectionFailureAndExplicitRetry() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Backend backend(directory.path());
    ledger::ApiClient admin, peer;
    admin.connectToService(directory.path(), false);
    peer.connectToService(directory.path(), false);
    admin.call("register", {{"username", "owner"}, {"password", "Password-42"}});
    peer.call("register", {{"username", "member"}, {"password", "Password-42"}});
    const auto user = admin.login("owner", "Password-42");
    const auto member = peer.login("member", "Password-42");
    const auto created = admin.call("create_ledger", {{"name", QStringLiteral("恢复与断线验收")}});
    const auto id = created.value("id");
    const auto invite =
        admin.call("invite", {{"ledger_id", id}, {"username", "member"}, {"role", "member"}});
    peer.call("accept_invitation", {{"invitation_id", invite.value("id")}});
    const auto book = admin.call("ledger_details", {{"ledger_id", id}});
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    const QString ready = QDir(directory.path()).filePath("ready");
    child.setArguments({"--crash-writer", directory.path(), ready});
    child.start();
    QVERIFY(child.waitForFinished(15000));
    QCOMPARE(child.exitCode(), 97);
    QVERIFY(QFile::exists(ready));
    ledger::RecoveryStore store(directory.path(), user.value("id").toString(), id.toString());
    QCOMPARE(store.entries().size(), 1);
    ledger::RecoveryStore other(directory.path(), member.value("id").toString(), id.toString());
    QVERIFY(other.entries().isEmpty());
    QVERIFY(admin.call("expense_list", {{"ledger_id", id}}).value("expenses").toArray().isEmpty());
    ledger::ExpensePage page(admin, false);
    page.resize(1400, 1000);
    page.show();
    page.setContext(book, user);
    restoreFirst(page);
    QCOMPARE(page.findChild<QLineEdit*>("expenseAmount")->text(), QString("37.25"));
    QCOMPARE(page.findChild<QLineEdit*>("expenseCategory")->text(), QStringLiteral("办公"));
    QCOMPARE(page.findChild<QDateEdit*>("expenseDate")->date(), QDate(2026, 11, 10));
    QCOMPARE(page.findChild<QComboBox*>("expenseSplitMethod")->currentData().toString(),
             QString("ratio"));
    auto* splits = page.findChild<QTableWidget*>("expenseSplits");
    QCOMPARE(qobject_cast<QLineEdit*>(splits->cellWidget(0, 2))->text(), QString("1"));
    QCOMPARE(qobject_cast<QLineEdit*>(splits->cellWidget(1, 2))->text(), QString("2"));
    QVERIFY(!store.claim(store.entries().first()));
    const QString screenshots = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) {
        page.repaint();
        QCoreApplication::processEvents();
        QVERIFY(page.grab().save(QDir(screenshots).filePath("stage9-restored-form.png")));
    }
    backend.stop();
    page.findChild<QPushButton*>("submitExpense")->click();
    QCOMPARE(page.findChild<QLineEdit*>("expenseAmount")->text(), QString("37.25"));
    QVERIFY(page.findChild<QLabel*>("expenseNotice")->text().contains(QStringLiteral("无法连接")));
    QCOMPARE(store.entries().size(), 1);
    backend.start();
    page.findChild<QPushButton*>("submitExpense")->click();
    const auto rows = admin.call("expense_list", {{"ledger_id", id}}).value("expenses").toArray();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().toObject().value("status").toString(), QString("pending"));
    QVERIFY(store.entries().isEmpty());
    page.clearContext();
}
void UiRecoveryTests::staleServerVersionKeepsLocalCopy() {
    QTemporaryDir directory;
    Backend backend(directory.path());
    ledger::ApiClient admin;
    admin.connectToService(directory.path(), false);
    admin.call("register", {{"username", "owner"}, {"password", "Password-42"}});
    const auto user = admin.login("owner", "Password-42");
    const auto book = admin.call("create_ledger", {{"name", "Stale recovery"}});
    const auto id = book.value("id");
    QJsonObject form{
        {"ledger_id", id},
        {"amount", "10"},
        {"date", "2026-11-10"},
        {"description", "Original"},
        {"payer_id", user.value("id")},
        {"split_method", "equal"},
        {"splits", QJsonArray{QJsonObject{{"user_id", user.value("id")}, {"value", ""}}}}};
    const auto saved = admin.call("expense_save_draft", form);
    form["expense_id"] = saved.value("id");
    form["version"] = saved.value("version");
    auto local = form;
    local["amount"] = "90";
    ledger::RecoveryStore store(directory.path(), user.value("id").toString(), id.toString());
    const auto copy_id = store.newId();
    auto lease = store.claim(copy_id);
    store.write(copy_id, {{"form", local}, {"rows", QJsonArray{}}, {"editing_status", "draft"}},
                *lease);
    lease.reset();
    form["amount"] = "20";
    form["lock_token"] = saved.value("lock_token");
    const auto latest = admin.call("expense_save_draft", form);
    ledger::ExpensePage page(admin, false);
    page.resize(1400, 1000);
    page.show();
    page.setContext(book, user);
    restoreFirst(page, true);
    QVERIFY(page.findChild<QLabel*>("expenseNotice")
                ->text()
                .contains(QStringLiteral("服务器账目已更新")));
    QCOMPARE(store.read(copy_id)
                 .value("snapshot")
                 .toObject()
                 .value("form")
                 .toObject()
                 .value("amount")
                 .toString(),
             QString("90"));
    const auto record =
        admin.call("expense_detail", {{"ledger_id", id}, {"expense_id", saved.value("id")}});
    QCOMPARE(record.value("version"), latest.value("version"));
    QCOMPARE(record.value("form").toObject().value("amount").toString(), QString("20"));
    page.clearContext();
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains("--crash-writer")) {
        const int index = app.arguments().indexOf("--crash-writer");
        return crashWriter(app.arguments().at(index + 1), app.arguments().at(index + 2));
    }
    QFile style(":/resources/styles.qss");
    if (style.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(style.readAll()));
    UiRecoveryTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ui_recovery_tests.moc"
