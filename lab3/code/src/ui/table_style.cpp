#include "ui/table_style.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>

namespace ledger {
void configureTable(QTableWidget* table) {
    table->setShowGrid(true);
    table->setGridStyle(Qt::SolidLine);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
}
QTableWidgetItem* centeredTableItem(const QString& text) {
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    item->setToolTip(text);
    return item;
}
void setTableButton(QTableWidget* table, int row, int column, QPushButton* button) {
    auto* container = new QWidget(table);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(0);
    button->setCursor(Qt::PointingHandCursor);
    button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    layout->addStretch();
    layout->addWidget(button);
    layout->addStretch();
    table->setCellWidget(row, column, container);
}
}  // namespace ledger
