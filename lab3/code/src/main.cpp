#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <exception>
#include <stdexcept>

#include "transport/api_client.h"
#include "ui/branding.h"
#include "ui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SoftwareEngineeringLab"));
    QCoreApplication::setApplicationName(QStringLiteral("SharedLedger"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.9.3"));
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setWindowIcon(ledger::sharedLedgerIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("多人共享记账桌面应用"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption data_option(QStringLiteral("data-dir"),
                                         QStringLiteral("Override the local data directory."),
                                         QStringLiteral("directory"));
    const QCommandLineOption smoke_option(
        QStringLiteral("smoke-test"),
        QStringLiteral("Initialize the window and database, then exit."));
    const QCommandLineOption screenshot_option(
        QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot before exiting."),
        QStringLiteral("file"));
    parser.addOptions({data_option, smoke_option, screenshot_option});
    parser.process(application);

    QString data_directory = parser.value(data_option);
    if (data_directory.isEmpty()) {
        data_directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    }

    try {
        ledger::ApiClient api;
        api.connectToService(data_directory);
        if (api.call(QStringLiteral("ping")).value("version").toString() !=
            QCoreApplication::applicationVersion()) {
            throw std::runtime_error(QStringLiteral("记账服务仍是旧版本。请保存草稿并关闭所有共享记"
                                                    "账窗口，等待约6秒，再运行run.cmd。")
                                         .toStdString());
        }

        QFile stylesheet(QStringLiteral(":/resources/styles.qss"));
        if (stylesheet.open(QIODevice::ReadOnly)) {
            application.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
        }
        ledger::MainWindow window(api);
        window.show();
        if (parser.isSet(smoke_option) || parser.isSet(screenshot_option)) {
            QTimer::singleShot(400, &window, [&] {
                if (parser.isSet(screenshot_option) &&
                    !window.grab().save(parser.value(screenshot_option))) {
                    application.exit(2);
                    return;
                }
                application.exit(0);
            });
        }
        return application.exec();
    } catch (const std::exception& error) {
        if (parser.isSet(smoke_option) || parser.isSet(screenshot_option)) {
            qCritical("Application startup failed: %s", error.what());
        } else {
            QMessageBox::critical(nullptr, QStringLiteral("无法启动共享记账"),
                                  QStringLiteral("无法连接记账服务或初始化本地数据。\n\n") +
                                      QString::fromUtf8(error.what()));
        }
        return 1;
    }
}
