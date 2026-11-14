#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QLocalServer>
#include <QLockFile>
#include <exception>

#include "identity/identity_service.h"
#include "infrastructure/database.h"
#include "transport/endpoint.h"
#include "transport/local_server.h"

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SharedLedgerService"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("data-dir"), QStringLiteral("Application data directory."),
                      QStringLiteral("directory")});
    parser.addOption({QStringLiteral("idle-timeout"), QStringLiteral("Exit after idle seconds."),
                      QStringLiteral("seconds"), QStringLiteral("5")});
    parser.process(application);
    const QString directory = parser.value(QStringLiteral("data-dir"));
    bool valid_timeout = false;
    const int timeout = parser.value(QStringLiteral("idle-timeout")).toInt(&valid_timeout);
    if (directory.isEmpty() || !valid_timeout || timeout < 1 || timeout > 3600 ||
        !QDir().mkpath(directory)) {
        return 1;
    }
    QLockFile owner(QDir(directory).filePath(QStringLiteral("service.lock")));
    if (!owner.tryLock(0)) {
        return 3;
    }
    try {
        ledger::Database database(
            QDir(directory).filePath(QStringLiteral("shared_ledger.sqlite3")));
        database.open();
        if (database.metadata(QStringLiteral("created_at")).isEmpty()) {
            database.setMetadata(QStringLiteral("created_at"),
                                 QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        }
        database.setMetadata(QStringLiteral("application_version"), QStringLiteral("0.9.3"));
        ledger::IdentityService identity(database);
        ledger::LocalServer server(identity, timeout);
        // The data-directory lock ensures this process owns any stale endpoint.
        QLocalServer::removeServer(ledger::localServerName(directory));
        if (!server.listen(ledger::localServerName(directory))) {
            return 2;
        }
        return application.exec();
    } catch (const std::exception&) {
        return 1;
    }
}
