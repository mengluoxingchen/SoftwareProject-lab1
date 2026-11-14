#include "ui/expense_details_dialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "domain/split.h"
#include "transport/api_client.h"
#include "ui/finance_dialogs.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
QString stateName(const QString& state) {
    const QHash<QString, QString> names{{"draft", "草稿"},       {"pending", "待审核"},
                                        {"approved", "已通过"},  {"rejected", "已驳回"},
                                        {"withdrawn", "已撤回"}, {"settled", "已纳入结算"}};
    return names.value(state, state);
}
QString when(qint64 time) {
    return QDateTime::fromSecsSinceEpoch(time).toString("yyyy-MM-dd HH:mm:ss");
}
QString methodName(const QString& method) {
    const QHash<QString, QString> names{
        {"equal", "按人数均摊"}, {"ratio", "按比例"}, {"fixed", "指定金额"}};
    return names.value(method, method);
}
QLabel* plain(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
QTableWidget* table(const QStringList& headings, QWidget* parent) {
    auto* result = new QTableWidget(0, static_cast<int>(headings.size()), parent);
    configureTable(result);
    result->setHorizontalHeaderLabels(headings);
    result->verticalHeader()->hide();
    result->verticalHeader()->setDefaultSectionSize(48);
    result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);

    result->setMinimumHeight(140);
    result->setMaximumHeight(220);
    return result;
}
void cells(QTableWidget* target, int row, const QStringList& values) {
    for (int col = 0; col < values.size(); ++col) {
        auto* item = centeredTableItem(values[col]);
        item->setToolTip(values[col]);
        target->setItem(row, col, item);
    }
}
QTableWidget* section(QVBoxLayout* layout, const QString& title, const QStringList& headings,
                      const char* name) {
    auto* panel = new QWidget(layout->parentWidget());
    auto* body = new QVBoxLayout(panel);
    body->setContentsMargins(0, 0, 0, 0);
    body->addWidget(plain(title, panel));
    auto* result = table(headings, panel);
    result->setObjectName(QString::fromLatin1(name));
    body->addWidget(result);
    layout->addWidget(panel);
    return result;
}
}  // namespace

