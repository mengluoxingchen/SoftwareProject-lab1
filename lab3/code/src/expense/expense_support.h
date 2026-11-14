#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QUuid>
#include <stdexcept>
#include <utility>

#include "domain/service_error.h"

namespace ledger::expense_detail {
[[noreturn]] inline void invalid(const QString& message, const QString& field) {
    throw ServiceError("validation", message, field);
}
inline QString text(const QJsonObject& object, const QString& key, int maximum = 500) {
    const auto value = object.value(key);
    if (!value.isUndefined() && !value.isString()) {
        invalid(QStringLiteral("字段格式不正确。"), key);
    }
    if (value.toString().size() > maximum) {
        invalid(QStringLiteral("输入内容超出允许长度。"), key);
    }
    const QString result = value.toString().trimmed();
    return result.isNull() ? QStringLiteral("") : result;
}
inline QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
inline QString json(const QJsonObject& value) {
    return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}
inline bool editable(const QString& role, const QString& creator, const QString& actor,
                     const QString& status) {
    return role != "read_only" && status != "settled" && status != "withdrawn" &&
           (creator == actor || (role == "admin" && status != "draft"));
}
class Transaction final {
public:
    explicit Transaction(QSqlDatabase database) : database_(std::move(database)) {
        if (!database_.transaction()) {
            throw std::runtime_error("Cannot begin expense transaction.");
        }
    }
    ~Transaction() {
        if (!committed_) {
            database_.rollback();
        }
    }
    void commit() {
        if (!database_.commit()) {
            throw std::runtime_error("Cannot commit expense transaction.");
        }
        committed_ = true;
    }

private:
    QSqlDatabase database_;
    bool committed_ = false;
};
}  // namespace ledger::expense_detail
