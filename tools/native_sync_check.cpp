// VM-only: real Client + real firmware adapter, synthetic server/account/EPUB.
#include "client.h"
#include "device.h"
#include "progress.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
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
    QEventLoop loop; bool ok=false,done=false;
    auto connection=QObject::connect(&c,&Client::completed,&loop,[&](const QString &operation,bool result) {
        if (operation=="settings") return;
        ok=result; done=true; loop.quit();
    });
    QTimer::singleShot(0,&loop,action);
    QTimer::singleShot(25000,&loop,&QEventLoop::quit);
    loop.exec(); QObject::disconnect(connection);
    if (!ok) std::fprintf(stderr,"%s\n",qPrintable(c.status()));
    return done && ok;
}
int main(int argc,char **argv) {
    qputenv("QT_PLUGIN_PATH","/ebrmain/plugins");
    qputenv("QT_QPA_PLATFORM","pocketbook2");
    QCoreApplication::setSetuidAllowed(true);
    const auto screen=setupDevice();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc,argv);
    require(argc==3 || (argc==4 && QString::fromLocal8Bit(argv[3])=="--ui"),"isolated directory and fault file supplied");
    require(!screen.isEmpty(),"native adapter initialized");
    Client c(QUrl("http://host.containers.internal:8766"),QString::fromLocal8Bit(argv[1]));
    require(wait(c,[&] { c.login("demo","demo"); }),"synthetic native login");
    const QString bookDir="/mnt/ext1/books/BookOrbit/"+QFileInfo(QString::fromLocal8Bit(argv[1])).fileName();
    require(QDir().mkpath(bookDir) && c.setDownloadDirectory(bookDir),"isolated native download directory");
    require(wait(c,[&] { c.download(0); }),"download controlled EPUB");
    const QString path=c.localFile(0);
    for (int i=0;i<30 && !readerBookIndexed(path);++i) {
        QEventLoop pause; QTimer::singleShot(200,&pause,&QEventLoop::quit); pause.exec();
    }
    require(readerBookIndexed(path),"firmware scanner registered the file");
    QString before;
    require(readerPosition(path,&before) && !nativeCfi(before).isEmpty(),"read saved stock-reader CFI");
    QString error;
    require(saveReaderPosition(path,before,"epubcfi(/6/2!/4/122/1)",readerProfile(),&error),"prepare distinct native paragraph 060");
    require(readerPosition(path,&before) && nativeCfi(before)=="epubcfi(/6/2!/4/122/1)","confirm distinct initial CFI");
    require(wait(c,[&] { c.syncProgress(0); }),"real saved reader position uploaded to fixture");
    QFile fault(QString::fromLocal8Bit(argv[2]));
    require(fault.open(QIODevice::WriteOnly) && fault.write("progress_remote")==15,"move fixture server to paragraph 030");
    fault.close();
    require(wait(c,[&] { c.syncProgress(0); }),"incoming CFI saved through firmware without opening");
    QString after;
    require(readerPosition(path,&after) && nativeCfi(after)=="epubcfi(/6/2!/4/62/1)","native SQLite readback matches server CFI");
    require(readerFileState(path)==ReaderFileState::Closed,"book stayed closed throughout sync");
    require(fault.open(QIODevice::WriteOnly) && fault.write("progress_range")==14,"move fixture server to a CFI range");
    fault.close();
    require(wait(c,[&] { c.syncProgress(0); }),"incoming range applied through real firmware adapter");
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
