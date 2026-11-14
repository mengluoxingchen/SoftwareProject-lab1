#include "ui/settlement_page.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/finance_dialogs.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
QLabel* label(const QString& text, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
QPushButton* button(const QString& text, const char* name, QWidget* parent) {
    auto* result = new QPushButton(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setCursor(Qt::PointingHandCursor);
    return result;
}
QTableWidget* table(const QStringList& headers, const char* name, QWidget* parent) {
    auto* result = new QTableWidget(0, static_cast<int>(headers.size()), parent);
    configureTable(result);
    result->setObjectName(QString::fromLatin1(name));
    result->setProperty("settlementTable", true);
    result->setHorizontalHeaderLabels(headers);
    result->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    result->verticalHeader()->hide();
    result->verticalHeader()->setDefaultSectionSize(46);
    result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);
    result->setMinimumHeight(160);
    return result;
}
void cell(QTableWidget* table, int row, int column, const QString& text) {
    auto* item = centeredTableItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    item->setToolTip(text);
    table->setItem(row, column, item);
}
QString timeText(const QJsonValue& value) {
    return QDateTime::fromSecsSinceEpoch(value.toInteger()).toString("yyyy-MM-dd HH:mm:ss");
}
void renderBalances(QTableWidget* table, const QJsonArray& members) {
    table->setRowCount(members.size());
    for (int row = 0; row < members.size(); ++row) {
        const auto member = members[row].toObject();
        const QStringList keys{"username", "paid", "owed", "repaid", "received", "balance"};
        for (int column = 0; column < keys.size(); ++column) {
            cell(table, row, column, member.value(keys[column]).toString());
        }
        table->item(row, 0)->setData(Qt::UserRole, member.value("user_id").toString());
        const QString balance = member.value("balance").toString();
        table->item(row, 5)->setForeground(balance.startsWith('-') ? QColor("#b42318")
                                                                   : QColor("#25765b"));
    }
}
bool confirm(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox dialog(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No,
                       parent);
    dialog.setTextFormat(Qt::PlainText);
    dialog.button(QMessageBox::Yes)->setText(QStringLiteral("确认"));
    dialog.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    dialog.setDefaultButton(QMessageBox::No);
    return dialog.exec() == QMessageBox::Yes;
}
QStringList balanceHeaders() {
    return {QStringLiteral("成员"),         QStringLiteral("净垫付（元）"),
            QStringLiteral("净承担（元）"), QStringLiteral("已付给别人"),
            QStringLiteral("已收到"),       QStringLiteral("未结清（元）")};
}
}  // namespace

