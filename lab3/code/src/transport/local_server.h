#pragma once

#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

namespace ledger {
class IdentityService;

class LocalServer final : public QObject {
public:
    LocalServer(IdentityService& service, int idle_seconds, QObject* parent = nullptr);
    ~LocalServer() override;
    bool listen(const QString& name);

private:
    void acceptConnections();
    void consume(::QLocalSocket* socket);
    IdentityService& service_;
    QLocalServer server_;
    QTimer idle_timer_;
    QHash<::QLocalSocket*, QByteArray> buffers_;
};
}  // namespace ledger
