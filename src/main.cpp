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

int main(int argc, char **argv) {
    QStringList args;
    for (int i=1; i<argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    const bool smoke = args.contains("--smoke-test");
    const bool offline = args.contains("--offline-test");
    const bool demo = args.contains("--demo") || !args.contains("--server");
    auto option = [&args](const QString &name, const QString &fallback) {
        const int i = args.indexOf(name);
        return i >= 0 && i+1 < args.size() ? args[i+1] : fallback;
    };
#ifdef POCKETBOOK_DEVICE
    const QString demoServer = "http://host.containers.internal:8765";
#else
    const QString demoServer = "http://127.0.0.1:8765";
#endif
    QString server = option("--server", demo ? demoServer : "https://books.lan");
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
        qputenv("QT_PLUGIN_PATH", "/ebrmain/plugins");
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
    QCoreApplication::setApplicationName("BookOrbit");
#ifdef POCKETBOOK_DEVICE
    // The stock library does not index EPUB files below /mnt/ext1/system.
    const QString defaultData = "/mnt/ext1/books/BookOrbit";
#else
    const QString defaultData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#endif
    const QString data = option("--data-dir", defaultData);
    Client client(QUrl(server), data, demo, nullptr, !args.contains("--server") && !args.contains("--demo"));
    if (offline) {
        const QString path = client.localFile(0);
        std::printf("cachedBooks=%lld localFile=%s\n", static_cast<long long>(client.books().size()), qPrintable(path));
        return path.isEmpty() ? 1 : 0;
    }
    if (smoke) {
        const QString expectedError = option("--expect-error", "");
        QObject::connect(&client, &Client::completed, &client, [&](const QString &operation, bool ok) {
            std::printf("operation=%s success=%d status=%s\n", qPrintable(operation), ok, qPrintable(client.status()));
            if (!ok) { app->exit(!expectedError.isEmpty() && operation.contains(expectedError) ? 0 : 1); return; }
            if (operation == "catalog") { client.showDownloaded(false); }
            if (operation == "catalog") QTimer::singleShot(0, &client, [&client] { client.download(0); });
            if (operation == "download") {
                std::printf("localFile=%s\n", qPrintable(client.localFile(0)));
                app->exit(expectedError.isEmpty() && !client.localFile(0).isEmpty() ? 0 : 1);
            }
        });
        QTimer::singleShot(0, &client, [&client] { client.login("demo", "demo"); });
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
    engine.rootContext()->setContextProperty("demoServerUrl", demoServer);
    engine.rootContext()->setContextProperty("demoMode", demo);
    engine.load(QUrl("qrc:/Main.qml"));
    std::fprintf(stderr, "bookorbit: QML roots %lld\n", static_cast<long long>(engine.rootObjects().size()));
    if (engine.rootObjects().isEmpty()) return 1;
    return app->exec();
}