SettlementPage::SettlementPage(ApiClient& api, QWidget* parent) : QWidget(parent), api_(api) {
    setObjectName("settlementPage");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    notice_ = label(QString(), this);
    notice_->setObjectName("settlementNotice");
    outer->addWidget(notice_);
    auto* toolbar = new QHBoxLayout();
    pending_ = label(QString(), this);
    pending_->setObjectName("pendingSettlementSummary");
    toolbar->addWidget(pending_, 1);
    auto* refresh_button = button(QStringLiteral("刷新结算"), "refreshSettlement", this);
    repay_ = button(QStringLiteral("记录还款"), "recordRepayment", this);
    generate_ = button(QStringLiteral("生成结算"), "generateSettlement", this);
    generate_->setToolTip(QStringLiteral("计算已通过账目的分摊并保存快照，不代表已经付款或还清。"));
    repay_->setToolTip(QStringLiteral("真实转账或现金支付后，再登记还款；软件不会自动付款。"));
    toolbar->addWidget(refresh_button);
    toolbar->addWidget(repay_);
    toolbar->addWidget(generate_);
    outer->addLayout(toolbar);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(16);
    auto* personal = new QFrame(content);
    personal->setObjectName("mySettlementCard");
    auto* personal_body = new QVBoxLayout(personal);
    personal_body->setContentsMargins(18, 14, 18, 14);
    personal_body->setSpacing(14);
    auto* personal_title = label(QStringLiteral("我的收还概览"), personal);
    personal_title->setObjectName("personalSectionTitle");
    personal_body->addWidget(personal_title);
    my_status_ = label({}, personal);
    my_status_->setObjectName("mySettlementStatus");
    my_status_->setProperty("summaryValue", true);
    personal_body->addWidget(my_status_);
    auto* metrics = new QHBoxLayout();
    metrics->setSpacing(12);
    const auto metric = [&](const QString& title, const char* name, QLabel*& value) {
        auto* frame = new QFrame(personal);
        frame->setObjectName("personalMetric");
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(5);
        auto* caption = label(title, frame);
        caption->setObjectName("personalMetricCaption");
        layout->addWidget(caption);
        value = label("0.00 元", frame);
        value->setObjectName(QString::fromLatin1(name));
        value->setProperty("personalMetricValue", true);
        layout->addWidget(value);
        metrics->addWidget(frame, 1);
    };
    metric(QStringLiteral("我的净垫付"), "myPaidAmount", my_paid_);
    metric(QStringLiteral("我的净承担"), "myOwedAmount", my_owed_);
    my_paid_->setToolTip(
        QStringLiteral("已纳入结算的实际付款，减去本人收到的商家退款；成员还款另计。"));
    my_owed_->setToolTip(QStringLiteral("已纳入结算且扣除商家退款后，本人最终承担的费用。"));
    metric(QStringLiteral("累计已还出"), "myRepaidAmount", my_repaid_);
    metric(QStringLiteral("累计已收回"), "myReceivedAmount", my_received_);
    personal_body->addLayout(metrics);
    auto* detail_row = new QHBoxLayout();
    detail_row->setSpacing(14);
    const auto detail = [&](const QString& title, const QStringList& headers, const char* name,
                            QLabel*& empty, QTableWidget*& rows, int stretch) {
        auto* frame = new QFrame(personal);
        frame->setObjectName("personalDetailSection");
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);
        auto* heading = label(title, frame);
        heading->setObjectName("personalSectionTitle");
        layout->addWidget(heading);
        empty = label({}, frame);
        empty->setObjectName("personalEmpty");
        layout->addWidget(empty);
        rows = table(headers, name, frame);
        rows->verticalHeader()->setDefaultSectionSize(36);
        rows->setMinimumHeight(80);
        rows->setMaximumHeight(170);
        layout->addWidget(rows);
        layout->addStretch();
        detail_row->addWidget(frame, stretch);
    };
    detail(QStringLiteral("当前收还建议"), {"对方成员", "方向", "金额（元）"},
           "mySettlementDirections", my_directions_empty_, my_directions_, 2);
    detail(QStringLiteral("近期本人收还 · 按登记顺序最近3条"),
           {"时间", "收还对象", "金额（元）", "登记人"}, "myRepaymentHistory", my_history_empty_,
           my_history_, 3);
    my_history_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    personal_body->addLayout(detail_row);
    auto* notes = new QFrame(personal);
    notes->setObjectName("personalScopeNotes");
    auto* notes_body = new QVBoxLayout(notes);
    notes_body->setContentsMargins(12, 9, 12, 9);
    notes_body->setSpacing(5);
    auto* notes_title = label(QStringLiteral("统计口径与权限"), notes);
    notes_title->setObjectName("personalScopeTitle");
    notes_body->addWidget(notes_title);
    my_scope_ = label({}, notes);
    my_scope_->setObjectName("mySettlementScope");
    notes_body->addWidget(my_scope_);
    personal_body->addWidget(notes);
    body->addWidget(personal);
    auto* cards = new QHBoxLayout();
    const auto card = [&](const QString& title, const char* name, QLabel*& value) {
        auto* frame = new QFrame(content);
        frame->setObjectName("settlementCard");
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(18, 14, 18, 14);
        layout->addWidget(label(title, frame));
        value = label(QStringLiteral("0.00 元"), frame);
        value->setObjectName(QString::fromLatin1(name));
        value->setProperty("summaryValue", true);
        layout->addWidget(value);
        cards->addWidget(frame, 1);
    };
    card(QStringLiteral("累计已结算净支出"), "settledTotal", total_);
    card(QStringLiteral("已结算支出的商家退款"), "refundTotal", refunds_);
    card(QStringLiteral("累计已登记还款"), "repaidTotal", repaid_);
    card(QStringLiteral("当前未结清总额"), "outstandingTotal", outstanding_);
    body->addLayout(cards);
    auto* heading = label(QStringLiteral("成员当前余额"), content);
    heading->setObjectName("sectionTitle");
    body->addWidget(heading);
    body->addWidget(label(QStringLiteral("统计已结算支出、已生效补记与纠错，并扣除商家退款。正数为"
                                         "应收，负数为应付；成员还款单独记录。"),
                          content));
    balances_ = table(balanceHeaders(), "memberBalances", content);
    balances_->setMinimumHeight(220);
    body->addWidget(balances_);
    suggestions_ = label(QString(), content);
    suggestions_->setObjectName("settlementSuggestions");
    body->addWidget(suggestions_);
    auto* history_title = label(QStringLiteral("结算历史（最近100次）"), content);
    history_title->setObjectName("sectionTitle");
    body->addWidget(history_title);
    body->addWidget(label(QStringLiteral("每次纳入全部已通过且未结算账目；快照保存生成时的累计余额"
                                         "，后续还款和结算不覆盖旧快照。"),
                          content));
    history_empty_ = label(QStringLiteral("暂无结算记录"), content);
    body->addWidget(history_empty_);
    history_ =
        table({QStringLiteral("生成时间"), QStringLiteral("操作人"), QStringLiteral("本次笔数"),
               QStringLiteral("原始金额（元）"), QStringLiteral("操作")},
              "settlementHistory", content);
    history_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    body->addWidget(history_);
    auto* repayment_title = label(QStringLiteral("还款记录（最近100条）"), content);
    repayment_title->setObjectName("sectionTitle");
    body->addWidget(repayment_title);
    repayment_empty_ = label(QStringLiteral("暂无还款记录"), content);
    body->addWidget(repayment_empty_);
    repayments_ =
        table({QStringLiteral("还款时间"), QStringLiteral("付款人"), QStringLiteral("收款人"),
               QStringLiteral("金额（元）"), QStringLiteral("登记人"), QStringLiteral("备注")},
              "repaymentHistory", content);
    repayments_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    body->addWidget(repayments_);
    auto* finance_title =
        label(QStringLiteral("退款与结算后调整（最近100条，双击查看前后明细）"), content);
    finance_title->setObjectName("sectionTitle");
    body->addWidget(finance_title);
    financial_history_ = table({"发生时间", "类型", "净额变动（元）", "处理人", "原因"},
                               "ledgerFinanceHistory", content);
    financial_history_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    body->addWidget(financial_history_);
    connect(financial_history_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        action([&] {
            showFinanceEventDialog(
                api_, book_, financial_history_->item(row, 0)->data(Qt::UserRole).toString(), this);
        });
    });
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    connect(refresh_button, &QPushButton::clicked, this, &SettlementPage::refresh);
    connect(generate_, &QPushButton::clicked, this, &SettlementPage::generate);
    connect(repay_, &QPushButton::clicked, this, &SettlementPage::recordRepayment);
}

