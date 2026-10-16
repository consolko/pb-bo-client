// VM-only: real Client + real firmware adapter, synthetic server/account/books.
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
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>

#ifdef BOOKORBIT_NATIVE_COMMIT_CHECK
static std::atomic_bool holdRecord{false},recordEntered{false},releaseRecord{false},recordOnWorker{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    if (holdRecord.load()) {
        char target[4096]; const auto length=::readlink(QFile::encodeName("/proc/self/fd/"+QString::number(fd)),target,sizeof(target));
        struct stat info{};
        if (length>0 && ::fstat(fd,&info)==0 && S_ISREG(info.st_mode) &&
            QByteArray(target,length).contains("/records/") && holdRecord.exchange(false)) {
            recordOnWorker=QThread::currentThread()!=QCoreApplication::instance()->thread(); recordEntered=true;
            QElapsedTimer deadline; deadline.start();
            while (!releaseRecord.load() && deadline.elapsed()<20000) QThread::msleep(1);
        }
    }
    return __real_fsync(fd);
}
#endif

static void require(bool ok, const char *label) {
    std::printf("%s: %s\n",ok?"PASS":"FAIL",label); std::fflush(stdout);
    if (!ok) std::exit(1);
}
static bool wait(Client &c, const std::function<void()> &action) {
    const bool ok=waitForClient(c,action);
    if (!ok) std::fprintf(stderr,"%s\n",qPrintable(c.status()));
    return ok;
}
static QQuickWindow *loadWindow(Client &client,QQmlApplicationEngine &engine,const QSize &screen) {
    engine.addImportPath("/ebrmain/qml");
    engine.rootContext()->setContextProperty("client",&client);
    engine.rootContext()->setContextProperty("screenWidth",screen.width());
    engine.rootContext()->setContextProperty("screenHeight",screen.height());
    engine.load(QUrl("qrc:/Main.qml"));
    require(!engine.rootObjects().isEmpty(),"production QML creates a native window");
    return qobject_cast<QQuickWindow *>(engine.rootObjects().first());
}
static QQuickItem *cancelButton(QQuickItem *parent) {
    if (parent->property("text").toString()==QCoreApplication::translate("BookOrbit","Cancel download") &&
        parent->metaObject()->indexOfSignal("clicked()")>=0) return parent;
    for (auto child:parent->childItems()) if (auto found=cancelButton(child)) return found;
    return nullptr;
}
static void tap(QQuickWindow *window,QQuickItem *button) {
    const auto point=button->mapToScene(QPointF(button->width()/2,button->height()/2));
    require(point.x()>=0 && point.x()<window->width() && point.y()>=0 && point.y()<window->height(),"cancel target is inside the native viewport");
    QMouseEvent press(QEvent::MouseButtonPress,point,point,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QCoreApplication::sendEvent(window,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,point,point,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(window,&release);
}

int main(int argc,char **argv) {
    QElapsedTimer startup; startup.start();
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
    require(argc==3 || argc==4,"isolated directory and fault file supplied");
    require(!screen.isEmpty(),"native adapter initialized");
    const QString root=QString::fromLocal8Bit(argv[1]);
    const QString scenario=argc==4 ? QString::fromLocal8Bit(argv[3]) : QString{};
    require(QStringList{"","--ui","--history-check","--fb2-restore","--fb2-restart","--startup-large",
        "--cancel-transfer","--cancel-commit","--cancel-empty","--cancel-present"}.contains(scenario),"known native scenario selected");
    Client c(QUrl("http://host.containers.internal:8766"),root);
    if (scenario=="--startup-large") {
        qint64 ready=-1,frame=-1; bool loadingAtFrame=false; int ticks=0,loadingTicks=0,warnings=0;
        Client::PresentationWork atRegistry;
        QObject::connect(&c,&Client::libraryLoadingChanged,&c,[&] { if (!c.libraryLoading()) { ready=startup.elapsed(); atRegistry=c.presentationWork(); } });
        QTimer heartbeat; QObject::connect(&heartbeat,&QTimer::timeout,&c,[&] { ++ticks; if (c.libraryLoading()) ++loadingTicks; }); heartbeat.start(10);
        QQmlApplicationEngine engine;
        QObject::connect(&engine,&QQmlApplicationEngine::warnings,&c,[&](const QList<QQmlError> &errors) { warnings+=errors.size(); });
        auto window=loadWindow(c,engine,screen); require(window,"native startup window exists");
        QObject::connect(window,&QQuickWindow::frameSwapped,&c,[&] {
            if (frame<0) { frame=startup.elapsed(); loadingAtFrame=c.libraryLoading(); }
        });
        window->show(); window->requestUpdate();
        require(waitUntil([&] { return frame>=0 && !c.libraryLoading(); },60000),"first frame and full registry both complete");
        const auto modelReady=startup.elapsed();
        require(c.bookModel()->rowCount()==5000,"all 5000 independently stored records reach the native model");
        require(loadingAtFrame && frame<ready && loadingTicks>0,"first native frame and GUI timers precede registry completion");
        require(!warnings,"large-library startup has no QML warnings");
        require(window->grabWindow().save(root+"/startup-loaded.png"),"capture loaded 5000-book native library");
        std::printf("MEASURE: books=5000 first_frame_ms=%lld registry_ready_ms=%lld model_ready_ms=%lld loading_at_frame=%d loading_ticks=%d ticks=%d\n",
            static_cast<long long>(frame),static_cast<long long>(ready),static_cast<long long>(modelReady),loadingAtFrame,loadingTicks,ticks);
        const auto work=c.presentationWork();
        std::printf("MEASURE: after_registry library_builds=%llu library_sorts=%llu sync_builds=%llu gui_file_checks=%llu\n",
            static_cast<unsigned long long>(work.libraryBuilds-atRegistry.libraryBuilds),
            static_cast<unsigned long long>(work.librarySorts-atRegistry.librarySorts),
            static_cast<unsigned long long>(work.syncBuilds-atRegistry.syncBuilds),
            static_cast<unsigned long long>(work.localFileChecks-atRegistry.localFileChecks));
        return 0;
    }
    require(waitForLibrary(c),"registry snapshot loaded");
    if (scenario=="--cancel-empty" || scenario=="--cancel-present" || scenario=="--fb2-restart") {
        c.showDownloaded(true); require(waitForMaintenance(c),"restart cleanup completes");
        const bool present=scenario!="--cancel-empty";
        require(c.books().size()==(present ? 1 : 0),"new process restores exactly the expected number of books");
        if (present) {
            const auto path=c.localFile(0); const auto id=scenario=="--fb2-restart" ? "202" : "101";
            const auto record=ClientWorkerCheck::record(c,id);
            require(!path.isEmpty() && digestFile(path)==record["sha256"].toString().toLatin1(),"restart record points to the complete unchanged file");
            if (scenario=="--fb2-restart") {
                QString position;
                require(readerPosition(path,&position) && bookCfi(path,position)=="epubcfi(/6/2!/4/2/2/2/1:0)","FB2 exact incoming coordinate survives a new process");
                require(!c.books()[0].toMap()["pendingProgress"].toBool(),"FB2 confirmed journal survives a new process");
            }
        } else require(QDir(ClientWorkerCheck::scope(c)+"/records").entryList({"*.json"}).isEmpty(),"cancelled first transfer leaves no registry entry after process restart");
        return 0;
    }
    if (scenario=="--cancel-transfer" || scenario=="--cancel-commit") {
        QFile fault(QString::fromLocal8Bit(argv[2]));
        auto mode=[&](const QByteArray &value) {
            require(fault.open(QIODevice::WriteOnly) && fault.write(value)==value.size(),"set cancellation fixture mode"); fault.close();
        };
        mode(""); require(wait(c,[&] { c.login(c.server(),"demo","demo"); }),"first-download cancellation signs in");
        const QString folder="/mnt/ext1/books/BookOrbit/"+QFileInfo(root).fileName();
        require(QDir().mkpath(folder) && c.setDownloadDirectory(folder),"cancellation owns an isolated native book folder");
        QQmlApplicationEngine engine; auto window=loadWindow(c,engine,screen);
        require(window && c.books().size()==3,"native cancellation window shows the fixture catalog");
        if (scenario=="--cancel-transfer") {
            mode("slow");
            require(!wait(c,[&] {
                c.download(0);
                require(waitUntil([&] { return ClientWorkerCheck::receiving(c); }),"first download is receiving its HTTP body");
                QEventLoop delay; QTimer::singleShot(200,&delay,&QEventLoop::quit); delay.exec();
                auto button=cancelButton(window->contentItem());
                require(button && button->isVisible() && button->isEnabled(),"cancel is available during first HTTP transfer");
                require(window->grabWindow().save(root+"/cancel-transfer.png"),"capture cancellable first transfer");
                tap(window,button);
            }),"native QML cancel stops the first transfer");
            require(waitForMaintenance(c) && ClientWorkerCheck::record(c,"101").isEmpty(),"cancel before commit creates no record");
            require(QDir(folder).entryList({"*.epub"}).isEmpty(),"cancel before commit creates no completed book file");
        } else {
#ifdef BOOKORBIT_NATIVE_COMMIT_CHECK
            holdRecord=true; c.download(0);
            require(waitUntil([&] { return recordEntered.load(); }),"ARM worker stops inside first records/101.json fsync");
            auto button=cancelButton(window->contentItem());
            require(button && button->isVisible() && !button->isEnabled() && !c.downloadCancellable(),"native QML disables cancel throughout final registry commit");
            require(window->grabWindow().save(root+"/cancel-commit.png"),"capture disabled cancellation during registry commit");
            ClientWorkerCheck::cleanup(c); tap(window,button); c.cancelDownload();
            int ticks=0; QTimer heartbeat; QObject::connect(&heartbeat,&QTimer::timeout,&c,[&] { ++ticks; }); heartbeat.start(10);
            require(waitUntil([&] { return ticks>=5; }) && recordOnWorker && c.busy() && c.downloading(),"GUI stays responsive while commit cancellation cannot release cleanup");
            require(QDir(folder).entryList({"*.epub"}).size()==1,"pending cleanup preserves the promoted first file");
            require(wait(c,[&] { releaseRecord=true; }) && waitForMaintenance(c),"first record is published before deferred cleanup completes");
            c.showDownloaded(true); require(c.books().size()==1 && !c.localFile(0).isEmpty(),"committed first download remains available after cancellation request");
#else
            require(false,"native commit wrapper is required");
#endif
        }
        mode(""); return 0;
    }
    if (argc==4 && QString::fromLocal8Bit(argv[3])=="--fb2-restore") {
        QFile fault(QString::fromLocal8Bit(argv[2]));
        auto mode=[&](const QByteArray &value) {
            require(fault.open(QIODevice::WriteOnly) && fault.write(value)==value.size(),"set synthetic server mode"); fault.close();
        };
        mode("features");
        require(wait(c,[&] { c.login(c.server(),"demo","demo"); }),"synthetic FB2 login");
        const QString folder="/mnt/ext1/books/BookOrbit/"+QFileInfo(QString::fromLocal8Bit(argv[1])).fileName();
        require(QDir().mkpath(folder) && c.setDownloadDirectory(folder),"isolated FB2 folder");
        require(wait(c,[&] { c.showDetail(1); }),"load synthetic FB2 variant");
        c.selectFile(202); mode("fb2_restore");
        require(wait(c,[&] { c.downloadSelected(); }),"download FB2 and complete automatic restoration");
        const QString path=c.localFile(0); QString position;
        require(readerBookIndexed(path),"scanner registered downloaded FB2");
        require(readerPosition(path,&position),"read restored native FB2 position");
        std::fprintf(stderr,"FB2 native=%s status=%s\n",qPrintable(position),qPrintable(c.status()));
        require(bookCfi(path,position)=="epubcfi(/6/2!/4/2/4/1:4)","first download restores exact server CFI through native setter");
        require(readerFileState(path)==ReaderFileState::Closed,"restoration does not open the reader");
        require(c.syncBooks().first().toMap()["state"]=="synced","restoration persists synchronized state");
        mode("features");
        const auto prepared=prepareReaderPosition(path,"epubcfi(/6/2!/4/2/4/1:8)"); UiMessage error;
        require(saveReaderPosition(path,position,prepared,readerProfile(),&error),"prepare a distinct exact native FB2 position");
        bool reset=false; ClientWorkerCheck::executor(c).submit(&c,[](const FileCancellation &) { resetProgressWork(); return true; },[&](bool,bool) { reset=true; });
        require(waitUntil([&] { return reset; }),"reset native worker parse counter");
        require(wait(c,[&] { c.syncFile(202); }),"native FB2 position uploads through BookOrbit HTTP contract");
        bool measured=false; quint64 parses=0;
        ClientWorkerCheck::executor(c).submit(&c,[](const FileCancellation &) { return progressWork().fb2Parses; },[&](quint64 value,bool) { parses=value; measured=true; });
        require(waitUntil([&] { return measured; }) && parses==1,"native FB2 upload uses one immutable full-file index");
        mode("fb2_remote"); require(wait(c,[&] { c.syncFile(202); }),"remote FB2 coordinate applies through real native setter");
        require(readerPosition(path,&position) && bookCfi(path,position)=="epubcfi(/6/2!/4/2/2/2/1:0)","native FB2 readback matches the incoming exact coordinate");
        require(wait(c,[&] { c.syncFile(202); }),"repeated native FB2 sync acknowledges without echo upload");
        return 0;
    }
    if (argc==4 && QString::fromLocal8Bit(argv[3])=="--history-check") {
        c.showDownloaded(true); c.refreshRecents(); require(waitForRecents(c),"native history refresh completes");
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
    require(readerPosition(path,&before),"read saved stock-reader position, including a fresh-book placeholder");
    UiMessage error;
    const auto prepared=prepareReaderPosition(path,"epubcfi(/6/2!/4/122/1)");
    const bool applied=saveReaderPosition(path,before,prepared,readerProfile(),&error);
    if (!applied) std::fprintf(stderr,"Native preparation failed: %s; coordinate=%d estimate=%d framework=%d file=%d reader=%d\n",
        qPrintable(error.code),prepared.coordinate,prepared.estimate,prepared.framework.valid,prepared.file.valid,int(readerFileState(path)));
    require(applied,"prepare distinct native paragraph 060");
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
    c.showDownloaded(true); c.refreshRecents(); require(waitForRecents(c),"native history refresh completes");
    require(c.recentBook()["fileId"].toInt()==101,"native recent snapshot identifies the exact account file");
    require(fault.open(QIODevice::WriteOnly) && fault.write("progress_range")==14,"move fixture server to a CFI range");
    fault.close();
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }),"incoming range applied through real firmware adapter");
    require(readerPosition(path,&after) && nativeCfi(after)=="epubcfi(/6/2!/4/42/1:0)","native readback is the range start");
    require(wait(c,[&] { c.syncAll(); }),"repeat batch is a no-op");
    Client restarted(QUrl("http://host.containers.internal:8766"),QString::fromLocal8Bit(argv[1])); require(waitForLibrary(restarted),"registry snapshot loaded");
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
