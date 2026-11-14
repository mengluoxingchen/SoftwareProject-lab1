#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/main_window.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
QLabel* plain(const QString& text, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
QTableWidget* table(const QStringList& headings, const char* name, QWidget* parent) {
    auto* result = new QTableWidget(0, static_cast<int>(headings.size()), parent);
    result->setObjectName(QString::fromLatin1(name));
    configureTable(result);
    result->setHorizontalHeaderLabels(headings);
    result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result->verticalHeader()->hide();
    result->verticalHeader()->setDefaultSectionSize(50);
    result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);
    result->setMinimumHeight(150);
    result->setMaximumHeight(300);
    return result;
}
bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& yes) {
    QMessageBox dialog(QMessageBox::Warning, title, text, QMessageBox::Yes | QMessageBox::No,
                       parent);
    dialog.setTextFormat(Qt::PlainText);
    dialog.setDefaultButton(QMessageBox::No);
    dialog.button(QMessageBox::Yes)->setText(yes);
    dialog.button(QMessageBox::Yes)->setProperty("danger", yes == QStringLiteral("确认删除"));
    dialog.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    return dialog.exec() == QMessageBox::Yes;
}
QString dateText(const QJsonValue& value) {
    return QDateTime::fromSecsSinceEpoch(value.toInteger()).toString("yyyy-MM-dd HH:mm:ss");
}
QString actionName(const QString& action) {
    const QHash<QString, QString> names{
        {"archive_ledger", "归档账本"},      {"delete_ledger_data", "删除账本业务数据"},
        {"restore_ledger", "恢复账本"},      {"create_ledger", "创建账本"},
        {"invite_member", "邀请成员"},       {"accept_invitation", "接受邀请"},
        {"change_role", "调整角色"},         {"create_settlement", "生成结算"},
        {"record_repayment", "登记还款"},    {"approve_expense", "通过审核"},
        {"reject_expense", "驳回账目"},      {"submit_expense", "提交账目"},
        {"save_expense_draft", "保存草稿"},  {"revise_expense", "修改并重新提交"},
        {"withdraw_expense", "撤回账目"},    {"record_refund", "登记商家退款"},
        {"record_supplement", "结算后补记"}, {"correct_settled_expense", "结算后纠错"},
        {"exit_ledger", "退出账本"},         {"transfer_admin", "交接管理员"}};
    return names.value(action, action);
}
}  // namespace

QWidget* MainWindow::createLedgerCloseControls(QWidget* parent) {
    close_ledger_panel_ = new QWidget(parent);
    auto* row = new QHBoxLayout(close_ledger_panel_);
    row->setContentsMargins(0, 0, 0, 0);
    close_ledger_hint_ = plain(QString(), close_ledger_panel_);
    row->addWidget(close_ledger_hint_, 1);
    close_ledger_button_ = new QPushButton(QStringLiteral("归档 / 删除账本"), close_ledger_panel_);
    close_ledger_button_->setObjectName("closeLedgerButton");
    close_ledger_button_->setCursor(Qt::PointingHandCursor);
    row->addWidget(close_ledger_button_);
    connect(close_ledger_button_, &QPushButton::clicked, this, &MainWindow::closeCurrentLedger);
    close_ledger_panel_->hide();
    return close_ledger_panel_;
}

QWidget* MainWindow::createClosedLedgerSection(QWidget* parent) {
    auto* panel = new QWidget(parent);
    auto* body = new QVBoxLayout(panel);
    body->setContentsMargins(0, 0, 0, 0);
    auto* title = plain(QStringLiteral("已归档 / 已删除的账本"), panel);
    title->setObjectName("sectionTitle");
    body->addWidget(title);
    body->addWidget(
        plain(QStringLiteral(
                  "归档可恢复后查看原数据；已删除账本只保留识别信息与日志，不可恢复业务数据。"),
              panel));
    closed_empty_ = plain(QStringLiteral("暂无已关闭账本"), panel);
    body->addWidget(closed_empty_);
    closed_ledgers_ = table({"账本", "处理结果", "处理时间", "操作"}, "closedLedgersTable", panel);
    closed_ledgers_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    body->addWidget(closed_ledgers_);
    return panel;
}