void SettlementPage::setContext(const QJsonObject& book, const QJsonObject& user) {
    book_ = book;
    user_ = user;
    overview_ = {};
    notice_->clear();
    refresh();
}

void SettlementPage::clearContext() {
    book_ = {};
    user_ = {};
    overview_ = {};
    balances_->setRowCount(0);
    history_->setRowCount(0);
    repayments_->setRowCount(0);
    financial_history_->setRowCount(0);
    refunds_->setText("0.00 元");
    pending_->clear();
    notice_->clear();
    suggestions_->clear();
    for (auto* item : {my_status_, my_scope_}) {
        item->clear();
    }
    for (auto* item : {my_paid_, my_owed_, my_repaid_, my_received_}) {
        item->setText("0.00 元");
    }
    my_directions_->setRowCount(0);
    my_history_->setRowCount(0);
    my_directions_->hide();
    my_history_->hide();
    my_directions_empty_->clear();
    my_history_empty_->clear();
    history_empty_->show();
    repayment_empty_->show();
    total_->setText("0.00 元");
    repaid_->setText("0.00 元");
    outstanding_->setText("0.00 元");
    generate_->setEnabled(false);
    repay_->setEnabled(false);
}

void SettlementPage::refresh() {
    if (book_.isEmpty()) {
        return;
    }
    action([this] {
        overview_ = api_.call("settlement_overview", {{"ledger_id", book_.value("id")}});
        render();
    });
}

