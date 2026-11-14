#include "ui/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <array>

#include "domain/password_policy.h"
#include "domain/service_error.h"
#include "transport/api_client.h"
#include "ui/branding.h"
#include "ui/browse_page.h"
#include "ui/expense_page.h"
#include "ui/settlement_page.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
constexpr std::array<const char*, 6> kTitles{"账本与成员", "账目管理",   "审核与状态",
                                             "结算与还款", "查询与统计", "操作日志"};
constexpr std::array<const char*, 6> kDescriptions{
    "管理共同记账的账本与成员",         "记录支出，查看费用分摊与状态", "核对成员提交的账目",
    "查看待还待收、实际还款与结算快照", "按条件查找有权限查看的账目",   "查看相关操作与账目变更"};

QLabel* label(const QString& text, const char* name, QWidget* parent) {
    auto* widget = new QLabel(text, parent);
    widget->setTextFormat(Qt::PlainText);
    widget->setObjectName(QString::fromLatin1(name));
    widget->setWordWrap(true);
    return widget;
}
QPushButton* button(const QString& text, const char* name, QWidget* parent) {
    auto* widget = new QPushButton(text, parent);
    widget->setObjectName(QString::fromLatin1(name));
    widget->setCursor(Qt::PointingHandCursor);
    return widget;
}
QLineEdit* input(const QString& placeholder, const char* name, QWidget* parent) {
    auto* widget = new QLineEdit(parent);
    widget->setObjectName(QString::fromLatin1(name));
    widget->setPlaceholderText(placeholder);
    widget->setClearButtonEnabled(true);
    return widget;
}
QString roleName(const QString& role) {
    if (role == QStringLiteral("admin")) {
        return QStringLiteral("管理员");
    }
    if (role == QStringLiteral("member")) {
        return QStringLiteral("普通成员");
    }
    return QStringLiteral("只读用户");
}
QTableWidget* table(const QStringList& headings, const char* name, QWidget* parent) {
    auto* widget = new QTableWidget(0, static_cast<int>(headings.size()), parent);
    configureTable(widget);
    widget->setObjectName(QString::fromLatin1(name));
    widget->setHorizontalHeaderLabels(headings);
    widget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    widget->verticalHeader()->hide();
    widget->verticalHeader()->setDefaultSectionSize(48);
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows);

    widget->setMinimumHeight(180);
    return widget;
}
}  // namespace

MainWindow::MainWindow(ApiClient& api, QWidget* parent) : QMainWindow(parent), api_(api) {
    setWindowTitle(QStringLiteral("共享记账"));
    setWindowIcon(sharedLedgerIcon());
    resize(1440, 900);
    setMinimumSize(1050, 700);
    screens_ = new QStackedWidget(this);
    screens_->setObjectName(QStringLiteral("applicationRoot"));
    screens_->addWidget(createLoginPage());
    screens_->addWidget(createWorkspace());
    setCentralWidget(screens_);
}

