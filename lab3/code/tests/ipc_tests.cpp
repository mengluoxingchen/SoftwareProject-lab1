#include <windows.h>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <thread>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "transport/endpoint.h"

class IpcTests final : public QObject {
    Q_OBJECT
private slots:
    void activeStartupConnectionPreventsIdleExit();
    void unobservedWriteReplySurvivesRestartWithoutDuplicate();
    void frameLimitsApplyPerMessageAndRejectFractionalIds();
    void historicalScopeAndTransferredRoleSurviveRestart();
    void independentClientsShareAuthorizationAndPersistentData();
    void disconnectedRequestIsNotReplayedAndNextRequestReconnects();
    void settlementRepaymentAndSnapshotSurviveRestart();
    void closedLedgerAndRetainedAuditSurviveRestart();
};

void IpcTests::independentClientsShareAuthorizationAndPersistentData() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    QString book_id;
    {
        ledger::ApiClient alice;
        ledger::ApiClient bob;
        alice.connectToService(directory.path(), false);
        bob.connectToService(directory.path(), false);
        alice.call("register", {{"username", "alice"}, {"password", "Password-42"}});
        bob.call("register", {{"username", "bob"}, {"password", "Password-42"}});
        const auto alice_user = alice.login("alice", "Password-42");
        const auto bob_user = bob.login("bob", "Password-42");
        const auto book = alice.call("create_ledger", {{"name", QStringLiteral("共享测试账本")}});
        book_id = book.value("id").toString();
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 bob.call("ledger_details", {{"ledger_id", book_id}}));
        const auto invite = alice.call(
            "invite", {{"ledger_id", book_id}, {"username", "bob"}, {"role", "read_only"}});
        const auto joined = bob.call("accept_invitation", {{"invitation_id", invite.value("id")}});
        QCOMPARE(joined.value("role").toString(), QStringLiteral("read_only"));
        QVERIFY(!joined.value("capabilities").toObject().value("write_expense").toBool());
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, bob.call("invite", {{"ledger_id", book_id},
                                                                           {"username", "alice"},
                                                                           {"role", "member"}}));

        QProcess duplicate;
        duplicate.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
        duplicate.setArguments(backend.arguments());
        duplicate.start();
        QVERIFY(duplicate.waitForFinished(3000));
        QCOMPARE(duplicate.exitCode(), 3);
        QCOMPARE(alice.call("ping").value("version").toString(), QStringLiteral("0.9.3"));

        QJsonObject expense{
            {"ledger_id", book_id},
            {"amount", "100.01"},
            {"date", "2026-11-09"},
            {"description", QStringLiteral("共享办公用品")},
            {"payer_id", alice_user.value("id")},
            {"split_method", "equal"},
            {"splits", QJsonArray{QJsonObject{{"user_id", alice_user.value("id")}, {"value", ""}},
                                  QJsonObject{{"user_id", bob_user.value("id")}, {"value", ""}}}}};
        const auto draft = alice.call("expense_save_draft", expense);
        QCOMPARE(
            bob.call("expense_list", {{"ledger_id", book_id}}).value("expenses").toArray().size(),
            0);
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 bob.call("expense_detail", {{"ledger_id", book_id},
                                                             {"expense_id", draft.value("id")}}));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, bob.call("expense_submit", expense));
        expense["expense_id"] = draft.value("id");
        expense["version"] = draft.value("version");
        expense["lock_token"] = draft.value("lock_token");
        const auto submitted = alice.call("expense_submit", expense).value("expense").toObject();
        QCOMPARE(submitted.value("status").toString(), QStringLiteral("pending"));
        QCOMPARE(
            bob.call("expense_list", {{"ledger_id", book_id}}).value("expenses").toArray().size(),
            1);
        QCOMPARE(
            bob.call("expense_detail", {{"ledger_id", book_id}, {"expense_id", draft.value("id")}})
                .value("shares")
                .toArray()
                .size(),
            2);
        auto invalid_expense = expense;
        invalid_expense.remove("expense_id");
        invalid_expense["amount"] = "-1";
        try {
            alice.call("expense_submit", invalid_expense);
            QFAIL("Invalid amount must fail through IPC.");
        } catch (const ledger::ServiceError& failure) {
            QCOMPARE(failure.field(), QStringLiteral("amount"));
        }

        QJsonObject reference{{"ledger_id", book_id},
                              {"expense_id", submitted.value("id")},
                              {"version", submitted.value("version")}};
        const auto lease = alice.call("expense_lock_acquire", reference);
        {
            ledger::ApiClient other_window;
            other_window.connectToService(directory.path(), false);
            other_window.login("alice", "Password-42");
            QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                     other_window.call("expense_lock_acquire", reference));
        }
        auto review = reference;
        review["decision"] = "approve";
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, bob.call("expense_review", review));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, alice.call("expense_review", review));
        reference["lock_token"] = lease.value("lock_token");
        QVERIFY(alice.call("expense_lock_release", reference).value("released").toBool());
        QCOMPARE(alice.call("expense_review", review).value("status").toString(),
                 QStringLiteral("approved"));
        QCOMPARE(alice.call("expense_history", reference).value("reviews").toArray().size(), 1);

        QLocalSocket malformed;
        malformed.connectToServer(ledger::localServerName(directory.path()));
        QVERIFY(malformed.waitForConnected());
        malformed.write("{bad json}\n");
        malformed.flush();
        QVERIFY(malformed.waitForReadyRead(2000));
        const auto response = QJsonDocument::fromJson(malformed.readLine()).object();
        QVERIFY(!response.value("ok").toBool());
        QCOMPARE(response.value("error").toObject().value("code").toString(),
                 QStringLiteral("bad_request"));
        malformed.abort();
        bob.logout();
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, bob.call("list_ledgers"));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 bob.call("expense_list", {{"ledger_id", book_id}}));
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient reopened;
        reopened.connectToService(directory.path(), false);
        reopened.login("alice", "Password-42");
        const auto books = reopened.call("list_ledgers").value("ledgers").toArray();
        QCOMPARE(books.size(), 1);
        QCOMPARE(books[0].toObject().value("id").toString(), book_id);
        QCOMPARE(reopened.call("ledger_details", {{"ledger_id", book_id}})
                     .value("members")
                     .toArray()
                     .size(),
                 2);
        const auto expenses =
            reopened.call("expense_list", {{"ledger_id", book_id}}).value("expenses").toArray();
        QCOMPARE(expenses.size(), 1);
        QCOMPARE(expenses[0].toObject().value("amount").toString(), QStringLiteral("100.01"));
        QCOMPARE(expenses[0].toObject().value("status").toString(), QStringLiteral("approved"));
        QCOMPARE(reopened
                     .call("expense_history", {{"ledger_id", book_id},
                                               {"expense_id", expenses[0].toObject().value("id")}})
                     .value("reviews")
                     .toArray()
                     .size(),
                 1);
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void IpcTests::activeStartupConnectionPreventsIdleExit() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    const auto cleanup = qScopeGuard([&] {
        if (backend.state() != QProcess::NotRunning) {
            backend.kill();
            backend.waitForFinished(4000);
        }
    });
    const QString pipe =
        QStringLiteral("\\\\.\\pipe\\") + ledger::localServerName(directory.path());
    std::atomic_bool connected = false;
    std::atomic_ulong last_pipe_error = 0;
    // Connect as soon as Windows creates a listener, before QLocalServer::listen returns.
    std::jthread eager([pipe, &connected, &last_pipe_error](std::stop_token stop) {
        HANDLE handle = INVALID_HANDLE_VALUE;
        QElapsedTimer deadline;
        deadline.start();
        while (!stop.stop_requested() && deadline.elapsed() < 20000) {
            handle = CreateFileW(reinterpret_cast<const wchar_t*>(pipe.utf16()),
                                 GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                connected.store(true);
                break;
            }
            last_pipe_error.store(GetLastError());
        }
        while (handle != INVALID_HANDLE_VALUE && !stop.stop_requested()) Sleep(1);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    });
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    ledger::ApiClient api;
    QTRY_VERIFY_WITH_TIMEOUT(connected.load() || backend.state() == QProcess::NotRunning, 20000);
    QVERIFY2(connected.load(),
             qPrintable(QStringLiteral(
                            "Early client did not connect: Windows error %1; backend exit %2; %3")
                            .arg(last_pipe_error.load())
                            .arg(backend.exitCode())
                            .arg(QString::fromUtf8(backend.readAllStandardError()))));
    QTest::qWait(1300);
    QVERIFY2(backend.state() == QProcess::Running, backend.readAllStandardError().constData());
    api.connectToService(directory.path(), false);
    QVERIFY(!api.call("ping").value("version").toString().isEmpty());
}

