#include "ui/expense_page.h"

#include <QApplication>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "domain/split.h"
#include "transport/api_client.h"
#include "ui/expense_details_dialog.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
class CenteredCheckDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.features.setFlag(QStyleOptionViewItem::HasCheckIndicator, false);
        background.text.clear();
        auto* style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        QStyleOptionButton check;
        check.state = option.state & (QStyle::State_Enabled | QStyle::State_Active);
        check.state |= index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? QStyle::State_On
                                                                             : QStyle::State_Off;
        check.rect = QStyle::alignedRect(
            option.direction, Qt::AlignCenter,
            QSize(style->pixelMetric(QStyle::PM_IndicatorWidth, &check, option.widget),
                  style->pixelMetric(QStyle::PM_IndicatorHeight, &check, option.widget)),
            option.rect);
        style->drawPrimitive(QStyle::PE_IndicatorCheckBox, &check, painter, option.widget);
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if (!(index.flags() & Qt::ItemIsEnabled) || !(index.flags() & Qt::ItemIsUserCheckable)) {
            return false;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton ||
                !option.rect.contains(mouse->position().toPoint())) {
                return false;
            }
        } else if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key != Qt::Key_Space && key != Qt::Key_Select) {
                return false;
            }
        } else {
            return false;
        }
        return model->setData(
            index,
            index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked : Qt::Checked,
            Qt::CheckStateRole);
    }
};

QString statusName(const QString& status) {
    static const QHash<QString, QString> names{{"draft", "草稿"},       {"pending", "待审核"},
                                               {"approved", "已通过"},  {"rejected", "已驳回"},
                                               {"withdrawn", "已撤回"}, {"settled", "已纳入结算"}};
    return names.value(status, status);
}
QPushButton* makeButton(const QString& title, const char* name, QWidget* parent) {
    auto* button = new QPushButton(title, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}
QLabel* makeLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
int question(QWidget* parent, const QString& title, const QString& text,
             QMessageBox::StandardButtons buttons, QMessageBox::StandardButton default_button) {
    QMessageBox dialog(QMessageBox::Question, title, text, buttons, parent);
    dialog.setTextFormat(Qt::PlainText);
    dialog.setDefaultButton(default_button);
    const QHash<QMessageBox::StandardButton, QString> labels{
        {QMessageBox::Yes, QStringLiteral("是")},
        {QMessageBox::No, QStringLiteral("否")},
        {QMessageBox::Save, QStringLiteral("保存")},
        {QMessageBox::Discard, QStringLiteral("放弃")},
        {QMessageBox::Cancel, QStringLiteral("取消")}};
    for (auto item = labels.begin(); item != labels.end(); ++item) {
        if (auto* button = dialog.button(item.key())) {
            button->setText(item.value());
        }
    }
    return dialog.exec();
}
}  // namespace

ExpensePage::ExpensePage(ApiClient& api, bool view_only, QWidget* parent, bool review_mode)
    : QWidget(parent), api_(api), view_only_(view_only), review_mode_(review_mode) {
    setObjectName(review_mode ? QStringLiteral("reviewExpensePage")
                  : view_only ? QStringLiteral("queryExpensePage")
                              : QStringLiteral("expensePage"));
    recovery_timer_ = new QTimer(this);
    recovery_timer_->setSingleShot(true);
    recovery_timer_->setInterval(400);
    connect(recovery_timer_, &QTimer::timeout, this, [this] { persistRecovery(); });
    lock_timer_ = new QTimer(this);
    lock_timer_->setInterval(20000);
    connect(lock_timer_, &QTimer::timeout, this, &ExpensePage::renewLock);
    lock_timer_->start();
    auto* body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    notice_ = makeLabel(QString(), this);
    notice_->setObjectName(QStringLiteral("expenseNotice"));
    notice_->setMinimumHeight(24);
    body->addWidget(notice_);
    stack_ = new QStackedWidget(this);
    stack_->addWidget(createList());
    stack_->addWidget(createForm());
    body->addWidget(stack_, 1);
}

