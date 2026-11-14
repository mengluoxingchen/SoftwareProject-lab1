#pragma once

#include <QByteArray>
#include <QString>

namespace ledger {

struct PasswordRecord {
    QByteArray salt;
    QByteArray hash;
    int iterations = 600000;
};

[[nodiscard]] QByteArray secureRandomBytes(int size);
[[nodiscard]] PasswordRecord hashPassword(const QString& password);
[[nodiscard]] bool verifyPassword(const QString& password, const PasswordRecord& record);

}  // namespace ledger
