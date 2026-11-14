#include "transport/local_server.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <cmath>
#include <exception>
#include <limits>

#include "domain/service_error.h"
#include "identity/identity_service.h"
#include "transport/endpoint.h"

namespace ledger {

LocalServer::LocalServer(IdentityService& service, int idle_seconds, QObject* parent)
    : QObject(parent), service_(service) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    server_.setMaxPendingConnections(32);
    idle_timer_.setSingleShot(true);
    idle_timer_.setInterval(idle_seconds * 1000);
    connect(&idle_timer_, &QTimer::timeout, this, [this] {
        if (buffers_.isEmpty()) {
            QCoreApplication::quit();
        }
    });
    connect(&server_, &QLocalServer::newConnection, this, &LocalServer::acceptConnections);
}

LocalServer::~LocalServer() {
    idle_timer_.stop();
    server_.close();
    for (auto* socket : buffers_.keys()) {
        QObject::disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        delete socket;
    }
    buffers_.clear();
}

bool LocalServer::listen(const QString& name) {
    if (!server_.listen(name)) {
        return false;
    }
    // Windows may deliver a connection synchronously inside listen().
    // Do not re-arm an idle timer already stopped by acceptConnections().
    if (buffers_.isEmpty()) {
        idle_timer_.start();
    }
    return true;
}

void LocalServer::acceptConnections() {
    while (auto* socket = server_.nextPendingConnection()) {
        if (buffers_.size() >= 64) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        idle_timer_.stop();
        socket->setReadBufferSize(maximumRequestBytes + 1);
        buffers_.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { consume(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
            socket->deleteLater();
            if (buffers_.isEmpty()) {
                idle_timer_.start();
            }
        });
        if (socket->bytesAvailable() > 0) {
            consume(socket);
        }
    }
}

void LocalServer::consume(::QLocalSocket* socket) {
    auto& buffer = buffers_[socket];
    buffer += socket->readAll();
    while (buffer.contains('\n')) {
        const qsizetype end = buffer.indexOf('\n');
        if (end + 1 > maximumRequestBytes) {
            socket->abort();
            return;
        }
        const QByteArray frame = buffer.left(end);
        buffer.remove(0, end + 1);
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(frame, &parse_error);
        const auto request = document.object();
        QJsonObject response{{"id", request.value("id")}};
        try {
            if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
                !request.value("id").isDouble() || request.value("id").toDouble() < 1 ||
                request.value("id").toDouble() > std::numeric_limits<int>::max() ||
                std::floor(request.value("id").toDouble()) != request.value("id").toDouble() ||
                !request.value("method").isString() ||
                request.value("method").toString().size() > 128 ||
                (!request.value("token").isUndefined() && !request.value("token").isString()) ||
                !request.value("params").isObject()) {
                throw ServiceError(QStringLiteral("bad_request"),
                                   QStringLiteral("请求格式不正确。"));
            }
            const auto result = service_.dispatch(request.value("method").toString(),
                                                  request.value("params").toObject(),
                                                  request.value("token").toString());
            response.insert("ok", true);
            response.insert("result", result);
        } catch (const ServiceError& error) {
            response.insert("ok", false);
            response.insert("error", QJsonObject{{"code", error.code()},
                                                 {"message", error.message()},
                                                 {"field", error.field()}});
        } catch (const std::exception&) {
            response.insert("ok", false);
            response.insert("error",
                            QJsonObject{{"code", "internal"},
                                        {"message", QStringLiteral("操作未完成，请刷新后重试。")}});
        }
        auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() > maximumReplyBytes) {
            bytes =
                QJsonDocument(
                    QJsonObject{
                        {"id", request.value("id")},
                        {"ok", false},
                        {"error",
                         QJsonObject{{"code", "result_too_large"},
                                     {"message",
                                      QStringLiteral(
                                          "结果过大，请缩小查询范围；写操作请先核对保存结果。")}}}})
                    .toJson(QJsonDocument::Compact) +
                '\n';
        }
        if (socket->bytesToWrite() > maximumReplyBytes * 2) {
            socket->abort();
            return;
        }
        socket->write(bytes);
        socket->flush();
    }
    if (buffer.size() >= maximumRequestBytes) socket->abort();
}

}  // namespace ledger