QWidget* ExpensePage::createList() {
    auto* page = new QWidget(this);
    auto* body = new QVBoxLayout(page);
    body->setContentsMargins(0, 0, 0, 0);
    auto* toolbar = new QHBoxLayout();
    filter_ = new QComboBox(page);
    filter_->setObjectName(QStringLiteral("expenseStatusFilter"));
    filter_->addItem(QStringLiteral("默认（不含已撤回）"), QStringLiteral(""));
    for (const QString& status :
         QStringList{"draft", "pending", "approved", "rejected", "withdrawn", "settled"}) {
        filter_->addItem(statusName(status), status);
    }
    toolbar->addWidget(filter_);
    toolbar->addStretch();
    recovery_button_ = makeButton(QStringLiteral("本地恢复副本"), "recoveryCopiesButton", page);
    toolbar->addWidget(recovery_button_);
    recovery_button_->hide();
    connect(recovery_button_, &QPushButton::clicked, this, &ExpensePage::showRecoveryCopies);
    auto* refresh = makeButton(QStringLiteral("刷新账目"), "refreshExpenses", page);
    new_button_ = makeButton(QStringLiteral("新增账目"), "newExpenseButton", page);
    new_button_->setVisible(false);
    toolbar->addWidget(refresh);
    toolbar->addWidget(new_button_);
    supplement_button_ = makeButton(QStringLiteral("结算后补记"), "supplementExpenseButton", page);
    supplement_button_->hide();
    toolbar->addWidget(supplement_button_);
    body->addLayout(toolbar);
    empty_ = makeLabel(QStringLiteral("暂无数据"), page);
    empty_->setObjectName(QStringLiteral("expenseEmpty"));
    body->addWidget(empty_);
    records_ = new QTableWidget(0, 8, page);
    configureTable(records_);
    records_->setObjectName(QStringLiteral("expenseTable"));
    records_->setHorizontalHeaderLabels(
        {QStringLiteral("日期"), QStringLiteral("消费说明"), QStringLiteral("实际付款人"),
         QStringLiteral("创建人"), QStringLiteral("原始金额（元）"), QStringLiteral("费用承担人"),
         QStringLiteral("状态"), QStringLiteral("操作")});
    records_->verticalHeader()->hide();
    records_->verticalHeader()->setDefaultSectionSize(54);
    records_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    records_->setSelectionBehavior(QAbstractItemView::SelectRows);

    const int widths[]{110, 190, 100, 85, 125, 150, 165, 255};
    for (int column = 0; column < 8; ++column) {
        records_->setColumnWidth(column, widths[column]);
    }
    records_->horizontalHeader()->setStretchLastSection(false);
    records_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    records_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    body->addWidget(records_, 1);
    body->addWidget(
        makeLabel(QStringLiteral("费用承担不是要求再次付款，实际垫付会在结算中抵扣。已通过账目生成"
                                 "结算后，可在“结算与还款”登记实际还款。草稿仅创建者可见。"),
                  page));
    if (review_mode_) {
        filter_->hide();
    }
    notices_panel_ = new QWidget(page);
    auto* notices_body = new QVBoxLayout(notices_panel_);
    notices_body->setContentsMargins(0, 0, 0, 0);
    notices_body->addWidget(
        makeLabel(QStringLiteral("我的账目通知（仅本人可见）"), notices_panel_));
    notices_ = new QTableWidget(0, 4, notices_panel_);
    configureTable(notices_);
    notices_->setObjectName("expenseNotices");
    notices_->setHorizontalHeaderLabels({"时间", "内容", "状态", "操作"});
    notices_->verticalHeader()->hide();
    notices_->verticalHeader()->setDefaultSectionSize(48);
    notices_->setMaximumHeight(155);
    notices_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    notices_->setColumnWidth(0, 155);
    notices_->setColumnWidth(2, 70);
    notices_->setColumnWidth(3, 105);
    notices_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    notices_body->addWidget(notices_);
    body->addWidget(notices_panel_);
    notices_panel_->hide();
    connect(refresh, &QPushButton::clicked, this, &ExpensePage::refresh);
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] { this->refresh(); });
    connect(new_button_, &QPushButton::clicked, this, &ExpensePage::startNew);
    connect(supplement_button_, &QPushButton::clicked, this, &ExpensePage::startSupplement);
    return page;
}

