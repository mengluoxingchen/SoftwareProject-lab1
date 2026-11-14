#include "recovery/recovery_store.h"

// clang-format off
// Windows crypto headers require the base Windows declarations first.
#include <windows.h>
#include <wincrypt.h>
// clang-format on

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#include "domain/service_error.h"

namespace ledger {
namespace {
constexpr qsizetype limit = 512 * 1024;
[[noreturn]] void failed(const QString& reason) {
    throw ServiceError("recovery", QStringLiteral("本地恢复副本：") + reason);
}
QByteArray protect(QByteArray bytes, QByteArray entropy, bool encrypt) {
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())};
    DATA_BLOB output{};
    const BOOL ok = encrypt ? CryptProtectData(&input, nullptr, &extra, nullptr, nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN, &output)
                            : CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr,
                                                 CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        failed(QStringLiteral("无法加密或读取（Windows错误%1）；原文件保留，请勿覆盖。")
                   .arg(GetLastError()));
    QByteArray result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}
bool hasSecret(const QJsonValue& value) {
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (QStringList{"password", "token", "lock_token"}.contains(it.key()) ||
                hasSecret(it.value()))
                return true;
        }
    }
    if (value.isArray())
        for (const auto& item : value.toArray())
            if (hasSecret(item)) return true;
    return false;
}
}  // namespace
RecoveryStore::RecoveryStore(const QString& data_directory, const QString& user,
                             const QString& ledger)
    : owner_(user), ledger_(ledger) {
    if (data_directory.isEmpty() || user.isEmpty() || ledger.isEmpty())
        failed(QStringLiteral("未选择账号或账本。"));
    const auto scope = QJsonDocument(QJsonObject{{"user", user}, {"ledger", ledger}})
                           .toJson(QJsonDocument::Compact);
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex());
    directory_ = QDir(data_directory).absoluteFilePath("recovery/" + hash);
}
QString RecoveryStore::newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString RecoveryStore::path(const QString& id) const {
    if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id)
        failed(QStringLiteral("副本标识不正确。"));
    return QDir(directory_).filePath(id + ".recovery");
}
QByteArray RecoveryStore::entropy(const QString& id) const {
    return QCryptographicHash::hash(
        (QDir::cleanPath(directory_).toCaseFolded() + "/" + id).toUtf8(),
        QCryptographicHash::Sha256);
}
QStringList RecoveryStore::entries() const {
    QStringList result;
    for (const auto& name : QDir(directory_).entryList({"*.recovery"}, QDir::Files, QDir::Time)) {
        const QString id = name.chopped(9);
        if (!QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id)
            result.append(id);
    }
    return result;
}
std::unique_ptr<RecoveryLease> RecoveryStore::claim(const QString& id) const {
    const auto filename = path(id);
    if (!QDir().mkpath(directory_)) failed(QStringLiteral("无法创建目录。"));
    auto lock = std::make_unique<QLockFile>(filename + ".lock");
    lock->setStaleLockTime(0);  // Never steal a live window's copy based on elapsed time.
    if (!lock->tryLock()) return {};
    auto lease = std::make_unique<RecoveryLease>();
    lease->path_ = filename;
    lease->lock_ = std::move(lock);
    return lease;
}
void RecoveryStore::write(const QString& id, const QJsonObject& snapshot,
                          const RecoveryLease& lease) const {
    if (!lease.isLocked() || lease.path_ != path(id))
        failed(QStringLiteral("副本正在其他窗口使用。"));
    if (hasSecret(snapshot)) failed(QStringLiteral("禁止保存登录凭据或编辑锁。"));
    if (!QFile::exists(path(id)) && entries().size() >= 100)
        failed(QStringLiteral("已有100份副本，请先清理不需要的副本。"));
    const QJsonObject envelope{{"schema", 1},
                               {"owner", owner_},
                               {"ledger", ledger_},
                               {"id", id},
                               {"saved_at", QDateTime::currentSecsSinceEpoch()},
                               {"snapshot", snapshot}};
    const auto plain = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (plain.size() > limit) failed(QStringLiteral("内容超过512KiB，原副本保留。"));
    const auto bytes = protect(plain, entropy(id), true);
    QSaveFile file(path(id));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        failed(QStringLiteral("磁盘写入失败，原副本保留；请手动保存草稿。"));
}
QJsonObject RecoveryStore::read(const QString& id) const {
    QFile file(path(id));
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit + 16384)
        failed(QStringLiteral("文件不可读或大小异常，原文件保留。"));
    QJsonParseError error;
    const auto document =
        QJsonDocument::fromJson(protect(file.readAll(), entropy(id), false), &error);
    const auto envelope = document.object();
    if (error.error != QJsonParseError::NoError || envelope.value("schema") != 1 ||
        envelope.value("owner") != owner_ || envelope.value("ledger") != ledger_ ||
        envelope.value("id") != id || !envelope.value("snapshot").isObject())
        failed(QStringLiteral("副本校验失败，原文件保留。"));
    return envelope;
}
void RecoveryStore::remove(const QString& id, const RecoveryLease& lease) const {
    if (!lease.isLocked() || lease.path_ != path(id) ||
        (QFile::exists(path(id)) && !QFile::remove(path(id))))
        failed(QStringLiteral("无法删除副本。"));
}
}  // namespace ledger
