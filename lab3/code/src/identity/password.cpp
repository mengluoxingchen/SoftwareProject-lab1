#include "identity/password.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <stdexcept>

namespace ledger {

QByteArray secureRandomBytes(int size) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (int index = 0; index < size; ++index) {
        bytes[index] = static_cast<char>(QRandomGenerator::system()->generate() & 0xffU);
    }
    return bytes;
}

PasswordRecord hashPassword(const QString& password) {
    PasswordRecord record;
    record.salt = secureRandomBytes(16);
    record.hash = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(),
                                                     record.salt, record.iterations, 32);
    if (record.hash.size() != 32) {
        throw std::runtime_error("Password derivation failed.");
    }
    return record;
}

bool verifyPassword(const QString& password, const PasswordRecord& record) {
    if (record.iterations < 100000 || record.iterations > 2000000 || record.salt.size() != 16 ||
        record.hash.size() != 32) {
        return false;
    }
    const QByteArray candidate = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), record.salt, record.iterations, 32);
    if (candidate.size() != record.hash.size()) {
        return false;
    }
    unsigned int difference = 0;
    for (qsizetype index = 0; index < candidate.size(); ++index) {
        difference |= static_cast<unsigned char>(candidate[index]) ^
                      static_cast<unsigned char>(record.hash[index]);
    }
    return difference == 0;
}

}  // namespace ledger