void SettlementPage::render() {
    pending_->setText(QStringLiteral("待结算：%1 笔，原始金额 %2 元，扣退款后 %3 元")
                          .arg(overview_.value("pending_count").toInt())
                          .arg(overview_.value("pending_amount").toString(),
                               overview_.value("pending_net_amount").toString()));
    const bool write = overview_.value("can_write").toBool();
    generate_->setVisible(write);
    repay_->setVisible(write);
    repay_->setText(overview_.value("can_record_for_others").toBool()
                        ? QStringLiteral("记录还款")
                        : QStringLiteral("登记本人已还款"));
    generate_->setEnabled(write && overview_.value("pending_count").toInt() > 0);
    repay_->setEnabled(write && overview_.value("has_settlements").toBool());
    total_->setText(overview_.value("total_expenses").toString() + QStringLiteral(" 元"));
    refunds_->setText(overview_.value("total_refunds").toString() + QStringLiteral(" 元"));
    repaid_->setText(overview_.value("total_repayments").toString() + QStringLiteral(" 元"));
    outstanding_->setText(overview_.value("outstanding").toString() + QStringLiteral(" 元"));
    renderBalances(balances_, overview_.value("members").toArray());
    renderPersonal();
    QStringList suggestions;
    const auto transfers = overview_.value("suggestions").toArray();
    for (const auto& entry : transfers) {
        if (suggestions.size() >= 20) {
            break;
        }
        const auto transfer = entry.toObject();
        suggestions.append(QStringLiteral("%1 → %2：%3 元")
                               .arg(transfer.value("payer").toString(),
                                    transfer.value("recipient").toString(),
                                    transfer.value("amount").toString()));
    }
    suggestions_->setText(
        !overview_.value("has_settlements").toBool()
            ? QStringLiteral("生成结算后显示应收应付与还款建议。")
        : suggestions.isEmpty()
            ? QStringLiteral("当前已结算款项均已结清。")
            : QStringLiteral("按净余额建议还款：%1%2")
                  .arg(suggestions.join(QStringLiteral("；")),
                       transfers.size() > 20 ? QStringLiteral("（仅显示前20项）") : QString()));
    const auto financial = overview_.value("financial_events").toArray();
    financial_history_->setRowCount(financial.size());
    for (int row = 0; row < financial.size(); ++row) {
        const auto item = financial[row].toObject();
        const QStringList values{timeText(item.value("occurred_at")),
                                 financeKind(item.value("kind").toString(),
                                             item.value("kind") == "correction" &&
                                                 item.value("new_expense_id").toString().isEmpty()),
                                 item.value("delta").toString(), item.value("actor").toString(),
                                 item.value("reason").toString()};
        for (int column = 0; column < values.size(); ++column) {
            cell(financial_history_, row, column, values[column]);
        }
        financial_history_->item(row, 0)->setData(Qt::UserRole, item.value("id"));
    }
    const auto history = overview_.value("history").toArray();
    history_empty_->setVisible(history.isEmpty());
    history_->setVisible(!history.isEmpty());
    history_->setRowCount(history.size());
    for (int row = 0; row < history.size(); ++row) {
        const auto snapshot = history[row].toObject();
        cell(history_, row, 0, timeText(snapshot.value("created_at")));
        cell(history_, row, 1, snapshot.value("actor").toString());
        cell(history_, row, 2, QString::number(snapshot.value("expense_count").toInt()));
        cell(history_, row, 3, snapshot.value("amount").toString());
        auto* view = button(QStringLiteral("查看快照"), "viewSettlementSnapshot", history_);
        setTableButton(history_, row, 4, view);
        connect(view, &QPushButton::clicked, this,
                [this, snapshot] { showSnapshot(snapshot.value("id").toString()); });
    }
    const auto repayments = overview_.value("repayments").toArray();
    repayment_empty_->setVisible(repayments.isEmpty());
    repayments_->setVisible(!repayments.isEmpty());
    repayments_->setRowCount(repayments.size());
    for (int row = 0; row < repayments.size(); ++row) {
        const auto payment = repayments[row].toObject();
        cell(repayments_, row, 0, timeText(payment.value("paid_at")));
        cell(repayments_, row, 1, payment.value("payer").toString());
        cell(repayments_, row, 2, payment.value("recipient").toString());
        cell(repayments_, row, 3, payment.value("amount").toString());
        cell(repayments_, row, 4, payment.value("actor").toString());
        cell(repayments_, row, 5,
             (payment.value("overpayment").toBool() ? QStringLiteral("已确认超额；") : QString()) +
                 payment.value("note").toString());
    }
}

