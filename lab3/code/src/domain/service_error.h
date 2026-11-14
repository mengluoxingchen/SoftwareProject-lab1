#pragma once

#include <QString>
#include <stdexcept>
#include <utility>

namespace ledger {

class ServiceError final : public std::runtime_error {
public:
    ServiceError(QString code, QString message, QString field = {})
        : std::runtime_error(message.toStdString()),
          code_(std::move(code)),
          message_(std::move(message)),
          field_(std::move(field)) {}
    [[nodiscard]] const QString& code() const { return code_; }
    [[nodiscard]] const QString& message() const { return message_; }
    [[nodiscard]] const QString& field() const { return field_; }

private:
    QString code_;
    QString message_;
    QString field_;
};

}  // namespace ledger
