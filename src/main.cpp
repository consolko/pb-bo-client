#include "client.h"
#include "device.h"
#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

int main(int argc, char **argv) {
#ifdef POCKETBOOK_DEVICE
    qputenv("QT_PLUGIN_PATH", "/ebrmain/plugins");
#endif
    QStringList args;
    for (int i=1; i<argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    const bool smoke = args.contains("--smoke-test");
    const bool offline = args.contains("--offline-test");
    auto option = [&args](const QString &name, const QString &fallback) {
        const int i = args.indexOf(name);
        return i >= 0 && i+1 < args.size() ? args[i+1] : fallback;
    };
    QString server = option("--server", "https://books.lan");
    while (server.endsWith('/')) server.chop(1);
    QSize screen(600, 800);
    std::unique_ptr<QCoreApplication> app;
    if (smoke || offline) {
#ifdef POCKETBOOK_DEVICE
        if (setupDevice().isEmpty()) return 3;
#endif
        app = std::make_unique<QCoreApplication>(argc, argv);
    }
    else {
#ifdef POCKETBOOK_DEVICE
        qputenv("QT_QPA_PLATFORM", "pocketbook2");
        QCoreApplication::setSetuidAllowed(true);
#endif
        std::fprintf(stderr, "bookorbit: initialize device\n");
        screen = setupDevice();
        if (screen.isEmpty()) return 0;
        std::fprintf(stderr, "bookorbit: screen %dx%d\n", screen.width(), screen.height());
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        app = std::make_unique<QGuiApplication>(argc, argv);
        std::fprintf(stderr, "bookorbit: Qt ready\n");
        QGuiApplication::setFont(QFont(deviceFont()));
    }
    QCoreApplication::setApplicationName("bookorbit");
#ifdef POCKETBOOK_DEVICE
    const QString defaultData = "/mnt/ext1/applications/bookorbit";
#else
    const QString defaultData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
    // Isolated storage is only available to explicit command-line checks.
    const QString data = smoke || offline ? option("--data-dir", defaultData) : defaultData;
    Client client(QUrl(server), data, nullptr, !args.contains("--server"));
    if (offline) {
        const QString path = client.localFile(0);
        std::printf("cachedBooks=%lld localFile=%s\n", static_cast<long long>(client.books().size()), qPrintable(path));
        return path.isEmpty() ? 1 : 0;
    }
    if (smoke) {
        QString username, password;
        {
            // Credentials travel over stdin, never argv, environment or a file.
            QFile input;
            if (!input.open(stdin, QIODevice::ReadOnly)) return 3;
            const auto credentials = QJsonDocument::fromJson(input.readLine(16384)).object();
            username = credentials["username"].toString();
            password = credentials["password"].toString();
            if (username.isEmpty() || password.isEmpty()) return 3;
        }
        const int bookId = option("--book-id", "0").toInt();
        if (bookId <= 0) return 3;
        const QString expectedError = option("--expect-error", "");
        QObject::connect(&client, &Client::completed, &client, [&](const QString &operation, bool ok) {
            std::printf("operation=%s success=%d status=%s\n", qPrintable(operation), ok, qPrintable(client.status()));
            if (!ok) { app->exit(!expectedError.isEmpty() && operation.contains(expectedError) ? 0 : 1); return; }
            if (operation == "catalog") {
                int selected = bookId == 0 ? 0 : -1;
                const auto books = client.books();
                for (int i=0; i<books.size(); ++i)
                    if (books[i].toMap()["bookId"].toInt() == bookId) selected = i;
                if (selected < 0) { std::fprintf(stderr, "Selected book is absent from first catalog page\n"); app->exit(3); return; }
                QTimer::singleShot(0, &client, [&client, selected] { client.download(selected); });
            }
            if (operation == "download") {
                client.showDownloaded(true);
                std::printf("localFile=%s\n", qPrintable(client.localFile(0)));
                if (expectedError.isEmpty() && !client.localFile(0).isEmpty()) {
                    QTimer::singleShot(0, &client, [&client] { client.logout(); });
                    return;
                }
                app->exit(expectedError.isEmpty() && !client.localFile(0).isEmpty() ? 0 : 1);
            }
            if (operation == "logout") app->exit(0);
        });
        QTimer::singleShot(0, &client, [&client, &username, &password] { client.login(username, password); password.clear(); });
        QTimer::singleShot(45000, app.get(), [&app] { app->exit(2); });
        return app->exec();
    }
    QQmlApplicationEngine engine;
#ifdef POCKETBOOK_DEVICE
    engine.addImportPath("/ebrmain/qml");
#endif
    engine.rootContext()->setContextProperty("client", &client);
    engine.rootContext()->setContextProperty("screenWidth", screen.width());
    engine.rootContext()->setContextProperty("screenHeight", screen.height());
    engine.load(QUrl("qrc:/Main.qml"));
    std::fprintf(stderr, "bookorbit: QML roots %lld\n", static_cast<long long>(engine.rootObjects().size()));
    if (engine.rootObjects().isEmpty()) return 1;
    QTimer::singleShot(0, &client, &Client::restoreSession);
    return app->exec();
}