void SettlementPage::renderPersonal() {
    const QString user_id = user_.value("id").toString();
    QJsonObject mine;
    for (const auto& item : overview_.value("members").toArray()) {
        if (item.toObject().value("user_id").toString() == user_id) {
            mine = item.toObject();
            break;
        }
    }
    const QString amount = mine.value("balance").toString();
    const bool owing = amount.startsWith('-');
    my_status_->setText(!overview_.value("has_settlements").toBool()
                            ? QStringLiteral("待生成结算，暂未形成正式待还余额")
                        : mine.isEmpty()   ? QStringLiteral("暂无本人结算明细")
                        : owing            ? QStringLiteral("我还需还款：%1 元").arg(amount.mid(1))
                        : amount != "0.00" ? QStringLiteral("我还可收回：%1 元").arg(amount)
                                           : QStringLiteral("已结清：当前待还 / 待收 0.00 元"));
    my_status_->setStyleSheet(owing ? "color:#b42318;" : "color:#25765b;");
    my_paid_->setText(mine.value("paid").toString("0.00") + " 元");
    my_owed_->setText(mine.value("owed").toString("0.00") + " 元");
    my_repaid_->setText(mine.value("repaid").toString("0.00") + " 元");
    my_received_->setText(mine.value("received").toString("0.00") + " 元");
    my_directions_->setRowCount(0);
    for (const auto& item : overview_.value("suggestions").toArray()) {
        const auto transfer = item.toObject();
        const bool pay = transfer.value("payer_id").toString() == user_id;
        if (pay || transfer.value("recipient_id").toString() == user_id) {
            const int row = my_directions_->rowCount();
            my_directions_->insertRow(row);
            cell(my_directions_, row, 0, transfer.value(pay ? "recipient" : "payer").toString());
            cell(my_directions_, row, 1, pay ? QStringLiteral("需归还") : QStringLiteral("可收回"));
            cell(my_directions_, row, 2, transfer.value("amount").toString());
        }
    }
    my_history_->setRowCount(0);
    for (const auto& item : overview_.value("personal_repayments").toArray()) {
        const auto payment = item.toObject();
        const bool paid = payment.value("payer_id").toString() == user_id;
        if (my_history_->rowCount() < 3 &&
            (paid || payment.value("recipient_id").toString() == user_id)) {
            const int row = my_history_->rowCount();
            my_history_->insertRow(row);
            cell(my_history_, row, 0, timeText(payment.value("paid_at")));
            cell(my_history_, row, 1,
                 paid ? QStringLiteral("还给 ") + payment.value("recipient").toString()
                      : QStringLiteral("收到 ") + payment.value("payer").toString());
            cell(my_history_, row, 2, payment.value("amount").toString());
            cell(my_history_, row, 3, payment.value("actor").toString());
        }
    }
    const auto compact = [](QTableWidget* rows, QLabel* empty, const QString& text) {
        empty->setText(text);
        empty->setVisible(rows->rowCount() == 0);
        rows->setVisible(rows->rowCount() > 0);
        rows->setFixedHeight(qBound(
            80, rows->horizontalHeader()->sizeHint().height() + rows->rowCount() * 36 + 2, 170));
    };
    compact(my_directions_, my_directions_empty_, QStringLiteral("当前没有本人的收还建议。"));
    compact(my_history_, my_history_empty_,
            QStringLiteral("暂无近期本人收还记录，累计金额见上方。"));
    my_scope_->setText(
        QStringLiteral(
            "统计范围：仅已纳入结算的费用，另有 %1 笔待结算；“已纳入结算”不等于成员间已还清。\n"
            "金额口径：净垫付已减本人收到的商家退款，净承担已扣退款；成员还款单独统计。\n"
            "收还方式：按账本净余额合并，多笔费用可互相抵扣。真实转账后再登记，软件不会自动付款。%"
            "2")
            .arg(overview_.value("pending_count").toInt())
            .arg(overview_.value("can_write").toBool()
                     ? QString()
                     : QStringLiteral("\n当前权限：只读成员可查看；实际还款请管理员代登记，或由管理"
                                      "员调整为普通成员后自行登记。")));
}