ExpenseDetailsDialog::ExpenseDetailsDialog(ApiClient& api, const QJsonObject& book,
                                           const QString& expense_id, bool review_mode,
                                           QWidget* parent)
    : QDialog(parent), api_(api), book_(book), expense_id_(expense_id), review_mode_(review_mode) {
    setObjectName("expenseDetailsDialog");
    setWindowTitle(review_mode ? QStringLiteral("审核账目") : QStringLiteral("账目详情与历史"));
    resize(880, 780);
    auto* body = new QVBoxLayout(this);
    error_ = plain({}, this);
    error_->setObjectName("reviewError");
    error_->setStyleSheet("color:#b42318;");
    body->addWidget(error_);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* sections = new QVBoxLayout(content);
    summary_ = new QFormLayout();
    sections->addLayout(summary_);
    lock_ = plain({}, content);
    lock_->setObjectName("expenseLockState");
    sections->addWidget(lock_);
    sections->addWidget(plain("费用承担明细", content));
    payment_explanation_ = plain({}, content);
    payment_explanation_->setObjectName("expensePaymentExplanation");
    sections->addWidget(payment_explanation_);
    shares_ = table({"费用承担人", "应承担（元）"}, content);
    shares_->setObjectName("expenseDetailShares");
    sections->addWidget(shares_);
    similar_ = section(sections, "相似账目（日期、金额、付款人与说明相同）",
                       {"记录编号", "创建人", "状态", "详情"}, "similarExpenseTable");
    reviews_ = section(sections, "审核记录", {"时间", "审核人", "结果", "原因 / 备注"},
                       "expenseReviewHistory");
    revisions_ = section(sections, "修改历史（选择一行查看修改前后明细）",
                         {"时间", "修改人", "修改原因", "修改前金额", "修改后金额"},
                         "expenseRevisionHistory");
    revision_text_ = new QPlainTextEdit(content);
    revision_text_->setObjectName("expenseRevisionDetails");
    revision_text_->setReadOnly(true);
    revision_text_->setMaximumHeight(190);
    sections->addWidget(revision_text_);
    withdrawals_ =
        section(sections, "撤回记录", {"时间", "撤回人", "原因"}, "expenseWithdrawalHistory");
    for (auto* history_table : {reviews_, revisions_, withdrawals_}) {
        history_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
        history_table->setColumnWidth(0, 185);
        history_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
        history_table->setColumnWidth(1, 95);
    }
    reviews_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    reviews_->setColumnWidth(2, 75);
    for (int column : {3, 4}) {
        revisions_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        revisions_->setColumnWidth(column, 110);
    }
    revisions_->verticalHeader()->setDefaultSectionSize(60);
    finance_summary_ = plain({}, content);
    finance_summary_->setObjectName("expenseFinanceSummary");
    sections->addWidget(finance_summary_);
    net_shares_ = section(sections, "当前有效费用承担（已扣除商家退款）",
                          {"费用承担人", "应承担（元）"}, "expenseNetShares");
    finance_history_ =
        section(sections, "退款与结算后调整（最近100条，双击查看前后明细）",
                {"时间", "类型", "净额变动（元）", "处理人", "原因"}, "expenseFinanceHistory");
    finance_history_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(finance_history_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        try {
            showFinanceEventDialog(
                api_, book_, finance_history_->item(row, 0)->data(Qt::UserRole).toString(), this);
        } catch (const ServiceError& failure) {
            error_->setText(failure.message());
            if (failure.code() == "unauthenticated") {
                emit sessionExpired(failure.message());
                reject();
            }
        }
    });
    sections->addStretch();
    scroll->setWidget(content);
    body->addWidget(scroll, 1);
    review_form_ = new QWidget(this);
    auto* controls = new QVBoxLayout(review_form_);
    controls->setContentsMargins(0, 0, 0, 0);
    confirmed_ = new QCheckBox("已核对相似记录，确认当前账目可通过审核", review_form_);
    confirmed_->setObjectName("reviewSimilarConfirmed");
    controls->addWidget(confirmed_);
    reason_ = new QLineEdit(review_form_);
    reason_->setObjectName("reviewReason");
    reason_->setMaxLength(500);
    reason_->setPlaceholderText("驳回原因（驳回时必填，通过时可填备注）");
    controls->addWidget(reason_);
    auto* decisions = new QHBoxLayout();
    decisions->addStretch();
    auto* reject_button = new QPushButton("驳回账目", review_form_);
    reject_button->setObjectName("rejectExpense");
    decisions->addWidget(reject_button);
    auto* approve = new QPushButton("确认通过", review_form_);
    approve->setObjectName("approveExpense");
    decisions->addWidget(approve);
    controls->addLayout(decisions);
    body->addWidget(review_form_);
    auto* footer = new QDialogButtonBox(this);
    refund_button_ = footer->addButton("登记商家退款", QDialogButtonBox::ActionRole);
    refund_button_->setObjectName("recordExpenseRefund");
    replacement_button_ = footer->addButton("查看当前调整记录", QDialogButtonBox::ActionRole);
    replacement_button_->setObjectName("viewReplacementExpense");
    connect(refund_button_, &QPushButton::clicked, this, [this] {
        try {
            if (recordRefundDialog(api_, book_, record_, this)) {
                reload();
                error_->clear();
            }
        } catch (const ServiceError& failure) {
            error_->setText(failure.message());
            if (failure.code() == "unauthenticated") {
                emit sessionExpired(failure.message());
                reject();
            }
        }
    });
    connect(replacement_button_, &QPushButton::clicked, this, [this] {
        try {
            ExpenseDetailsDialog current(
                api_, book_,
                record_.value("financial").toObject().value("current_expense_id").toString(), false,
                this);
            connect(&current, &ExpenseDetailsDialog::sessionExpired, this,
                    &ExpenseDetailsDialog::sessionExpired);
            current.exec();
        } catch (const ServiceError& failure) {
            error_->setText(failure.message());
            if (failure.code() == "unauthenticated") {
                emit sessionExpired(failure.message());
                reject();
            }
        }
    });
    auto* refresh = footer->addButton("刷新详情", QDialogButtonBox::ActionRole);
    auto* close = footer->addButton("关闭", QDialogButtonBox::RejectRole);
    close->setObjectName("closeExpenseDetails");
    body->addWidget(footer);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(refresh, &QPushButton::clicked, this, [this] {
        try {
            reload();
            error_->clear();
        } catch (const ServiceError& error) {
            error_->setText(error.message());
            if (error.code() == "unauthenticated") {
                emit sessionExpired(error.message());
                this->reject();
            }
        }
    });
    connect(approve, &QPushButton::clicked, this, [this] { decide("approve"); });
    connect(reject_button, &QPushButton::clicked, this, [this] { decide("reject"); });
    connect(revisions_, &QTableWidget::currentCellChanged, this, [this] { showRevision(); });
    reload();
}