QWidget* MainWindow::createLoginPage() {
    auto* page = new QWidget(screens_);
    auto* layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* introduction = new QFrame(page);
    introduction->setObjectName(QStringLiteral("sidebar"));
    auto* intro_layout = new QVBoxLayout(introduction);
    intro_layout->setContentsMargins(60, 70, 60, 70);
    intro_layout->addWidget(createBrandWidget(false, introduction));
    intro_layout->addStretch();
    intro_layout->addWidget(
        label(QStringLiteral("共享记账\n每一笔，都清楚。"), "loginHeadline", introduction));
    intro_layout->addWidget(label(QStringLiteral("与伙伴共享账本\n记录支出，核对分摊，安心结算。"),
                                  "loginCaption", introduction));
    intro_layout->addStretch();
    layout->addWidget(introduction, 1);
    auto* form_host = new QWidget(page);
    auto* host_layout = new QVBoxLayout(form_host);
    host_layout->setAlignment(Qt::AlignCenter);
    auto* card = new QFrame(form_host);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedWidth(450);
    auto* form = new QVBoxLayout(card);
    form->setContentsMargins(38, 38, 38, 38);
    form->setSpacing(18);
    form->addWidget(label(QStringLiteral("欢迎使用共享记账"), "emptyTitle", card));
    form->addWidget(
        label(QStringLiteral("使用自己的账号访问有权限的账本"), "pageDescription", card));
    auth_mode_ = new QTabBar(card);
    auth_mode_->setObjectName(QStringLiteral("authMode"));
    auth_mode_->addTab(QStringLiteral("登录"));
    auth_mode_->addTab(QStringLiteral("注册"));
    form->addWidget(auth_mode_);
    username_ = input(QStringLiteral("用户名（1–32位）"), "usernameInput", card);
    username_->setToolTip(
        QStringLiteral("支持单个汉字、字母或数字，也可使用下划线和短横线；用户名不能重复。"));
    // Reject overlong names on submission instead of silently truncating a paste.
    username_->setMaxLength(128);
    password_ = input(QStringLiteral("请输入密码"), "passwordInput", card);
    // Keep the login input compatible with passwords saved before the new policy.
    // Registration validates length explicitly rather than silently truncating a paste.
    password_->setMaxLength(128);
    password_->setEchoMode(QLineEdit::Password);
    confirmation_ = input(QStringLiteral("再次输入密码"), "confirmationInput", card);
    confirmation_->setMaxLength(128);
    confirmation_->setEchoMode(QLineEdit::Password);
    confirmation_->hide();
    form->addWidget(username_);
    form->addWidget(password_);
    form->addWidget(confirmation_);
    password_hint_ =
        label(QStringLiteral("8–32位，须同时包含英文字母和数字。\n可使用英文符号，不含空格。"),
              "passwordHint", card);
    password_hint_->hide();
    form->addWidget(password_hint_);
    auth_error_ = label(QString(), "authError", card);
    auth_error_->setMinimumHeight(24);
    form->addWidget(auth_error_);
    auth_submit_ = button(QStringLiteral("登录"), "authSubmit", card);
    auth_submit_->setDefault(true);
    form->addWidget(auth_submit_);
    connect(auth_mode_, &QTabBar::currentChanged, this, [this](int index) {
        confirmation_->setVisible(index == 1);
        password_hint_->setVisible(index == 1);
        password_->setPlaceholderText(index == 1 ? QStringLiteral("设置密码（8–32位）")
                                                 : QStringLiteral("请输入密码"));
        auth_submit_->setText(index == 1 ? QStringLiteral("注册并登录") : QStringLiteral("登录"));
        auth_error_->clear();
        password_->clear();
        confirmation_->clear();
    });
    connect(auth_submit_, &QPushButton::clicked, this, &MainWindow::submitAuthentication);
    connect(password_, &QLineEdit::returnPressed, this, &MainWindow::submitAuthentication);
    connect(confirmation_, &QLineEdit::returnPressed, this, &MainWindow::submitAuthentication);
    host_layout->addWidget(card);
    layout->addWidget(form_host, 1);
    return page;
}

