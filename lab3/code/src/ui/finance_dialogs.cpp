#include "ui/finance_dialogs.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "domain/service_error.h"
#include "domain/split.h"
#include "transport/api_client.h"
#include "ui/table_style.h"

namespace ledger {
namespace {
QLabel* plain(const QString& text, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
qint64 cents(QString value) {
    value.remove('.');
    return value.toLongLong();
}
QString dateText(const QJsonValue& value) {
    return QDateTime::fromSecsSinceEpoch(value.toInteger()).toString("yyyy-MM-dd HH:mm:ss");
}
}  // namespace
QString financeKind(const QString& kind, bool voided) {
    return kind == "refund"       ? QStringLiteral("退款")
           : kind == "supplement" ? QStringLiteral("结算后补记")
           : voided               ? QStringLiteral("结算后冲销")
                                  : QStringLiteral("结算后纠错");
}

bool recordRefundDialog(ApiClient& api, const QJsonObject& book, const QJsonObject& record,
                        QWidget* parent) {
    const auto info = api.call(
        "finance_info", {{"ledger_id", book.value("id")}, {"source_id", record.value("id")}});
    if (!info.value("can_refund").toBool()) {
        throw ServiceError(
            "forbidden",
            QStringLiteral("当前记录不可退款，请核对角色、状态、剩余金额或最新调整记录。"));
    }
    QDialog dialog(parent);
    dialog.setObjectName("refundDialog");
    dialog.setWindowTitle(QStringLiteral("登记商家退款"));
    dialog.setMinimumWidth(600);
    auto* body = new QVBoxLayout(&dialog);
    body->setContentsMargins(24, 20, 24, 20);
    body->setSpacing(14);
    auto* title = plain(QStringLiteral("登记实际收到的商家退款"), &dialog);
    title->setObjectName("sectionTitle");
    body->addWidget(title);
    body->addWidget(plain(
        QStringLiteral("支出：%1\n原始金额 %2 元，累计退款 %3 元，剩余可退 %4 元。")
            .arg(record.value("form").toObject().value("description").toString(),
                 info.value("gross_amount").toString(), info.value("refunded_amount").toString(),
                 info.value("net_amount").toString()),
        &dialog));
    body->addWidget(plain(QStringLiteral("收款人请选择实际拿到商家退款的成员。退款不同于成员间还款"
                                         "；登记后按剩余承担比例分配，原记录和旧快照保留。"),
                          &dialog));
    auto* grid = new QGridLayout();
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    auto* receiver = new QComboBox(&dialog);
    receiver->setObjectName("refundReceiver");
    const auto receivers = info.value("receivers").toArray().isEmpty()
                               ? book.value("members").toArray()
                               : info.value("receivers").toArray();
    for (const auto& value : receivers) {
        const auto member = value.toObject();
        receiver->addItem(
            member.value("username").toString() + (member.value("status").toString() == "exited"
                                                       ? QStringLiteral("（历史成员）")
                                                       : QString()),
            member.value("id").toString());
    }
    receiver->setCurrentIndex(receiver->findData(info.value("payer_id").toString()));
    auto* amount = new QLineEdit(&dialog);
    amount->setObjectName("refundAmount");
    amount->setMaxLength(32);
    amount->setPlaceholderText(QStringLiteral("填写本次实际退款金额"));
    auto* time = new QDateTimeEdit(QDateTime::currentDateTime(), &dialog);
    time->setObjectName("refundTime");
    time->setCalendarPopup(true);
    time->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    auto* reason = new QLineEdit(&dialog);
    reason->setObjectName("refundReason");
    reason->setMaxLength(500);
    reason->setPlaceholderText(QStringLiteral("必填，如退货或商家多收款"));
    grid->addWidget(plain(QStringLiteral("实际退款收款人 *"), &dialog), 0, 0);
    grid->addWidget(receiver, 1, 0, 1, 2);
    grid->addWidget(plain(QStringLiteral("本次退款金额（元）*"), &dialog), 2, 0);
    grid->addWidget(plain(QStringLiteral("退款时间 *"), &dialog), 2, 1);
    grid->addWidget(amount, 3, 0);
    grid->addWidget(time, 3, 1);
    body->addLayout(grid);
    body->addWidget(plain(QStringLiteral("退款原因 *"), &dialog));
    body->addWidget(reason);
    auto* error = plain(QString(), &dialog);
    error->setObjectName("refundError");
    error->setStyleSheet("color:#b42318;");
    body->addWidget(error);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto* save = buttons->button(QDialogButtonBox::Save);
    save->setText(QStringLiteral("核对并登记退款"));
    save->setObjectName("confirmRefund");
    save->style()->unpolish(save);
    save->style()->polish(save);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    body->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(amount, &QLineEdit::textChanged, error, &QLabel::clear);
    QObject::connect(reason, &QLineEdit::textChanged, error, &QLabel::clear);
    const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString expired;
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        try {
            api.call("finance_refund", {{"ledger_id", book.value("id")},
                                        {"source_id", record.value("id")},
                                        {"amount", amount->text()},
                                        {"receiver_id", receiver->currentData().toString()},
                                        {"occurred_at", time->dateTime().toSecsSinceEpoch()},
                                        {"reason", reason->text()},
                                        {"expected_token", info.value("token")},
                                        {"request_key", key}});
            dialog.accept();
        } catch (const ServiceError& failure) {
            error->setText(
                failure.message() +
                (failure.code() == "connection"
                     ? QStringLiteral("输入已保留，请先核对是否已登记，勿重复录入同一退款。")
                     : QString()));
            if (failure.code() == "unauthenticated") {
                expired = failure.message();
                dialog.reject();
            }
        }
    });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (!expired.isEmpty()) {
        throw ServiceError("unauthenticated", expired);
    }
    return accepted;
}

