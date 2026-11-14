#pragma once

#include <QSqlDatabase>
#include <QString>

namespace ledger {

class Database final {
public:
    explicit Database(QString path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    void open();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] int schemaVersion() const;
    [[nodiscard]] QString metadata(const QString& key) const;
    void setMetadata(const QString& key, const QString& value);
    // Available only to backend code; the desktop target does not link this library.
    [[nodiscard]] QSqlDatabase connection() const;

private:
    void migrate();
    void execute(const QString& statement);

    QString path_;
    QString connection_name_;
    QSqlDatabase connection_;
};

}  // namespace ledger
