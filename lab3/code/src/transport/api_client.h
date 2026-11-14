#pragma once

#include <QJsonObject>
#include <QLocalSocket>

namespace ledger {

class ApiClient final {
public:
    ApiClient() = default;
    ~ApiClient();
    void connectToService(const QString& data_directory, bool auto_start = true);
    QJsonObject call(const QString& method, const QJsonObject& parameters = {});
    QJsonObject login(const QString& username, const QString& password);
    void logout();
    [[nodiscard]] QString dataDirectory() const { return data_directory_; }
    [[nodiscard]] QString currentUserId() const { return user_id_; }
    [[nodiscard]] bool isAuthenticated() const { return !token_.isEmpty(); }

private:
    QLocalSocket socket_;
    QByteArray receive_buffer_;
    QString token_;
    QString user_id_;
    QString data_directory_;
    bool auto_start_ = true;
    int next_id_ = 0;
};

}  // namespace ledger
