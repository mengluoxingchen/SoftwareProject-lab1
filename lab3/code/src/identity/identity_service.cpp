#include "identity/identity_service.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlError>
#include <QUuid>
#include <utility>

#include "domain/password_policy.h"
#include "domain/service_error.h"
#include "identity/password.h"
#include "infrastructure/audit_store.h"
#include "infrastructure/database.h"

namespace ledger {
namespace {

QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString usernameKey(const QString& username) {
    return username.trimmed().normalized(QString::NormalizationForm_C).toCaseFolded();
}
QString requiredString(const QJsonObject& parameters, const QString& key, int maximum = 128) {
    const auto value = parameters.value(key);
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > maximum) {
        throw ServiceError(QStringLiteral("validation"),
                           QStringLiteral("输入不完整或超出允许长度。"));
    }
    return value.toString();
}
QString memberRole(const QJsonObject& parameters) {
    const QString role = requiredString(parameters, QStringLiteral("role"));
    if (role != QStringLiteral("member") && role != QStringLiteral("read_only")) {
        throw ServiceError(QStringLiteral("validation"),
                           QStringLiteral("请选择普通成员或只读用户。"));
    }
    return role;
}
QByteArray tokenHash(const QString& token) {
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
}
class Transaction final {
public:
    explicit Transaction(QSqlDatabase database) : database_(std::move(database)) {
        if (!database_.transaction()) {
            throw std::runtime_error("Cannot begin transaction.");
        }
    }
    ~Transaction() {
        if (!committed_) {
            database_.rollback();
        }
    }
    void commit() {
        if (!database_.commit()) {
            throw std::runtime_error("Cannot commit transaction.");
        }
        committed_ = true;
    }

private:
    QSqlDatabase database_;
    bool committed_ = false;
};

}  // namespace

IdentityService::IdentityService(Database& database, Clock clock)
    : database_(database.connection()),
      clock_(std::move(clock)),
      expenses_(database, clock_),
      settlements_(database, clock_),
      finances_(database, clock_),
      queries_(database) {
    if (!clock_) {
        clock_ = [] { return QDateTime::currentSecsSinceEpoch(); };
    }
}

QSqlQuery IdentityService::query(const QString& sql, const QVariantList& parameters) const {
    QSqlQuery result(database_);
    if (!result.prepare(sql)) {
        throw std::runtime_error(result.lastError().text().toStdString());
    }
    for (const auto& parameter : parameters) {
        result.addBindValue(parameter);
    }
    if (!result.exec()) {
        throw std::runtime_error(result.lastError().text().toStdString());
    }
    return result;
}

QJsonObject IdentityService::dispatch(const QString& method, const QJsonObject& parameters,
                                      const QString& token) {
    if (method == QStringLiteral("ping")) {
        return {{"version", "0.9.3"}};
    }
    if (method == QStringLiteral("register")) {
        return registerUser(parameters);
    }
    if (method == QStringLiteral("login")) {
        return login(parameters);
    }
    const QString actor = authenticate(token);
    if (method.startsWith("query_") || method.startsWith("audit_")) {
        return queries_.dispatch(actor, method, parameters);
    }
    if (method == "membership_exit_preview") {
        return membershipPreview(actor, parameters);
    }
    if (method == "exit_ledger" || method == "transfer_admin") {
        return changeMembership(actor, parameters, method == "exit_ledger");
    }
    if (method.startsWith("expense_") || method.startsWith("finance_")) {
        if (const auto historical = historicalRead(actor, method, parameters)) {
            return *historical;
        }
    }
    if (method.startsWith(QStringLiteral("finance_"))) {
        return finances_.dispatch(actor, method, parameters);
    }
    if (method == "ledger_close_preview") {
        return closePreview(actor, parameters);
    }
    if (method == "ledger_close") {
        return closeLedger(actor, parameters);
    }
    if (method == "closed_ledgers") {
        return closedLedgers(actor);
    }
    if (method == "restore_ledger") {
        return restoreLedger(actor, parameters);
    }
    if (method == "closed_ledger_logs") {
        return closedLedgerLogs(actor, parameters);
    }
    if (method.startsWith(QStringLiteral("settlement_"))) {
        return settlements_.dispatch(actor, method, parameters);
    }
    if (method.startsWith(QStringLiteral("expense_"))) {
        return expenses_.dispatch(actor, method, parameters,
                                  QString::fromLatin1(tokenHash(token).toHex()));
    }
    if (method == QStringLiteral("me")) {
        return user(actor);
    }
    if (method == QStringLiteral("logout")) {
        query(QStringLiteral("DELETE FROM sessions WHERE token_hash=?"), {tokenHash(token)});
        return {};
    }
    if (method == QStringLiteral("list_ledgers")) {
        return listLedgers(actor);
    }
    if (method == QStringLiteral("create_ledger")) {
        return createLedger(actor, parameters);
    }
    if (method == QStringLiteral("ledger_details")) {
        return ledgerDetails(actor, parameters);
    }
    if (method == QStringLiteral("invite")) {
        return invite(actor, parameters);
    }
    if (method == QStringLiteral("invitations")) {
        return invitations(actor);
    }
    if (method == QStringLiteral("accept_invitation")) {
        return acceptInvitation(actor, parameters);
    }
    if (method == QStringLiteral("change_role")) {
        return changeRole(actor, parameters);
    }
    throw ServiceError(QStringLiteral("unknown_method"), QStringLiteral("不支持的操作。"));
}