QWidget* ExpensePage::createForm() {
    auto* form = new QWidget(this);
    auto* outer = new QVBoxLayout(form);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(form);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QFrame(scroll);
    page->setObjectName(QStringLiteral("expenseFormCard"));
    auto* body = new QVBoxLayout(page);
    body->setContentsMargins(24, 22, 24, 22);
    body->setSpacing(15);
    auto* heading = new QHBoxLayout();
    form_title_ = makeLabel(QStringLiteral("新增账目"), page);
    form_title_->setObjectName(QStringLiteral("sectionTitle"));
    heading->addWidget(form_title_, 1);
    heading->addStretch();
    auto* reacquire = makeButton(QStringLiteral("重新获取编辑锁"), "reacquireExpenseLock", page);
    heading->addWidget(reacquire);
    connect(reacquire, &QPushButton::clicked, this, &ExpensePage::reacquireLock);
    auto* back = makeButton(QStringLiteral("返回列表"), "backToExpenseList", page);
    heading->addWidget(back);
    body->addLayout(heading);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(10);
    amount_ = new QLineEdit(page);
    amount_->setObjectName(QStringLiteral("expenseAmount"));
    amount_->setPlaceholderText(QStringLiteral("例如90.00"));
    amount_->setMaxLength(32);
    payer_ = new QComboBox(page);
    payer_->setObjectName(QStringLiteral("expensePayer"));
    date_ = new QDateEdit(QDate::currentDate(), page);
    date_->setObjectName(QStringLiteral("expenseDate"));
    date_->setCalendarPopup(true);
    date_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    category_ = new QLineEdit(page);
    category_->setObjectName(QStringLiteral("expenseCategory"));
    category_->setMaxLength(80);
    category_->setPlaceholderText(QStringLiteral("选填，如办公、餐饮"));
    description_ = new QLineEdit(page);
    description_->setObjectName(QStringLiteral("expenseDescription"));
    description_->setMaxLength(500);
    description_->setPlaceholderText(QStringLiteral("填写支出用途"));
    method_ = new QComboBox(page);
    method_->setObjectName(QStringLiteral("expenseSplitMethod"));
    method_->addItem(QStringLiteral("按人数均摊"), QStringLiteral("equal"));
    method_->addItem(QStringLiteral("按比例（如1:2）"), QStringLiteral("ratio"));
    method_->addItem(QStringLiteral("指定金额"), QStringLiteral("fixed"));
    grid->addWidget(makeLabel(QStringLiteral("消费金额 *"), page), 0, 0);
    grid->addWidget(makeLabel(QStringLiteral("实际付款人 *（已垫付整笔金额）"), page), 0, 1);
    grid->addWidget(amount_, 1, 0);
    grid->addWidget(payer_, 1, 1);
    grid->addWidget(makeLabel(QStringLiteral("消费日期 *"), page), 2, 0);
    grid->addWidget(makeLabel(QStringLiteral("消费类别（选填）"), page), 2, 1);
    grid->addWidget(date_, 3, 0);
    grid->addWidget(category_, 3, 1);
    grid->addWidget(makeLabel(QStringLiteral("消费说明 *"), page), 4, 0);
    grid->addWidget(description_, 5, 0);
    grid->addWidget(makeLabel(QStringLiteral("分摊方式 *"), page), 4, 1);
    grid->addWidget(method_, 5, 1);
    body->addLayout(grid);
    reason_panel_ = new QWidget(page);
    auto* reason_body = new QVBoxLayout(reason_panel_);
    reason_body->setContentsMargins(0, 0, 0, 0);
    reason_label_ =
        makeLabel(QStringLiteral("修改原因 *（修改已提交账目后需重新审核）"), reason_panel_);
    reason_body->addWidget(reason_label_);
    change_reason_ = new QLineEdit(reason_panel_);
    change_reason_->setObjectName("expenseChangeReason");
    change_reason_->setMaxLength(500);
    reason_body->addWidget(change_reason_);
    body->addWidget(reason_panel_);
    reason_panel_->hide();
    body->addWidget(makeLabel(
        QStringLiteral(
            "付款人已经垫付整笔消费。下方勾选最终承担费用的人；付款人自己也消费时，也要勾选。"
            "已垫付的钱会抵扣其应承担部分，不会要求重复付款。"),
        page));
    auto* members_toolbar = new QHBoxLayout();
    members_toolbar->addWidget(
        makeLabel(QStringLiteral("谁承担这笔消费 *（至少一人，包含参与消费的付款人）"), page), 1);
    manage_members_button_ =
        makeButton(QStringLiteral("管理账本成员"), "manageExpenseMembers", page);
    auto* refresh_members =
        makeButton(QStringLiteral("刷新可选成员"), "refreshExpenseMembers", page);
    members_toolbar->addWidget(manage_members_button_);
    members_toolbar->addWidget(refresh_members);
    body->addLayout(members_toolbar);
    member_help_ = makeLabel(QString(), page);
    member_help_->setObjectName("expenseMemberHelp");
    body->addWidget(member_help_);
    split_hint_ = makeLabel(QString(), page);
    split_hint_->setObjectName("expenseSplitHint");
    body->addWidget(split_hint_);
    splits_ = new QTableWidget(0, 6, page);
    configureTable(splits_);
    splits_->setObjectName(QStringLiteral("expenseSplits"));
    splits_->setHorizontalHeaderLabels(
        {QStringLiteral("承担"), QStringLiteral("成员"), QStringLiteral("比例 / 指定金额"),
         QStringLiteral("应承担（元）"), QStringLiteral("已垫付（元）"),
         QStringLiteral("本笔应收 / 应还")});
    splits_->horizontalHeaderItem(5)->setToolTip(QStringLiteral(
        "本笔垫付减应承担的差额；未抵扣退款、已有还款或其他账目，以结算页当前余额为准。"));
    splits_->verticalHeader()->hide();
    splits_->verticalHeader()->setDefaultSectionSize(54);
    splits_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    splits_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    splits_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    splits_->setColumnWidth(0, 70);
    splits_->setItemDelegateForColumn(0, new CenteredCheckDelegate(splits_));
    splits_->setMinimumHeight(180);
    splits_->setMaximumHeight(300);

    body->addWidget(splits_);
    preview_ = makeLabel(QStringLiteral("填写金额后显示分摊预览。"), page);
    preview_->setObjectName(QStringLiteral("expensePreview"));
    body->addWidget(preview_);
    body->addWidget(makeLabel(
        QStringLiteral("均摊及比例分摊会自动分配分位尾差，保证合计等于消费金额。"), page));
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    draft_button_ = makeButton(QStringLiteral("保存草稿"), "saveExpenseDraft", form);
    submit_button_ = makeButton(QStringLiteral("提交审核"), "submitExpense", form);
    buttons->addWidget(draft_button_);
    buttons->addWidget(submit_button_);
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);
    recovery_status_ = makeLabel(QString(), form);
    recovery_status_->setObjectName("recoveryStatus");
    outer->addWidget(recovery_status_);
    outer->addLayout(buttons);
    const auto changed = [this] {
        if (!loading_) {
            dirty_ = true;
            queueRecovery();
            last_activity_.start();
            preview();
        }
    };
    for (auto* edit : {amount_, description_, category_, change_reason_}) {
        connect(edit, &QLineEdit::textChanged, this, changed);
    }
    connect(date_, &QDateEdit::dateChanged, this, changed);
    connect(payer_, &QComboBox::currentIndexChanged, this, changed);
    connect(splits_, &QTableWidget::itemChanged, this, changed);
    connect(method_, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_) {
            for (int row = 0; row < splits_->rowCount(); ++row) {
                auto* value = qobject_cast<QLineEdit*>(splits_->cellWidget(row, 2));
                value->setText(method_->currentData() == "ratio" ? QStringLiteral("1") : QString());
            }
            dirty_ = true;
            queueRecovery();
            last_activity_.start();
            preview();
        }
    });
    connect(back, &QPushButton::clicked, this, [this] {
        if (confirmLeave()) {
            stack_->setCurrentIndex(0);
            refresh();
        }
    });
    connect(draft_button_, &QPushButton::clicked, this, [this] { saveDraft(); });
    connect(submit_button_, &QPushButton::clicked, this, &ExpensePage::submit);
    connect(manage_members_button_, &QPushButton::clicked, this,
            &ExpensePage::manageMembersRequested);
    connect(refresh_members, &QPushButton::clicked, this, &ExpensePage::refreshMembers);
    return form;
}

void ExpensePage::setContext(const QJsonObject& book, const QJsonObject& user) {
    const bool changed =
        book_.value("id") != book.value("id") || user_.value("id") != user.value("id");
    const bool next_write =
        !view_only_ && book.value("capabilities").toObject().value("write_expense").toBool();
    if (changed || (can_write_ && !next_write)) {
        persistRecovery();
        releaseRecovery();
        releaseLock();
    }
    book_ = book;
    user_ = user;
    can_write_ =
        !view_only_ && book.value("capabilities").toObject().value("write_expense").toBool();
    new_button_->setVisible(can_write_);
    supplement_button_->setVisible(can_write_);
    draft_button_->setEnabled(can_write_);
    submit_button_->setEnabled(can_write_);
    if (changed || !can_write_) {
        resetForm();
        stack_->setCurrentIndex(0);
    } else {
        syncMembers();
    }
    refresh();
    updateRecoveryButton();
}

void ExpensePage::clearContext() {
    persistRecovery();
    releaseRecovery();
    releaseLock();
    notices_->setRowCount(0);
    notices_panel_->hide();
    book_ = {};
    user_ = {};
    can_write_ = false;
    resetForm();
    records_->setRowCount(0);
    empty_->show();
    new_button_->hide();
    supplement_button_->hide();
    stack_->setCurrentIndex(0);
    notice_->clear();
}