void SettlementPage::generate() {
    if (overview_.isEmpty()) {
        return;
    }
    if (!confirm(this, QStringLiteral("确认生成结算"),
                 QStringLiteral(
                     "将本账本全部 %1 笔已通过账目、共 %2 "
                     "元纳入结算。结算后这些账目不可直接修改或撤回，当前结果将保存为历史快照。")
                     .arg(overview_.value("pending_count").toInt())
                     .arg(overview_.value("pending_amount").toString()))) {
        return;
    }
    action([this] {
        const auto result =
            api_.call("settlement_create", {{"ledger_id", book_.value("id")},
                                            {"preview_token", overview_.value("preview_token")}});
        refresh();
        message(result.value("reused").toBool() ? QStringLiteral("该次结算已生成，已显示现有结果。")
                                                : QStringLiteral("结算已生成，历史快照已保存。"));
    });
}

void SettlementPage::recordRepayment() {
    if (overview_.isEmpty()) {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName("repaymentDialog");
    dialog.setWindowTitle(QStringLiteral("记录还款"));
    dialog.setMinimumWidth(560);
    auto* body = new QVBoxLayout(&dialog);
    body->setContentsMargins(24, 20, 24, 20);
    body->setSpacing(14);
    auto* title = label(QStringLiteral("记录实际发生的还款"), &dialog);
    title->setObjectName("sectionTitle");
    body->addWidget(title);
    body->addWidget(
        label(QStringLiteral("请先完成真实转账或现金支付，这里只登记记录，不会自动付款。普通成员登"
                             "记本人付出的还款，管理员可代登记。金额须核对实"
                             "际付款人与收款人；超额需再次确认。"),
              &dialog));
    auto* grid = new QGridLayout();
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    auto* payer = new QComboBox(&dialog);
    payer->setObjectName("repaymentPayer");
    auto* recipient = new QComboBox(&dialog);
    recipient->setObjectName("repaymentRecipient");
    const auto members = book_.value("financial_members").toArray().isEmpty()
                             ? book_.value("members").toArray()
                             : book_.value("financial_members").toArray();
    for (const auto& entry : members) {
        const auto member = entry.toObject();
        const QString name =
            member.value("username").toString() +
            (member.value("status") == "exited" ? QStringLiteral("（已退出）") : QString());
        recipient->addItem(name, member.value("id").toString());
        if (overview_.value("can_record_for_others").toBool() ||
            member.value("id") == user_.value("id")) {
            payer->addItem(name, member.value("id").toString());
        }
    }
    payer->setCurrentIndex(payer->findData(user_.value("id").toString()));
    if (recipient->currentData() == payer->currentData() && recipient->count() > 1) {
        recipient->setCurrentIndex(1);
    }
    auto* amount = new QLineEdit(&dialog);
    amount->setObjectName("repaymentAmount");
    amount->setMaxLength(32);
    amount->setPlaceholderText(QStringLiteral("例如20.00"));
    for (const auto& entry : overview_.value("suggestions").toArray()) {
        const auto suggestion = entry.toObject();
        if (suggestion.value("payer_id") == user_.value("id")) {
            recipient->setCurrentIndex(
                recipient->findData(suggestion.value("recipient_id").toString()));
            amount->setText(suggestion.value("amount").toString());
            break;
        }
    }
    auto* time = new QDateTimeEdit(QDateTime::currentDateTime(), &dialog);
    time->setObjectName("repaymentTime");
    time->setCalendarPopup(true);
    time->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    auto* note = new QLineEdit(&dialog);
    note->setObjectName("repaymentNote");
    note->setMaxLength(500);
    note->setPlaceholderText(QStringLiteral("选填，如转账备注"));
    grid->addWidget(label(QStringLiteral("付款人 *"), &dialog), 0, 0);
    grid->addWidget(label(QStringLiteral("收款人 *"), &dialog), 0, 1);
    grid->addWidget(payer, 1, 0);
    grid->addWidget(recipient, 1, 1);
    grid->addWidget(label(QStringLiteral("还款金额（元）*"), &dialog), 2, 0);
    grid->addWidget(label(QStringLiteral("还款时间 *"), &dialog), 2, 1);
    grid->addWidget(amount, 3, 0);
    grid->addWidget(time, 3, 1);
    body->addLayout(grid);
    auto* balance_hint = label(QString(), &dialog);
    balance_hint->setObjectName("repaymentBalanceHint");
    const auto update_hint = [&, balance_hint] {
        QString payer_balance, recipient_balance;
        for (const auto& entry : overview_.value("members").toArray()) {
            const auto row = entry.toObject();
            if (row.value("user_id").toString() == payer->currentData().toString()) {
                payer_balance = row.value("balance").toString();
            }
            if (row.value("user_id").toString() == recipient->currentData().toString()) {
                recipient_balance = row.value("balance").toString();
            }
        }
        balance_hint->setText(
            QStringLiteral("付款人当前未结清：%1 元；收款人：%2 元。正数应收，负数应付。")
                .arg(payer_balance, recipient_balance));
    };
    connect(payer, &QComboBox::currentIndexChanged, &dialog, update_hint);
    connect(recipient, &QComboBox::currentIndexChanged, &dialog, update_hint);
    update_hint();
    body->addWidget(balance_hint);
    body->addWidget(note);
    auto* error = label(QString(), &dialog);
    error->setObjectName("repaymentError");
    error->setStyleSheet("color:#b42318;");
    body->addWidget(error);
    const auto clear_error = [error] { error->clear(); };
    connect(amount, &QLineEdit::textChanged, &dialog, clear_error);
    connect(note, &QLineEdit::textChanged, &dialog, clear_error);
    connect(payer, &QComboBox::currentIndexChanged, &dialog, clear_error);
    connect(recipient, &QComboBox::currentIndexChanged, &dialog, clear_error);
    connect(time, &QDateTimeEdit::dateTimeChanged, &dialog, clear_error);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto* save = buttons->button(QDialogButtonBox::Save);
    save->setText(QStringLiteral("确认记录"));
    save->setObjectName("confirmRepayment");
    save->style()->unpolish(save);
    save->style()->polish(save);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    body->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto token = overview_.value("balance_token");
    connect(save, &QPushButton::clicked, &dialog, [&, this] {
        QJsonObject request{{"ledger_id", book_.value("id")},
                            {"payer_id", payer->currentData().toString()},
                            {"recipient_id", recipient->currentData().toString()},
                            {"amount", amount->text()},
                            {"paid_at", time->dateTime().toSecsSinceEpoch()},
                            {"note", note->text()},
                            {"request_key", key},
                            {"balance_token", token}};
        try {
            try {
                api_.call("settlement_repay", request);
            } catch (const ServiceError& failure) {
                if (failure.code() != "confirmation_required") {
                    throw;
                }
                if (!confirm(&dialog, QStringLiteral("确认超额还款"), failure.message())) {
                    return;
                }
                request["confirm_overpayment"] = true;
                api_.call("settlement_repay", request);
            }
            dialog.accept();
        } catch (const ServiceError& failure) {
            error->setText(failure.message() +
                           (failure.code() == "connection"
                                ? QStringLiteral("请先刷新核对是否已登记，勿重复录入同一实际还款。")
                                : QString()));
            if (failure.code() == "unauthenticated") {
                dialog.reject();
                emit sessionExpired(failure.message());
            }
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        refresh();
        message(QStringLiteral("还款已登记，当前余额已更新；原结算快照保持不变。"));
    }
}

void SettlementPage::showSnapshot(const QString& id) {
    action([this, id] {
        const auto snapshot = api_.call("settlement_detail",
                                        {{"ledger_id", book_.value("id")}, {"settlement_id", id}});
        QDialog dialog(this);
        dialog.setObjectName("settlementSnapshotDialog");
        dialog.setWindowTitle(QStringLiteral("结算历史快照"));
        dialog.resize(1000, 650);
        auto* body = new QVBoxLayout(&dialog);
        body->setContentsMargins(22, 20, 22, 20);
        body->addWidget(label(
            QStringLiteral("生成时间：%1　操作人：%2\n本次纳入 %3 笔、原始金额 %4 "
                           "元；截至当时累计净支出 %5 元。")
                .arg(timeText(snapshot.value("created_at")), snapshot.value("actor").toString())
                .arg(snapshot.value("new_count").toInt())
                .arg(snapshot.value("new_amount").toString(),
                     snapshot.value("total_expenses").toString()),
            &dialog));
        body->addWidget(label(QStringLiteral("本次纳入扣退款后的净额：%1 元。")
                                  .arg(snapshot.value("new_net_amount")
                                           .toString(snapshot.value("new_amount").toString())),
                              &dialog));
        body->addWidget(
            label(QStringLiteral("以下是生成时的余额，后续还款不会修改本页。正数应收，负数应付。"),
                  &dialog));
        auto* members = table(balanceHeaders(), "snapshotBalances", &dialog);
        renderBalances(members, snapshot.value("members").toArray());
        body->addWidget(members, 1);
        body->addWidget(label(QStringLiteral("本次纳入账目（最多显示1000笔）"), &dialog));
        auto* expenses = table({QStringLiteral("消费日期"), QStringLiteral("消费说明"),
                                QStringLiteral("金额（元）"), QStringLiteral("结算前版本")},
                               "snapshotExpenses", &dialog);
        const auto records = snapshot.value("expenses").toArray();
        expenses->setRowCount(records.size());
        for (int row = 0; row < records.size(); ++row) {
            const auto record = records[row].toObject();
            cell(expenses, row, 0, record.value("date").toString());
            cell(expenses, row, 1, record.value("description").toString());
            cell(expenses, row, 2, record.value("amount").toString());
            cell(expenses, row, 3, QString::number(record.value("version").toInt()));
        }
        body->addWidget(expenses, 1);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        body->addWidget(close);
        dialog.exec();
    });
}

void SettlementPage::action(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const ServiceError& error) {
        message(
            error.message() +
                (error.code() == "connection" ? QStringLiteral("请刷新核对操作结果。") : QString()),
            true);
        if (error.code() == "unauthenticated") {
            emit sessionExpired(error.message());
        }
    }
}
void SettlementPage::message(const QString& text, bool error) {
    notice_->setText(text);
    notice_->setStyleSheet(error ? "color:#b42318;" : "color:#25765b;");
}
}  // namespace ledger
