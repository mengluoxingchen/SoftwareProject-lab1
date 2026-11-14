#pragma once
#include <QJsonObject>
#include <QLockFile>
#include <QStringList>
#include <memory>
namespace ledger {
class RecoveryLease final {
    friend class RecoveryStore;
    QString path_;
    std::unique_ptr<QLockFile> lock_;

public:
    bool isLocked() const { return lock_ && lock_->isLocked(); }
};
class RecoveryStore final {
public:
    RecoveryStore(const QString& data_directory, const QString& user, const QString& ledger);
    static QString newId();
    QStringList entries() const;
    std::unique_ptr<RecoveryLease> claim(const QString& id) const;
    void write(const QString& id, const QJsonObject& snapshot, const RecoveryLease& lease) const;
    QJsonObject read(const QString& id) const;
    void remove(const QString& id, const RecoveryLease& lease) const;
    QString path(const QString& id) const;

private:
    QByteArray entropy(const QString& id) const;
    QString directory_, owner_, ledger_;
};
}  // namespace ledger
