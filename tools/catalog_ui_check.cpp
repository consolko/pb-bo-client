// Production QML and native firmware adapter, isolated fixture/account/files only.
#include "client.h"
#include "device.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QMouseEvent>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char *label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
    if (!ok) std::exit(1);
}
static void settle(int ms = 250) {
    QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
}
static bool wait(Client &client, const std::function<void()> &action) {
    QEventLoop loop; bool done = false, ok = false;
    auto connection = QObject::connect(&client, &Client::completed, &loop, [&](const QString &op, bool success) {
        if (op == "settings") return;
        done = true; ok = success; loop.quit();
    });
    QTimer::singleShot(0, &loop, action);
    QTimer::singleShot(25000, &loop, &QEventLoop::quit); loop.exec();
    QObject::disconnect(connection);
    if (!ok) std::fprintf(stderr, "%s\n", qPrintable(client.status()));
    settle();
    return done && ok;
}

int main(int argc, char **argv) {
    qputenv("QT_PLUGIN_PATH", "/ebrmain/plugins"); qputenv("QT_QPA_PLATFORM", "pocketbook2");
    QCoreApplication::setSetuidAllowed(true);
    const auto screen = setupDevice();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv); QGuiApplication::setFont(QFont(deviceFont()));
    require(argc >= 3 && !screen.isEmpty(), "isolated UI check initialized");
    const QString root = QString::fromLocal8Bit(argv[1]);
    QFile fault(QString::fromLocal8Bit(argv[2]));
    require(fault.open(QIODevice::WriteOnly) && fault.write("features") == 8, "enable synthetic multi-format library"); fault.close();
    Client client(QUrl("http://host.containers.internal:8766"), root, nullptr, false);
    const QString destination = "/mnt/ext1/books/BookOrbit/"+QFileInfo(root).fileName();
    require(QDir().mkpath(destination) && client.setDownloadDirectory(destination), "isolated native download destination");
    QQmlApplicationEngine engine; engine.addImportPath("/ebrmain/qml");
    int warnings = 0;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) { ++warnings; std::fprintf(stderr, "%s\n", qPrintable(error.toString())); }
    });
    engine.rootContext()->setContextProperty("client", &client);
    engine.rootContext()->setContextProperty("screenWidth", screen.width());
    engine.rootContext()->setContextProperty("screenHeight", screen.height());
    engine.load(QUrl("qrc:/Main.qml"));
    require(!engine.rootObjects().isEmpty(), "production QML creates the window");
    auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    require(window != nullptr, "native QQuickWindow available");
    std::function<QQuickItem *(QQuickItem *, const QString &)> findItem = [&](QQuickItem *parent, const QString &name) -> QQuickItem * {
        if (parent->objectName() == name) return parent;
        for (auto child : parent->childItems()) if (auto found = findItem(child, name)) return found;
        return nullptr;
    };
    auto tap = [&](const char *name) {
        auto item = findItem(window->contentItem(), QString::fromLatin1(name));
        require(item && item->isVisible() && item->isEnabled(), name);
        const auto point = item->mapToScene(QPointF(item->width()/2, item->height()/2));
        require(point.x() >= 0 && point.x() < window->width() && point.y() >= 0 && point.y() < window->height(), "touch target is inside viewport");
        QMouseEvent press(QEvent::MouseButtonPress, point, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &release);
    };
    auto capture = [&](const char *name) {
        settle(400);
        require(window->grabWindow().save(root+"/"+QString::fromLatin1(name)+".png"), name);
    };
    require(wait(client, [&] { client.login("demo", "demo"); }), "load catalog into production QML"); capture("catalog");
    auto catalog = window->findChild<QQuickItem *>("catalog");
    catalog->setProperty("contentY", 140); settle();
    require(wait(client, [&] { tap("book-2"); }), "touch opens PDF-only book details");
    capture("pdf-only"); tap("navigationButton"); settle();
    require(!client.detailVisible() && catalog->property("contentY").toDouble() > 100, "back restores list position");
    catalog->setProperty("contentY", 0); settle();
    require(wait(client, [&] { tap("book-1"); }), "touch opens full metadata"); capture("details");
    tap("formatButton"); settle(); capture("formats");
    tap("file-201"); tap("formatCancel"); settle();
    require(client.detail()["fileId"].toInt() == 101, "cancel format selection preserves original file");
    tap("formatButton"); settle(); tap("file-201"); tap("formatConfirm"); settle();
    require(client.detail()["fileId"].toInt() == 201, "confirmed touch selection chooses PDF");
    require(wait(client, [&] { tap("downloadButton"); }), "touch downloads chosen PDF");
    const QString pdfPath = client.localFile(0);
    require(pdfPath.endsWith(".pdf"), "selected PDF stored with PDF extension");
    tap("formatButton"); settle(); tap("file-202"); tap("formatConfirm"); settle();
    require(wait(client, [&] { tap("downloadButton"); }), "touch downloads FB2 beside PDF");
    const QString fb2Path = client.localFile(0);
    require(fb2Path.endsWith(".fb2") && QFile::exists(pdfPath), "native multi-format files coexist");
    capture("downloaded-detail");
    tap("navigationButton"); settle();
    require(wait(client, [&] { tap("collectionsTab"); }), "touch loads collection list"); capture("collections");
    require(wait(client, [&] { tap("collection-12"); }), "touch enters public collection"); capture("collection-books");
    auto search = window->findChild<QQuickItem *>("searchField");
    require(search != nullptr, "collection search field available");
    search->setProperty("text", "Автор коллекции");
    require(wait(client, [&] { tap("searchButton"); }) && client.total() == 20, "search stays within collection");
    capture("collection-search");
    tap("downloadsTab"); settle(); capture("downloads");
    require(client.books().size() == 1, "UI groups two files into one downloaded book");
    tap("book-1"); settle(); capture("offline-details");
    for (int i=0; i<30 && (!readerBookIndexed(pdfPath) || !readerBookIndexed(fb2Path)); ++i) settle(200);
    require(readerBookIndexed(pdfPath) && readerBookIndexed(fb2Path), "native scanner indexes PDF and FB2");
    std::printf("PDF=%s\nFB2=%s\n", qPrintable(pdfPath), qPrintable(fb2Path));
    require(warnings == 0, "no QML warnings during navigation and downloads");
    if (argc > 3) { QTimer::singleShot(180000, &app, &QCoreApplication::quit); return app.exec(); }
}