QWidget* MainWindow::createWorkspace() {
    auto* page = new QWidget(screens_);
    auto* layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* sidebar = new QFrame(page);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(230);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(20, 30, 20, 28);
    side->setSpacing(22);
    side->addWidget(createBrandWidget(true, sidebar));
    side->addSpacing(25);
    side->addWidget(label(QStringLiteral("工作空间"), "sidebarCaption", sidebar));
    navigation_ = new QListWidget(sidebar);
    navigation_->setObjectName(QStringLiteral("navigation"));
    navigation_->setFocusPolicy(Qt::NoFocus);
    for (const auto* title : kTitles) {
        navigation_->addItem(QString::fromUtf8(title));
    }
    side->addWidget(navigation_, 1);
    side->addWidget(label(QStringLiteral("多人共享记账\n在同一个账本里，清楚记录每一笔。"),
                          "sidebarFooter", sidebar));
    layout->addWidget(sidebar);
    auto* content = new QWidget(page);
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(32, 24, 32, 24);
    body->setSpacing(15);
    auto* toolbar = new QHBoxLayout();
    ledgers_ = new QComboBox(content);
    ledgers_->setObjectName(QStringLiteral("ledgerSelector"));
    ledgers_->setMinimumWidth(250);
    toolbar->addWidget(ledgers_);
    toolbar->addStretch();
    account_ = label(QStringLiteral("未登录"), "accountBadge", content);
    toolbar->addWidget(account_);
    auto* refresh = button(QStringLiteral("刷新"), "refreshWorkspace", content);
    auto* logout = button(QStringLiteral("退出登录"), "logoutButton", content);
    toolbar->addWidget(refresh);
    toolbar->addWidget(logout);
    body->addLayout(toolbar);
    page_title_ = label(QString(), "pageTitle", content);
    page_description_ = label(QString(), "pageDescription", content);
    body->addWidget(page_title_);
    body->addWidget(page_description_);
    notice_ = label(QString(), "notice", content);
    notice_->setMinimumHeight(24);
    body->addWidget(notice_);
    pages_ = new QStackedWidget(content);
    pages_->addWidget(createLedgerPage());
    for (std::size_t index = 1; index < kTitles.size(); ++index) {
        if (index == 4 || index == 5) {
            auto* browse = new BrowsePage(api_, index == 5, pages_);
            if (index == 4) {
                query_expenses_ = browse;
            } else {
                audit_page_ = browse;
            }
            connect(browse, &BrowsePage::sessionExpired, this, [this](const QString& reason) {
                clearSession();
                auth_error_->setText(reason);
            });
            connect(browse, &BrowsePage::viewSettlementRequested, this,
                    [this] { navigation_->setCurrentRow(3); });
            pages_->addWidget(browse);
            continue;
        }
        if (index == 3) {
            settlement_page_ = new SettlementPage(api_, pages_);
            connect(settlement_page_, &SettlementPage::sessionExpired, this,
                    [this](const QString& reason) {
                        clearSession();
                        auth_error_->setText(reason);
                    });
            pages_->addWidget(settlement_page_);
            continue;
        }
        if (index == 1 || index == 2) {
            auto* expense = new ExpensePage(api_, index != 1, pages_, index == 2);
            if (index == 1) {
                expense_page_ = expense;
            } else if (index == 2) {
                review_expenses_ = expense;
            }
            connect(expense, &ExpensePage::sessionExpired, this, [this](const QString& reason) {
                clearSession();
                auth_error_->setText(reason);
            });
            connect(expense, &ExpensePage::manageMembersRequested, this, [this] {
                navigation_->setCurrentRow(0);
                if (pages_->currentIndex() == 0) {
                    runAction([this] { loadLedger(); });
                }
            });
            pages_->addWidget(expense);
            continue;
        }
        auto* empty = new QFrame(pages_);
        empty->setObjectName(QStringLiteral("emptyCard"));
        auto* box = new QVBoxLayout(empty);
        box->addStretch();
        auto* heading = label(QStringLiteral("暂无记录"), "emptyTitle", empty);
        heading->setAlignment(Qt::AlignCenter);
        box->addWidget(heading);
        auto* explanation =
            label(QStringLiteral("当前账本中还没有可展示的记录。"), "emptyDescription", empty);
        explanation->setAlignment(Qt::AlignCenter);
        box->addWidget(explanation);
        box->addStretch();
        pages_->addWidget(empty);
    }
    body->addWidget(pages_, 1);
    layout->addWidget(content, 1);
    connect(navigation_, &QListWidget::currentRowChanged, this, &MainWindow::selectPage);
    connect(ledgers_, &QComboBox::currentIndexChanged, this,
            [this] { runAction([this] { loadLedger(); }); });
    connect(refresh, &QPushButton::clicked, this, [this] {
        runAction([this] {
            refreshWorkspace();
            notify(QStringLiteral("已刷新。"));
        });
    });
    connect(logout, &QPushButton::clicked, this, &MainWindow::signOut);
    navigation_->setCurrentRow(0);
    return page;
}

