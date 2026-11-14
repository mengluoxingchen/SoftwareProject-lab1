#include "transport/api_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QThread>
#include <limits>

#include "domain/service_error.h"
#include "transport/endpoint.h"

namespace ledger {
namespace {
[[noreturn]] void connectionError() {
    throw ServiceError(QStringLiteral("connection"),
                       QStringLiteral("无法连接本机记账服务，请稍后重试，并检查提交结果。"));
}
}  // namespace

ApiClient::~ApiClient() {
    try {
        if (socket_.state() == QLocalSocket::ConnectedState) {
            logout();
        }
    } catch (...) { /* A lost connection must not throw from destruction. */
    }
    socket_.abort();
}

void ApiClient::connectToService(const QString& data_directory, bool auto_start) {
    if (!QDir().mkpath(data_directory)) {
        connectionError();
    }
    const QString endpoint = localServerName(data_directory);
    data_directory_ = normalizedDataDirectory(data_directory);
    auto_start_ = auto_start;
    receive_buffer_.clear();
    auto connect_once = [&] {
        socket_.abort();
        socket_.connectToServer(endpoint);
        return socket_.waitForConnected(150);
    };
    if (connect_once()) {
        return;
    }
    if (auto_start) {
        QProcess service;
        service.setProgram(QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("ledger_service.exe")));
        service.setArguments(
            {QStringLiteral("--data-dir"), normalizedDataDirectory(data_directory)});
        if (!service.startDetached()) {
            connectionError();
        }
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        if (connect_once()) {
            return;
        }
        QThread::msleep(25);
    }
    connectionError();
}

QJsonObject ApiClient::call(const QString& method, const QJsonObject& parameters) {
    if (socket_.state() != QLocalSocket::ConnectedState) {
        if (data_directory_.isEmpty()) {
            connectionError();
        }
        // Reconnect only before a new user request; never replay an uncertain write.
        connectToService(data_directory_, auto_start_);
    }
    if (next_id_ == std::numeric_limits<int>::max()) next_id_ = 0;
    const int request_id = ++next_id_;
    const QJsonObject request{
        {"id", request_id}, {"method", method}, {"params", parameters}, {"token", token_}};
    const QByteArray message = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (message.size() > maximumRequestBytes) {
        throw ServiceError("request_too_large",
                           QStringLiteral("输入内容过大，尚未发送；请减少内容后重试。"));
    }
    if (socket_.write(message) != message.size()) {
        socket_.abort();
        receive_buffer_.clear();
        connectionError();
    }
    socket_.flush();
    QElapsedTimer timer;
    timer.start();
    while (!receive_buffer_.contains('\n')) {
        receive_buffer_ += socket_.readAll();
        if (receive_buffer_.size() > maximumReplyBytes || timer.elapsed() >= 10000) {
            socket_.abort();
            receive_buffer_.clear();
            connectionError();
        }
        if (receive_buffer_.contains('\n')) {
            break;
        }
        if (!socket_.waitForReadyRead(10000 - static_cast<int>(timer.elapsed()))) {
            socket_.abort();
            receive_buffer_.clear();
            connectionError();
        }
    }
    const qsizetype end = receive_buffer_.indexOf('\n');
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(receive_buffer_.left(end), &error);
    receive_buffer_.remove(0, end + 1);
    const auto reply = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        reply.value("id").toInt(-1) != request_id || !reply.value("ok").isBool()) {
        socket_.abort();
        receive_buffer_.clear();
        connectionError();
    }
    if (!reply.value("ok").toBool()) {
        const auto failure = reply.value("error").toObject();
        const QString code = failure.value("code").toString();
        if (code == QStringLiteral("unauthenticated")) {
            token_.clear();
            user_id_.clear();
        }
        throw ServiceError(code, failure.value("message").toString(),
                           failure.value("field").toString());
    }
    return reply.value("result").toObject();
}

QJsonObject ApiClient::login(const QString& username, const QString& password) {
    if (isAuthenticated()) {
        logout();
    }
    const auto result =
        call(QStringLiteral("login"), {{"username", username}, {"password", password}});
    token_ = result.value("token").toString();
    user_id_ = result.value("user").toObject().value("id").toString();
    return result.value("user").toObject();
}

void ApiClient::logout() {
    if (token_.isEmpty()) {
        return;
    }
    try {
        call(QStringLiteral("logout"));
    } catch (...) {
        token_.clear();
        user_id_.clear();
        throw;
    }
    token_.clear();
    user_id_.clear();
}

}  // namespace ledger
