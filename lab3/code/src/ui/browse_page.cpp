#include "ui/browse_page.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/expense_details_dialog.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
QLabel* plain(const QString& text, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    return value;
}
QLineEdit* input(const char* name, const QString& placeholder, QWidget* parent, int length = 128) {
    auto* value = new QLineEdit(parent);
    value->setObjectName(name);
    value->setPlaceholderText(placeholder);
    value->setClearButtonEnabled(true);
    value->setMaxLength(length);
    return value;
}
QPushButton* button(const QString& title, const char* name, QWidget* parent) {
    auto* result = new QPushButton(title, parent);
    result->setObjectName(name);
    return result;
}
QString stateName(const QString& state) {
    static const QMap<QString, QString> names{{"draft", "草稿"},       {"pending", "待审核"},
                                              {"approved", "已通过"},  {"rejected", "已驳回"},
                                              {"withdrawn", "已撤回"}, {"settled", "已纳入结算"}};
    return names.value(state, state);
}
const QList<QPair<QString, QString>> auditActions{
    {"create_ledger", "创建账本"},       {"invite_member", "邀请成员"},
    {"accept_invitation", "接受邀请"},   {"change_role", "调整角色"},
    {"transfer_admin", "交接管理员"},    {"exit_ledger", "退出账本"},
    {"archive_ledger", "归档账本"},      {"delete_ledger_data", "删除业务数据"},
    {"restore_ledger", "恢复账本"},      {"save_expense_draft", "保存私有草稿"},
    {"submit_expense", "提交账目"},      {"revise_expense", "修改并重审"},
    {"approve_expense", "审核通过"},     {"reject_expense", "审核驳回"},
    {"withdraw_expense", "撤回账目"},    {"create_settlement", "生成结算"},
    {"record_repayment", "登记还款"},    {"record_refund", "登记商家退款"},
    {"record_supplement", "结算后补记"}, {"correct_settled_expense", "结算后纠错"}};
