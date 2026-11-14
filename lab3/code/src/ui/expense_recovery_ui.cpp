#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "domain/write_request.h"
#include "transport/api_client.h"
#include "ui/expense_page.h"
#include "ui/table_style.h"

namespace ledger {
void ExpensePage::queueRecovery() {
    // Keep a bounded interval even while typing continuously.
    if (!loading_ && can_write_ && !recovery_timer_->isActive()) recovery_timer_->start();
}
QJsonObject ExpensePage::recoverySnapshot() const {
    auto form = formData();
    form.remove("lock_token");
    QJsonArray rows;
    for (int row = 0; row < splits_->rowCount(); ++row) {
        // An explicitly excluded former member must not prevent recovering the valid form.
        if (!splits_->item(row, 0)->data(Qt::UserRole + 1).toBool() &&
            splits_->item(row, 0)->checkState() != Qt::Checked)
            continue;
        rows.append(
            QJsonObject{{"id", splits_->item(row, 0)->data(Qt::UserRole).toString()},
                        {"username", splits_->item(row, 1)->data(Qt::UserRole).toString()},
                        {"selected", splits_->item(row, 0)->checkState() == Qt::Checked},
                        {"value", qobject_cast<QLineEdit*>(splits_->cellWidget(row, 2))->text()}});
    }
    return {{"form", form},
            {"rows", rows},
            {"editing_status", editing_status_},
            {"change_reason", change_reason_->text()},
            {"financial_mode", financial_mode_},
            {"financial_source", financial_source_id_},
            {"financial_token", financial_token_},
            {"financial_key", financial_request_key_},
            {"pending_write", pending_write_}};
}
bool ExpensePage::persistRecovery() {
    recovery_timer_->stop();
    if (!dirty_ || loading_ || view_only_ || book_.value("id").toString().isEmpty() ||
        user_.value("id").toString().isEmpty())
        return true;
    try {
        RecoveryStore store(api_.dataDirectory(), user_.value("id").toString(),
                            book_.value("id").toString());
        if (recovery_id_.isEmpty()) {
            recovery_id_ = RecoveryStore::newId();
            recovery_lease_ = store.claim(recovery_id_);
        }
        if (!recovery_lease_)
            throw ServiceError("recovery", QStringLiteral("本地副本被其他窗口占用。"));
        store.write(recovery_id_, recoverySnapshot(), *recovery_lease_);
        recovery_status_->setText(
            QStringLiteral("已保存本地恢复副本（%1），尚未保存到服务或提交审核。")
                .arg(QTime::currentTime().toString("HH:mm:ss")));
        updateRecoveryButton();
        return true;
    } catch (const ServiceError& error) {
        recovery_status_->setText(error.message());
        return false;
    }
}
void ExpensePage::releaseRecovery() {
    recovery_timer_->stop();
    recovery_lease_.reset();
    recovery_id_.clear();
    pending_write_ = {};
    if (recovery_status_) recovery_status_->clear();
}
void ExpensePage::removeRecovery() {
    recovery_timer_->stop();
    if (!recovery_id_.isEmpty() && recovery_lease_) {
        RecoveryStore store(api_.dataDirectory(), user_.value("id").toString(),
                            book_.value("id").toString());
        store.remove(recovery_id_, *recovery_lease_);
    }
    releaseRecovery();
    updateRecoveryButton();
}
void ExpensePage::updateRecoveryButton() {
    if (!recovery_button_) return;
    recovery_button_->setVisible(can_write_);
    if (!can_write_) return;
    try {
        RecoveryStore store(api_.dataDirectory(), user_.value("id").toString(),
                            book_.value("id").toString());
        recovery_button_->setText(QStringLiteral("本地恢复副本（%1）").arg(store.entries().size()));
    } catch (const ServiceError&) {
        recovery_button_->setText(QStringLiteral("本地恢复副本"));
    }
}
void ExpensePage::viewRecovery(const QJsonObject& envelope) {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("查看本地输入副本（只读）"));
    dialog.resize(700, 580);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QTextEdit(&dialog);
    text->setReadOnly(true);
    const auto snapshot = envelope.value("snapshot").toObject();
    const auto form = snapshot.value("form").toObject();
    const auto rows = snapshot.value("rows").toArray();
    QString payer;
    QStringList parts;
    for (const auto& item : rows) {
        const auto row = item.toObject();
        if (row.value("id") == form.value("payer_id")) payer = row.value("username").toString();
        parts.append(
            QStringLiteral("%1 %2 · 比例/指定金额：%3")
                .arg(row.value("selected").toBool() ? QStringLiteral("☑") : QStringLiteral("☐"),
                     row.value("username").toString(), row.value("value").toString()));
    }
    const QString method = form.value("split_method").toString();
    QStringList lines{
        QStringLiteral("本地副本仅保留输入，保存或提交结果请以服务器账目为准。"),
        QStringLiteral("副本时间：%1")
            .arg(QDateTime::fromSecsSinceEpoch(envelope.value("saved_at").toInteger())
                     .toString("yyyy-MM-dd HH:mm:ss")),
        QStringLiteral("消费金额：%1 元").arg(form.value("amount").toString()),
        QStringLiteral("消费日期：%1").arg(form.value("date").toString()),
        QStringLiteral("实际付款人：%1").arg(payer),
        QStringLiteral("消费说明：%1").arg(form.value("description").toString()),
        QStringLiteral("消费类别：%1").arg(form.value("category").toString()),
        QStringLiteral("分摊方式：%1")
            .arg(method == "ratio"   ? QStringLiteral("按比例")
                 : method == "fixed" ? QStringLiteral("指定金额")
                                     : QStringLiteral("按人数均摊")),
        QStringLiteral("费用承担明细："),
        parts.join("\n"),
        QStringLiteral("修改/调整原因：%1").arg(snapshot.value("change_reason").toString())};
    if (!snapshot.value("pending_write").toObject().isEmpty())
        lines.append(QStringLiteral("曾尝试保存或提交，请先核对服务器结果，勿据此重新新增。"));
    text->setPlainText(lines.join("\n\n"));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
void ExpensePage::showRecoveryCopies() {
    action([this] {
        if (!api_.isAuthenticated() || api_.currentUserId() != user_.value("id").toString() ||
            !can_write_)
            throw ServiceError("forbidden",
                               QStringLiteral("请使用副本所属账号登录并选择可编辑账本。"));
        RecoveryStore store(api_.dataDirectory(), user_.value("id").toString(),
                            book_.value("id").toString());
        QDialog dialog(this);
        dialog.setObjectName("recoveryCopiesDialog");
        dialog.setWindowTitle(QStringLiteral("本地恢复副本"));
        dialog.resize(1000, 400);
        auto* layout = new QVBoxLayout(&dialog);
        auto* help = new QLabel(QStringLiteral("这些副本仅用于恢复未保存输入，不会自动提交。"
                                               "其他窗口正在编辑的副本须关闭该窗口后再恢复。"),
                                &dialog);
        help->setWordWrap(true);
        layout->addWidget(help);
        auto* table = new QTableWidget(0, 4, &dialog);
        table->setObjectName("recoveryCopiesTable");
        configureTable(table);
        table->setHorizontalHeaderLabels({"保存时间", "消费说明", "类型 / 状态", "操作"});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        layout->addWidget(table);
        for (const auto& id : store.entries()) {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setRowHeight(row, 58);
            QJsonObject envelope;
            QString error;
            try {
                envelope = store.read(id);
            } catch (const ServiceError& e) {
                error = e.message();
            }
            auto lease = store.claim(id);
            const auto snapshot = envelope.value("snapshot").toObject();
            const auto form = snapshot.value("form").toObject();
            table->setItem(
                row, 0,
                centeredTableItem(envelope.isEmpty() ? QStringLiteral("不可读")
                                                     : QDateTime::fromSecsSinceEpoch(
                                                           envelope.value("saved_at").toInteger())
                                                           .toString("MM-dd HH:mm:ss")));
            table->setItem(row, 1, centeredTableItem(form.value("description").toString()));
            table->setItem(
                row, 2,
                centeredTableItem(!error.isEmpty() ? QStringLiteral("校验失败，原文件保留")
                                  : !lease         ? QStringLiteral("其他窗口正在编辑")
                                                   : QStringLiteral("可查看 / 恢复")));
            auto* actions = new QWidget(table);
            auto* buttons = new QHBoxLayout(actions);
            buttons->setContentsMargins(2, 2, 2, 2);
            auto* restore = new QPushButton(QStringLiteral("恢复"), actions);
            restore->setObjectName("restoreRecovery");
            restore->setEnabled(lease && error.isEmpty());
            auto* view = new QPushButton(QStringLiteral("查看"), actions);
            view->setEnabled(error.isEmpty());
            auto* remove = new QPushButton(QStringLiteral("删除"), actions);
            remove->setEnabled(lease != nullptr);
            for (auto* button : {restore, view, remove}) buttons->addWidget(button);
            table->setCellWidget(row, 3, actions);
            lease.reset();
            connect(view, &QPushButton::clicked, &dialog,
                    [this, envelope] { viewRecovery(envelope); });
            connect(restore, &QPushButton::clicked, &dialog, [this, &dialog, id] {
                dialog.accept();
                action([this, id] { restoreRecovery(id); });
            });
            connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, id] {
                if (QMessageBox::question(
                        &dialog, QStringLiteral("删除本地副本"),
                        QStringLiteral("确定删除这份本地输入？此操作不会删除服务器账目。"),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                    return;
                action([&] {
                    RecoveryStore scoped(api_.dataDirectory(), user_.value("id").toString(),
                                         book_.value("id").toString());
                    auto claim = scoped.claim(id);
                    if (!claim)
                        throw ServiceError("recovery", QStringLiteral("副本正在其他窗口使用。"));
                    scoped.remove(id, *claim);
                    dialog.accept();
                    updateRecoveryButton();
                });
            });
        }
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(close);
        dialog.exec();
    });
}
void ExpensePage::restoreRecovery(const QString& id) {
    if (!confirmLeave()) return;
    if (!api_.isAuthenticated() || api_.currentUserId() != user_.value("id").toString())
        throw ServiceError("forbidden", QStringLiteral("请重新登录副本所属账号。"));
    RecoveryStore store(api_.dataDirectory(), user_.value("id").toString(),
                        book_.value("id").toString());
    auto lease = store.claim(id);
    if (!lease) throw ServiceError("recovery", QStringLiteral("副本正在其他窗口使用。"));
    const auto envelope = store.read(id);
    const auto snapshot = envelope.value("snapshot").toObject();
    const auto form = snapshot.value("form").toObject();
    const auto fresh = api_.call("ledger_details", {{"ledger_id", book_.value("id")}});
    if (!fresh.value("capabilities").toObject().value("write_expense").toBool())
        throw ServiceError("forbidden", QStringLiteral("当前账本无编辑权限，副本保留。"));
    const auto pending = snapshot.value("pending_write").toObject();
    if (!pending.isEmpty()) {
        const auto result =
            api_.call(pending.value("method") == "finance_adjustment" ? "finance_write_status"
                                                                      : "expense_write_status",
                      {{"ledger_id", book_.value("id")}, {"request_key", pending.value("key")}});
        if (result.value("found").toBool()) {
            message(
                QStringLiteral("此副本对应的操作已成功保存，请打开服务器账目核对。副本保留供查看，"
                               "不会再次提交。"));
            viewRecovery(envelope);
            return;
        }
    }
    QSet<QString> current;
    for (const auto& member : fresh.value("members").toArray())
        current.insert(member.toObject().value("id").toString());
    if ((!form.value("payer_id").toString().isEmpty() &&
         !current.contains(form.value("payer_id").toString()))) {
        viewRecovery(envelope);
        throw ServiceError("conflict",
                           QStringLiteral("原付款人已退出账本，副本保留，请核对后重新填写。"));
    }
    for (const auto& row : snapshot.value("rows").toArray()) {
        if (!current.contains(row.toObject().value("id").toString())) {
            viewRecovery(envelope);
            throw ServiceError("conflict",
                               QStringLiteral("成员已变化，副本保留供查看，请核对后重新填写。"));
        }
    }
    const QString mode = snapshot.value("financial_mode").toString();
    QJsonObject acquired;
    if (!form.value("expense_id").toString().isEmpty()) {
        const auto record = api_.call("expense_detail", {{"ledger_id", book_.value("id")},
                                                         {"expense_id", form.value("expense_id")}});
        if (record.value("version") != form.value("version") ||
            !record.value("can_edit").toBool()) {
            viewRecovery(envelope);
            throw ServiceError(
                "conflict",
                QStringLiteral("服务器账目已更新或不可编辑。未覆盖服务器内容，副本保留。"));
        }
        acquired = api_.call("expense_lock_acquire", {{"ledger_id", book_.value("id")},
                                                      {"expense_id", form.value("expense_id")},
                                                      {"version", form.value("version")}});
    }
    if (mode == "correction") {
        const auto source = api_.call(
            "finance_info",
            {{"ledger_id", book_.value("id")}, {"source_id", snapshot.value("financial_source")}});
        if (fresh.value("role") != "admin" ||
            source.value("token") != snapshot.value("financial_token") ||
            source.value("superseded").toBool() || source.value("status") != "settled") {
            viewRecovery(envelope);
            throw ServiceError("conflict",
                               QStringLiteral("原账目财务状态已变化，副本保留，请重新发起纠错。"));
        }
    }
    book_ = fresh;
    fillForm({{"id", form.value("expense_id")},
              {"version", form.value("version")},
              {"status", snapshot.value("editing_status")},
              {"form", form}});
    {
        const QScopedValueRollback<bool> guard(loading_, true);
        for (const auto& item : snapshot.value("rows").toArray()) {
            const auto saved = item.toObject();
            for (int row = 0; row < splits_->rowCount(); ++row) {
                if (splits_->item(row, 0)->data(Qt::UserRole) == saved.value("id").toString()) {
                    splits_->item(row, 0)->setCheckState(
                        saved.value("selected").toBool() ? Qt::Checked : Qt::Unchecked);
                    qobject_cast<QLineEdit*>(splits_->cellWidget(row, 2))
                        ->setText(saved.value("value").toString());
                }
            }
        }
        change_reason_->setText(snapshot.value("change_reason").toString());
        financial_mode_ = mode;
        financial_source_id_ = snapshot.value("financial_source").toString();
        financial_token_ = snapshot.value("financial_token").toString();
        financial_request_key_ = snapshot.value("financial_key").toString();
        if (!mode.isEmpty()) {
            reason_panel_->show();
            draft_button_->hide();
            findChild<QPushButton*>("reacquireExpenseLock")->hide();
            reason_label_->setText(mode == "correction" ? QStringLiteral("纠错原因 *")
                                                        : QStringLiteral("补记原因 *"));
            form_title_->setText(mode == "correction" ? QStringLiteral("恢复结算后纠错")
                                                      : QStringLiteral("恢复结算后补记"));
            submit_button_->setText(QStringLiteral("核对并提交"));
        }
    }
    lock_token_ = acquired.value("lock_token").toString();
    recovery_id_ = id;
    recovery_lease_ = std::move(lease);
    pending_write_ = pending;
    dirty_ = true;
    last_activity_.start();
    stack_->setCurrentIndex(1);
    if (editing_id_.isEmpty() && mode.isEmpty())
        form_title_->setText(QStringLiteral("恢复未保存账目"));
    preview();
    persistRecovery();
    message(QStringLiteral("已恢复输入。请核对后手动保存或提交，尚未自动入账。"));
}
QJsonObject ExpensePage::prepareWrite(const QString& method, QJsonObject parameters) {
    QString key = pending_write_.value("key").toString();
    if (key.isEmpty()) key = RecoveryStore::newId();
    parameters["request_key"] = key;
    auto safe = parameters;
    safe.remove("lock_token");
    pending_write_ = {{"method", method}, {"key", key}, {"params", safe}};
    dirty_ = true;
    // Flush before a synchronous RPC so a crash during the call retains its request key.
    if (!persistRecovery())
        throw ServiceError(
            "recovery", QStringLiteral("无法保存本地恢复副本，请检查磁盘后重试；本次尚未发送。"));
    return parameters;
}
bool ExpensePage::reconcileWrite() {
    if (pending_write_.isEmpty()) return false;
    const bool finance = pending_write_.value("method") == "finance_adjustment";
    const auto result =
        api_.call(finance ? "finance_write_status" : "expense_write_status",
                  {{"ledger_id", book_.value("id")}, {"request_key", pending_write_.value("key")}});
    if (!result.value("found").toBool()) return false;
    // Preserve later edits and make the successful server operation explicit. Never replay it.
    persistRecovery();
    dirty_ = false;
    releaseLock();
    releaseRecovery();
    stack_->setCurrentIndex(0);
    reloadList();
    updateRecoveryButton();
    message(QStringLiteral(
        "上次操作已成功保存，本次未重复提交。请核对服务器账目；本地副本保留供查看或删除。"));
    return true;
}
}  // namespace ledger