void ExpenseDetailsDialog::reload() {
    const QJsonObject reference{{"ledger_id", book_.value("id")}, {"expense_id", expense_id_}};
    record_ = api_.call("expense_detail", reference);
    history_ = api_.call("expense_history", reference);
    while (summary_->rowCount() > 0) {
        summary_->removeRow(0);
    }
    const auto info =
        api_.call("finance_info", {{"ledger_id", book_.value("id")}, {"source_id", expense_id_}});
    const auto form = record_.value("form").toObject();
    for (const auto& field : QList<QPair<QString, QString>>{
             {"消费说明", form.value("description").toString()},
             {"原始金额（元）", form.value("amount").toString()},
             {"消费日期", form.value("date").toString()},
             {"实际付款人", record_.value("payer").toString()},
             {"创建人", record_.value("creator").toString()},
             {"消费类别", form.value("category").toString()},
             {"分摊方式", methodName(form.value("split_method").toString())},
             {"状态", stateName(record_.value("status").toString())},
             {"修改情况", record_.value("corrected").toBool() ? QStringLiteral("已更正")
                                                              : QStringLiteral("暂无修改")}}) {
        summary_->addRow(field.first, plain(field.second, this));
    }
    if (form.value("post_settlement_adjustment").toBool()) {
        summary_->addRow(QStringLiteral("补记原因"),
                         plain(form.value("adjustment_reason").toString(), this));
    }
    lock_->setText(record_.value("locked").toBool()
                       ? QStringLiteral("正在由 %1 编辑，锁到期时间：%2。持锁期间不能审核或撤回。")
                             .arg(record_.value("lock_holder").toString(),
                                  when(record_.value("lock_expires_at").toInteger()))
                       : QStringLiteral("当前未被编辑锁定。"));
    const auto shares = record_.value("shares").toArray();
    const auto stored_money = [](const QString& text) {
        return text == "0.00" ? qint64(0) : parseMoney(text);
    };
    qint64 own_share = 0;
    shares_->setRowCount(static_cast<int>(shares.size()));
    for (int row = 0; row < shares.size(); ++row) {
        const auto item = shares[row].toObject();
        cells(shares_, row, {item.value("username").toString(), item.value("amount").toString()});
        if (item.value("user_id") == form.value("payer_id")) {
            own_share = stored_money(item.value("amount").toString());
        }
    }
    const qint64 prepaid = stored_money(info.value("gross_amount").toString());
    payment_explanation_->setText(
        QStringLiteral("%1已垫付%2元，本人原始应承担%3元。%4\n"
                       "下表是费用归属，不是要求付款人再付一次。此处为原始分摊；退款、后续调整和已"
                       "还款请结合财务明细及“结算与还款”的当前余额核对。")
            .arg(record_.value("payer").toString(), formatMoney(prepaid), formatMoney(own_share),
                 prepaid == own_share ? QStringLiteral("本笔无需其他人归还。")
                                      : QStringLiteral("其他承担人原始合计需归还%1元。")
                                            .arg(formatMoney(prepaid - own_share))));
    const auto similar = record_.value("similar_records").toArray();
    similar_->setRowCount(static_cast<int>(similar.size()));
    for (int row = 0; row < similar.size(); ++row) {
        const auto item = similar[row].toObject();
        cells(similar_, row,
              {item.value("id").toString().left(8), item.value("creator").toString(),
               stateName(item.value("status").toString())});
        auto* view = new QPushButton("查看", similar_);
        view->setObjectName("viewSimilarExpense");
        setTableButton(similar_, row, 3, view);
        connect(view, &QPushButton::clicked, this, [this, item] {
            try {
                ExpenseDetailsDialog nested(api_, book_, item.value("id").toString(), false, this);
                connect(&nested, &ExpenseDetailsDialog::sessionExpired, this,
                        &ExpenseDetailsDialog::sessionExpired);
                nested.exec();
            } catch (const ServiceError& error) {
                error_->setText(error.message());
            }
        });
    }
    const auto reviews = history_.value("reviews").toArray();
    reviews_->setRowCount(static_cast<int>(reviews.size()));
    for (int row = 0; row < reviews.size(); ++row) {
        const auto item = reviews[row].toObject();
        cells(
            reviews_, row,
            {when(item.value("time").toInteger()), item.value("actor").toString(),
             item.value("decision") == "approve" ? QStringLiteral("通过") : QStringLiteral("驳回"),
             item.value("reason").toString()});
    }
    const auto revisions = history_.value("revisions").toArray();
    revisions_->setRowCount(static_cast<int>(revisions.size()));
    for (int row = 0; row < revisions.size(); ++row) {
        const auto item = revisions[row].toObject();
        cells(revisions_, row,
              {when(item.value("time").toInteger()), item.value("actor").toString(),
               item.value("reason").toString(),
               item.value("before").toObject().value("amount").toString(),
               item.value("after").toObject().value("amount").toString()});
    }
    revision_text_->clear();
    if (!revisions.isEmpty()) {
        revisions_->setCurrentCell(static_cast<int>(revisions.size()) - 1, 0);
        showRevision();
    }
    const auto withdrawals = history_.value("withdrawals").toArray();
    withdrawals_->setRowCount(static_cast<int>(withdrawals.size()));
    for (int row = 0; row < withdrawals.size(); ++row) {
        const auto item = withdrawals[row].toObject();
        cells(withdrawals_, row,
              {when(item.value("time").toInteger()), item.value("actor").toString(),
               item.value("reason").toString()});
    }
    refund_button_->setVisible(info.value("can_refund").toBool());
    const bool superseded = info.value("superseded").toBool();
    replacement_button_->setVisible(superseded &&
                                    !info.value("current_expense_id").toString().isEmpty());
    finance_summary_->setVisible(record_.value("has_finance").toBool() ||
                                 record_.value("is_adjustment").toBool());
    finance_summary_->setText(
        superseded ? (info.value("current_expense_id").toString().isEmpty()
                          ? QStringLiteral("此支出已冲销，原记录仅供历史核对；实际还款仍然保留。")
                          : QStringLiteral("此记录已由后续调整替代，原始金额与原始分摊保留。请查看"
                                           "当前调整记录核对有效金额。"))
                   : QStringLiteral("当前原始支出 %1 元，累计商家退款 %2 元，净支出 %3 元。")
                         .arg(info.value("gross_amount").toString(),
                              info.value("refunded_amount").toString(),
                              info.value("net_amount").toString()));
    const auto effective = info.value("effective_shares").toArray();
    net_shares_->setRowCount(effective.size());
    for (int row = 0; row < effective.size(); ++row) {
        const auto share = effective[row].toObject();
        cells(net_shares_, row,
              {share.value("username").toString(), share.value("amount").toString()});
    }
    net_shares_->parentWidget()->setVisible(!superseded &&
                                            info.value("refunded_amount").toString() != "0.00");
    const auto events = info.value("events").toArray();
    finance_history_->setRowCount(events.size());
    for (int row = 0; row < events.size(); ++row) {
        const auto item = events[row].toObject();
        cells(finance_history_, row,
              {when(item.value("occurred_at").toInteger()),
               financeKind(item.value("kind").toString(),
                           item.value("kind") == "correction" &&
                               item.value("new_expense_id").toString().isEmpty()),
               item.value("delta").toString(), item.value("actor").toString(),
               item.value("reason").toString()});
        finance_history_->item(row, 0)->setData(Qt::UserRole, item.value("id"));
    }
    finance_history_->parentWidget()->setVisible(!events.isEmpty());
    confirmed_->setChecked(false);
    confirmed_->setVisible(!similar.isEmpty());
    review_form_->setVisible(review_mode_ && record_.value("can_review").toBool());
    for (auto* section_table : {similar_, reviews_, revisions_, withdrawals_}) {
        section_table->parentWidget()->setVisible(section_table->rowCount() > 0);
    }
    revision_text_->setVisible(!revisions.isEmpty());
    for (auto* section_table :
         {shares_, similar_, reviews_, revisions_, withdrawals_, net_shares_, finance_history_}) {
        section_table->setFixedHeight(
            qMin(240, section_table->horizontalHeader()->height() +
                          section_table->verticalHeader()->defaultSectionSize() *
                              qMax(1, section_table->rowCount()) +
                          4));
    }
}