QJsonObject IdentityService::registerUser(const QJsonObject& parameters) {
    const QString username = requiredString(parameters, QStringLiteral("username"), 32)
                                 .trimmed()
                                 .normalized(QString::NormalizationForm_C);
    const auto password_value = parameters.value(QStringLiteral("password"));
    if (!password_value.isString()) {
        throw ServiceError(QStringLiteral("validation"), QStringLiteral("请输入密码。"));
    }
    const QString password = password_value.toString();
    static const QRegularExpression valid_name(QStringLiteral("^[\\p{L}\\p{N}_-]{1,32}$"));
    if (!valid_name.match(username).hasMatch()) {
        throw ServiceError(QStringLiteral("validation"),
                           QStringLiteral("用户名需为1至32位文字、数字、下划线或短横线。"));
    }
    const QString password_error = PasswordPolicy::registrationError(password);
    if (!password_error.isEmpty()) {
        throw ServiceError(QStringLiteral("validation"), password_error);
    }
    auto existing =
        query(QStringLiteral("SELECT id FROM users WHERE username_key=?"), {usernameKey(username)});
    if (existing.next()) {
        throw ServiceError(QStringLiteral("conflict"), QStringLiteral("该用户名已被使用。"));
    }
    const auto password_record = hashPassword(password);
    const QString id = identifier();
    query(QStringLiteral("INSERT INTO users VALUES(?,?,?,?,?,?,?)"),
          {id, username, usernameKey(username), password_record.salt, password_record.hash,
           password_record.iterations, clock_()});
    return user(id);
}

QJsonObject IdentityService::login(const QJsonObject& parameters) {
    const QString key = usernameKey(requiredString(parameters, QStringLiteral("username"), 32));
    const QString password = requiredString(parameters, QStringLiteral("password"));
    auto& attempt = login_attempts_[key];
    if (attempt.blocked_until > clock_()) {
        throw ServiceError(QStringLiteral("rate_limited"),
                           QStringLiteral("尝试次数过多，请30秒后重试。"));
    }
    if (attempt.blocked_until != 0) {
        attempt = {};
    }
    auto account = query(QStringLiteral("SELECT id,password_salt,password_hash,password_iterations "
                                        "FROM users WHERE username_key=?"),
                         {key});
    const bool found = account.next();
    const PasswordRecord record =
        found ? PasswordRecord{account.value(1).toByteArray(), account.value(2).toByteArray(),
                               account.value(3).toInt()}
              : PasswordRecord{QByteArray(16, '\0'), QByteArray(32, '\0'), 600000};
    const bool correct = verifyPassword(password, record);
    if (!found || !correct) {
        if (++attempt.failures >= 5) {
            attempt.blocked_until = clock_() + 30;
        }
        throw ServiceError(QStringLiteral("invalid_credentials"),
                           QStringLiteral("用户名或密码错误。"));
    }
    login_attempts_.remove(key);
    const QString token = QString::fromLatin1(secureRandomBytes(32).toHex());
    query(QStringLiteral("DELETE FROM sessions WHERE expires_at<=?"), {clock_()});
    query(QStringLiteral("INSERT INTO sessions VALUES(?,?,?)"),
          {tokenHash(token), account.value(0), clock_() + 8 * 60 * 60});
    return {{"token", token}, {"user", user(account.value(0).toString())}};
}

