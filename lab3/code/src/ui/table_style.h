#pragma once

#include <QString>

class QTableWidget;
class QTableWidgetItem;
class QPushButton;

namespace ledger {
void configureTable(QTableWidget* table);
QTableWidgetItem* centeredTableItem(const QString& text = {});
void setTableButton(QTableWidget* table, int row, int column, QPushButton* button);
}  // namespace ledger