void showFinanceEventDialog(ApiClient& api, const QJsonObject& book, const QString& event_id,
                            QWidget* parent) {
    const auto result =
        api.call("finance_event", {{"ledger_id", book.value("id")}, {"event_id", event_id}});
    const auto before = result.value("before").toObject(), after = result.value("after").toObject();
    QDialog dialog(parent);
    dialog.setObjectName("financeEventDialog");
    dialog.setWindowTitle(QStringLiteral("退款与调整历史"));
    dialog.resize(900, 620);
    auto* body = new QVBoxLayout(&dialog);
    body->setContentsMargins(22, 20, 22, 20);
    body->setSpacing(12);
    auto* title = plain(financeKind(result.value("kind").toString(),
                                    after.value("expense_id").toString().isEmpty()),
                        &dialog);
    title->setObjectName("sectionTitle");
    body->addWidget(title);
    body->addWidget(plain(
        QStringLiteral("时间：%1　处理人：%2\n原因：%3\n原始金额：%4 → %5 元；净支出：%6 → %7 元。")
            .arg(dateText(result.value("occurred_at")), result.value("actor").toString(),
                 result.value("reason").toString(), before.value("gross_amount").toString("0.00"),
                 after.value("gross_amount").toString(),
                 before.value("net_amount").toString("0.00"), after.value("net_amount").toString()),
        &dialog));
    if (result.value("kind") == "refund") {
        body->addWidget(
            plain(QStringLiteral("本次商家退款 %1 元，实际收款人：%2。以下表格展示费用承担变化。")
                      .arg(result.value("amount").toString(), result.value("receiver").toString()),
                  &dialog));
    }
    body->addWidget(
        plain(QStringLiteral("历史前后值固定保存；原支出和原结算快照不会被本页覆盖。"), &dialog));
    if (result.value("kind") == "correction") {
        QMap<QString, QString> names;
        for (const auto& value : book.value("members").toArray()) {
            const auto member = value.toObject();
            names[member.value("id").toString()] = member.value("username").toString();
        }
        const auto describe = [&](const QJsonObject& state) {
            const auto form = state.value("form").toObject();
            const QString payer = state.value("payer_id").toString();
            const QString method = form.value("split_method").toString();
            return QStringLiteral("实际付款人：%1；消费日期：%2\n说明：%3；类别：%4；分摊方式：%5")
                .arg(names.value(payer, payer), form.value("date").toString(),
                     form.value("description").toString(), form.value("category").toString(),
                     method == "equal"   ? QStringLiteral("按人数均摊")
                     : method == "ratio" ? QStringLiteral("按比例")
                                         : QStringLiteral("指定金额"));
        };
        auto* forms = new QPlainTextEdit(&dialog);
        forms->setObjectName("financeEventForms");
        forms->setReadOnly(true);
        forms->setPlainText(
            QStringLiteral("调整前：\n%1\n\n调整后：\n%2")
                .arg(describe(before), after.value("expense_id").toString().isEmpty()
                                           ? QStringLiteral("已冲销，不产生新支出。")
                                           : describe(after)));
        forms->setMinimumHeight(150);
        forms->setMaximumHeight(190);
        body->addWidget(forms);
        dialog.resize(900, 780);
    }
    auto* table = new QTableWidget(0, 4, &dialog);
    configureTable(table);
    table->setObjectName("financeEventShares");
    table->setHorizontalHeaderLabels(
        {"成员", "调整前承担（元）", "调整后承担（元）", "承担变动（元）"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(48);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    struct Values {
        QString name;
        qint64 before = 0, after = 0;
    };
    QMap<QString, Values> members;
    for (const auto& value : before.value("effective_shares").toArray()) {
        const auto row = value.toObject();
        auto& member = members[row.value("user_id").toString()];
        member.name = row.value("username").toString();
        member.before = cents(row.value("amount").toString());
    }
    for (const auto& value : after.value("effective_shares").toArray()) {
        const auto row = value.toObject();
        auto& member = members[row.value("user_id").toString()];
        member.name = row.value("username").toString();
        member.after = cents(row.value("amount").toString());
    }
    table->setRowCount(static_cast<int>(members.size()));
    int row = 0;
    for (auto it = members.begin(); it != members.end(); ++it, ++row) {
        const auto& member = it.value();
        const QStringList values{member.name.isEmpty() ? it.key() : member.name,
                                 formatMoney(member.before), formatMoney(member.after),
                                 formatMoney(member.after - member.before)};
        for (int column = 0; column < values.size(); ++column) {
            table->setItem(row, column, centeredTableItem(values[column]));
        }
    }
    body->addWidget(table, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    body->addWidget(close);
    dialog.exec();
}
}  // namespace ledger