QString IdentityService::authenticate(const QString& token) const {
    if (token.size() != 64) {
        throw ServiceError(QStringLiteral("unauthenticated"), QStringLiteral("请先登录。"));
    }
    auto session =
        query(QStringLiteral("SELECT user_id FROM sessions WHERE token_hash=? AND expires_at>?"),
              {tokenHash(token), clock_()});
    if (!session.next()) {
        throw ServiceError(QStringLiteral("unauthenticated"),
                           QStringLiteral("登录已失效，请重新登录。"));
    }
    return session.value(0).toString();
}

QJsonObject IdentityService::user(const QString& id) const {
    auto account = query(QStringLiteral("SELECT id,username FROM users WHERE id=?"), {id});
    if (!account.next()) {
        throw std::runtime_error("Account no longer exists.");
    }
    return {{"id", account.value(0).toString()}, {"username", account.value(1).toString()}};
}

QString IdentityService::requireMember(const QString& ledger_id, const QString& user_id) const {
    auto membership = query(
        QStringLiteral(
            "SELECT m.role FROM memberships m JOIN ledgers l ON l.id=m.ledger_id "
            "WHERE m.ledger_id=? AND m.user_id=? AND m.status='active' AND l.status='active'"),
        {ledger_id, user_id});
    if (!membership.next()) {
        throw ServiceError(QStringLiteral("forbidden"), QStringLiteral("无权限访问该账本。"));
    }
    return membership.value(0).toString();
}

void IdentityService::requireAdmin(const QString& ledger_id, const QString& user_id) const {
    if (requireMember(ledger_id, user_id) != QStringLiteral("admin")) {
        throw ServiceError(QStringLiteral("forbidden"),
                           QStringLiteral("无权限，只有账本管理员可以执行此操作。"));
    }
}

void IdentityService::log(const QString& ledger_id, const QString& actor, const QString& action,
                          const QString& target, const QString& detail) {
    const auto log_id = identifier();
    query(QStringLiteral("INSERT INTO operation_logs VALUES(?,?,?,?,?,?,?)"),
          {log_id, ledger_id, actor, action, target, detail, clock_()});
    appendAuditEvidence(database_, log_id);
}

QJsonObject IdentityService::listLedgers(const QString& actor) const {
    auto rows =
        query(QStringLiteral("SELECT l.id,l.name,m.role,l.created_at,m.status FROM ledgers l JOIN "
                             "memberships m ON l.id=m.ledger_id "
                             "WHERE m.user_id=? AND ((m.status='active' AND l.status='active') OR "
                             "(m.status='exited' AND m.keep_history=1 AND l.status<>'deleted')) "
                             "ORDER BY l.created_at,l.id"),
              {actor});
    QJsonArray result;
    while (rows.next()) {
        result.append(QJsonObject{
            {"id", rows.value(0).toString()},
            {"name", rows.value(1).toString()},
            {"role", rows.value(4).toString() == "exited" ? QStringLiteral("history_read_only")
                                                          : rows.value(2).toString()},
            {"historical", rows.value(4).toString() == "exited"},
            {"created_at", rows.value(3).toLongLong()}});
    }
    return {{"ledgers", result}};
}

QJsonObject IdentityService::createLedger(const QString& actor, const QJsonObject& parameters) {
    const QString name = requiredString(parameters, QStringLiteral("name"), 80).trimmed();
    if (name.isEmpty()) {
        throw ServiceError(QStringLiteral("validation"), QStringLiteral("请填写账本名称。"));
    }
    const QString id = identifier();
    Transaction transaction(database_);
    query(QStringLiteral("INSERT INTO ledgers(id,name,created_by,created_at) VALUES(?,?,?,?)"),
          {id, name, actor, clock_()});
    query(QStringLiteral(
              "INSERT INTO memberships(ledger_id,user_id,role,joined_at) VALUES(?,?,'admin',?)"),
          {id, actor, clock_()});
    log(id, actor, QStringLiteral("create_ledger"), id, name);
    transaction.commit();
    return ledgerDetails(actor, {{"ledger_id", id}});
}

