// VM-only: real Client + real firmware adapter, synthetic server/account/EPUB.
#include "client.h"
#include "device.h"
#include "progress.h"
#include "check_wait.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char *label) {
    std::printf("%s: %s\n",ok?"PASS":"FAIL",label); std::fflush(stdout);
    if (!ok) std::exit(1);
}
static bool wait(Client &c, const std::function<void()> &action) {
    const bool ok=waitForClient(c,action);
    if (!ok) std::fprintf(stderr,"%s\n",qPrintable(c.status()));
    return ok;
}
int main(int argc,char **argv) {
    qputenv("QT_PLUGIN_PATH","/ebrmain/plugins");
    qputenv("QT_QPA_PLATFORM","pocketbook2");
    QCoreApplication::setSetuidAllowed(true);
    const auto screen=setupDevice();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc,argv);
    QTranslator russian;
    require(russian.load(":/translations/bookorbit_ru.qm"), "Russian translation catalog loaded");
    app.installTranslator(&russian);
    QLocale::setDefault(QLocale("ru"));
    require(argc==3 || (argc==4 && (QString::fromLocal8Bit(argv[3])=="--ui" || QString::fromLocal8Bit(argv[3])=="--history-check")),"isolated directory and fault file supplied");
    require(!screen.isEmpty(),"native adapter initialized");
    Client c(QUrl("http://host.containers.internal:8766"),QString::fromLocal8Bit(argv[1]));
    if (argc==4 && QString::fromLocal8Bit(argv[3])=="--history-check") {
        c.showDownloaded(true); c.refreshRecents(); QCoreApplication::processEvents();
        require(c.historyAvailable() && c.recentBook()["fileId"].toInt()==101,"stock-library open appears in the account recent snapshot after restart");
        QQmlApplicationEngine engine; engine.addImportPath("/ebrmain/qml");
        engine.rootContext()->setContextProperty("client",&c);
        engine.rootContext()->setContextProperty("screenWidth",screen.width());
        engine.rootContext()->setContextProperty("screenHeight",screen.height());
        int warnings=0;
        QObject::connect(&engine,&QQmlApplicationEngine::warnings,&app,[&](const QList<QQmlError> &errors) { warnings+=errors.size(); });
        engine.load(QUrl("qrc:/Main.qml"));
        require(!engine.rootObjects().isEmpty(),"downloads load in production QML without authentication");
        auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QEventLoop settle; QTimer::singleShot(600,&settle,&QEventLoop::quit); settle.exec();
        require(window->grabWindow().save(QString::fromLocal8Bit(argv[1])+"/native-downloads.png"),"capture downloaded list without a recent block");
        std::function<QQuickItem *(QQuickItem *)> find=[&](QQuickItem *parent)->QQuickItem * {
            if (parent->objectName()=="bookAction-"+c.recentBook()["bookId"].toString()) return parent;
            for (auto child:parent->childItems()) if (auto item=find(child)) return item;
            return nullptr;
        };
        auto button=find(window->contentItem());
        require(button && button->isVisible() && button->isEnabled(),"downloaded book read action is accessible offline");
        require(wait(c,[&] {
            QEventLoop polish; QTimer::singleShot(40,&polish,&QEventLoop::quit); polish.exec();
            const QPointF point=button->mapToScene(QPointF(button->width()/2,button->height()/2));
            require(point.x()>=0 && point.x()<window->width() && point.y()>=0 && point.y()<window->height(),"downloaded book tap is inside the viewport");
            QMouseEvent press(QEvent::MouseButtonPress,point,point,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QCoreApplication::sendEvent(window,&press);
            require(button->property("pressed").toBool(),"book button receives the native QML press");
            QMouseEvent release(QEvent::MouseButtonRelease,point,point,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QCoreApplication::sendEvent(window,&release);
        }),"one native QML tap opens the exact downloaded file without login");
        require(warnings==0,"no QML warnings in offline reading scenario");
        return 0;
    }
    require(wait(c,[&] { c.login(c.server(), "demo","demo"); }),"synthetic native login");
    const QString bookDir="/mnt/ext1/books/BookOrbit/"+QFileInfo(QString::fromLocal8Bit(argv[1])).fileName();
    require(QDir().mkpath(bookDir) && c.setDownloadDirectory(bookDir),"isolated native download directory");
    require(wait(c,[&] { c.download(0); }),"download controlled EPUB");
    const QString path=c.localFile(0);
    for (int i=0;i<30 && !readerBookIndexed(path);++i) {
        QEventLoop pause; QTimer::singleShot(200,&pause,&QEventLoop::quit); pause.exec();
    }
    require(readerBookIndexed(path),"firmware scanner registered the file");
    const auto initialRecents=readerRecents({path});
    require(initialRecents.available && initialRecents.profile==readerProfile() && initialRecents.files.contains(path) && initialRecents.files[path].bookId>0,
        "read native recents using current profile and verified schema");
    QString before;
    require(readerPosition(path,&before) && !nativeCfi(before).isEmpty(),"read saved stock-reader CFI");
    QString error;
    require(saveReaderPosition(path,before,"epubcfi(/6/2!/4/122/1)",readerProfile(),&error),"prepare distinct native paragraph 060");
    require(readerPosition(path,&before) && nativeCfi(before)=="epubcfi(/6/2!/4/122/1)","confirm distinct initial CFI");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }),"real saved reader position uploaded to fixture");
    QFile fault(QString::fromLocal8Bit(argv[2]));
    require(fault.open(QIODevice::WriteOnly) && fault.write("progress_remote")==15,"move fixture server to paragraph 030");
    fault.close();
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }),"incoming CFI saved through firmware without opening");
    QString after;
    require(readerPosition(path,&after) && nativeCfi(after)=="epubcfi(/6/2!/4/62/1)","native SQLite readback matches server CFI");
    require(readerFileState(path)==ReaderFileState::Closed,"book stayed closed throughout sync");
    const auto afterRecents=readerRecents({path});
    require(afterRecents.available && afterRecents.files[path].openTime>=initialRecents.files[path].openTime && afterRecents.files[path].openTime>0,
        "incoming native position can advance recents without a reading session");
    c.showDownloaded(true); c.refreshRecents(); QCoreApplication::processEvents();
    require(c.recentBook()["fileId"].toInt()==101,"native recent snapshot identifies the exact account file");
    require(fault.open(QIODevice::WriteOnly) && fault.write("progress_range")==14,"move fixture server to a CFI range");
    fault.close();
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }),"incoming range applied through real firmware adapter");
    require(readerPosition(path,&after) && nativeCfi(after)=="epubcfi(/6/2!/4/42/1:0)","native readback is the range start");
    require(wait(c,[&] { c.syncAll(); }),"repeat batch is a no-op");
    Client restarted(QUrl("http://host.containers.internal:8766"),QString::fromLocal8Bit(argv[1]));
    require(!restarted.books().isEmpty() && !restarted.books()[0].toMap()["pendingProgress"].toBool(),"confirmed state survives client restart");
    std::printf("localFile=%s\n",qPrintable(path));
    if (argc==4) {
        c.showDownloaded(true);
        QQmlApplicationEngine engine;
        engine.addImportPath("/ebrmain/qml");
        engine.rootContext()->setContextProperty("client",&c);
        engine.rootContext()->setContextProperty("screenWidth",screen.width());
        engine.rootContext()->setContextProperty("screenHeight",screen.height());
        engine.load(QUrl("qrc:/Main.qml"));
        require(!engine.rootObjects().isEmpty(),"production QML loaded with isolated synthetic account");
        QTimer::singleShot(20000,&app,&QCoreApplication::quit);
        return app.exec();
    }
}
