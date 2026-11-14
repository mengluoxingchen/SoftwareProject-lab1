#include <QApplication>
#include <QComboBox>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include "domain/service_error.h"
#include "infrastructure/database.h"
#include "transport/api_client.h"
#include "ui/browse_page.h"
#include "ui/main_window.h"

namespace {
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
void choose(QComboBox* combo, const QString& value) {
    combo->setCurrentIndex(combo->findData(value));
}
}  // namespace
class UiQueryTests final : public QObject {
    Q_OBJECT
private slots:
    void thousandRowsVisibleQueryThreeRunAverage();
    void filterPaginateSummarizeAndReadScopedRetainedAudit();
};
void UiQueryTests::filterPaginateSummarizeAndReadScopedRetainedAudit() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    try {
        {
            ledger::ApiClient admin, member, third, api;
            for (auto* client : {&admin, &member, &third, &api}) {
                client->connectToService(directory.path(), false);
            }
            admin.call("register", {{"username", "owner"}, {"password", "Password-42"}});
            member.call("register", {{"username", "member"}, {"password", "Password-42"}});
            third.call("register", {{"username", "third"}, {"password", "Password-42"}});
            const auto a = admin.login("owner", "Password-42"),
                       b = member.login("member", "Password-42"),
                       c = third.login("third", "Password-42");
            const auto id =
                admin.call("create_ledger", {{"name", QStringLiteral("查询统计与日志验收")}})
                    .value("id");
            for (auto* client : {&member, &third}) {
                const auto invite =
                    admin.call("invite", {{"ledger_id", id},
                                          {"username", client == &member ? "member" : "third"},
                                          {"role", client == &member ? "member" : "read_only"}});
                client->call("accept_invitation", {{"invitation_id", invite.value("id")}});
            }
            QJsonObject form{
                {"ledger_id", id},
                {"amount", "75"},
                {"date", "2026-11-10"},
                {"description", "Office_100%"},
                {"category", "办公"},
                {"payer_id", a.value("id")},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", b.value("id")}, {"value", ""}}}}};
            auto record = admin.call("expense_submit", form).value("expense").toObject();
            const auto reference =
                QJsonObject{{"ledger_id", id}, {"expense_id", record.value("id")}};
            auto lock = admin.call("expense_lock_acquire", reference);
            auto edit = form;
            edit["amount"] = "90";
            edit["expense_id"] = record.value("id");
            edit["version"] = record.value("version");
            edit["lock_token"] = lock.value("lock_token");
            edit["change_reason"] = QStringLiteral("核对凭证，金额由75改为90");
            record = admin.call("expense_submit", edit).value("expense").toObject();
            admin.call("expense_review", {{"ledger_id", id},
                                          {"expense_id", record.value("id")},
                                          {"version", record.value("version")},
                                          {"decision", "approve"},
                                          {"reason", "Checked"}});
            for (int n = 0; n < 54; ++n) {
                auto other = form;
                other["amount"] = "10";
                other["description"] = QStringLiteral("Other %1").arg(n);
                other["date"] = "2026-11-09";
                other["category"] = "餐饮";
                other["splits"] =
                    QJsonArray{QJsonObject{{"user_id", c.value("id")}, {"value", ""}}};
                admin.call("expense_submit", other);
            }
            auto private_form = form;
            private_form["description"] = "Private draft";
            private_form["amount"] = "77";
            private_form["category"] = "私人草稿分类";
            private_form["payer_id"] = b.value("id");
            member.call("expense_save_draft", private_form);
            QFile css(":/resources/styles.qss");
            QVERIFY(css.open(QIODevice::ReadOnly));
            qApp->setStyleSheet(QString::fromUtf8(css.readAll()));
            ledger::MainWindow window(api);
            window.resize(1580, 1100);
            window.show();
            QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
            const auto login = [&](const QString& name) {
                window.findChild<QLineEdit*>("usernameInput")->setText(name);
                window.findChild<QLineEdit*>("passwordInput")->setText("Password-42");
                window.findChild<QPushButton*>("authSubmit")->click();
            };
            login("owner");
            auto* navigation = window.findChild<QListWidget*>("navigation");
            navigation->setCurrentRow(4);
            auto* query = window.findChild<QWidget*>("queryExpensePage");
            auto* records = query->findChild<QTableWidget*>("expenseTable");
            QCOMPARE(records->rowCount(), 50);
            query->findChild<QPushButton*>("queryNext")->click();
            QCOMPARE(records->rowCount(), 5);
            query->findChild<QPushButton*>("queryPrevious")->click();
            QCOMPARE(records->rowCount(), 50);
            query->findChild<QLineEdit*>("queryDateFrom")->setText("2026-11-10");
            query->findChild<QLineEdit*>("queryDateTo")->setText("2026-11-10");
            query->findChild<QLineEdit*>("queryAmountMin")->setText("90");
            query->findChild<QLineEdit*>("queryAmountMax")->setText("90.00");
            query->findChild<QLineEdit*>("queryDescription")->setText("OFFICE_100%");
            choose(query->findChild<QComboBox*>("queryPayer"), a.value("id").toString());
            choose(query->findChild<QComboBox*>("queryCategory"), "办公");
            choose(query->findChild<QComboBox*>("expenseStatusFilter"), "approved");
            query->findChild<QPushButton*>("refreshExpenses")->click();
            QCOMPARE(records->rowCount(), 1);
            QVERIFY(query->findChild<QLabel*>("querySummary")->text().contains("90.00"));
            QCOMPARE(query->findChild<QTableWidget*>("queryGroups")->rowCount(), 1);
            QVERIFY(screenshot(window, "stage8-query-combined.png"));
            query->findChild<QLineEdit*>("queryDateFrom")->setText("2026-02-30");
            query->findChild<QPushButton*>("refreshExpenses")->click();
            QVERIFY(!query->findChild<QLabel*>("queryNotice")->text().isEmpty());
            QCOMPARE(query->findChild<QLineEdit*>("queryDateFrom")->text(),
                     QStringLiteral("2026-02-30"));
            query->findChild<QLineEdit*>("queryDateFrom")->setText("2026-11-10");
            query->findChild<QPushButton*>("refreshExpenses")->click();
            bool detail_seen = false;
            QTimer::singleShot(0, qApp, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog) {
                    detail_seen =
                        dialog->objectName() == "expenseDetailsDialog" &&
                        dialog->findChild<QTableWidget*>("expenseDetailShares")->rowCount() == 1;
                    dialog->reject();
                }
            });
            records->cellWidget(0, 7)->findChild<QPushButton*>("viewExpense")->click();
            QVERIFY(detail_seen);
            choose(query->findChild<QComboBox*>("queryGrouping"), "months");
            QCOMPARE(query->findChild<QTableWidget*>("queryGroups")->item(0, 0)->text(),
                     QStringLiteral("2026-11"));
            navigation->setCurrentRow(5);
            auto* audit = window.findChild<QWidget*>("auditPage");
            choose(audit->findChild<QComboBox*>("auditAction"), "revise_expense");
            audit->findChild<QPushButton*>("searchAudit")->click();
            auto* logs = audit->findChild<QTableWidget*>("auditTable");
            QCOMPARE(logs->rowCount(), 1);
            QCOMPARE(logs->item(0, 3)->text(), QStringLiteral("75"));
            QCOMPARE(logs->item(0, 4)->text(), QStringLiteral("90"));
            QVERIFY(logs->showGrid());
            QCOMPARE(logs->item(0, 3)->textAlignment(), Qt::AlignCenter);
            QVERIFY(screenshot(window, "stage8-audit-admin.png"));
            bool audit_detail = false;
            QTimer::singleShot(0, qApp, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog) {
                    audit_detail = dialog->objectName() == "auditDetailDialog";
                    screenshot(*dialog, "stage8-audit-detail.png");
                    dialog->reject();
                }
            });
            logs->cellWidget(0, 7)->findChild<QPushButton*>("viewAuditDetail")->click();
            QVERIFY(audit_detail);
            window.findChild<QPushButton*>("logoutButton")->click();
            login("member");
            navigation->setCurrentRow(5);
            QCOMPARE(audit->findChild<QComboBox*>("auditScope")->findData("all"), -1);
            choose(audit->findChild<QComboBox*>("auditAction"), "revise_expense");
            audit->findChild<QPushButton*>("searchAudit")->click();
            QCOMPARE(logs->rowCount(), 1);
            QCOMPARE(logs->item(0, 1)->text(), QStringLiteral("owner"));
            QVERIFY_THROWS_EXCEPTION(
                ledger::ServiceError,
                member.call("audit_list", {{"ledger_id", id}, {"scope", "all"}}));
            QVERIFY(screenshot(window, "stage8-audit-related.png"));
            const auto before =
                member.call("audit_list", {{"ledger_id", id}}).value("total").toInt();
            const auto exit = member.call("membership_exit_preview", {{"ledger_id", id}});
            member.call("exit_ledger", {{"ledger_id", id},
                                        {"preview_token", exit.value("preview_token")},
                                        {"confirmed", true},
                                        {"keep_history", true},
                                        {"request_key", "exit"}});
            record = admin.call("expense_detail", reference);
            lock = admin.call("expense_lock_acquire", reference);
            edit["amount"] = "100";
            edit["version"] = record.value("version");
            edit["lock_token"] = lock.value("lock_token");
            edit["change_reason"] = "After member exit";
            edit["splits"] = QJsonArray{QJsonObject{{"user_id", c.value("id")}, {"value", ""}}};
            admin.call("expense_submit", edit);
            window.findChild<QPushButton*>("refreshWorkspace")->click();
            navigation->setCurrentRow(4);
            query->findChild<QPushButton*>("resetQuery")->click();
            QVERIFY(query->findChild<QLabel*>("querySummary")->text().contains("90.00"));
            navigation->setCurrentRow(5);
            audit->findChild<QPushButton*>("resetAudit")->click();
            QCOMPARE(member.call("audit_list", {{"ledger_id", id}}).value("total").toInt(), before);
            const auto third_exit = third.call("membership_exit_preview", {{"ledger_id", id}});
            third.call("exit_ledger", {{"ledger_id", id},
                                       {"preview_token", third_exit.value("preview_token")},
                                       {"confirmed", true},
                                       {"keep_history", false},
                                       {"request_key", "exit-third"}});
            const auto close = admin.call("ledger_close_preview", {{"ledger_id", id}});
            admin.call("ledger_close", {{"ledger_id", id},
                                        {"preview_token", close.value("preview_token")},
                                        {"confirmed", true},
                                        {"mode", "delete"},
                                        {"confirm_name", QStringLiteral("查询统计与日志验收")}});
            window.findChild<QPushButton*>("refreshWorkspace")->click();
            navigation->setCurrentRow(5);
            QCOMPARE(audit->findChild<QTableWidget*>("auditTable")->rowCount(), before);
            QVERIFY(audit->findChild<QLabel*>("auditSummary")
                        ->text()
                        .contains(QStringLiteral("与我相关")));
            QVERIFY(screenshot(window, "stage8-deleted-related-logs.png"));
            QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                     member.call("query_expenses", {{"ledger_id", id}}));
            window.findChild<QPushButton*>("logoutButton")->click();
            login("third");
            navigation->setCurrentRow(5);
            QCOMPARE(audit->findChild<QComboBox*>("auditLedgerSelector")->count(), 0);
            QCOMPARE(audit->findChild<QComboBox*>("auditActor")->count(), 1);
            QCOMPARE(logs->rowCount(), 0);
            QVERIFY(!audit->findChild<QPushButton*>("auditNext")->isEnabled());
            window.close();
        }
        QVERIFY(backend.waitForFinished(4000));
        QCOMPARE(backend.exitCode(), 0);
    } catch (const ledger::ServiceError& failure) {
        backend.kill();
        backend.waitForFinished(4000);
        QFAIL(qPrintable(failure.code() + ": " + failure.message()));
    }
}
void UiQueryTests::thousandRowsVisibleQueryThreeRunAverage() {
    QFile css(":/resources/styles.qss");
    QVERIFY(css.open(QIODevice::ReadOnly));
    qApp->setStyleSheet(QString::fromUtf8(css.readAll()));
    QTemporaryDir directory;
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "60"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    ledger::ApiClient api;
    api.connectToService(directory.path(), false);
    api.call("register", {{"username", "benchmark"}, {"password", "Password-42"}});
    const auto user = api.login("benchmark", "Password-42");
    const auto book = api.call("create_ledger", {{"name", QStringLiteral("1000条可见查询验收")}});
    const auto id = book.value("id").toString(), uid = user.value("id").toString();
    backend.kill();
    QVERIFY(backend.waitForFinished());
    {
        ledger::Database db(directory.filePath("shared_ledger.sqlite3"));
        db.open();
        QVERIFY(db.connection().transaction());
        QSqlQuery sql(db.connection());
        for (int n = 0; n < 1000; ++n) {
            const QString description = QStringLiteral("性能验收记录 %1").arg(n);
            const auto payload =
                QJsonDocument(QJsonObject{{"amount", "10.01"},
                                          {"date", "2026-11-10"},
                                          {"payer_id", uid},
                                          {"description", description},
                                          {"category", "办公"},
                                          {"split_method", "equal"},
                                          {"splits", QJsonArray{QJsonObject{{"user_id", uid},
                                                                            {"value", ""}}}}})
                    .toJson(QJsonDocument::Compact);
            sql.prepare(
                "INSERT INTO expenses(id,ledger_id,creator_id,payer_id,amount_cents,expense_date,"
                "description,category,legacy_expense_type,split_method,status,payload,similar_key,"
                "version,created_at,updated_at) "
                "VALUES(?,?,?,?,1001,'2026-11-10',?,'办公','','equal',"
                "'approved',?,'',1,1,1)");
            const auto eid = QStringLiteral("benchmark-%1").arg(n);
            for (const auto& value :
                 QStringList{eid, id, uid, uid, description, QString::fromUtf8(payload)})
                sql.addBindValue(value);
            QVERIFY2(sql.exec(), qPrintable(sql.lastError().text()));
            sql.prepare("INSERT INTO expense_shares VALUES(?,?,1001)");
            sql.addBindValue(eid);
            sql.addBindValue(uid);
            QVERIFY(sql.exec());
        }
        QVERIFY(db.connection().commit());
    }
    backend.start();
    QVERIFY(backend.waitForStarted());
    api.connectToService(directory.path(), false);
    ledger::BrowsePage page(api, false);
    page.resize(1550, 1000);
    page.show();
    page.setContext(api.call("ledger_details", {{"ledger_id", id}}), user);
    auto* size = page.findChild<QComboBox*>("queryPageSize");
    size->setCurrentIndex(size->findData(200));
    QJsonArray times;
    qint64 sum = 0;
    for (int run = 0; run < 3; ++run) {
        QElapsedTimer clock;
        clock.start();
        page.findChild<QPushButton*>("refreshExpenses")->click();
        QCoreApplication::processEvents();
        page.repaint();
        const auto elapsed = clock.elapsed();
        times.append(elapsed);
        sum += elapsed;
        QCOMPARE(page.findChild<QTableWidget*>("expenseTable")->rowCount(), 200);
        QVERIFY(page.findChild<QLabel*>("queryPagination")
                    ->text()
                    .contains(QStringLiteral("共1000条")));
        QVERIFY(page.findChild<QLabel*>("querySummary")->text().contains("10010.00"));
    }
    qInfo() << "1000 records, 200 visible, query/filter/statistics/render ms:" << times << "average"
            << sum / 3.0;
    QVERIFY2(sum / 3.0 <= 2000.0, "Visible 1000-record query average exceeds 2 seconds");
    const auto out = qEnvironmentVariable("LEDGER_SCREENSHOT_DIR");
    if (!out.isEmpty()) {
        QFile result(QDir(out).filePath("stage9-query-performance.json"));
        QVERIFY(result.open(QIODevice::WriteOnly));
        result.write(
            QJsonDocument(QJsonObject{{"records", 1000},
                                      {"page_size", 200},
                                      {"runs_ms", times},
                                      {"average_ms", sum / 3.0},
                                      {"includes", "IPC/filter/sort/statistics/widget rendering"}})
                .toJson());
        QVERIFY(screenshot(page, "stage9-query-1000.png"));
    }
    page.clearContext();
    backend.kill();
    QVERIFY(backend.waitForFinished());
}
QTEST_MAIN(UiQueryTests)
#include "ui_query_tests.moc"