QJsonObject IdentityService::ledgerDetails(const QString& actor,
                                           const QJsonObject& parameters) const {
    const QString id = requiredString(parameters, QStringLiteral("ledger_id"));
    if (const auto history = historicalRead(actor, "history_context", parameters)) {
        return *history;
    }
    const QString role = requireMember(id, actor);
    auto book = query(QStringLiteral("SELECT name,created_at FROM ledgers WHERE id=?"), {id});
    book.next();
    auto rows = query(QStringLiteral("SELECT u.id,u.username,m.role,m.joined_at FROM memberships m "
                                     "JOIN users u ON u.id=m.user_id "
                                     "WHERE m.ledger_id=? AND m.status='active' ORDER BY "
                                     "(m.role='admin') DESC,m.joined_at,u.username"),
                      {id});
    QJsonArray members;
    while (rows.next()) {
        members.append(QJsonObject{{"id", rows.value(0).toString()},
                                   {"username", rows.value(1).toString()},
                                   {"role", rows.value(2).toString()},
                                   {"joined_at", rows.value(3).toLongLong()}});
    }
    QJsonArray financial_members;
    auto past = query(
        "SELECT u.id,u.username,m.status,m.settlement_deadline,m.exited_at,m.keep_history FROM "
        "memberships m JOIN users u ON "
        "u.id=m.user_id WHERE m.ledger_id=? ORDER BY u.username,u.id",
        {id});
    while (past.next()) {
        financial_members.append(QJsonObject{{"id", past.value(0).toString()},
                                             {"username", past.value(1).toString()},
                                             {"status", past.value(2).toString()},
                                             {"settlement_deadline", past.value(3).toString()},
                                             {"exited_at", past.value(4).toLongLong()},
                                             {"keep_history", past.value(5).toBool()}});
    }
    const bool admin = role == QStringLiteral("admin");
    const bool write = role != QStringLiteral("read_only");
    return {{"id", id},
            {"name", book.value(0).toString()},
            {"created_at", book.value(1).toLongLong()},
            {"role", role},
            {"members", members},
            {"financial_members", financial_members},
            {"capabilities", QJsonObject{{"manage_members", admin},
                                         {"review", admin},
                                         {"write_expense", write},
                                         {"settle", write}}}};
}

QJsonObject IdentityService::invite(const QString& actor, const QJsonObject& parameters) {
    const QString ledger_id = requiredString(parameters, QStringLiteral("ledger_id"));
    requireAdmin(ledger_id, actor);
    const QString key = usernameKey(requiredString(parameters, QStringLiteral("username"), 32));
    const QString role = memberRole(parameters);
    auto target = query(QStringLiteral("SELECT id FROM users WHERE username_key=?"), {key});
    if (!target.next()) {
        throw ServiceError(QStringLiteral("not_found"),
                           QStringLiteral("该用户尚未注册，请先让对方注册账号。"));
    }
    const QString target_id = target.value(0).toString();
    auto member =
        query(QStringLiteral(
                  "SELECT 1 FROM memberships WHERE ledger_id=? AND user_id=? AND status='active'"),
              {ledger_id, target_id});
    if (member.next()) {
        throw ServiceError(QStringLiteral("conflict"), QStringLiteral("该用户已经是账本成员。"));
    }
    auto pending = query(QStringLiteral("SELECT 1 FROM invitations WHERE ledger_id=? AND "
                                        "target_user_id=? AND status='pending'"),
                         {ledger_id, target_id});
    if (pending.next()) {
        throw ServiceError(QStringLiteral("conflict"), QStringLiteral("已有等待对方接受的邀请。"));
    }
    const QString id = identifier();
    Transaction transaction(database_);
    query(QStringLiteral(
              "INSERT INTO invitations(id,ledger_id,target_user_id,inviter_id,role,created_at) "
              "VALUES(?,?,?,?,?,?)"),
          {id, ledger_id, target_id, actor, role, clock_()});
    log(ledger_id, actor, QStringLiteral("invite_member"), target_id, role);
    transaction.commit();
    return {{"id", id}};
}