void MainWindow::renderClosedLedgers() {
    const auto books = api_.call("closed_ledgers").value("ledgers").toArray();
    closed_ledgers_->setRowCount(books.size());
    closed_ledgers_->setVisible(!books.isEmpty());
    closed_empty_->setVisible(books.isEmpty());
    for (int row = 0; row < books.size(); ++row) {
        const auto book = books[row].toObject();
        const bool archived = book.value("status").toString() == "archived";
        closed_ledgers_->setItem(row, 0, centeredTableItem(book.value("name").toString()));
        closed_ledgers_->item(row, 0)->setData(Qt::UserRole, book.value("id").toString());
        closed_ledgers_->setItem(row, 1,
                                 centeredTableItem(archived ? QStringLiteral("已归档")
                                                            : QStringLiteral("业务数据已删除")));
        closed_ledgers_->setItem(row, 2, centeredTableItem(dateText(book.value("closed_at"))));
        auto* actions = new QWidget(closed_ledgers_);
        auto* buttons = new QHBoxLayout(actions);
        buttons->setContentsMargins(5, 3, 5, 3);
        buttons->addStretch();
        auto* logs = new QPushButton(QStringLiteral("查看日志"), actions);
        logs->setObjectName("viewClosedLedgerLogs");
        buttons->addWidget(logs);
        connect(logs, &QPushButton::clicked, this,
                [this, book] { showClosedLedgerLogs(book.value("id").toString()); });
        if (archived) {
            auto* restore = new QPushButton(QStringLiteral("恢复账本"), actions);
            restore->setObjectName("restoreLedgerButton");
            buttons->addWidget(restore);
            connect(restore, &QPushButton::clicked, this, [this, book] {
                if (!confirm(
                        this, QStringLiteral("恢复归档账本"),
                        QStringLiteral(
                            "恢复“%"
                            "1”后可继续使用原账目、结算和还款。原待接受邀请已取消，需要重新邀请。")
                            .arg(book.value("name").toString()),
                        QStringLiteral("确认恢复"))) {
                    return;
                }
                runAction([this, book] {
                    api_.call("restore_ledger", {{"ledger_id", book.value("id")},
                                                 {"close_token", book.value("close_token")},
                                                 {"confirmed", true}});
                    refreshWorkspace(book.value("id").toString());
                    notify(QStringLiteral("账本已恢复，原业务数据保留。"));
                });
            });
        }
        buttons->addStretch();
        closed_ledgers_->setCellWidget(row, 3, actions);
    }
}

