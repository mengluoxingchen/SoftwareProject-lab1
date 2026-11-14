#include "transport/endpoint.h"

#include <QCryptographicHash>
#include <QDir>

namespace ledger {
QString normalizedDataDirectory(const QString& directory) {
    const QDir path(directory);
    const QString canonical = path.canonicalPath();
    return QDir::cleanPath(canonical.isEmpty() ? path.absolutePath() : canonical);
}
QString localServerName(const QString& directory) {
    const QByteArray key = normalizedDataDirectory(directory).toCaseFolded().toUtf8();
    return QStringLiteral("SharedLedger-") +
           QString::fromLatin1(
               QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(32));
}
}  // namespace ledger