void IpcTests::disconnectedRequestIsNotReplayedAndNextRequestReconnects() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    const auto cleanup = qScopeGuard([&] {
        if (backend.state() != QProcess::NotRunning) {
            backend.kill();
            backend.waitForFinished(4000);
        }
    });
    QString phase = "initial connect";
    try {
        backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
        backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
        backend.start();
        QVERIFY(backend.waitForStarted());
        {
            ledger::ApiClient api;
            phase = "initial connect";
            api.connectToService(directory.path(), false);
            phase = "register";
            api.call("register", {{"username", "owner"}, {"password", "Password-42"}});
            phase = "login";
            const auto user = api.login("owner", "Password-42");
            phase = "create ledger";
            const auto book = api.call("create_ledger", {{"name", "Reconnect"}});
            phase = "immediate RPC burst";
            for (int request = 0; request < 1000; ++request) {
                QCOMPARE(api.call("ping").value("version").toString(), QStringLiteral("0.9.3"));
            }
            const QJsonObject expense{
                {"ledger_id", book.value("id")},
                {"amount", "10.00"},
                {"date", "2026-11-09"},
                {"payer_id", user.value("id")},
                {"description", "Recover after service interruption"},
                {"split_method", "equal"},
                {"splits", QJsonArray{QJsonObject{{"user_id", user.value("id")}, {"value", ""}}}}};
            backend.kill();
            QVERIFY(backend.waitForFinished(4000));
            QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, api.call("expense_submit", expense));
            QVERIFY(api.isAuthenticated());
            backend.start();
            QVERIFY(backend.waitForStarted());
            phase = "list after restart";
            QCOMPARE(api.call("expense_list", {{"ledger_id", book.value("id")}})
                         .value("expenses")
                         .toArray()
                         .size(),
                     0);
            phase = "submit after restart";
            QCOMPARE(api.call("expense_submit", expense)
                         .value("expense")
                         .toObject()
                         .value("status")
                         .toString(),
                     QStringLiteral("pending"));
            phase = "final list";
            QCOMPARE(api.call("expense_list", {{"ledger_id", book.value("id")}})
                         .value("expenses")
                         .toArray()
                         .size(),
                     1);
        }
        QVERIFY(backend.waitForFinished(4000));
        QCOMPARE(backend.exitCode(), 0);
    } catch (const ledger::ServiceError& failure) {
        backend.waitForFinished(10);
        QFAIL(qPrintable(phase + " / " + failure.code() + ": " + failure.message() +
                         " / backend state=" + QString::number(backend.state()) +
                         " exit=" + QString::number(backend.exitCode()) +
                         " / server=" + QString::fromUtf8(backend.readAllStandardError())));
    }
}