QString actionName(const QString& value) {
    for (const auto& entry : auditActions) {
        if (entry.first == value) {
            return entry.second;
        }
    }
    return value;
}
void cells(QTableWidget* table, int row, const QStringList& values) {
    for (int col = 0; col < values.size(); ++col) {
        auto* item = centeredTableItem(values[col].isEmpty() ? QStringLiteral("—") : values[col]);
        item->setToolTip(values[col]);
        table->setItem(row, col, item);
    }
}
QTableWidget* table(const char* name, const QStringList& headers, QWidget* parent) {
    auto* value = new QTableWidget(0, headers.size(), parent);
    value->setObjectName(name);
    configureTable(value);
    value->setHorizontalHeaderLabels(headers);
    value->verticalHeader()->hide();
    value->verticalHeader()->setDefaultSectionSize(58);
    value->setEditTriggers(QAbstractItemView::NoEditTriggers);
    value->setSelectionBehavior(QAbstractItemView::SelectRows);
    value->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    return value;
}
void restore(QComboBox* combo, const QString& id, const QString& label) {
    const int index = combo->findData(id);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    } else if (!id.isEmpty()) {
        combo->addItem(label, id);
        combo->setCurrentIndex(combo->count() - 1);
    }
}
}  // namespace
BrowsePage::BrowsePage(ApiClient& api, bool audit, QWidget* parent)
    : QWidget(parent), api_(api), audit_(audit) {
    setObjectName(audit ? "auditPage" : "queryExpensePage");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    notice_ = plain({}, this);
    notice_->setObjectName(audit ? "auditNotice" : "queryNotice");
    outer->addWidget(notice_);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(10);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(18);
    for (int col = 0; col < 3; ++col) {
        grid->setColumnStretch(col, 1);
    }
    from_ = input(audit ? "auditDateFrom" : "queryDateFrom", "YYYY-MM-DD（选填）", content, 10);
    to_ = input(audit ? "auditDateTo" : "queryDateTo", "YYYY-MM-DD（选填）", content, 10);
    keyword_ = input(audit ? "auditKeyword" : "queryDescription",
                     audit ? QStringLiteral("搜索原因、内容或消费说明")
                           : QStringLiteral("消费说明包含，按文字匹配"),
                     content, 500);
    const auto field = [&](const QString& name, QWidget* value, int row, int column) {
        grid->addWidget(plain(name, content), row, column);
        grid->addWidget(value, row + 1, column);
    };
    field("开始日期", from_, 0, 0);
    field("结束日期", to_, 0, 1);
    if (audit) {
        ledger_ = new QComboBox(content);
        ledger_->setObjectName("auditLedgerSelector");
        field("日志所属账本（含已关闭）", ledger_, 0, 2);
        scope_ = new QComboBox(content);
        scope_->setObjectName("auditScope");
        field("查看范围", scope_, 2, 0);
        action_ = new QComboBox(content);
        action_->setObjectName("auditAction");
        action_->addItem("全部操作", "");
        for (const auto& value : auditActions) {
            action_->addItem(value.second, value.first);
        }
        field("操作类型", action_, 2, 1);
        actor_ = new QComboBox(content);
        actor_->setObjectName("auditActor");
        actor_->addItem("全部可见操作人", "");
        field("操作人", actor_, 2, 2);
        grid->addWidget(plain("关键词", content), 4, 0);
        grid->addWidget(keyword_, 5, 0, 1, 3);
    } else {
        payer_ = new QComboBox(content);
        payer_->setObjectName("queryPayer");
        field("实际付款人", payer_, 0, 2);
        minimum_ = input("queryAmountMin", "不限（元）", content, 32);
        maximum_ = input("queryAmountMax", "不限（元）", content, 32);
        field("最小原始金额", minimum_, 2, 0);
        field("最大原始金额", maximum_, 2, 1);
        category_ = new QComboBox(content);
        category_->setObjectName("queryCategory");
        field("消费类别", category_, 2, 2);
        grid->addWidget(plain("消费说明", content), 4, 0);
        grid->addWidget(keyword_, 5, 0, 1, 2);
        state_ = new QComboBox(content);
        state_->setObjectName("expenseStatusFilter");
        state_->addItem("默认（不含已撤回）", "");
        for (const auto& state :
             QStringList{"draft", "pending", "approved", "rejected", "withdrawn", "settled"}) {
            state_->addItem(stateName(state), state);
        }
        field("账目状态", state_, 4, 2);
    }
    body->addLayout(grid);
    auto* toolbar = new QHBoxLayout();
    toolbar->addStretch();
    if (!audit_) {
        settlement_button_ = button("我的待还 / 已还", "openMySettlement", content);
        settlement_button_->hide();
        toolbar->addWidget(settlement_button_);
        connect(settlement_button_, &QPushButton::clicked, this,
                &BrowsePage::viewSettlementRequested);
    }
    auto* clear = button("重置筛选", audit ? "resetAudit" : "resetQuery", content);
    auto* search =
        button(audit ? "查询日志" : "查询账目", audit ? "searchAudit" : "refreshExpenses", content);
    toolbar->addWidget(clear);
    toolbar->addWidget(search);
    body->addLayout(toolbar);
    if (!audit_) {
        auto* help = plain(
            "“已纳入结算”表示已计算分摊，不表示已还清；请到“我的待还 / "
            "已还”查看当前余额及实际还款记录。",
            content);
        help->setObjectName("querySettlementHelp");
        body->addWidget(help);
    }
    summary_ = plain({}, content);
    summary_->setObjectName(audit ? "auditSummary" : "querySummary");
    body->addWidget(summary_);
    auto* scope_note = plain(
        audit ? QStringLiteral("普通成员仅查看相关账目/"
                               "财务变更；历史用户仅查看退出前的日志。私有草稿仅本人可见。财务调整"
                               "金额以净支出为口径，其余为原始金额。")
              : QStringLiteral(
                    "汇总覆盖全部筛选结果，仅统计已通过/"
                    "已结算且未被替代的支出，并扣商家退款；草稿、待审、驳回、撤回和还款不计消费。"),
        content);
    scope_note->setObjectName(audit ? "auditScopeNote" : "queryScopeNote");
    body->addWidget(scope_note);
    empty_ = plain("暂无数据", content);
    empty_->setObjectName(audit ? "auditEmpty" : "expenseEmpty");
    body->addWidget(empty_);
    table_ = table(audit ? "auditTable" : "expenseTable",
                   audit ? QStringList{"时间", "操作人", "操作", "变更前（元）", "变更后（元）",
                                       "结果", "原因 / 内容", "详情"}
                         : QStringList{"日期", "消费说明", "实际付款人", "创建人", "原始金额（元）",
                                       "费用承担人", "状态", "操作"},
                   content);
    table_->setMinimumHeight(230);
    body->addWidget(table_, 1);
    const QList<int> widths = audit ? QList<int>{160, 95, 140, 105, 105, 145, 250, 85}
                                    : QList<int>{110, 210, 100, 90, 130, 180, 165, 95};
    for (int col = 0; col < 8; ++col) {
        table_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Interactive);
        table_->setColumnWidth(col, widths[col]);
    }
    table_->horizontalHeader()->setSectionResizeMode(audit ? 6 : 1, QHeaderView::Stretch);
    auto* paging = new QHBoxLayout();
    page_label_ = plain({}, content);
    page_label_->setObjectName(audit ? "auditPagination" : "queryPagination");
    previous_ = button("上一页", audit ? "auditPrevious" : "queryPrevious", content);
    next_ = button("下一页", audit ? "auditNext" : "queryNext", content);
    size_ = new QComboBox(content);
    size_->setObjectName(audit ? "auditPageSize" : "queryPageSize");
    for (int size : {50, 100, 200}) {
        size_->addItem(QStringLiteral("每页%1条").arg(size), size);
    }
    paging->addWidget(page_label_, 1);
    paging->addWidget(size_);
    paging->addWidget(previous_);
    paging->addWidget(next_);
    body->addLayout(paging);
    if (!audit) {
        group_ = new QComboBox(content);
        group_->setObjectName("queryGrouping");
        group_->addItem("按类别汇总", "categories");
        group_->addItem("按月份汇总", "months");
        body->addWidget(group_);
        groups_ = table(
            "queryGroups",
            {"分组", "有效支出笔数", "原始金额（元）", "商家退款（元）", "净支出（元）"}, content);
        groups_->setMinimumHeight(100);
        groups_->setMaximumHeight(190);
        groups_->verticalHeader()->setDefaultSectionSize(42);
        body->addWidget(groups_);
        connect(group_, &QComboBox::currentIndexChanged, this, &BrowsePage::renderGroups);
    }
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    connect(search, &QPushButton::clicked, this, [this] {
        page_ = 1;
        refresh();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        reset();
        refresh();
    });
    connect(previous_, &QPushButton::clicked, this, [this] {
        if (page_ > 1) {
            --page_;
            refresh();
        }
    });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (page_ < pages_) {
            ++page_;
            refresh();
        }
    });
    connect(size_, &QComboBox::currentIndexChanged, this, [this] {
        page_ = 1;
        refresh();
    });
    if (audit) {
        connect(ledger_, &QComboBox::currentIndexChanged, this, [this] {
            if (!loading_) {
                configureScope();
                reset();
                refresh();
            }
        });
    } else {
        connect(state_, &QComboBox::currentIndexChanged, this, [this] {
            if (!loading_) {
                page_ = 1;
                refresh();
            }
        });
    }
}
void BrowsePage::reset() {
    loading_ = true;
    from_->clear();
    to_->clear();
    keyword_->clear();
    page_ = 1;
    if (audit_) {
        action_->setCurrentIndex(0);
        actor_->setCurrentIndex(0);
    } else {
        minimum_->clear();
        maximum_->clear();
        payer_->setCurrentIndex(0);
        category_->setCurrentIndex(0);
        state_->setCurrentIndex(0);
    }
    loading_ = false;
}
void BrowsePage::configureScope() {
    const QSignalBlocker blocker(scope_);
    const auto old = scope_->currentData().toString();
    scope_->clear();
    if (ledger_->currentData(Qt::UserRole + 1).toBool()) {
        scope_->addItem("全部管理日志", "all");
    }
    scope_->addItem("与我相关的变更", "related");
    if (!old.isEmpty() && scope_->findData(old) >= 0) {
        scope_->setCurrentIndex(scope_->findData(old));
    }
}
void BrowsePage::loadOptions() {
    if (audit_) {
        const auto selected = ledger_->currentData().toString();
        const auto books = api_.call("audit_ledgers").value("ledgers").toArray();
        const QSignalBlocker blocker(ledger_);
        ledger_->clear();
        for (const auto& value : books) {
            const auto book = value.toObject();
            ledger_->addItem(
                book.value("name").toString() +
                    (book.value("status") != "active"    ? QStringLiteral("（已关闭）")
                     : book.value("historical").toBool() ? QStringLiteral("（历史范围）")
                                                         : QString()),
                book.value("id").toString());
            ledger_->setItemData(ledger_->count() - 1, book.value("can_view_all").toBool(),
                                 Qt::UserRole + 1);
        }
        int index = ledger_->findData(selected);
        if (index < 0) {
            index = ledger_->findData(book_.value("id").toString());
        }
        if (index >= 0) {
            ledger_->setCurrentIndex(index);
        }
        configureScope();
        return;
    }
    const auto result = api_.call("query_options", {{"ledger_id", book_.value("id")}});
    const auto payer = payer_->currentData().toString(), payer_name = payer_->currentText(),
               category = category_->currentData().toString(),
               category_name = category_->currentText();
    const bool uncategorized = category_->currentData(Qt::UserRole + 1).toBool();
    const QSignalBlocker a(payer_), b(category_);
    payer_->clear();
    payer_->addItem("全部付款人", "");
    for (const auto& value : result.value("payers").toArray()) {
        const auto person = value.toObject();
        payer_->addItem(person.value("username").toString(), person.value("id").toString());
    }
    restore(payer_, payer, payer_name);
    category_->clear();
    category_->addItem("全部类别", "");
    category_->addItem("未分类", "");
    category_->setItemData(1, true, Qt::UserRole + 1);
    for (const auto& value : result.value("categories").toArray()) {
        category_->addItem(value.toString(), value.toString());
    }
    if (uncategorized) {
        category_->setCurrentIndex(1);
    } else {
        restore(category_, category, category_name);
    }
}
void BrowsePage::setContext(const QJsonObject& book, const QJsonObject& user) {
    const bool changed =
        book_.value("id") != book.value("id") || user_.value("id") != user.value("id");
    book_ = book;
    user_ = user;
    if (settlement_button_) {
        settlement_button_->setVisible(!book.isEmpty() && !book.value("historical").toBool());
    }
    if (user.isEmpty() || (!audit_ && book.isEmpty())) {
        clearContext();
        return;
    }
    action([&] {
        if (changed) {
            reset();
            if (audit_) {
                const QSignalBlocker blocker(ledger_);
                ledger_->clear();
                scope_->clear();
            }
        }
        loadOptions();
        refresh();
    });
}
void BrowsePage::clearContext() {
    if (settlement_button_) {
        settlement_button_->hide();
    }
    loading_ = true;
    from_->clear();
    to_->clear();
    keyword_->clear();
    if (minimum_) {
        minimum_->clear();
        maximum_->clear();
    }
    if (payer_) {
        payer_->clear();
        category_->clear();
        state_->setCurrentIndex(0);
    }
    if (actor_) {
        actor_->clear();
        actor_->addItem("全部可见操作人", "");
        action_->setCurrentIndex(0);
    }
    if (scope_) {
        scope_->clear();
    }
    book_ = {};
    user_ = {};
    response_ = {};
    page_ = pages_ = 1;
    table_->setRowCount(0);
    if (groups_) {
        groups_->setRowCount(0);
    }
    if (ledger_) {
        const QSignalBlocker blocker(ledger_);
        ledger_->clear();
    }
    notice_->clear();
    summary_->clear();
    page_label_->clear();
    empty_->show();
    previous_->setEnabled(false);
    next_->setEnabled(false);
    loading_ = false;
}
QJsonObject BrowsePage::parameters() const {
    QJsonObject result{
        {"ledger_id", audit_ ? QJsonValue(ledger_->currentData().toString()) : book_.value("id")},
        {"date_from", from_->text()},
        {"date_to", to_->text()},
        {"page", page_},
        {"page_size", size_->currentData().toInt()}};
    if (audit_) {
        result["scope"] = scope_->currentData().toString();
        result["action"] = action_->currentData().toString();
        result["actor_id"] = actor_->currentData().toString();
        result["keyword"] = keyword_->text();
    } else {
        result["amount_min"] = minimum_->text();
        result["amount_max"] = maximum_->text();
        result["payer_id"] = payer_->currentData().toString();
        result["category"] = category_->currentData().toString();
        result["uncategorized"] = category_->currentData(Qt::UserRole + 1).toBool();
        result["description"] = keyword_->text();
        result["status"] = state_->currentData().toString();
    }
    return result;
}
void BrowsePage::refresh() {
    if (loading_ || user_.isEmpty()) {
        return;
    }
    action([&] {
        if (audit_ && ledger_->currentIndex() < 0) {
            table_->setRowCount(0);
            response_ = {};
            page_ = pages_ = 1;
            page_label_->clear();
            previous_->setEnabled(false);
            next_->setEnabled(false);
            summary_->setText("暂无有权查看日志的账本");
            empty_->show();
            return;
        }
        if (!audit_ && book_.isEmpty()) {
            return;
        }
        auto response = api_.call(audit_ ? "audit_list" : "query_expenses", parameters());
        if (page_ > response.value("pages").toInt(1)) {
            page_ = response.value("pages").toInt(1);
            response = api_.call(audit_ ? "audit_list" : "query_expenses", parameters());
        }
        notice_->clear();
        render(response);
    });
}
void BrowsePage::render(const QJsonObject& response) {
    response_ = response;
    pages_ = response.value("pages").toInt(1);
    const auto rows = response.value(audit_ ? "logs" : "expenses").toArray();
    table_->setRowCount(rows.size());
    empty_->setVisible(rows.isEmpty());
    for (int row = 0; row < rows.size(); ++row) {
        const auto item = rows[row].toObject();
        QStringList values;
        if (audit_) {
            values = {QDateTime::fromSecsSinceEpoch(item.value("created_at").toInteger())
                          .toString("yyyy-MM-dd HH:mm:ss"),
                      item.value("actor").toString(),
                      actionName(item.value("action").toString()),
                      item.value("before_amount").toString(),
                      item.value("after_amount").toString(),
                      item.value("result").toString(),
                      item.value("reason").toString(item.value("detail").toString())};
        } else {
            values = {
                item.value("date").toString(),
                item.value("description").toString(),
                item.value("payer").toString(),
                item.value("creator").toString(),
                item.value("amount").toString(),
                item.value("participants").toString(),
                stateName(item.value("status").toString()) +
                    (item.value("superseded").toBool() ? QStringLiteral("\n已被调整替代 / 冲销")
                     : item.value("is_adjustment").toBool() ? QStringLiteral("\n结算后调整")
                     : item.value("corrected").toBool()     ? QStringLiteral("\n已更正")
                                                            : QString())};
        }
        cells(table_, row, values);
        table_->item(row, 0)->setData(Qt::UserRole, item.value("id"));
        auto* view = button("查看", audit_ ? "viewAuditDetail" : "viewExpense", table_);
        setTableButton(table_, row, 7, view);
        connect(view, &QPushButton::clicked, this,
                [this, item] { action([&] { showDetail(item.value("id").toString()); }); });
    }
    page_label_->setText(QStringLiteral("第%1 / %2页 · 共%3条")
                             .arg(page_)
                             .arg(pages_)
                             .arg(response.value("total").toInt()));
    previous_->setEnabled(page_ > 1);
    next_->setEnabled(page_ < pages_);
    if (audit_) {
        summary_->setText(
            QStringLiteral("%1 · %2 · 共%3条日志")
                .arg(response.value("name").toString(), response.value("scope") == "all"
                                                            ? QStringLiteral("管理员范围")
                                                            : QStringLiteral("与我相关的范围"))
                .arg(response.value("total").toInt()));
        const auto selected = actor_->currentData().toString(), name = actor_->currentText();
        const QSignalBlocker blocker(actor_);
        actor_->clear();
        actor_->addItem("全部可见操作人", "");
        for (const auto& value : response.value("actors").toArray()) {
            const auto person = value.toObject();
            actor_->addItem(person.value("username").toString(), person.value("id").toString());
        }
        restore(actor_, selected, name);
    } else {
        const auto stats = response.value("summary").toObject();
        summary_->setText(
            QStringLiteral("筛选结果%1笔 · 有效支出%2笔 · 原始%3元 · 商家退款%4元 · 净支出%5元")
                .arg(stats.value("matched_count").toInt())
                .arg(stats.value("count").toInt())
                .arg(stats.value("gross_amount").toString(),
                     stats.value("refund_amount").toString(),
                     stats.value("net_amount").toString()));
        renderGroups();
    }
}
void BrowsePage::renderGroups() {
    if (!groups_) {
        return;
    }
    const auto rows = response_.value(group_->currentData().toString()).toArray();
    groups_->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto item = rows[row].toObject();
        cells(groups_, row,
              {item.value("name").toString(), QString::number(item.value("count").toInt()),
               item.value("gross_amount").toString(), item.value("refund_amount").toString(),
               item.value("net_amount").toString()});
    }
}
void BrowsePage::showDetail(const QString& id) {
    if (!audit_) {
        ExpenseDetailsDialog dialog(api_, book_, id, false, this);
        connect(&dialog, &ExpenseDetailsDialog::sessionExpired, this, &BrowsePage::sessionExpired);
        dialog.exec();
        refresh();
        return;
    }
    QJsonObject request{{"ledger_id", ledger_->currentData().toString()},
                        {"scope", scope_->currentData().toString()},
                        {"log_id", id}};
    const auto result = api_.call("audit_detail", request);
    QDialog dialog(this);
    dialog.setObjectName("auditDetailDialog");
    dialog.setWindowTitle("操作日志明细");
    dialog.resize(850, 620);
    auto* body = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);
    for (const auto& pair : QList<QPair<QString, QString>>{
             {"时间", QDateTime::fromSecsSinceEpoch(result.value("created_at").toInteger())
                          .toString("yyyy-MM-dd HH:mm:ss")},
             {"操作人", result.value("actor").toString()},
             {"操作", actionName(result.value("action").toString())},
             {"关联对象编号", result.value("target").toString()},
             {"消费说明", result.value("expense_description").toString()},
             {"金额口径", result.value("amount_basis").toString().isEmpty()
                              ? (result.value("kind") == "repayment" ? QStringLiteral("实际还款")
                                                                     : QStringLiteral("原始支出"))
                              : QStringLiteral("净支出（已扣商家退款）")},
             {"变更前金额", result.value("before_amount").toString()},
             {"变更后金额", result.value("after_amount").toString()},
             {"结果", result.value("result").toString()},
             {"原因", result.value("reason").toString()},
             {"原始日志", result.value("detail").toString()}}) {
        form->addRow(pair.first,
                     plain(pair.second.isEmpty() ? QStringLiteral("—") : pair.second, content));
    }
    if (result.value("legacy").toBool()) {
        form->addRow(plain("升级前的日志仅补充可从原历史确定的信息，缺失字段不推测。", content));
    }
    scroll->setWidget(content);
    body->addWidget(scroll, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText("关闭");
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    body->addWidget(close);
    dialog.exec();
}
void BrowsePage::action(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const ServiceError& failure) {
        notice_->setText(failure.message());
        notice_->setStyleSheet("color:#b42318;");
        if (failure.code() == "unauthenticated") {
            emit sessionExpired(failure.message());
        }
    }
}
}  // namespace ledger
