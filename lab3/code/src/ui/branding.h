#pragma once

#include <QIcon>

class QWidget;

namespace ledger {
[[nodiscard]] QIcon sharedLedgerIcon();
QWidget* createBrandWidget(bool compact, QWidget* parent);
}  // namespace ledger