QJsonObject IdentityService::invitations(const QString& actor) const {
    auto rows =
        query(QStringLiteral(
                  "SELECT i.id,l.name,u.username,i.role,i.created_at FROM invitations i JOIN "
                  "ledgers l ON "
                  "l.id=i.ledger_id JOIN users u ON u.id=i.inviter_id WHERE i.target_user_id=? "
                  "AND i.status='pending' AND l.status='active' ORDER BY i.created_at DESC,i.id"),
              {actor});
    QJsonArray result;
    while (rows.next()) {
        result.append(QJsonObject{{"id", rows.value(0).toString()},
                                  {"ledger_name", rows.value(1).toString()},
                                  {"inviter", rows.value(2).toString()},
                                  {"role", rows.value(3).toString()}});
    }
    return {{"invitations", result}};
}

QJsonObject IdentityService::acceptInvitation(const QString& actor, const QJsonObject& parameters) {
    const QString id = requiredString(parameters, QStringLiteral("invitation_id"));
    auto invitation = query(QStringLiteral("SELECT ledger_id,role FROM invitations WHERE id=? AND "
                                           "target_user_id=? AND status='pending'"),
                            {id, actor});
    if (!invitation.next()) {
        throw ServiceError(QStringLiteral("forbidden"),
                           QStringLiteral("该邀请不可接受，或不属于当前账号。"));
    }
    const QString ledger_id = invitation.value(0).toString();
    const QString role = invitation.value(1).toString();
    auto book =
        query(QStringLiteral("SELECT 1 FROM ledgers WHERE id=? AND status='active'"), {ledger_id});
    if (!book.next()) {
        throw ServiceError(QStringLiteral("conflict"), QStringLiteral("该账本已不可加入。"));
    }
    Transaction transaction(database_);
    query(QStringLiteral(
              "INSERT INTO memberships(ledger_id,user_id,role,joined_at) VALUES(?,?,?,?) "
              "ON CONFLICT(ledger_id,user_id) DO UPDATE SET role=excluded.role,status='active',"
              "joined_at=excluded.joined_at,exited_at=NULL,keep_history=0"),
          {ledger_id, actor, role, clock_()});
    query("DELETE FROM membership_history WHERE ledger_id=? AND user_id=?", {ledger_id, actor});
    query(
        "UPDATE memberships SET settlement_deadline=\'\',history_context=\'{}\' WHERE ledger_id=? "
        "AND user_id=?",
        {ledger_id, actor});
    query(QStringLiteral("UPDATE invitations SET status='accepted',accepted_at=? WHERE id=?"),
          {clock_(), id});
    log(ledger_id, actor, QStringLiteral("accept_invitation"), actor, role);
    transaction.commit();
    return ledgerDetails(actor, {{"ledger_id", ledger_id}});
}

QJsonObject IdentityService::changeRole(const QString& actor, const QJsonObject& parameters) {
    const QString ledger_id = requiredString(parameters, QStringLiteral("ledger_id"));
    requireAdmin(ledger_id, actor);
    const QString target_id = requiredString(parameters, QStringLiteral("user_id"));
    const QString role = memberRole(parameters);
    const QString previous = requireMember(ledger_id, target_id);
    if (previous == QStringLiteral("admin")) {
        throw ServiceError(QStringLiteral("conflict"),
                           QStringLiteral("管理员权限需要通过负责人交接变更。"));
    }
    Transaction transaction(database_);
    query(QStringLiteral("UPDATE memberships SET role=? WHERE ledger_id=? AND user_id=?"),
          {role, ledger_id, target_id});
    log(ledger_id, actor, QStringLiteral("change_role"), target_id,
        previous + QStringLiteral(" -> ") + role);
    transaction.commit();
    return ledgerDetails(actor, {{"ledger_id", ledger_id}});
}

}  // namespace ledger