void ExpensePage::startNew() {
    if (!can_write_ || !confirmLeave()) {
        return;
    }
    resetForm();
    stack_->setCurrentIndex(1);
    notice_->clear();
}

void ExpensePage::resetForm() {
    releaseRecovery();
    loading_ = true;
    financial_mode_.clear();
    financial_source_id_.clear();
    financial_token_.clear();
    financial_request_key_.clear();
    editing_post_adjustment_ = false;
    record_adjustment_reason_.clear();
    reason_label_->setText(QStringLiteral("修改原因 *（修改已提交账目后需重新审核）"));
    editing_id_.clear();
    editing_status_ = "draft";
    lock_token_.clear();
    change_reason_->clear();
    reason_panel_->hide();
    draft_button_->show();
    findChild<QPushButton*>("reacquireExpenseLock")->show();
    submit_button_->setText(QStringLiteral("提交审核"));
    last_activity_.start();
    version_ = 0;
    amount_->clear();
    description_->clear();
    category_->clear();
    date_->setDate(QDate::currentDate());
    method_->setCurrentIndex(0);
    payer_->clear();
    splits_->setRowCount(0);
    const auto members = book_.value("members").toArray();
    for (const auto& entry : members) {
        const auto member = entry.toObject();
        payer_->addItem(member.value("username").toString(), member.value("id").toString());
        addBearerRow(member, true);
    }
    payer_->setCurrentIndex(payer_->findData(user_.value("id").toString()));
    form_title_->setText(QStringLiteral("新增账目"));
    loading_ = false;
    dirty_ = false;
    markField({});
    syncMembers();
}

void ExpensePage::addBearerRow(const QJsonObject& member, bool checked) {
    const int row = splits_->rowCount();
    splits_->insertRow(row);
    auto* selected = centeredTableItem();
    selected->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    selected->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    selected->setData(Qt::UserRole, member.value("id").toString());
    selected->setData(Qt::UserRole + 1, true);
    splits_->setItem(row, 0, selected);
    auto* name = centeredTableItem(member.value("username").toString());
    name->setData(Qt::UserRole, member.value("username").toString());
    name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    name->setTextAlignment(Qt::AlignCenter);
    splits_->setItem(row, 1, name);
    auto* value = new QLineEdit(splits_);
    value->setObjectName("expenseSplitValue");
    value->setMaxLength(32);
    value->setAlignment(Qt::AlignCenter);
    if (method_->currentData() == "ratio") {
        value->setText(QStringLiteral("1"));
    }
    splits_->setCellWidget(row, 2, value);
    for (int column = 3; column < 6; ++column) {
        auto* result = centeredTableItem(QStringLiteral("—"));
        result->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        splits_->setItem(row, column, result);
    }
    connect(value, &QLineEdit::textChanged, this, [this] {
        if (!loading_) {
            dirty_ = true;
            queueRecovery();
            last_activity_.start();
            preview();
        }
    });
}

void ExpensePage::syncMembers() {
    const auto members = book_.value("members").toArray();
    bool has_departed = false;
    {
        const QScopedValueRollback<bool> guard(loading_, true);
        const QString payer = payer_->currentData().toString();
        payer_->clear();
        QHash<QString, int> existing;
        QSet<QString> current;
        for (int row = 0; row < splits_->rowCount(); ++row) {
            existing.insert(splits_->item(row, 0)->data(Qt::UserRole).toString(), row);
        }
        for (const auto& entry : members) {
            const auto member = entry.toObject();
            const QString id = member.value("id").toString();
            current.insert(id);
            payer_->addItem(member.value("username").toString(), id);
            if (!existing.contains(id)) {
                // Joining a ledger must not silently change an expense being entered.
                addBearerRow(member, false);
            } else {
                splits_->item(existing.value(id), 1)
                    ->setData(Qt::UserRole, member.value("username").toString());
            }
        }
        for (int row = 0; row < splits_->rowCount(); ++row) {
            auto* selected = splits_->item(row, 0);
            auto* name = splits_->item(row, 1);
            const bool active = current.contains(selected->data(Qt::UserRole).toString());
            selected->setData(Qt::UserRole + 1, active);
            const QString username = name->data(Qt::UserRole).toString();
            name->setText(active ? username : username + QStringLiteral("（已退出）"));
            name->setForeground(active ? QBrush() : QBrush(QColor("#b42318")));
            name->setToolTip(active ? username
                                    : QStringLiteral("原分摊输入保留；请取消勾选并重新核对分摊，"
                                                     "不能将已退出成员用于新的提交。"));
            has_departed |= !active;
        }
        payer_->setCurrentIndex(payer_->findData(payer));
    }
    const bool can_manage = book_.value("capabilities").toObject().value("manage_members").toBool();
    manage_members_button_->setText(can_manage ? QStringLiteral("管理账本成员")
                                               : QStringLiteral("查看账本成员"));
    member_help_->setText(
        QStringLiteral(
            "当前账本 %1 位成员。新增人员须由管理员邀请并接受，再点“刷新可选成员”后勾选。")
            .arg(members.size()) +
        (has_departed ? QStringLiteral(" 红色“已退出”行保留原输入，请取消勾选后重新核对分摊。")
                      : QString()));
    const int table_height = splits_->horizontalHeader()->sizeHint().height() +
                             splits_->rowCount() * splits_->verticalHeader()->defaultSectionSize() +
                             2 * splits_->frameWidth();
    splits_->setMinimumHeight(qBound(180, table_height, 300));
    preview();
}

void ExpensePage::refreshMembers() {
    action([this] {
        const auto book =
            api_.call(QStringLiteral("ledger_details"), {{"ledger_id", book_.value("id")}});
        setContext(book, user_);
        message(QStringLiteral("可选成员已刷新，已填内容保留；新增成员请手动勾选。"));
    });
}