QWidget* MainWindow::createLedgerPage() {
    auto* scroll = new QScrollArea(pages_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    auto* body = new QVBoxLayout(page);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(18);
    auto* create_row = new QHBoxLayout();
    new_ledger_name_ = input(QStringLiteral("填写新账本名称"), "newLedgerName", page);
    new_ledger_name_->setMaxLength(80);
    auto* create = button(QStringLiteral("创建账本"), "createLedgerButton", page);
    create_row->addWidget(new_ledger_name_, 1);
    create_row->addWidget(create);
    body->addLayout(create_row);
    ledger_summary_ =
        label(QStringLiteral("还没有账本，可以先创建账本或接受邀请。"), "ledgerSummary", page);
    body->addWidget(ledger_summary_);
    body->addWidget(createLedgerCloseControls(page));
    body->addWidget(createMembershipControls(page));
    members_ = table({QStringLiteral("成员"), QStringLiteral("角色"), QStringLiteral("加入时间"),
                      QStringLiteral("操作")},
                     "membersTable", page);
    body->addWidget(members_);
    departed_panel_ = new QWidget(page);
    auto* departed_body = new QVBoxLayout(departed_panel_);
    departed_body->setContentsMargins(0, 0, 0, 0);
    departed_body->addWidget(
        label(QStringLiteral("已退出成员（历史费用与还款保留）"), "sectionTitle", departed_panel_));
    departed_members_ = table({"成员", "退出时间", "历史只读权限", "结清截止日期"},
                              "departedMembersTable", departed_panel_);
    departed_body->addWidget(departed_members_);
    departed_panel_->hide();
    body->addWidget(departed_panel_);
    invitation_form_ = new QWidget(page);
    auto* invite_row = new QHBoxLayout(invitation_form_);
    invite_row->setContentsMargins(0, 0, 0, 0);
    invite_username_ =
        input(QStringLiteral("输入已注册用户的用户名"), "inviteUsername", invitation_form_);
    invite_username_->setMaxLength(128);
    invite_role_ = new QComboBox(invitation_form_);
    invite_role_->setObjectName(QStringLiteral("inviteRole"));
    invite_role_->addItem(QStringLiteral("普通成员"), QStringLiteral("member"));
    invite_role_->addItem(QStringLiteral("只读用户"), QStringLiteral("read_only"));
    auto* invite_button = button(QStringLiteral("邀请成员"), "inviteButton", invitation_form_);
    invite_row->addWidget(invite_username_, 1);
    invite_row->addWidget(invite_role_);
    invite_row->addWidget(invite_button);
    body->addWidget(invitation_form_);
    body->addWidget(label(QStringLiteral("收到的邀请"), "sectionTitle", page));
    invitations_ = table({QStringLiteral("账本"), QStringLiteral("邀请人"),
                          QStringLiteral("加入后的角色"), QStringLiteral("操作")},
                         "invitationsTable", page);
    body->addWidget(invitations_);
    body->addWidget(createClosedLedgerSection(page));
    body->addStretch();
    scroll->setWidget(page);
    connect(create, &QPushButton::clicked, this, [this] {
        runAction([this] {
            const auto book =
                api_.call(QStringLiteral("create_ledger"), {{"name", new_ledger_name_->text()}});
            new_ledger_name_->clear();
            refreshWorkspace(book.value("id").toString());
            notify(QStringLiteral("账本创建成功，你是该账本的管理员。"));
        });
    });
    connect(invite_button, &QPushButton::clicked, this, [this] {
        runAction([this] {
            api_.call(QStringLiteral("invite"), {{"ledger_id", ledgers_->currentData().toString()},
                                                 {"username", invite_username_->text()},
                                                 {"role", invite_role_->currentData().toString()}});
            invite_username_->clear();
            notify(QStringLiteral("邀请已发送，等待对方登录后接受。"));
        });
    });
    return scroll;
}

void MainWindow::submitAuthentication() {
    auth_error_->clear();
    if (auth_mode_->currentIndex() == 1) {
        const QString error = PasswordPolicy::registrationError(password_->text());
        if (!error.isEmpty()) {
            auth_error_->setText(error);
            return;
        }
    }
    if (auth_mode_->currentIndex() == 1 && password_->text() != confirmation_->text()) {
        auth_error_->setText(QStringLiteral("两次输入的密码不一致。"));
        return;
    }
    auth_submit_->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        if (auth_mode_->currentIndex() == 1) {
            api_.call(QStringLiteral("register"),
                      {{"username", username_->text()}, {"password", password_->text()}});
        }
        user_ = api_.login(username_->text(), password_->text());
        password_->clear();
        confirmation_->clear();
        refreshWorkspace();
        screens_->setCurrentIndex(1);
        notify(QStringLiteral("欢迎，") + user_.value("username").toString());
    } catch (const ServiceError& error) {
        auth_error_->setText(error.message());
    }
    QApplication::restoreOverrideCursor();
    auth_submit_->setEnabled(true);
}

