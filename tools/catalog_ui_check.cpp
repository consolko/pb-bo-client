// Production QML and native firmware adapter, isolated fixture/account/files only.
#include "client.h"
#include "device.h"
#include "check_wait.h"
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
#include <QImage>
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
    const bool ok = waitForClient(client, action);
    if (!ok) std::fprintf(stderr, "%s\n", qPrintable(client.status()));
    settle();
    return ok;
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
    auto tap = [&](const char *name, double x = 0.5, double y = 0.5) {
        auto item = findItem(window->contentItem(), QString::fromLatin1(name));
        require(item && item->isVisible() && item->isEnabled(), name);
        const auto point = item->mapToScene(QPointF(item->width()*x, item->height()*y));
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
    auto item = [&](const char *name) { return findItem(window->contentItem(), QString::fromLatin1(name)); };
    require(item("reading-1")->property("text").toString() == "Читаю · 37,5 %", "catalog shows reading progress");
    require(item("reading-2")->property("text").toString() == "Прочитано", "finished books show status without percentage");
    require(item("reading-3")->property("text").toString() == "Читаю", "invalid progress has no fabricated percentage");
    for (const auto name : {"syncButtonIcon", "navigationButtonIcon", "exitButtonIcon", "bookAction-1Icon"})
        require(item(name) && item(name)->property("status").toInt() == 1, "SVG icon loaded by native Qt image plugin");
    require(item("nextPage")->width() >= 100*screen.width()/600.0 &&
            item("previousPage")->width() == item("nextPage")->width(), "pagination buttons have equal generous width");
    for (int i=1; i<=3; ++i) {
        const auto row = findItem(window->contentItem(), "book-"+QString::number(i));
        const auto action = findItem(row, "bookAction-"+QString::number(i));
        require(action && action->mapToItem(row, QPointF(0, action->height())).y() <= row->height()-8*screen.width()/600.0,
                "download button stays above separator including a two-line title");
        const auto cover = findItem(row, "cover-"+QString::number(i));
        const auto title = findItem(row, "title-"+QString::number(i));
        require(qAbs(cover->mapToItem(row, QPointF()).y()-title->mapToItem(row, QPointF()).y()) < 1,
                "cover and title share the same top edge for every row");
        require(qAbs(action->mapToItem(row, QPointF(0,action->height())).y()-cover->mapToItem(row, QPointF(0,cover->height())).y()) < 1,
                "download action aligns with bottom of cover block");
    }
    require(!item("author-2")->isVisible() &&
            qAbs(item("reading-2")->mapToScene(QPointF()).y()-item("title-2")->mapToScene(QPointF(0,item("title-2")->height())).y()-4*screen.width()/600.0) < 1,
            "missing author leaves only normal spacing between title and status");
    const auto first = item("book-1"), second = item("book-2");
    const QImage frame = window->grabWindow();
    int separators = 0; bool previousLine = false;
    for (int y=int(first->mapToScene(QPointF(0, first->height())).y())-3; y<=int(second->mapToScene(QPointF()).y())+3; ++y) {
        int dark = 0, left = int(first->mapToScene(QPointF()).x())+5, right = left+int(first->width())-10;
        for (int x=left; x<right; ++x) if (qGray(frame.pixel(x,y)) < 220) ++dark;
        const bool line = dark > (right-left)*0.95;
        if (line && !previousLine) ++separators;
        previousLine = line;
    }
    require(separators == 1, "rendered gap between first two books has exactly one separator");
    require(item("syncButton")->mapToScene(QPointF()).x() < item("navigationButton")->mapToScene(QPointF()).x(), "sync icon precedes settings in header");
    require(wait(client, [&] { tap("syncButton"); }) && client.status().contains("Нет скачанных книг"), "header sync action uses existing synchronization handler");
    tap("navigationButton"); settle();
    require(item("diagnosticPath")->property("text").toString() == client.diagnosticLogPath(), "log hint contains only path");
    require(qAbs(item("connectButton")->width()-item("logoutButton")->width()) < 1 &&
            qAbs(item("connectButton")->mapToScene(QPointF()).y()-item("logoutButton")->mapToScene(QPointF()).y()) < 1,
            "connect and logout occupy equal widths in one row");
    capture("settings"); tap("addConnection"); settle();
    auto password = item("passwordField");
    password->setProperty("text", "ui-test-only"); settle();
    require(password->property("displayText") != password->property("text"), "password starts masked");
    tap("showPassword"); settle();
    require(password->property("displayText") == password->property("text"), "show-password switch reveals entered text");
    tap("showPassword"); settle();
    require(password->property("displayText") != password->property("text"), "show-password switch masks text again");
    tap("showPassword"); tap("navigationButton"); settle(); tap("addConnection"); settle();
    require(password->property("text").toString().isEmpty() && !item("showPassword")->property("checked").toBool(), "reopening connection form clears password and resets visibility");
    capture("connection"); tap("navigationButton"); tap("navigationButton"); settle();
    require(wait(client, [&] { tap("nextPage"); }) && client.page() == 1, "wide next-page button works");
    require(wait(client, [&] { tap("previousPage"); }) && client.page() == 0, "wide previous-page button works");
    for (const auto point : {QPointF(0.05,0.4), QPointF(0.5,0.15), QPointF(0.5,0.42), QPointF(0.6,0.92)}) {
        require(wait(client, [&] { tap("book-1", point.x(), point.y()); }), "cover title author and blank row area open details");
        require(item("detailReading")->property("text").toString() == "Читаю · 37,5 %", "details keep catalog percentage beside author");
        tap("navigationButton"); settle();
    }
    require(wait(client, [&] { tap("bookAction-2"); }), "row download icon downloads single-format book");
    require(!client.detailVisible() && client.books()[1].toMap()["downloaded"].toBool(), "nested download action does not open row details");
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
    require(client.books().size() == 2, "UI groups two formats plus a separate downloaded book");
    tap("book-1"); settle(); capture("offline-details");
    for (int i=0; i<30 && (!readerBookIndexed(pdfPath) || !readerBookIndexed(fb2Path)); ++i) settle(200);
    require(readerBookIndexed(pdfPath) && readerBookIndexed(fb2Path), "native scanner indexes PDF and FB2");
    std::printf("PDF=%s\nFB2=%s\n", qPrintable(pdfPath), qPrintable(fb2Path));
    require(warnings == 0, "no QML warnings during navigation and downloads");
    if (argc > 3) { QTimer::singleShot(180000, &app, &QCoreApplication::quit); return app.exec(); }
}