QJsonObject ExpensePage::formData() const {
    QJsonArray parts;
    for (int row = 0; row < splits_->rowCount(); ++row) {
        if (splits_->item(row, 0)->checkState() == Qt::Checked) {
            parts.append(QJsonObject{
                {"user_id", splits_->item(row, 0)->data(Qt::UserRole).toString()},
                {"value", qobject_cast<QLineEdit*>(splits_->cellWidget(row, 2))->text()}});
        }
    }
    QJsonObject result{{"ledger_id", book_.value("id")},
                       {"amount", amount_->text()},
                       {"date", date_->date().toString(Qt::ISODate)},
                       {"payer_id", payer_->currentData().toString()},
                       {"description", description_->text()},
                       {"category", category_->text()},
                       {"split_method", method_->currentData().toString()},
                       {"splits", parts}};
    if (!editing_id_.isEmpty()) {
        result["expense_id"] = editing_id_;
        result["version"] = version_;
        result["lock_token"] = lock_token_;
        result["change_reason"] = change_reason_->text();
    }
    if (financial_mode_ == "supplement" || editing_post_adjustment_) {
        result["post_settlement_adjustment"] = true;
        result["adjustment_reason"] =
            financial_mode_ == "supplement" ? change_reason_->text() : record_adjustment_reason_;
    }
    return result;
}

void ExpensePage::preview() {
    if (loading_) {
        return;
    }
    const QSignalBlocker blocker(splits_);
    const bool voided = financial_mode_ == "correction" &&
                        amount_->text().trimmed().contains(QRegularExpression("^0(?:\\.0{1,2})?$"));
    if (voided) {
        splits_->horizontalHeaderItem(2)->setText(QStringLiteral("冲销无需新分摊"));
        split_hint_->setText(QStringLiteral(
            "金额为 0 表示冲销。保留原记录和实际还款；已经退款的支出不能冲销至 0。"));
        for (int row = 0; row < splits_->rowCount(); ++row) {
            splits_->cellWidget(row, 2)->hide();
            splits_->item(row, 3)->setText(QStringLiteral("0.00"));
            splits_->item(row, 4)->setText(QStringLiteral("—"));
            splits_->item(row, 5)->setText(QStringLiteral("以调整后余额为准"));
        }
        preview_->setText(QStringLiteral("冲销后净支出为 0；已发生还款可能产生反向应付。"));
        return;
    }
    for (int row = 0; row < splits_->rowCount(); ++row) {
        splits_->cellWidget(row, 2)->show();
    }
    QVector<SplitPart> parts;
    QStringList unavailable;
    const QString method = method_->currentData().toString();
    const bool equal = method == "equal";
    const bool ratio = method == "ratio";
    splits_->horizontalHeaderItem(2)->setText(equal   ? QStringLiteral("自动均摊（无需填写）")
                                              : ratio ? QStringLiteral("分摊比例")
                                                      : QStringLiteral("承担金额（元）"));
    split_hint_->setText(
        equal ? QStringLiteral(
                    "当前为按人数均摊，数值栏无需填写；如需手动填写，请在上方切换分摊方式。")
        : ratio
            ? QStringLiteral("给已勾选的成员填写正数比例，例如 1 和 2，表示分别承担 1/3 和 2/3。")
            : QStringLiteral(
                  "给已勾选的成员填写承担金额（元），合计须等于消费金额；未勾选的成员无需填写。"));
    for (int row = 0; row < splits_->rowCount(); ++row) {
        auto* value = qobject_cast<QLineEdit*>(splits_->cellWidget(row, 2));
        const bool checked = splits_->item(row, 0)->checkState() == Qt::Checked;
        const bool active = splits_->item(row, 0)->data(Qt::UserRole + 1).toBool();
        value->setEnabled(!equal && checked && active);
        value->setPlaceholderText(!checked ? QStringLiteral("勾选后填写")
                                  : equal  ? QStringLiteral("自动计算")
                                  : ratio  ? QStringLiteral("例如 1 或 2")
                                           : QStringLiteral("例如 30.00"));
        value->setToolTip(split_hint_->text());
        for (int column = 3; column < 6; ++column) {
            splits_->item(row, column)->setText(QStringLiteral("—"));
        }
        if (splits_->item(row, 0)->checkState() == Qt::Checked) {
            parts.append({splits_->item(row, 0)->data(Qt::UserRole).toString(), value->text()});
            if (!active) unavailable.append(splits_->item(row, 1)->data(Qt::UserRole).toString());
        }
    }
    if (!unavailable.isEmpty()) {
        preview_->setText(QStringLiteral("成员 %1 已退出。原输入已保留，请取消勾选并重新核对分摊，"
                                         "再保存或提交。")
                              .arg(unavailable.join(QStringLiteral("、"))));
        return;
    }
    if (amount_->text().trimmed().isEmpty()) {
        preview_->setText(QStringLiteral("填写金额后显示分摊预览。"));
        return;
    }
    try {
        const qint64 total = parseMoney(amount_->text());
        const auto values = calculateSplit(total, method, parts);
        QHash<QString, qint64> allocations;
        for (const auto& value : values) {
            allocations.insert(value.user_id, value.amount);
        }
        const QString payer_id = payer_->currentData().toString();
        if (payer_id.isEmpty()) {
            preview_->setText(QStringLiteral("请选择实际付款人后查看本笔应收、应还。"));
            return;
        }
        QStringList transfers;
        for (int row = 0; row < splits_->rowCount(); ++row) {
            const QString id = splits_->item(row, 0)->data(Qt::UserRole).toString();
            const qint64 owed = allocations.value(id), paid = id == payer_id ? total : 0;
            const qint64 net = paid - owed;
            splits_->item(row, 3)->setText(formatMoney(owed));
            splits_->item(row, 4)->setText(formatMoney(paid));
            splits_->item(row, 5)->setText(net > 0 ? QStringLiteral("应收 %1").arg(formatMoney(net))
                                           : net < 0
                                               ? QStringLiteral("应还 %1").arg(formatMoney(-net))
                                               : QStringLiteral("无需收还"));
            if (id != payer_id && owed > 0) {
                transfers.append(QStringLiteral("%1需还给%2 %3元")
                                     .arg(splits_->item(row, 1)->text(), payer_->currentText(),
                                          formatMoney(owed)));
            }
        }
        QString summary =
            QStringLiteral("分摊合计：%1元。%2已垫付%1元，自己承担%3元。\n%4")
                .arg(formatMoney(total), payer_->currentText(),
                     formatMoney(allocations.value(payer_id)),
                     transfers.isEmpty()     ? QStringLiteral("本笔无需其他人归还。")
                     : transfers.size() <= 3 ? transfers.join(QStringLiteral("；"))
                                             : transfers.mid(0, 3).join(QStringLiteral("；")) +
                                                   QStringLiteral("；其余见上表。"));
        if (equal && parts.size() == 1) {
            summary += QStringLiteral(
                "\n当前仅1人承担，均摊会将整笔费用分给该人；付款人也消费时，请同时勾选付款人。");
        }
        summary += QStringLiteral(
            "\n以上为本笔原始分摊，未抵扣退款、已还款或其他账目；实际待还以“结算与还款”的当前余额为"
            "准。审核并生成结算后，再登记实际还款；记账不会自动转账。");
        preview_->setText(summary);
    } catch (const ServiceError& error) {
        preview_->setText(error.message());
    }
}