void MainWindow::refreshWorkspace(const QString& preferred_ledger) {
    user_ = api_.call(QStringLiteral("me"));
    account_->setText(user_.value("username").toString());
    const QString selected =
        preferred_ledger.isEmpty() ? ledgers_->currentData().toString() : preferred_ledger;
    const auto books = api_.call(QStringLiteral("list_ledgers")).value("ledgers").toArray();
    {
        const QSignalBlocker blocker(ledgers_);
        ledgers_->clear();
        for (const auto& value : books) {
            const auto book = value.toObject();
            ledgers_->addItem(book.value("name").toString() + (book.value("historical").toBool()
                                                                   ? QStringLiteral("（历史只读）")
                                                                   : QString()),
                              book.value("id").toString());
        }
        const int index = ledgers_->findData(selected);
        if (index >= 0) {
            ledgers_->setCurrentIndex(index);
        }
    }
    renderInvitations(api_.call(QStringLiteral("invitations")));
    renderClosedLedgers();
    loadLedger();
}

void MainWindow::loadLedger() {
    notice_->clear();
    const QString requested = ledgers_->currentData().toString();
    if (requested != active_ledger_id_ && !expense_page_->confirmLeave()) {
        const QSignalBlocker blocker(ledgers_);
        ledgers_->setCurrentIndex(ledgers_->findData(active_ledger_id_));
        return;
    }
    if (ledgers_->currentIndex() < 0) {
        active_ledger_id_.clear();
        historical_ = false;
        exit_ledger_button_->hide();
        transfer_admin_button_->hide();
        expense_page_->clearContext();
        query_expenses_->clearContext();
        review_expenses_->clearContext();
        settlement_page_->clearContext();
        members_->setRowCount(0);
        departed_members_->setRowCount(0);
        departed_panel_->hide();
        invitation_form_->hide();
        close_ledger_panel_->hide();
        ledger_summary_->setText(QStringLiteral("还没有账本，可以先创建账本或接受下方邀请。"));
        for (int index = 1; index < 6; ++index) {
            navigation_->item(index)->setHidden(true);
        }
        audit_page_->setContext({}, user_);
        navigation_->item(5)->setHidden(false);
        navigation_->setCurrentRow(0);
        return;
    }
    const auto book = api_.call(QStringLiteral("ledger_details"),
                                {{"ledger_id", ledgers_->currentData().toString()}});
    historical_ = book.value("historical").toBool();
    renderMembers(book);
    active_ledger_id_ = requested;
    expense_page_->setContext(book, user_);
    query_expenses_->setContext(book, user_);
    audit_page_->setContext(book, user_);
    if (historical_) {
        review_expenses_->clearContext();
        settlement_page_->clearContext();
    } else {
        review_expenses_->setContext(book, user_);
        settlement_page_->setContext(book, user_);
    }
    const auto capabilities = book.value("capabilities").toObject();
    invitation_form_->setVisible(capabilities.value("manage_members").toBool());
    navigation_->item(1)->setHidden(!capabilities.value("write_expense").toBool());
    navigation_->item(2)->setHidden(!capabilities.value("review").toBool());
    // Active read-only members may inspect balances; write controls stay permission-gated.
    navigation_->item(3)->setHidden(historical_);
    navigation_->item(4)->setHidden(false);
    navigation_->item(5)->setHidden(false);
    if (navigation_->currentItem()->isHidden()) {
        navigation_->setCurrentRow(0);
    }
}