void IpcTests::settlementRepaymentAndSnapshotSurviveRestart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    QString book_id, snapshot_id, owner_id, member_id, original_id;
    QJsonObject refund_request, adjustment_request;
    const auto balance = [](const QJsonObject& data, const QString& id) {
        for (const auto& entry : data.value("members").toArray()) {
            const auto row = entry.toObject();
            if (row.value("user_id").toString() == id) {
                return row.value("balance").toString();
            }
        }
        return QString();
    };
    {
        ledger::ApiClient owner, member;
        owner.connectToService(directory.path(), false);
        member.connectToService(directory.path(), false);
        owner.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        member.call("register", {{"username", "member"}, {"password", "Password-42"}});
        owner_id = owner.login("owner", "Password-42").value("id").toString();
        member_id = member.login("member", "Password-42").value("id").toString();
        book_id = owner.call("create_ledger", {{"name", "Settlement persistence"}})
                      .value("id")
                      .toString();
        const auto invitation = owner.call(
            "invite", {{"ledger_id", book_id}, {"username", "member"}, {"role", "member"}});
        member.call("accept_invitation", {{"invitation_id", invitation.value("id")}});
        auto record =
            member
                .call("expense_submit",
                      {{"ledger_id", book_id},
                       {"amount", "90"},
                       {"date", "2026-11-09"},
                       {"payer_id", owner_id},
                       {"description", "Restart proof"},
                       {"split_method", "equal"},
                       {"splits", QJsonArray{QJsonObject{{"user_id", owner_id}, {"value", ""}},
                                             QJsonObject{{"user_id", member_id}, {"value", ""}}}}})
                .value("expense")
                .toObject();
        owner.call("expense_review", {{"ledger_id", book_id},
                                      {"expense_id", record.value("id")},
                                      {"version", record.value("version")},
                                      {"decision", "approve"}});
        auto overview = member.call("settlement_overview", {{"ledger_id", book_id}});
        snapshot_id =
            member
                .call("settlement_create",
                      {{"ledger_id", book_id}, {"preview_token", overview.value("preview_token")}})
                .value("id")
                .toString();
        overview = member.call("settlement_overview", {{"ledger_id", book_id}});
        const QJsonObject payment{{"ledger_id", book_id},
                                  {"payer_id", member_id},
                                  {"recipient_id", owner_id},
                                  {"amount", "20.00"},
                                  {"paid_at", QDateTime::currentSecsSinceEpoch()},
                                  {"request_key", "restart-payment"},
                                  {"balance_token", overview.value("balance_token")}};
        member.call("settlement_repay", payment);
        QVERIFY(member.call("settlement_repay", payment).value("reused").toBool());
        auto forged = payment;
        forged["request_key"] = "forged-actor";
        forged["payer_id"] = owner_id;
        forged["actor_id"] = owner_id;
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError, member.call("settlement_repay", forged));
        QCOMPARE(balance(owner.call("settlement_overview", {{"ledger_id", book_id}}), owner_id),
                 QStringLiteral("25.00"));
        original_id = record.value("id").toString();
        const auto financial =
            owner.call("finance_info", {{"ledger_id", book_id}, {"source_id", original_id}});
        refund_request = {{"ledger_id", book_id},
                          {"source_id", original_id},
                          {"amount", "30"},
                          {"receiver_id", owner_id},
                          {"reason", "Partial refund"},
                          {"occurred_at", QDateTime::currentSecsSinceEpoch()},
                          {"expected_token", financial.value("token")},
                          {"request_key", "persistent-refund"}};
        auto forged_refund = refund_request;
        forged_refund["actor_id"] = owner_id;
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 member.call("finance_refund", forged_refund));
        owner.call("finance_refund", refund_request);
        const auto current =
            owner.call("finance_info", {{"ledger_id", book_id}, {"source_id", original_id}});
        adjustment_request = {
            {"ledger_id", book_id},
            {"kind", "correction"},
            {"source_id", original_id},
            {"amount", "150"},
            {"date", "2026-11-09"},
            {"payer_id", owner_id},
            {"description", "Restart proof corrected"},
            {"split_method", "equal"},
            {"splits", QJsonArray{QJsonObject{{"user_id", owner_id}, {"value", ""}},
                                  QJsonObject{{"user_id", member_id}, {"value", ""}}}},
            {"reason", "Recheck receipt"},
            {"expected_token", current.value("token")},
            {"request_key", "persistent-correction"}};
        const auto preview = owner.call("finance_adjustment_preview", adjustment_request);
        adjustment_request["confirmed"] = true;
        adjustment_request["confirmation_token"] = preview.value("confirmation_token");
        owner.call("finance_adjustment", adjustment_request);
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient owner;
        owner.connectToService(directory.path(), false);
        owner.login("owner", "Password-42");
        const auto overview = owner.call("settlement_overview", {{"ledger_id", book_id}});
        QCOMPARE(balance(overview, owner_id), QStringLiteral("40.00"));
        QCOMPARE(balance(overview, member_id), QStringLiteral("-40.00"));
        QCOMPARE(overview.value("repayments").toArray().size(), 1);
        QCOMPARE(overview.value("total_expenses").toString(), QStringLiteral("120.00"));
        QCOMPARE(overview.value("total_refunds").toString(), QStringLiteral("30.00"));
        QVERIFY(owner.call("finance_refund", refund_request).value("reused").toBool());
        QVERIFY(owner.call("finance_adjustment", adjustment_request).value("reused").toBool());
        const auto original =
            owner.call("expense_detail", {{"ledger_id", book_id}, {"expense_id", original_id}});
        QCOMPARE(original.value("form").toObject().value("amount").toString(),
                 QStringLiteral("90"));
        QVERIFY(original.value("superseded").toBool());
        const auto snapshot = owner.call("settlement_detail",
                                         {{"ledger_id", book_id}, {"settlement_id", snapshot_id}});
        QCOMPARE(balance(snapshot, owner_id), QStringLiteral("45.00"));
        QCOMPARE(balance(snapshot, member_id), QStringLiteral("-45.00"));
        QCOMPARE(snapshot.value("total_repayments").toString(), QStringLiteral("0.00"));
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void IpcTests::closedLedgerAndRetainedAuditSurviveRestart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    QString id;
    {
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        api.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        api.login("owner", "Password-42");
        id = api.call("create_ledger", {{"name", "Temporary ledger"}}).value("id").toString();
        auto preview = api.call("ledger_close_preview", {{"ledger_id", id}});
        api.call("ledger_close", {{"ledger_id", id},
                                  {"preview_token", preview.value("preview_token")},
                                  {"mode", "archive"},
                                  {"confirm_name", "Temporary ledger"},
                                  {"confirmed", true}});
        api.call("restore_ledger", {{"ledger_id", id},
                                    {"close_token", preview.value("preview_token")},
                                    {"confirmed", true}});
        preview = api.call("ledger_close_preview", {{"ledger_id", id}});
        const QJsonObject request{{"ledger_id", id},
                                  {"preview_token", preview.value("preview_token")},
                                  {"mode", "delete"},
                                  {"confirm_name", "Temporary ledger"},
                                  {"confirmed", true}};
        api.call("ledger_close", request);
        QVERIFY(api.call("ledger_close", request).value("reused").toBool());
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient api;
        api.connectToService(directory.path(), false);
        api.login("owner", "Password-42");
        QCOMPARE(api.call("list_ledgers").value("ledgers").toArray().size(), 0);
        const auto closed = api.call("closed_ledgers").value("ledgers").toArray();
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed[0].toObject().value("status").toString(), QStringLiteral("deleted"));
        const auto logs =
            api.call("closed_ledger_logs", {{"ledger_id", id}}).value("logs").toArray();
        QCOMPARE(logs.size(), 4);
        QCOMPARE(logs[0].toObject().value("action").toString(),
                 QStringLiteral("delete_ledger_data"));
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void IpcTests::historicalScopeAndTransferredRoleSurviveRestart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", directory.path(), "--idle-timeout", "1"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    QString id, old_id, new_id, related_log;
    {
        ledger::ApiClient a, b;
        a.connectToService(directory.path(), false);
        b.connectToService(directory.path(), false);
        a.call("register", {{"username", "owner"}, {"password", "Password-42"}});
        b.call("register", {{"username", "member"}, {"password", "Password-42"}});
        const auto owner = a.login("owner", "Password-42"),
                   member = b.login("member", "Password-42");
        id = a.call("create_ledger", {{"name", "Persistent historical scope"}})
                 .value("id")
                 .toString();
        const auto invitation =
            a.call("invite", {{"ledger_id", id}, {"username", "member"}, {"role", "member"}});
        b.call("accept_invitation", {{"invitation_id", invitation.value("id")}});
        QJsonObject form{
            {"ledger_id", id},
            {"amount", "90"},
            {"date", "2026-11-09"},
            {"payer_id", owner.value("id")},
            {"description", "Before exit"},
            {"split_method", "equal"},
            {"splits", QJsonArray{QJsonObject{{"user_id", member.value("id")}, {"value", ""}}}}};
        const auto record = b.call("expense_submit", form).value("expense").toObject();
        old_id = record.value("id").toString();
        const auto preview = a.call("membership_exit_preview", {{"ledger_id", id}});
        a.call("exit_ledger", {{"ledger_id", id},
                               {"preview_token", preview.value("preview_token")},
                               {"request_key", "persistent-exit"},
                               {"confirmed", true},
                               {"keep_history", true},
                               {"successor_id", member.value("id")}});
        const auto audit = a.call("audit_list", {{"ledger_id", id}});
        QCOMPARE(audit.value("total").toInt(), 1);
        related_log = audit.value("logs").toArray()[0].toObject().value("id").toString();
        form["date"] = "2020-01-01";
        form["amount"] = "12";
        form["description"] = "New backdated entry";
        form["payer_id"] = member.value("id");
        new_id = b.call("expense_submit", form).value("expense").toObject().value("id").toString();
    }
    QVERIFY(backend.waitForFinished(4000));
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient a, b;
        a.connectToService(directory.path(), false);
        b.connectToService(directory.path(), false);
        a.login("owner", "Password-42");
        b.login("member", "Password-42");
        QCOMPARE(b.call("ledger_details", {{"ledger_id", id}}).value("role").toString(),
                 QStringLiteral("admin"));
        QVERIFY(a.call("ledger_details", {{"ledger_id", id}}).value("historical").toBool());
        QCOMPARE(a.call("expense_list", {{"ledger_id", id}}).value("expenses").toArray().size(), 1);
        QCOMPARE(a.call("expense_detail", {{"ledger_id", id}, {"expense_id", old_id}})
                     .value("form")
                     .toObject()
                     .value("amount")
                     .toString(),
                 QStringLiteral("90"));
        QVERIFY_THROWS_EXCEPTION(
            ledger::ServiceError,
            a.call("expense_detail", {{"ledger_id", id}, {"expense_id", new_id}}));
        QVERIFY_THROWS_EXCEPTION(
            ledger::ServiceError,
            a.call("invite", {{"ledger_id", id}, {"username", "owner"}, {"role", "member"}}));
        QCOMPARE(a.call("audit_list", {{"ledger_id", id}}).value("total").toInt(), 1);
        const auto close = b.call("ledger_close_preview", {{"ledger_id", id}});
        b.call("ledger_close", {{"ledger_id", id},
                                {"mode", "delete"},
                                {"confirmed", true},
                                {"confirm_name", "Persistent historical scope"},
                                {"preview_token", close.value("preview_token")}});
        QCOMPARE(a.call("audit_list", {{"ledger_id", id}}).value("total").toInt(), 1);
    }
    QVERIFY(backend.waitForFinished(4000));
    backend.start();
    QVERIFY(backend.waitForStarted());
    {
        ledger::ApiClient a;
        a.connectToService(directory.path(), false);
        a.login("owner", "Password-42");
        const auto logs = a.call("audit_list", {{"ledger_id", id}});
        QCOMPARE(logs.value("total").toInt(), 1);
        QCOMPARE(a.call("audit_detail", {{"ledger_id", id}, {"log_id", related_log}})
                     .value("action")
                     .toString(),
                 QStringLiteral("submit_expense"));
        QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                                 a.call("query_expenses", {{"ledger_id", id}}));
    }
    QVERIFY(backend.waitForFinished(4000));
    QCOMPARE(backend.exitCode(), 0);
}