void ExpensePage::refresh() {
    if (book_.isEmpty()) {
        return;
    }
    if (review_mode_ && !book_.value("capabilities").toObject().value("review").toBool()) {
        records_->setRowCount(0);
        return;
    }
    action([this] { reloadList(); });
}

void ExpensePage::reloadList() {
    const auto response = api_.call(
        review_mode_ ? "expense_review_queue" : "expense_list",
        {{"ledger_id", book_.value("id")}, {"status", filter_->currentData().toString()}});
    const auto rows = response.value("expenses").toArray();
    supplement_button_->setEnabled(can_write_ && response.value("can_supplement").toBool());
    supplement_button_->setToolTip(
        QStringLiteral("账本生成过结算后可用。普通成员提交审核；管理员核对后直接生效。"));
    records_->setRowCount(static_cast<int>(rows.size()));
    empty_->setVisible(rows.isEmpty());
    for (int row = 0; row < rows.size(); ++row) {
        const auto record = rows[row].toObject();
        const QString id = record.value("id").toString();
        const QStringList cells{
            record.value("date").toString(),
            record.value("description").toString(),
            record.value("payer").toString(),
            record.value("creator").toString(),
            record.value("amount").toString(),
            record.value("participants").toString(),
            statusName(record.value("status").toString()) +
                (record.value("superseded").toBool()      ? QStringLiteral("\n已被调整替代 / 冲销")
                 : record.value("is_adjustment").toBool() ? QStringLiteral("\n结算后调整")
                 : record.value("corrected").toBool()     ? QStringLiteral("\n已更正")
                                                          : QString())};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item =
                centeredTableItem(cells[column].isEmpty() ? QStringLiteral("—") : cells[column]);
            item->setToolTip(cells[column]);
            item->setData(Qt::UserRole, id);
            records_->setItem(row, column, item);
        }
        auto* operations = new QWidget(records_);
        auto* buttons = new QHBoxLayout(operations);
        buttons->setContentsMargins(2, 2, 2, 2);
        buttons->setSpacing(5);
        auto* view = makeButton(QStringLiteral("查看"), "viewExpense", operations);
        buttons->addWidget(view);
        connect(view, &QPushButton::clicked, this,
                [this, id] { action([this, id] { showDetail(id); }); });
        if (can_write_ && record.value("can_edit").toBool()) {
            const bool draft = record.value("status") == "draft";
            auto* edit = makeButton(draft ? QStringLiteral("编辑草稿") : QStringLiteral("修改"),
                                    draft ? "editExpenseDraft" : "editExpense", operations);
            buttons->addWidget(edit);
            connect(edit, &QPushButton::clicked, this,
                    [this, id] { action([this, id] { openDraft(id); }); });
        }
        if (can_write_ && record.value("can_withdraw").toBool()) {
            auto* withdraw = makeButton(QStringLiteral("撤回"), "withdrawExpense", operations);
            buttons->addWidget(withdraw);
            connect(withdraw, &QPushButton::clicked, this,
                    [this, record] { action([this, record] { withdrawRecord(record); }); });
        }
        if (!review_mode_ && book_.value("role") == "admin" &&
            record.value("status") == "settled" && !record.value("superseded").toBool()) {
            auto* correct =
                makeButton(QStringLiteral("结算后纠错"), "correctSettledExpense", operations);
            buttons->addWidget(correct);
            connect(correct, &QPushButton::clicked, this,
                    [this, id] { action([this, id] { startCorrection(id); }); });
        }
        records_->setRowHeight(
            row, record.value("is_adjustment").toBool() || record.value("superseded").toBool()
                     ? 68
                     : 54);
        records_->setCellWidget(row, 7, operations);
    }
    reloadNotices();
}

void ExpensePage::openDraft(const QString& id) {
    if (!confirmLeave()) {
        return;
    }
    const auto lease =
        api_.call("expense_lock_acquire", {{"ledger_id", book_.value("id")}, {"expense_id", id}});
    const auto record = lease.value("expense").toObject();
    if (!record.value("can_edit").toBool()) {
        throw ServiceError("forbidden", QStringLiteral("该账目不可编辑。"));
    }
    fillForm(record);
    lock_token_ = lease.value("lock_token").toString();
    last_activity_.start();
    stack_->setCurrentIndex(1);
    notice_->clear();
}