void MainWindow::renderMembers(const QJsonObject& book) {
    const auto rows = book.value("members").toArray();
    const bool admin = book.value("role").toString() == QStringLiteral("admin");
    const bool historical = book.value("historical").toBool();
    exit_ledger_button_->setVisible(!historical);
    transfer_admin_button_->setVisible(admin && rows.size() > 1);
    close_ledger_panel_->setVisible(admin);
    close_ledger_button_->setEnabled(admin && rows.size() == 1);
    close_ledger_hint_->setText(
        rows.size() > 1
            ? QStringLiteral("账本还有其他当前成员，需先处理成员退出或负责人交接，不能直接解散。")
            : QStringLiteral("归档保留数据，可恢复；删除业务数据不可恢复，操作日志始终保留。"));
    ledger_summary_->setText(QStringLiteral("%1  ·  当前成员 %2 人  ·  我的角色：%3")
                                 .arg(book.value("name").toString())
                                 .arg(rows.size())
                                 .arg(roleName(book.value("role").toString())));
    if (historical) {
        ledger_summary_->setText(
            QStringLiteral("%1 · "
                           "已退出（历史只读）\n仅显示退出时保存的账目、成员和明细；当前余额与后续"
                           "变化不可见。退出时间：%2；结清截止：%3")
                .arg(book.value("name").toString(),
                     QDateTime::fromSecsSinceEpoch(book.value("exited_at").toInteger())
                         .toString("yyyy-MM-dd HH:mm"),
                     book.value("settlement_deadline").toString().isEmpty()
                         ? QStringLiteral("未设置")
                         : book.value("settlement_deadline").toString()));
    }
    QJsonArray departed;
    for (const auto& value : book.value("financial_members").toArray()) {
        if (value.toObject().value("status") == "exited") {
            departed.append(value);
        }
    }
    departed_panel_->setVisible(!historical && !departed.isEmpty());
    departed_members_->setRowCount(departed.size());
    for (int row = 0; row < departed.size(); ++row) {
        const auto member = departed[row].toObject();
        const QStringList values{
            member.value("username").toString(),
            QDateTime::fromSecsSinceEpoch(member.value("exited_at").toInteger())
                .toString("yyyy-MM-dd HH:mm"),
            member.value("keep_history").toBool() ? QStringLiteral("保留")
                                                  : QStringLiteral("不保留"),
            member.value("settlement_deadline").toString().isEmpty()
                ? QStringLiteral("未设置")
                : member.value("settlement_deadline").toString()};
        for (int column = 0; column < values.size(); ++column) {
            departed_members_->setItem(row, column, centeredTableItem(values[column]));
        }
    }
    members_->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < rows.size(); ++row) {
        const auto member = rows[row].toObject();
        const QString role = member.value("role").toString();
        members_->setItem(row, 0, centeredTableItem(member.value("username").toString()));
        members_->setItem(row, 1, centeredTableItem(roleName(role)));
        members_->setItem(
            row, 2,
            centeredTableItem(QDateTime::fromSecsSinceEpoch(member.value("joined_at").toInteger())
                                  .toString(QStringLiteral("yyyy-MM-dd"))));
        members_->removeCellWidget(row, 3);
        members_->setItem(row, 3, centeredTableItem(QStringLiteral("—")));
        if (admin && role != QStringLiteral("admin")) {
            const QString next_role = role == QStringLiteral("member") ? QStringLiteral("read_only")
                                                                       : QStringLiteral("member");
            auto* change =
                button(next_role == QStringLiteral("member") ? QStringLiteral("设为普通成员")
                                                             : QStringLiteral("设为只读"),
                       "changeRoleButton", members_);
            setTableButton(members_, row, 3, change);
            connect(change, &QPushButton::clicked, this, [this, member, next_role] {
                runAction([this, member, next_role] {
                    api_.call(QStringLiteral("change_role"),
                              {{"ledger_id", ledgers_->currentData().toString()},
                               {"user_id", member.value("id").toString()},
                               {"role", next_role}});
                    loadLedger();
                    notify(QStringLiteral("成员权限已更新。"));
                });
            });
        }
    }
}