void ExpenseDetailsDialog::decide(const QString& decision) {
    try {
        api_.call("expense_review", {{"ledger_id", book_.value("id")},
                                     {"expense_id", expense_id_},
                                     {"version", record_.value("version")},
                                     {"decision", decision},
                                     {"reason", reason_->text()},
                                     {"confirm_similar", confirmed_->isChecked()}});
        accept();
    } catch (const ServiceError& error) {
        error_->setText(error.message());
        if (error.code() == "unauthenticated") {
            emit sessionExpired(error.message());
            reject();
        }
    }
}

void ExpenseDetailsDialog::showRevision() {
    const auto revisions = history_.value("revisions").toArray();
    const int row = revisions_->currentRow();
    if (row < 0 || row >= revisions.size()) {
        return;
    }
    const auto revision = revisions[row].toObject();
    QHash<QString, QString> names;
    const auto saved_names = history_.value("names").toObject();
    for (auto it = saved_names.begin(); it != saved_names.end(); ++it) {
        names.insert(it.key(), it.value().toString());
    }
    for (const auto& value : book_.value("members").toArray()) {
        const auto member = value.toObject();
        names.insert(member.value("id").toString(), member.value("username").toString());
    }
    const auto describe = [&](const QJsonObject& form, const QJsonArray& shares) {
        QStringList allocation;
        for (const auto& value : shares) {
            const auto share = value.toObject();
            allocation.append(
                names.value(share.value("user_id").toString(), share.value("username").toString()) +
                " " + share.value("amount").toString() + "元");
        }
        return QStringLiteral(
                   "金额：%1 "
                   "元；日期：%2；实际付款人：%3\n说明：%4；类别：%5\n分摊方式：%7\n费用承担：%6")
            .arg(form.value("amount").toString(), form.value("date").toString(),
                 names.value(form.value("payer_id").toString(), form.value("payer_id").toString()),
                 form.value("description").toString(), form.value("category").toString(),
                 allocation.join("；"), methodName(form.value("split_method").toString()));
    };
    revision_text_->setPlainText(QStringLiteral("修改前（%1）：\n%2\n\n修改后（%3）：\n%4")
                                     .arg(stateName(revision.value("before_status").toString()),
                                          describe(revision.value("before").toObject(),
                                                   revision.value("before_shares").toArray()),
                                          stateName(revision.value("after_status").toString()),
                                          describe(revision.value("after").toObject(),
                                                   revision.value("after_shares").toArray())));
}
}  // namespace ledger