void IpcTests::unobservedWriteReplySurvivesRestartWithoutDuplicate() {
    QTemporaryDir dir;
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", dir.path(), "--idle-timeout", "60"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    ledger::ApiClient api;
    api.connectToService(dir.path(), false);
    api.call("register", {{"username", "owner"}, {"password", "Password-42"}});
    const auto user = api.login("owner", "Password-42");
    const auto book = api.call("create_ledger", {{"name", "Lost reply"}});
    const auto token =
        api.call("login", {{"username", "owner"}, {"password", "Password-42"}}).value("token");
    QJsonObject form{
        {"ledger_id", book.value("id")},
        {"amount", "12.34"},
        {"date", "2026-11-10"},
        {"description", "Unobserved reply"},
        {"payer_id", user.value("id")},
        {"split_method", "equal"},
        {"request_key", "stable-after-restart"},
        {"splits", QJsonArray{QJsonObject{{"user_id", user.value("id")}, {"value", ""}}}}};
    QLocalSocket socket;
    socket.connectToServer(ledger::localServerName(dir.path()));
    QVERIFY(socket.waitForConnected());
    const auto frame =
        QJsonDocument(
            QJsonObject{
                {"id", 1}, {"method", "expense_submit"}, {"params", form}, {"token", token}})
            .toJson(QJsonDocument::Compact) +
        '\n';
    QCOMPARE(socket.write(frame), qint64(frame.size()));
    socket.flush();
    QVERIFY(socket.waitForReadyRead(10000));
    // Intentionally discard the response without reading its content.
    socket.abort();
    backend.kill();
    QVERIFY(backend.waitForFinished());
    backend.start();
    QVERIFY(backend.waitForStarted());
    api.connectToService(dir.path(), false);
    QVERIFY(api.call("expense_write_status",
                     {{"ledger_id", book.value("id")}, {"request_key", "stable-after-restart"}})
                .value("found")
                .toBool());
    QVERIFY(api.call("expense_submit", form).value("reused").toBool());
    QCOMPARE(api.call("expense_list", {{"ledger_id", book.value("id")}})
                 .value("expenses")
                 .toArray()
                 .size(),
             1);
    backend.kill();
    QVERIFY(backend.waitForFinished());
}
void IpcTests::frameLimitsApplyPerMessageAndRejectFractionalIds() {
    QTemporaryDir dir;
    QProcess backend;
    backend.setProgram(QString::fromUtf8(LEDGER_SERVICE_PATH));
    backend.setArguments({"--data-dir", dir.path(), "--idle-timeout", "60"});
    backend.start();
    QVERIFY(backend.waitForStarted());
    ledger::ApiClient api;
    api.connectToService(dir.path(), false);
    QVERIFY_THROWS_EXCEPTION(ledger::ServiceError,
                             api.call("ping", {{"pad", QString(1100000, 'x')}}));
    QCOMPARE(api.call("ping").value("version").toString(), QString("0.9.3"));
    QLocalSocket socket;
    socket.connectToServer(ledger::localServerName(dir.path()));
    QVERIFY(socket.waitForConnected());
    QByteArray frames;
    for (int n = 1; n <= 2; ++n)
        frames += QJsonDocument(QJsonObject{{"id", n},
                                            {"method", "ping"},
                                            {"params", QJsonObject{{"pad", QString(600000, 'x')}}}})
                      .toJson(QJsonDocument::Compact) +
                  '\n';
    QCOMPARE(socket.write(frames), qint64(frames.size()));
    socket.flush();
    QByteArray replies;
    QElapsedTimer timer;
    timer.start();
    while (replies.count('\n') < 2 && timer.elapsed() < 10000) {
        socket.waitForReadyRead(1000);
        replies += socket.readAll();
    }
    QCOMPARE(replies.count('\n'), 2);
    for (const auto& line : replies.split('\n'))
        if (!line.isEmpty()) QVERIFY(QJsonDocument::fromJson(line).object().value("ok").toBool());
    socket.write("{\"id\":1.5,\"method\":\"ping\",\"params\":{}}\n");
    socket.flush();
    QVERIFY(socket.waitForReadyRead(5000));
    QCOMPARE(QJsonDocument::fromJson(socket.readAll())
                 .object()
                 .value("error")
                 .toObject()
                 .value("code")
                 .toString(),
             QString("bad_request"));
    socket.abort();
    backend.kill();
    QVERIFY(backend.waitForFinished());
}
QTEST_GUILESS_MAIN(IpcTests)
#include "ipc_tests.moc"