void ExpensePage::fillForm(const QJsonObject& record) {
    resetForm();
    loading_ = true;
    const auto form = record.value("form").toObject();
    editing_post_adjustment_ = form.value("post_settlement_adjustment").toBool();
    record_adjustment_reason_ = form.value("adjustment_reason").toString();
    editing_id_ = record.value("id").toString();
    version_ = record.value("version").toInt();
    editing_status_ = record.value("status").toString();
    amount_->setText(form.value("amount").toString());
    description_->setText(form.value("description").toString());
    category_->setText(form.value("category").toString());
    const QDate date = QDate::fromString(form.value("date").toString(), Qt::ISODate);
    date_->setDate(date.isValid() ? date : QDate::currentDate());
    payer_->setCurrentIndex(payer_->findData(form.value("payer_id").toString()));
    method_->setCurrentIndex(method_->findData(form.value("split_method").toString()));
    QHash<QString, QString> values;
    QHash<QString, QString> names;
    QSet<QString> present;
    for (int row = 0; row < splits_->rowCount(); ++row) {
        present.insert(splits_->item(row, 0)->data(Qt::UserRole).toString());
    }
    for (const auto& value : book_.value("financial_members").toArray()) {
        const auto member = value.toObject();
        names.insert(member.value("id").toString(), member.value("username").toString());
    }
    for (const auto& value : record.value("shares").toArray()) {
        const auto share = value.toObject();
        names.insert(share.value("user_id").toString(), share.value("username").toString());
    }
    for (const auto& part : form.value("splits").toArray()) {
        const auto value = part.toObject();
        const QString id = value.value("user_id").toString();
        values.insert(id, value.value("value").toString());
        if (!present.contains(id)) {
            // Never silently redistribute an old draft after a member leaves.
            addBearerRow({{"id", id}, {"username", names.value(id, id)}}, true);
            present.insert(id);
        }
    }
    for (int row = 0; row < splits_->rowCount(); ++row) {
        const QString id = splits_->item(row, 0)->data(Qt::UserRole).toString();
        splits_->item(row, 0)->setCheckState(values.contains(id) ? Qt::Checked : Qt::Unchecked);
        qobject_cast<QLineEdit*>(splits_->cellWidget(row, 2))->setText(values.value(id));
    }
    const bool draft = editing_status_ == "draft";
    form_title_->setText(draft ? QStringLiteral("编辑草稿") : QStringLiteral("修改账目"));
    reason_panel_->setVisible(!draft);
    draft_button_->setVisible(draft);
    submit_button_->setText(draft ? QStringLiteral("提交审核")
                                  : QStringLiteral("保存修改并重新提交"));
    loading_ = false;
    dirty_ = false;
    syncMembers();
}

bool ExpensePage::saveDraft() {
    if (!financial_mode_.isEmpty()) {
        return submit();
    }
    bool saved = false;
    action([&] {
        markField({});
        if (reconcileWrite()) {
            saved = true;
            return;
        }
        auto parameters = prepareWrite("expense_save_draft", formData());
        const auto result = api_.call("expense_save_draft", parameters);
        if (result.value("reused").toBool()) {
            reconcileWrite();
            saved = true;
            return;
        }
        removeRecovery();
        editing_id_ = result.value("id").toString();
        version_ = result.value("version").toInt();
        lock_token_ = result.value("lock_token").toString();
        last_activity_.start();
        dirty_ = false;
        saved = true;
        form_title_->setText(QStringLiteral("编辑草稿"));
        message(QStringLiteral("草稿已保存，可以继续编辑或提交。"));
    });
    return saved;
}