void MainWindow::closeCurrentLedger() {
    QJsonObject preview;
    runAction([&] {
        preview =
            api_.call("ledger_close_preview", {{"ledger_id", ledgers_->currentData().toString()}});
    });
    if (preview.isEmpty()) {
        return;
    }
    if (!preview.value("can_close").toBool()) {
        notify(preview.value("reason").toString(), true);
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("closeLedgerDialog");
    dialog.setWindowTitle(QStringLiteral("归档或删除账本"));
    dialog.setMinimumWidth(560);
    auto* body = new QVBoxLayout(&dialog);
    body->setContentsMargins(24, 20, 24, 20);
    body->setSpacing(14);
    auto* title =
        plain(QStringLiteral("当前账本：%1").arg(preview.value("name").toString()), &dialog);
    title->setObjectName("sectionTitle");
    body->addWidget(title);
    body->addWidget(
        plain(QStringLiteral(
                  "影响范围：%1笔账目、%2次结算、%3条还款、%5条退款与调整；%4份待接受邀请将取消。")
                  .arg(preview.value("expense_count").toInteger())
                  .arg(preview.value("settlement_count").toInteger())
                  .arg(preview.value("repayment_count").toInteger())
                  .arg(preview.value("pending_invitations").toInteger())
                  .arg(preview.value("finance_count").toInteger()),
              &dialog));
    auto* mode = new QComboBox(&dialog);
    mode->setObjectName("closeLedgerMode");
    mode->addItem(QStringLiteral("归档（保留业务数据，可恢复）"), "archive");
    mode->addItem(QStringLiteral("删除业务数据（不可恢复）"), "delete");
    body->addWidget(mode);
    auto* description = plain(QString(), &dialog);
    body->addWidget(description);
    auto* name_label = plain(QStringLiteral("请输入当前账本名称以确认删除"), &dialog);
    body->addWidget(name_label);
    auto* name = new QLineEdit(&dialog);
    name->setObjectName("closeLedgerConfirmName");
    name->setMaxLength(80);
    body->addWidget(name);
    auto* error = plain(QString(), &dialog);
    error->setObjectName("closeLedgerError");
    error->setStyleSheet("color:#b42318;");
    body->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* proceed = buttons->button(QDialogButtonBox::Ok);
    proceed->setObjectName("confirmCloseLedger");
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    body->addWidget(buttons);
    const auto update_mode = [&] {
        const bool remove = mode->currentData().toString() == "delete";
        name_label->setVisible(remove);
        name->setVisible(remove);
        error->clear();
        description->setText(remove ? QStringLiteral("将永久删除账目、分摊、审核/"
                                                     "修改历史、通知、结算快照、还款和邀请。操作日"
                                                     "志、账本识别及日志权限依据保留。")
                                    : QStringLiteral("原账目、结算与还款保留，账本移出工作列表；可"
                                                     "在下方已关闭账本区域恢复。操作日志保留。"));
        proceed->setText(remove ? QStringLiteral("继续删除") : QStringLiteral("继续归档"));
        proceed->setProperty("danger", remove);
        proceed->style()->unpolish(proceed);
        proceed->style()->polish(proceed);
    };
    connect(mode, &QComboBox::currentIndexChanged, &dialog, update_mode);
    connect(name, &QLineEdit::textChanged, error, &QLabel::clear);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update_mode();
    QString closed_status;
    connect(proceed, &QPushButton::clicked, &dialog, [&] {
        const bool remove = mode->currentData().toString() == "delete";
        if (remove && name->text().trimmed() != preview.value("name").toString()) {
            error->setText(QStringLiteral("输入的账本名称不一致，请重新核对。"));
            return;
        }
        if (!confirm(
                &dialog,
                remove ? QStringLiteral("确认删除账本业务数据") : QStringLiteral("确认归档账本"),
                remove ? QStringLiteral(
                             "确认永久删除“%1”的全部业务数据？此操作不可恢复，操作日志仍保留。")
                             .arg(preview.value("name").toString())
                       : QStringLiteral("确认归档“%1”？数据会保留，可稍后恢复。")
                             .arg(preview.value("name").toString()),
                remove ? QStringLiteral("确认删除") : QStringLiteral("确认归档"))) {
            return;
        }
        try {
            const auto result = api_.call(
                "ledger_close",
                {{"ledger_id", preview.value("id")},
                 {"preview_token", preview.value("preview_token")},
                 {"mode", mode->currentData().toString()},
                 {"confirm_name", remove ? name->text() : preview.value("name").toString()},
                 {"confirmed", true}});
            closed_status = result.value("status").toString();
            dialog.accept();
        } catch (const ServiceError& failure) {
            error->setText(failure.message());
            if (failure.code() == "unauthenticated") {
                dialog.reject();
                clearSession();
                auth_error_->setText(failure.message());
            }
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        runAction([&] {
            refreshWorkspace();
            notify(closed_status == "deleted"
                       ? QStringLiteral("账本业务数据已删除，操作日志保留，可在下方查看。")
                       : QStringLiteral("账本已归档，可在下方恢复或查看日志。"));
        });
    }
}

void MainWindow::showClosedLedgerLogs(const QString& id) {
    QJsonObject log_data;
    runAction([&] { log_data = api_.call("closed_ledger_logs", {{"ledger_id", id}}); });
    if (log_data.isEmpty()) {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("closedLedgerLogsDialog");
    dialog.setWindowTitle(QStringLiteral("已关闭账本的操作日志"));
    dialog.resize(1100, 650);
    auto* body = new QVBoxLayout(&dialog);
    body->addWidget(
        plain(QStringLiteral("账本：%1；显示最近1000条日志，已有日志不会随业务数据删除。")
                  .arg(log_data.value("name").toString()),
              &dialog));
    auto* logs = table({"时间", "操作人", "操作", "内容"}, "closedLedgerLogsTable", &dialog);
    logs->setMaximumHeight(QWIDGETSIZE_MAX);
    logs->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    logs->setColumnWidth(0, 185);
    logs->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    logs->setColumnWidth(1, 110);
    logs->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    logs->setColumnWidth(2, 165);
    const auto rows = log_data.value("logs").toArray();
    logs->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto entry = rows[row].toObject();
        logs->setItem(row, 0, centeredTableItem(dateText(entry.value("created_at"))));
        logs->setItem(row, 1, centeredTableItem(entry.value("actor").toString()));
        logs->setItem(row, 2, centeredTableItem(actionName(entry.value("action").toString())));
        logs->setItem(row, 3, centeredTableItem(entry.value("detail").toString()));
    }
    body->addWidget(logs, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    body->addWidget(close);
    dialog.exec();
}
}  // namespace ledger
