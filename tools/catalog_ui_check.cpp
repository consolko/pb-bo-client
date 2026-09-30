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
#include <QCryptographicHash>
#include <QJsonDocument>
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
    tap("coverGridToggle"); settle();
    auto grid = item("coverCatalog");
    require(window->property("coverGrid").toBool() && grid->isVisible() && !item("catalog")->isVisible(), "toggle switches catalog from list to cover grid");
    require(grid->property("count").toInt() == client.books().size() && client.page() == 0, "grid uses current catalog page");
    require(qAbs(item("gridBook-1")->y()-item("gridBook-3")->y()) < 1 && item("gridBook-4")->y() > item("gridBook-1")->y(), "grid lays out three books per row");
    require(item("gridCover-1")->property("status").toInt() == 1 && item("gridPlaceholder-4")->isVisible(), "grid renders cached cover and missing-cover fallback");
    capture("cover-grid");
    require(wait(client, [&] { tap("gridBook-1"); }) && client.detailVisible(), "cover tap opens existing book details");
    tap("navigationButton"); settle();
    require(grid->isVisible() && window->property("coverGrid").toBool(), "back from book keeps grid mode");
    require(QMetaObject::invokeMethod(window, "pageContent", Q_ARG(QVariant, 1)), "invoke hardware-page scrolling route"); settle();
    require(grid->property("contentY").toDouble() > 0, "page scrolling targets grid");
    grid->setProperty("contentY", 270*screen.width()/600.0); settle();
    const double gridOffset = grid->property("contentY").toDouble();
    require(wait(client, [&] { tap("gridBook-4"); }), "scrolled cover tap opens correct book");
    require(client.detail()["bookId"].toInt() == 4, "grid delegates retain correct book identity");
    tap("navigationButton"); settle();
    require(qAbs(grid->property("contentY").toDouble()-gridOffset) < 1, "back restores grid scroll offset");
    require(wait(client, [&] { tap("nextPage"); }) && client.page() == 1 && grid->isVisible(), "pagination keeps cover grid mode");
    require(wait(client, [&] { tap("previousPage"); }) && client.page() == 0, "grid previous page works");
    item("searchField")->setProperty("text", "grid-no-such-book");
    require(wait(client, [&] { tap("searchButton"); }) && client.total() == 0 && !grid->isVisible() && item("catalog")->isVisible(), "empty grid search uses existing empty-state message");
    item("searchField")->setProperty("text", "");
    require(wait(client, [&] { tap("searchButton"); }) && grid->isVisible(), "clearing search restores cover grid");
    tap("coverGridToggle"); settle();
    require(!window->property("coverGrid").toBool() && item("catalog")->isVisible() && !grid->isVisible(), "toggle restores original list");
    require(wait(client, [&] { tap("bookAction-2"); }), "row download icon downloads single-format book");
    require(!client.detailVisible() && client.books()[1].toMap()["downloaded"].toBool(), "nested download action does not open row details");
    auto catalog = window->findChild<QQuickItem *>("catalog");
    catalog->setProperty("contentY", 140); settle();
    require(wait(client, [&] { tap("book-2"); }), "touch opens PDF-only book details");
    capture("pdf-only"); tap("navigationButton"); settle();
    require(!client.detailVisible() && catalog->property("contentY").toDouble() > 100, "back restores list position");
    catalog->setProperty("contentY", 0); settle();
    require(wait(client, [&] { tap("book-1"); }), "touch opens full metadata"); capture("details");
    require(qAbs(item("detailCoverFrame")->width() - 180*screen.width()/600.0) < 1, "detail cover uses enlarged reference proportions");
    require(qAbs(item("detailCoverFrame")->mapToScene(QPointF()).y()-item("detailTitle")->mapToScene(QPointF()).y()) < 1, "detail cover and title align at top");
    require(item("downloadButton")->mapToScene(QPointF()).x() > item("detailCoverFrame")->mapToScene(QPointF(item("detailCoverFrame")->width(),0)).x(), "primary action stays beside cover");
    require(item("detailEdition")->property("text").toString().contains("240 стр.") && item("detailRating")->isVisible() && item("detailGenres")->isVisible(), "edition rating and genre are visible in summary");
    require(item("detailProgress")->isVisible() && item("detailProgress")->property("value").toDouble() == 37.5, "detail bar uses server percentage");
    require(item("detailDescription")->mapToScene(QPointF()).y() >= item("downloadButton")->mapToScene(QPointF(0,item("downloadButton")->height())).y(), "description follows hero without overlap");
    require(!item("descriptionToggle")->isVisible(), "short description does not show redundant expansion action");
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
    auto setFault = [&](const QByteArray &mode) {
        require(fault.open(QIODevice::WriteOnly) && fault.write(mode)==mode.size(), "set verification fixture mode"); fault.close();
    };
    auto reveal = [&](const char *scrollName, const char *targetName) {
        auto scroll=item(scrollName);
        auto flick=qvariant_cast<QObject *>(scroll->property("contentItem"));
        auto viewport=qobject_cast<QQuickItem *>(flick);
        require(viewport!=nullptr, "scroll viewport available");
        const double y=item(targetName)->mapToItem(viewport,QPointF()).y();
        const double next=flick->property("contentY").toDouble()+y-20*screen.width()/600.0;
        flick->setProperty("contentY",qMax(0.0,qMin(next,flick->property("contentHeight").toDouble()-viewport->height())));
        settle();
    };
    require(wait(client,[&] { client.showDownloaded(false); }) && wait(client,[&] { client.showDetail(1); }),
            "load all variants before adding EPUB to downloaded library");
    client.selectFile(101);
    require(wait(client,[&] { client.downloadSelected(); }), "prepare EPUB beside PDF and FB2 for full-check UI");
    tap("navigationButton"); settle(); tap("navigationButton"); settle();
    reveal("settingsScroll","verifyLibraryButton");
    capture("library-check-settings");
    setFault("slow");
    require(!wait(client,[&] {
        tap("verifyLibraryButton");
        QTimer::singleShot(150,&client,[&] {
            require(client.verifyingLibrary() && item("cancelVerificationButton")->isVisible(), "verification cancellation control appears");
            tap("cancelVerificationButton");
        });
    }) && !client.verifyingLibrary(), "UI cancellation stops full verification");
    setFault("changed");
    reveal("settingsScroll","verifyLibraryButton");
    require(!wait(client,[&] { tap("verifyLibraryButton"); }) && client.status().contains("отличается: 1"), "settings action checks files without synchronizing progress");
    tap("navigationButton"); settle();
    client.showDetail(1); settle(); client.selectFile(101); settle();
    require(client.detail()["remoteFileChanged"].toBool() && item("fileCheckResult")->property("text").toString().contains("приостановлен"), "card exposes durable mismatch warning");
    reveal("detailScroll","redownloadButton"); capture("library-check-mismatch");
    setFault("features");
    require(wait(client,[&] { tap("redownloadButton"); }) && !client.detail()["remoteFileChanged"].toBool(), "card redownload action clears verified mismatch");
    client.closeDetail(); client.showDownloaded(true); settle();
    tap("syncStatusButton"); settle();
    require(item("syncList")->isVisible() && item("syncList")->property("count").toInt()==1 && client.syncSummary()["total"].toInt()==1,
            "sync list opens with one EPUB book, independently of catalog and other formats");
    capture("sync-list");
    tap("syncFilter-synced"); settle();
    require(item("syncList")->property("count").toInt()==0, "unverified book does not appear under synchronized filter");
    tap("syncFilter-attention"); settle();
    require(item("syncList")->property("count").toInt()==1, "attention filter exposes the unverified book");
    tap("syncFilter-all"); settle(); tap("syncBook-book:1"); settle();
    require(item("syncDetails")->isVisible() && item("syncState-101")->property("text").toString()=="Ещё не проверено", "book opens file-specific sync actions");
    capture("sync-details");
    // Seed only this test account's own journal; no native reader database is changed.
    const auto key=QCryptographicHash::hash((client.server()+"\n"+client.username()).toUtf8(),QCryptographicHash::Sha256).toHex().left(24);
    QFile recordFile(root+"/"+QString::fromLatin1(key)+"/records/101.json");
    require(recordFile.open(QIODevice::ReadOnly), "read isolated download record");
    auto record=QJsonDocument::fromJson(recordFile.readAll()).object(); recordFile.close();
    record["progress"]=QJsonObject{{"profile",readerProfile()},{"conflictLocal","epubcfi(/6/2!/4/14/1:0)"},
        {"conflictRemote",QJsonObject{{"cfi","epubcfi(/6/2!/4/42/1:0)"},{"percentage",40},{"pageNumber",QJsonValue::Null}}}};
    require(recordFile.open(QIODevice::WriteOnly|QIODevice::Truncate) && recordFile.write(QJsonDocument(record).toJson())>0, "seed isolated conflict"); recordFile.close();
    require(client.configure(client.server(),client.username()), "reload saved conflict without network authentication"); settle();
    tap("choosePosition-101"); settle();
    require(client.progressConflict() && !item("applyConflict")->isEnabled() && !item("conflictLocal")->property("checked").toBool() && !item("conflictRemote")->property("checked").toBool(), "offline conflict preview has no default choice and cannot write");
    capture("sync-conflict-offline");
    tap("conflictLocal"); settle();
    require(!item("applyConflict")->isEnabled(), "choosing a valid position offline does not enable apply");
    tap("deferConflict"); settle();
    require(!client.progressConflict() && client.syncBooks()[0].toMap()["state"]=="conflict", "later closes the dialog but retains the conflict");
    require(wait(client,[&] { client.restoreSession(); }), "restore test session for conflict controls");
    tap("choosePosition-101"); settle();
    require(!item("applyConflict")->isEnabled(), "reopening conflict clears the old radio selection");
    tap("conflictRemote"); settle();
    require(item("applyConflict")->isEnabled(), "authenticated explicit selection enables apply");
    for (int opening=0; opening<3; ++opening) {
        capture("sync-conflict-selected");
        const QImage frame(root+"/sync-conflict-selected.png");
        for (const auto name : {"applyConflictLabel","deferConflictLabel"}) {
            const auto label=item(name);
            require(label && label->isVisible() && !label->property("text").toString().isEmpty(), "conflict footer label exists");
            const auto bounds=label->mapRectToScene(label->boundingRect()).toAlignedRect().intersected(frame.rect());
            int ink=0;
            for (int y=bounds.top(); y<=bounds.bottom(); ++y)
                for (int x=bounds.left(); x<=bounds.right(); ++x) {
                    const int gray=qGray(frame.pixel(x,y));
                    if (QString::fromLatin1(name)=="applyConflictLabel" ? gray>220 : gray<40) ++ink;
                }
            require(ink>100, "conflict footer text is rendered in captured pixels");
        }
        tap("deferConflict"); settle();
        if (opening<2) {
            tap("choosePosition-101"); settle();
            require(!item("applyConflict")->isEnabled(), "repeated opening clears selection");
            tap("conflictRemote"); settle();
        }
    }
    tap("navigationButton"); settle(); capture("sync-list-conflict");
    tap("navigationButton"); settle();
    require(item("catalog")->isVisible() && !item("syncList")->isVisible(), "back restores the downloaded library");
    std::printf("PDF=%s\nFB2=%s\n", qPrintable(pdfPath), qPrintable(fb2Path));
    require(warnings == 0, "no QML warnings during navigation and downloads");
    if (argc > 3) { QTimer::singleShot(180000, &app, &QCoreApplication::quit); return app.exec(); }
}