bool ExpensePage::submit() {
    if (financial_mode_ == "correction" ||
        (financial_mode_ == "supplement" && book_.value("role") == "admin")) {
        return submitAdjustment();
    }
    bool submitted = false;
    action([this, &submitted] {
        markField({});
        if (reconcileWrite()) {
            submitted = true;
            return;
        }
        auto form = formData();
        if (financial_mode_ == "supplement" && change_reason_->text().trimmed().isEmpty()) {
            throw ServiceError("validation", QStringLiteral("请填写补记原因。"), "change_reason");
        }
        if (financial_mode_ == "supplement" &&
            question(this, QStringLiteral("提交结算后补记"),
                     QStringLiteral(
                         "本次补记交由管理员审核，通过后计入当前余额；原结算快照保留。确认提交？"),
                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        form = prepareWrite("expense_submit", form);
        auto response = api_.call("expense_submit", form);
        if (response.value("confirmation_required").toBool()) {
            const int count = static_cast<int>(response.value("similar_records").toArray().size());
            if (question(
                    this, QStringLiteral("发现相似账目"),
                    QStringLiteral(
                        "其他成员已提交%"
                        "1条日期、金额、付款人和说明相同的账目。\n是否仍要提交，交由管理员核对？")
                        .arg(count),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
                message(QStringLiteral("已取消提交，表单内容保留。"));
                return;
            }
            form["confirm_similar"] = true;
            response = api_.call("expense_submit", form);
        }
        if (response.value("reused").toBool()) {
            reconcileWrite();
            submitted = true;
            return;
        }
        removeRecovery();
        dirty_ = false;
        lock_token_.clear();
        submitted = true;
        editing_id_.clear();
        version_ = 0;
        {
            const QSignalBlocker blocker(filter_);
            filter_->setCurrentIndex(0);
        }
        stack_->setCurrentIndex(0);
        reloadList();
        message(financial_mode_ == "supplement"
                    ? QStringLiteral("补记已提交，待管理员审核；通过后自动计入当前余额。")
                    : QStringLiteral("提交成功，账目状态为待审核。"));
        financial_mode_.clear();
    });
    return submitted;
}

bool ExpensePage::confirmLeave() {
    if (dirty_) {
        const int answer = question(
            this, QStringLiteral("保留未保存的账目"),
            !financial_mode_.isEmpty()
                ? QStringLiteral("当前退款相关调整尚未登记。是否先核对并提交本次补记或纠错？")
            : editing_status_ == "draft"
                ? QStringLiteral("当前账目还有未保存的内容。是否先保存草稿？")
                : QStringLiteral("当前修改尚未提交。是否保存修改并重新提交审核？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            persistRecovery();
            return false;
        }
        if (answer == QMessageBox::Discard) {
            action([this] { removeRecovery(); });
            if (!recovery_id_.isEmpty()) return false;
        }
        if (answer == QMessageBox::Save && !(!financial_mode_.isEmpty()   ? submit()
                                             : editing_status_ == "draft" ? saveDraft()
                                                                          : submit())) {
            return false;
        }
    }
    dirty_ = false;
    releaseLock();
    stack_->setCurrentIndex(0);
    return true;
}

void ExpensePage::showDetail(const QString& id) {
    ExpenseDetailsDialog dialog(api_, book_, id, review_mode_, this);
    connect(&dialog, &ExpenseDetailsDialog::sessionExpired, this, &ExpensePage::sessionExpired);
    dialog.exec();
    reloadList();
}

void ExpensePage::releaseLock() {
    if (!lock_token_.isEmpty() && api_.isAuthenticated() && !book_.isEmpty()) {
        try {
            api_.call("expense_lock_release", {{"ledger_id", book_.value("id")},
                                               {"expense_id", editing_id_},
                                               {"lock_token", lock_token_}});
        } catch (const ServiceError&) { /* Server lease expires after a lost connection. */
        }
    }
    lock_token_.clear();
}

void ExpensePage::renewLock() {
    if (lock_token_.isEmpty() || !can_write_ || stack_->currentIndex() != 1 ||
        last_activity_.elapsed() > 60000) {
        return;
    }
    action([this] {
        api_.call("expense_lock_renew", {{"ledger_id", book_.value("id")},
                                         {"expense_id", editing_id_},
                                         {"lock_token", lock_token_}});
    });
}

void ExpensePage::reacquireLock() {
    if (editing_id_.isEmpty()) {
        message(QStringLiteral("新账目尚未保存，无需编辑锁。"));
        return;
    }
    action([this] {
        const auto lease = api_.call(
            "expense_lock_acquire",
            {{"ledger_id", book_.value("id")}, {"expense_id", editing_id_}, {"version", version_}});
        lock_token_ = lease.value("lock_token").toString();
        last_activity_.start();
        message(QStringLiteral("编辑锁已获取，当前输入保留。"));
    });
}

void ExpensePage::reloadNotices() {
    if (review_mode_ || book_.value("historical").toBool()) {
        notices_panel_->hide();
        return;
    }
    const auto notices = api_.call("expense_notifications", {{"ledger_id", book_.value("id")}})
                             .value("notices")
                             .toArray();
    notices_panel_->setVisible(!notices.isEmpty());
    notices_->setRowCount(static_cast<int>(notices.size()));
    for (int row = 0; row < notices.size(); ++row) {
        const auto item = notices[row].toObject();
        const QStringList values{
            QDateTime::fromSecsSinceEpoch(item.value("time").toInteger()).toString("MM-dd HH:mm"),
            item.value("message").toString(),
            item.value("read").toBool() ? QStringLiteral("已读") : QStringLiteral("未读")};
        for (int col = 0; col < values.size(); ++col) {
            auto* cell = centeredTableItem(values[col]);
            cell->setToolTip(values[col]);
            notices_->setItem(row, col, cell);
        }
        auto* read = makeButton(QStringLiteral("标为已读"), "markExpenseNoticeRead", notices_);
        read->setEnabled(!item.value("read").toBool());
        setTableButton(notices_, row, 3, read);
        connect(read, &QPushButton::clicked, this, [this, item] {
            action([this, item] {
                api_.call("expense_notice_read",
                          {{"ledger_id", book_.value("id")}, {"notice_id", item.value("id")}});
                reloadNotices();
            });
        });
    }
}

void ExpensePage::withdrawRecord(const QJsonObject& record) {
    QDialog dialog(this);
    dialog.setObjectName("withdrawExpenseDialog");
    dialog.setWindowTitle(QStringLiteral("确认撤回账目"));
    dialog.resize(580, 230);
    auto* body = new QVBoxLayout(&dialog);
    body->addWidget(
        makeLabel(QStringLiteral(
                      "撤回后该账目不再参与统计和结算，原记录与操作原因会保留。请填写原因并确认。"),
                  &dialog));
    auto* reason = new QLineEdit(&dialog);
    reason->setObjectName("withdrawReason");
    reason->setMaxLength(500);
    reason->setPlaceholderText(QStringLiteral("撤回原因（必填）"));
    body->addWidget(reason);
    auto* error = makeLabel({}, &dialog);
    error->setObjectName("withdrawError");
    error->setStyleSheet("color:#b42318;");
    body->addWidget(error);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    auto* confirm = buttons->addButton(QStringLiteral("确认撤回"), QDialogButtonBox::AcceptRole);
    confirm->setObjectName("confirmWithdrawExpense");
    body->addWidget(buttons);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(confirm, &QPushButton::clicked, &dialog, [&, record] {
        try {
            api_.call("expense_withdraw", {{"ledger_id", book_.value("id")},
                                           {"expense_id", record.value("id")},
                                           {"version", record.value("version")},
                                           {"reason", reason->text()}});
            dialog.accept();
        } catch (const ServiceError& failure) {
            error->setText(failure.message());
            if (failure.code() == "unauthenticated") {
                emit sessionExpired(failure.message());
                dialog.reject();
            }
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        reloadList();
        message(QStringLiteral("账目已撤回，记录和原因已保留。"));
    }
}

void ExpensePage::action(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const ServiceError& error) {
        message(error.message() +
                    (error.code() == "connection"
                         ? QStringLiteral("当前表单内容已保留，可先查看列表核对是否已提交。")
                         : QString()),
                true);
        markField(error.field());
        if (error.code() == "unauthenticated") {
            emit sessionExpired(error.message());
        }
    }
}

void ExpensePage::message(const QString& text, bool error) {
    notice_->setText(text);
    notice_->setStyleSheet(error ? QStringLiteral("color:#b42318;")
                                 : QStringLiteral("color:#25765b;"));
}

void ExpensePage::markField(const QString& field) {
    const QHash<QString, QWidget*> widgets{{"amount", amount_},
                                           {"date", date_},
                                           {"payer_id", payer_},
                                           {"description", description_},
                                           {"split_method", method_},
                                           {"splits", splits_},
                                           {"change_reason", change_reason_}};
    for (auto item = widgets.begin(); item != widgets.end(); ++item) {
        item.value()->setProperty("invalid",
                                  item.key() == ((field == "reason" || field == "adjustment_reason")
                                                     ? QStringLiteral("change_reason")
                                                     : field));
        item.value()->style()->unpolish(item.value());
        item.value()->style()->polish(item.value());
    }
}

}  // namespace ledger