void MainWindow::renderInvitations(const QJsonObject& result) {
    const auto rows = result.value("invitations").toArray();
    invitations_->setRowCount(static_cast<int>(rows.size()));
    for (int row = 0; row < rows.size(); ++row) {
        const auto invitation = rows[row].toObject();
        invitations_->setItem(row, 0,
                              centeredTableItem(invitation.value("ledger_name").toString()));
        invitations_->setItem(row, 1, centeredTableItem(invitation.value("inviter").toString()));
        invitations_->setItem(row, 2,
                              centeredTableItem(roleName(invitation.value("role").toString())));
        auto* accept = button(QStringLiteral("接受邀请"), "acceptInvitationButton", invitations_);
        accept->setCursor(Qt::PointingHandCursor);
        setTableButton(invitations_, row, 3, accept);
        connect(accept, &QPushButton::clicked, this, [this, invitation] {
            runAction([this, invitation] {
                const auto book = api_.call(QStringLiteral("accept_invitation"),
                                            {{"invitation_id", invitation.value("id").toString()}});
                refreshWorkspace(book.value("id").toString());
                notify(QStringLiteral("已加入账本。"));
            });
        });
    }
}

void MainWindow::selectPage(int index) {
    if (index < 0 || index >= 6) {
        return;
    }
    if (pages_->currentIndex() == 1 && index != 1 && !expense_page_->confirmLeave()) {
        const QSignalBlocker blocker(navigation_);
        navigation_->setCurrentRow(1);
        return;
    }
    page_title_->setText(historical_ && index == 4
                             ? QStringLiteral("历史账目（退出时快照）")
                             : QString::fromUtf8(kTitles[static_cast<std::size_t>(index)]));
    page_description_->setText(QString::fromUtf8(kDescriptions[static_cast<std::size_t>(index)]));
    pages_->setCurrentIndex(index);
    if (index == 1) {
        expense_page_->refresh();
    }
    if (index == 4) {
        query_expenses_->refresh();
    }
    if (index == 2) {
        review_expenses_->refresh();
    }
    if (index == 3) {
        settlement_page_->refresh();
    }
    if (index == 5) {
        audit_page_->refresh();
    }
}

void MainWindow::signOut() {
    if (!expense_page_->confirmLeave()) {
        return;
    }
    try {
        api_.logout();
    } catch (const ServiceError&) { /* Clear local state even if disconnected. */
    }
    clearSession();
}

void MainWindow::clearSession() {
    expense_page_->clearContext();
    query_expenses_->clearContext();
    audit_page_->clearContext();
    review_expenses_->clearContext();
    settlement_page_->clearContext();
    active_ledger_id_.clear();
    user_ = {};
    {
        const QSignalBlocker blocker(ledgers_);
        ledgers_->clear();
    }
    members_->setRowCount(0);
    invitations_->setRowCount(0);
    closed_ledgers_->setRowCount(0);
    closed_empty_->show();
    close_ledger_panel_->hide();
    account_->setText(QStringLiteral("未登录"));
    new_ledger_name_->clear();
    invite_username_->clear();
    username_->clear();
    password_->clear();
    confirmation_->clear();
    auth_mode_->setCurrentIndex(0);
    auth_error_->clear();
    screens_->setCurrentIndex(0);
}

void MainWindow::runAction(const std::function<void()>& action) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        action();
    } catch (const ServiceError& error) {
        if (error.code() == QStringLiteral("unauthenticated")) {
            clearSession();
            auth_error_->setText(error.message());
        } else {
            notify(error.message(), true);
        }
    }
    QApplication::restoreOverrideCursor();
}

void MainWindow::notify(const QString& text, bool error) {
    notice_->setText(text);
    notice_->setStyleSheet(error ? QStringLiteral("color:#b42318;")
                                 : QStringLiteral("color:#25765b;"));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (api_.isAuthenticated() && !expense_page_->confirmLeave()) {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

}  // namespace ledger
