#pragma once

#include <QString>

namespace ledger {
inline constexpr qsizetype maximumRequestBytes = 1024 * 1024;
inline constexpr qsizetype maximumReplyBytes = 16 * 1024 * 1024;
[[nodiscard]] QString normalizedDataDirectory(const QString& directory);
[[nodiscard]] QString localServerName(const QString& directory);
}  // namespace ledger
