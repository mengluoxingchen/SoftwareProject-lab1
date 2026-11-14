#include <QRegularExpression>
#include <QUuid>
#include <stdexcept>
#include <utility>

#include "domain/password_policy.h"
#include "domain/service_error.h"
#include "identity/identity_service.h"
#include "identity/password.h"

namespace ledger {
namespace {
class SecurityTransaction final {
public:
    explicit SecurityTransaction(QSqlDatabase db) : db_(std::move(db)) {
        if (!db_.transaction()) throw std::runtime_error("Cannot begin account transaction.");
    }
    ~SecurityTransaction() {
        if (!committed_) db_.rollback();
    }
    void commit() {
        if (!db_.commit()) throw std::runtime_error("Cannot commit account transaction.");
        committed_ = true;
    }

private:
    QSqlDatabase db_;
    bool committed_ = false;
};
QString field(const QJsonObject& parameters, const QString& name) {
    const auto value = parameters.value(name);
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 128) {
        throw ServiceError("validation", QStringLiteral("请填写完整信息，且每项不超过128位。"));
    }
    return value.toString();
}
QString newPassword(const QJsonObject& parameters) {
    const QString password = field(parameters, "new_password");
    const QString error = PasswordPolicy::registrationError(password);
    if (!error.isEmpty()) throw ServiceError("validation", error);
    if (password != field(parameters, "confirm_password")) {
        throw ServiceError("validation", QStringLiteral("两次输入的新密码不一致。"));
    }
    return password;
}
QString normalizedCode(QString value) { return value.trimmed().remove(' ').remove('-').toUpper(); }
}  // namespace

QJsonObject IdentityService::accountSecurity(const QString& actor) const {
    auto record = query("SELECT created_at FROM account_recovery WHERE user_id=?", {actor});
    const bool enabled = record.next();
    return {{"has_recovery_code", enabled},
            {"recovery_created_at", enabled ? record.value(0).toLongLong() : 0}};
}

void IdentityService::recordSecurityEvent(const QString& actor, const QString& action) {
    // Account events are private and never expose credentials in shared ledger logs.
    query("INSERT INTO account_security_events(id,user_id,action,created_at) VALUES(?,?,?,?)",
          {QUuid::createUuid().toString(QUuid::WithoutBraces), actor, action, clock_()});
}

QString IdentityService::storeRecoveryCode(const QString& actor) {
    const QString secret = QString::fromLatin1(secureRandomBytes(16).toHex()).toUpper();
    const auto record = hashPassword(secret);
    query(
        "INSERT INTO account_recovery VALUES(?,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET "
        "salt=excluded.salt,hash=excluded.hash,iterations=excluded.iterations,created_at=excluded."
        "created_at",
        {actor, record.salt, record.hash, record.iterations, clock_()});
    QStringList groups;
    for (int i = 0; i < secret.size(); i += 4) groups.append(secret.mid(i, 4));
    return groups.join('-');
}

void IdentityService::verifyCurrentPassword(const QString& actor, const QString& password) {
    auto& attempt = security_attempts_["current:" + actor];
    if (attempt.blocked_until > clock_()) {
        throw ServiceError("rate_limited", QStringLiteral("验证次数过多，请30秒后重试。"));
    }
    if (attempt.blocked_until != 0) attempt = {};
    auto row = query("SELECT password_salt,password_hash,password_iterations FROM users WHERE id=?",
                     {actor});
    if (!row.next()) throw std::runtime_error("Account not found.");
    if (!verifyPassword(password, {row.value(0).toByteArray(), row.value(1).toByteArray(),
                                   row.value(2).toInt()})) {
        if (++attempt.failures >= 5) attempt.blocked_until = clock_() + 30;
        throw ServiceError("invalid_credentials", QStringLiteral("当前密码不正确。"));
    }
    security_attempts_.remove("current:" + actor);
}

QJsonObject IdentityService::generateRecoveryCode(const QString& actor,
                                                  const QJsonObject& parameters) {
    verifyCurrentPassword(actor, field(parameters, "current_password"));
    SecurityTransaction transaction(database_);
    const auto code = storeRecoveryCode(actor);
    recordSecurityEvent(actor, "recovery_generated");
    transaction.commit();
    return {{"recovery_code", code}};
}

QJsonObject IdentityService::changePassword(const QString& actor, const QJsonObject& parameters) {
    const QString replacement = newPassword(parameters);
    const QString current = field(parameters, "current_password");
    verifyCurrentPassword(actor, current);
    if (replacement == current) {
        throw ServiceError("validation", QStringLiteral("新密码不能与当前密码相同。"));
    }
    const auto record = hashPassword(replacement);
    SecurityTransaction transaction(database_);
    query("UPDATE users SET password_salt=?,password_hash=?,password_iterations=? WHERE id=?",
          {record.salt, record.hash, record.iterations, actor});
    query("DELETE FROM sessions WHERE user_id=?", {actor});
    recordSecurityEvent(actor, "password_changed");
    transaction.commit();
    return {};
}

QJsonObject IdentityService::resetPassword(const QJsonObject& parameters) {
    const QString password = newPassword(parameters);
    QString name = field(parameters, "username")
                       .trimmed()
                       .normalized(QString::NormalizationForm_C)
                       .toCaseFolded();
    if (name.size() > 32) throw ServiceError("validation", QStringLiteral("用户名最多32位。"));
    const QString code = normalizedCode(field(parameters, "recovery_code"));
    auto& attempt = security_attempts_["reset:" + name];
    if (attempt.blocked_until > clock_()) {
        throw ServiceError("rate_limited", QStringLiteral("验证次数过多，请30秒后重试。"));
    }
    if (attempt.blocked_until != 0) attempt = {};
    auto row = query(
        "SELECT u.id,r.salt,r.hash,r.iterations FROM users u LEFT JOIN account_recovery r "
        "ON r.user_id=u.id WHERE u.username_key=?",
        {name});
    const bool exists = row.next();
    const bool configured = exists && !row.value(1).isNull();
    const PasswordRecord record =
        configured ? PasswordRecord{row.value(1).toByteArray(), row.value(2).toByteArray(),
                                    row.value(3).toInt()}
                   : PasswordRecord{QByteArray(16, '\0'), QByteArray(32, '\0'), 600000};
    const bool verified = verifyPassword(code, record);
    static const QRegularExpression format("^[A-F0-9]{32}$");
    if (!configured || !format.match(code).hasMatch() || !verified) {
        if (++attempt.failures >= 5) attempt.blocked_until = clock_() + 30;
        throw ServiceError("invalid_recovery",
                           QStringLiteral("用户名或恢复码不正确，或该账号尚未设置恢复码。"));
    }
    const QString actor = row.value(0).toString();
    const auto replacement = hashPassword(password);
    SecurityTransaction transaction(database_);
    query("UPDATE users SET password_salt=?,password_hash=?,password_iterations=? WHERE id=?",
          {replacement.salt, replacement.hash, replacement.iterations, actor});
    const QString next_code = storeRecoveryCode(actor);
    query("DELETE FROM sessions WHERE user_id=?", {actor});
    recordSecurityEvent(actor, "password_reset");
    transaction.commit();
    security_attempts_.remove("reset:" + name);
    login_attempts_.remove(name);
    return {{"recovery_code", next_code}};
}
}  // namespace ledger
