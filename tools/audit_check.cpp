// Regression checks use the real Client and QML, but never a native reader.
#include "client.h"
#include "device.h"
#include "progress.h"
#include "sync_decision.h"
#include "check_wait.h"
#include "worker_gate.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QImageReader>
#include <QEventLoop>
#include <QTimer>
#include <QCryptographicHash>
#include <QJSValue>
#include <QUuid>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QQuickItem>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

#ifdef BOOKORBIT_STORAGE_CHECK
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
enum class StorageFailure { None,JournalFile,JournalDirectory,BookFile,Rename,BookDirectory,RecordFile,RecordDirectory,ProgressDirectory,OldUnlink,JournalUnlink };
static StorageFailure storageFailure=StorageFailure::None;
static QString storageScope,storageOld;
static std::atomic<int> storageFaults{0};
static std::atomic_bool holdRecord{false},recordEntered{false},releaseRecord{false};
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    const auto path=QFileInfo("/proc/self/fd/"+QString::number(fd)).symLinkTarget();
    struct stat info{}; if (::fstat(fd,&info)!=0) return -1;
    const bool directory=S_ISDIR(info.st_mode);
    if (!directory && path.startsWith(storageScope+"/records/") && holdRecord.exchange(false)) {
        recordEntered=true;
        while (!releaseRecord.load()) QThread::msleep(1);
    }
    const bool journalExists=!QDir(storageScope+"/transfers").entryList({"*.json"}).isEmpty();
    bool fail=(!directory && storageFailure==StorageFailure::JournalFile && path.startsWith(storageScope+"/transfers/")) ||
        (directory && storageFailure==StorageFailure::JournalDirectory && path==storageScope+"/transfers" && journalExists) ||
        (!directory && storageFailure==StorageFailure::BookFile && QFileInfo(path).absolutePath()==storageScope) ||
        (!directory && storageFailure==StorageFailure::RecordFile && path.startsWith(storageScope+"/records/"));
    if (directory && storageFailure==StorageFailure::BookDirectory && path==storageScope &&
        QDir(storageScope).entryList({"101-*.epub"}).size()>=2) fail=true;
    if (directory && storageFailure==StorageFailure::RecordDirectory && path==storageScope+"/records") {
        QFile record(storageScope+"/records/101.json");
        if (record.open(QIODevice::ReadOnly)) fail=QJsonDocument::fromJson(record.readAll()).object()["filename"].toString()!=QFileInfo(storageOld).fileName();
    }
    if (directory && storageFailure==StorageFailure::ProgressDirectory && path==storageScope+"/records") fail=true;
    if (fail) { ++storageFaults; errno=EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __real_rename(const char *source,const char *destination);
extern "C" int __wrap_rename(const char *source,const char *destination) {
    if (storageFailure==StorageFailure::Rename && QFile::decodeName(source).endsWith(".part")) { ++storageFaults; errno=EIO; return -1; }
    return __real_rename(source,destination);
}
extern "C" int __real_unlink(const char *path);
extern "C" int __wrap_unlink(const char *path) {
    const auto name=QFile::decodeName(path);
    if ((storageFailure==StorageFailure::OldUnlink && name==storageOld) ||
        (storageFailure==StorageFailure::JournalUnlink && name.startsWith(storageScope+"/transfers/"))) {
        ++storageFaults; errno=EACCES; return -1;
    }
    return __real_unlink(path);
}
#endif

QString position;
QString profile="default";
bool indexed=true;
bool opened=false;
ReaderRecents history;
bool connectNetwork() { return true; }
QString readerProfile() { return profile; }
bool readerPosition(const QString &,QString *value) { *value=position; return true; }
ReaderFileState readerFileState(const QString &) { return ReaderFileState::Closed; }
std::function<ReaderRecents(const FileCancellation &)> readerRecentsTask(const QStringList &,const QString &profile) {
    auto snapshot=history; snapshot.profile=profile;
    return [snapshot](const FileCancellation &) { return snapshot; };
}
bool readerBookIndexed(const QString &) { return indexed; }
void scanBook(const QString &) {}
bool openReader(const QString &) { opened=true; return true; }
bool saveReaderPosition(const QString &,const QString &expected,const PreparedReaderPosition &prepared,const QString &expectedProfile,UiMessage *error) {
    if (expected!=position || expectedProfile!=profile) { *error=uiMessage("previous_result_unavailable"); return false; }
    if (!prepared.coordinate || !prepared.estimate) return false;
    position=prepared.native;
    return true;
}

void require(bool condition,const char *label) {
    std::printf("%s: %s\n",condition ? "PASS" : "FAIL",label);
    std::fflush(stdout);
    if (!condition) std::exit(1);
}
QByteArray bytes(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"read fixture"); return file.readAll();
}
void mode(const QString &path,const QByteArray &value) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(value)==value.size(),"set fixture mode");
}
bool wait(Client &client,const std::function<void()> &action) {
    bool completed=false;
    const bool result=waitForClient(client,action,&completed);
    require(completed,"client operation completed");
    return result;
}
bool until(const std::function<bool()> &predicate) {
    QEventLoop loop; QTimer poll;
    QObject::connect(&poll,&QTimer::timeout,&loop,[&] { if (predicate()) loop.quit(); });
    poll.start(10); QTimer::singleShot(10000,&loop,&QEventLoop::quit);
    if (!predicate()) loop.exec();
    return predicate();
}
int posts(const QString &root) {
    return QJsonDocument::fromJson(bytes(root+"/requests.json")).object()["POST /api/v1/books/files/101/progress"].toInt();
}

void storageFailureChecks(const QUrl &endpoint,const QString &root,const QString &fault) {
#ifdef BOOKORBIT_STORAGE_CHECK
    int number=0;
    for (const auto failure:{StorageFailure::JournalFile,StorageFailure::JournalDirectory,StorageFailure::BookFile,
        StorageFailure::Rename,StorageFailure::BookDirectory,StorageFailure::RecordFile,StorageFailure::RecordDirectory,
        StorageFailure::OldUnlink,StorageFailure::JournalUnlink}) {
        const auto data=root+"/storage-failure-"+QString::number(++number);
        Client client(endpoint,data,nullptr,false); require(waitForLibrary(client),"registry snapshot loaded"); mode(fault,"");
        require(wait(client,[&] { client.login(endpoint.toString(),"demo","demo"); }),"login for isolated storage failure");
        wait(client,[&] { client.download(0); });
        require(waitForRecents(client) && waitForMaintenance(client),"initial download maintenance settles");
        client.showDownloaded(true); storageOld=client.localFile(0); storageScope=ClientWorkerCheck::scope(client);
        require(!storageOld.isEmpty(),"initial book exists before injected storage failure");
        const auto original=bytes(storageOld);
        mode(fault,"changed"); storageFaults=0; storageFailure=failure;
        wait(client,[&] { client.download(0); });
        require(waitForMaintenance(client) && storageFaults>0,"storage fault reaches the real Client transfer protocol");
        const bool afterCommit=failure==StorageFailure::OldUnlink || failure==StorageFailure::JournalUnlink;
        require((afterCommit || (QFile::exists(storageOld) && bytes(storageOld)==original)) &&
            (failure==StorageFailure::JournalFile || !QDir(storageScope+"/transfers").entryList({"*.json"}).isEmpty()),
            "failed critical transition retains the previous book and a recovery journal");
        if (failure==StorageFailure::RecordDirectory)
            require(QDir(storageScope).entryList({"101-*.epub"}).size()>=2,"unconfirmed record replacement retains both book versions");
        storageFailure=StorageFailure::None;
        QString retried;
        QByteArray retriedBytes;
        if (failure==StorageFailure::RecordFile) {
            require(wait(client,[&] { client.download(0); }),"retry succeeds after the registry write error clears");
            require(waitForRecents(client) && waitForMaintenance(client),"successful retry maintenance settles");
            retried=client.localFile(0); retriedBytes=bytes(retried);
            require(retried!=storageOld && retriedBytes!=original,"successful retry selects the replacement book");
            require(QDir(storageScope+"/transfers").entryList({"*.json"}).isEmpty(),
                "successful retry retires earlier transfer journals before restart");
        }
        Client restarted(endpoint,data,nullptr,false); require(waitForLibrary(restarted),"registry snapshot loaded");
        require((restarted.configure(endpoint.toString(),"demo") && waitForLibrary(restarted)) && waitForRecents(restarted) && waitForMaintenance(restarted),"restart rechecks and confirms transfer state");
        const auto selected=restarted.localFile(0);
        require(!selected.isEmpty() && QFileInfo::exists(selected),"restart keeps the file selected by the persisted registry");
        if (failure==StorageFailure::RecordFile) {
            require(selected==retried && bytes(selected)==retriedBytes,"restart preserves the successfully retried book and its bytes");
            require(QDir(storageScope+"/transfers").entryList({"*.json"}).isEmpty() &&
                QDir(storageScope).entryList({"101-*.epub"})==QStringList{QFileInfo(selected).fileName()},
                "registry write failure, successful retry and restart leave no obsolete journal or book copy");
        }
    }
    storageScope.clear(); storageOld.clear(); mode(fault,"");
#else
    Q_UNUSED(endpoint); Q_UNUSED(root); Q_UNUSED(fault);
#endif
}

void coverCancellationChecks(Client &client,const QString &fault) {
    require(waitForRecents(client),"history settles before cover cancellation");
    auto &executor=ClientWorkerCheck::executor(client); WorkerGate gate(executor,&client);
    require(until([&] { return gate.entered->load(); }),"hold worker before stale cover decode");
    const auto before=client.presentationWork().coverDecodes;
    mode(fault,"cover_changed"); require(wait(client,[&] { client.refresh(); }),"queue a cover behind occupied worker");
    require(until([&] { return ClientWorkerCheck::coverPending(client); }),"cover decode task is queued");
    ClientWorkerCheck::stopCovers(client);
    require(wait(client,[&] { client.refresh(); }),"queue a fresh cover generation");
    require(until([&] { return ClientWorkerCheck::coverPending(client); }),"fresh cover retains its own task token");
    gate.open(); require(until([&] { return !ClientWorkerCheck::coverPending(client); }),"fresh cover completes after stale work drains");
    require(client.presentationWork().coverDecodes==before+1,"cancelled queued cover never decodes and cannot cancel its successor");
    mode(fault,""); require(wait(client,[&] { client.refresh(); }),"restore cover fixture version");
    require(until([&] { return !ClientWorkerCheck::coverPending(client); }),"restored cover completes");
}

void cleanupRaces(const QUrl &endpoint,const QString &root,const QByteArray &epub) {
    for(int race=0;race<4;++race) {
        Client client(endpoint,root+"/cleanup-race-"+QString::number(race),nullptr,false); require(waitForLibrary(client),"registry snapshot loaded");
        require((client.configure(endpoint.toString(),"demo") && waitForLibrary(client)),"configure cleanup race account");
        require(waitForRecents(client),"cleanup race history settles");
        const auto scope=ClientWorkerCheck::scope(client);
        const auto stem="700-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto path=scope+"/"+stem+".epub",journal=scope+"/transfers/"+stem+".json";
        const QJsonObject file{{"id",700},{"format","epub"},{"role","primary"}};
        const QJsonObject book{{"id",700},{"title","Cleanup race"},{"selectedFile",file},{"files",QJsonArray{file}}};
        const QJsonObject record{{"bookId",700},{"fileId",700},{"filename",stem+".epub"},{"format","epub"},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(epub,QCryptographicHash::Sha256).toHex())},
            {"bytes",epub.size()},{"book",book}};
        require(QDir().mkpath(scope+"/transfers"),"create cleanup transfer fixture");
        mode(path,epub); mode(scope+"/records/700.json",QJsonDocument(record).toJson());
        mode(journal,QJsonDocument(QJsonObject{{"fileId",700},{"previous",QJsonObject{}},{"next",record}}).toJson());
        auto &executor=ClientWorkerCheck::executor(client);
        WorkerGate first(executor,&client);
        require(until([&] { return first.entered->load(); }),"hold old cleanup");
        ClientWorkerCheck::store(client,"700",record); ClientWorkerCheck::cleanup(client);
        require(ClientWorkerCheck::cleanupRunning(client),"old cleanup is scheduled");
        const auto old=ClientWorkerCheck::cleanupId(client);
        WorkerGate next(executor,&client);
        if(race==3) {
            first.open();
            QElapsedTimer deadline; deadline.start();
            while(!next.entered->load() && deadline.elapsed()<10000) QThread::msleep(1);
            require(next.entered->load(),"old work completes before its GUI callback");
        }
        if(race==0 || race==3) {
            client.logout();
            require(ClientWorkerCheck::cleanupCancelled(client),"logout explicitly cancels cleanup state");
            ClientWorkerCheck::cleanup(client);
        } else if(race==1) {
            ClientWorkerCheck::invalidateSession(client);
            require(ClientWorkerCheck::cleanupCancelled(client),"session invalidation explicitly cancels cleanup state");
            ClientWorkerCheck::cleanup(client);
        } else { ClientWorkerCheck::cancelCleanup(client); ClientWorkerCheck::cleanup(client); }
        const auto fresh=ClientWorkerCheck::cleanupId(client);
        require(fresh>old && ClientWorkerCheck::cleanupRunning(client),"a fresh cleanup can be scheduled after cancellation");
        first.open(); require(until([&] { return next.entered->load(); }),"old cleanup drains before fresh work");
        QCoreApplication::processEvents();
        require(ClientWorkerCheck::cleanupRunning(client) && ClientWorkerCheck::cleanupId(client)==fresh && QFile::exists(journal),"old callback cannot clear the fresh task or consume its journal");
        next.open();
        require(waitForMaintenance(client) && !QFile::exists(journal) && bytes(path)==epub,"fresh cleanup finishes and preserves the registered book");
    }
}

void commitBoundaryChecks(const QUrl &endpoint,const QString &root,const QString &fault) {
#ifdef BOOKORBIT_STORAGE_CHECK
    const auto previousPosition=position;
    const auto data=root+"/first-download-commit";
    {
        Client client(endpoint,data,nullptr,false); require(waitForLibrary(client),"empty registry loaded for commit race");
        mode(fault,""); position.clear();
        require(wait(client,[&] { client.login(endpoint.toString(),"demo","demo"); }),"commit race signs in");
        storageScope=ClientWorkerCheck::scope(client); recordEntered=false; releaseRecord=false; holdRecord=true;
        client.download(0);
        require(until([&] { return recordEntered.load(); }),"worker stops inside records/101.json fsync before replacement");
        ClientWorkerCheck::cleanup(client); client.cancelDownload();
        int ticks=0; QTimer timer; QObject::connect(&timer,&QTimer::timeout,[&] { ++ticks; }); timer.start(5);
        require(client.busy() && client.downloading() && !client.downloadCancellable() && until([&] { return ticks>=5; }),
            "GUI cancel during commit keeps busy, blocks cleanup and still processes events");
        require(wait(client,[&] { releaseRecord=true; }) && waitForMaintenance(client),"commit completes before cleanup can run");
        client.showDownloaded(true);
        require(!client.localFile(0).isEmpty(),"first committed file remains registered in memory");
    }
    Client restarted(endpoint,data); require(waitForLibrary(restarted) && waitForMaintenance(restarted),"restart after commit race");
    require(!restarted.localFile(0).isEmpty() && QFile::exists(restarted.localFile(0)),"restart never sees a registry pointing to a deleted first download");

    Client client(endpoint,root+"/progress-intents",nullptr,false); require(waitForLibrary(client),"progress intent fixture loaded");
    mode(fault,""); position.clear();
    require(wait(client,[&] { client.login(endpoint.toString(),"demo","demo"); }) && wait(client,[&] { client.download(0); }),"progress intent fixture downloads");
    storageScope=ClientWorkerCheck::scope(client);
    const QString base="epubcfi(/6/2!/4/62/1)";
    for (bool incoming:{true,false}) for (const auto failure:{StorageFailure::RecordFile,StorageFailure::ProgressDirectory}) {
        auto record=ClientWorkerCheck::record(client,"101");
        record.remove("syncStatus");
        record["progress"]=incoming ? QJsonObject{} : QJsonObject{{"profile",profile},{"localBase",base},
            {"remoteBase",QJsonObject{{"cfi",base},{"pageNumber",QJsonValue::Null},{"percentage",40}}}};
        require(ClientWorkerCheck::save(client,"101",record),"seed explicit baseline for intent fault");
        position=incoming ? QString{} : "pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
        const auto beforePosition=position; const int before=posts(root);
        mode(fault,"progress_remote"); storageFaults=0; storageFailure=failure;
        require(!wait(client,[&] { client.syncFile(101); }) && storageFaults>0 && posts(root)==before && position==beforePosition,
            "failed file or directory fsync blocks both native mutation and server POST");
        storageFailure=StorageFailure::None;
        const auto saved=QJsonDocument::fromJson(bytes(storageScope+"/records/101.json")).object()["progress"].toObject();
        if (failure==StorageFailure::ProgressDirectory)
            require(saved.contains(incoming ? "pending" : "outgoing"),"unconfirmed replacement retains the visible recovery intent");
        Client recovery(endpoint,root+"/progress-intents"); require(waitForLibrary(recovery),"restart loads progress journal after storage fault");
        require(ClientWorkerCheck::record(recovery,"101")["progress"]==saved,"disk and memory agree after restart of failed intent");
    }
    position=previousPosition; mode(fault,"");
#else
    Q_UNUSED(endpoint) Q_UNUSED(root) Q_UNUSED(fault)
#endif
}

void workerRaces(Client &client,const QString &root,const QString &fault) {
    auto &executor=ClientWorkerCheck::executor(client);
    require(waitForRecents(client),"history settled before worker races");
    {
        const auto path=client.localFile(0);
        history={"default",true,{{path,{1,100}}}};
        client.refreshRecents(); require(waitForRecents(client) && !client.recentBook().isEmpty(),"seed current profile history");
        WorkerGate gate(executor,&client);
        require(until([&] { return gate.entered->load(); }),"hold worker during history refresh");
        client.refreshRecents(); profile="worker-history-other"; history.files.clear(); client.refreshRecents();
        require(client.recentBook().isEmpty() && !client.historyAvailable(),"profile switch immediately removes the old history");
        gate.open(); require(waitForRecents(client) && client.historyAvailable() && client.recentBook().isEmpty(),
                             "late old-profile history is rejected before the fresh snapshot is published");
        profile="default"; history={}; client.refreshRecents(); require(waitForRecents(client),"restore history fixture");
    }
    {
        WorkerGate gate(executor,&client);
        require(until([&] { return gate.entered->load(); }),"occupy worker before opening");
        opened=false; client.open(0);
        int ticks=0; QTimer timer; QObject::connect(&timer,&QTimer::timeout,[&] { ++ticks; }); timer.start(5);
        require(client.busy() && until([&] { return ticks>=5; }) && !opened,
                "open publishes busy and GUI timers keep running before hashing starts");
        require(wait(client,[&] { gate.open(); }) && opened,"opening resumes after worker completion");
    }
    const QString path=client.localFile(0); const auto original=bytes(path);
    position="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
    for(int race=0;race<4;++race) {
        WorkerGate gate(executor,&client);
        require(until([&] { return gate.entered->load(); }),"occupy worker before sync");
        const int before=posts(root); client.syncFile(101);
        require(client.busy(),"sync publishes busy before local parsing and hashing");
        if(race==0) profile="changed-during-worker";
        if(race==1) position="pbr:/webkit?##epubcfi(/6/2!/4/8/1)";
        if(race==2) ClientWorkerCheck::changeAccount(client);
        if(race==3) { auto modified=original; modified[modified.size()-1]^=1; mode(path,modified); }
        require(!wait(client,[&] { gate.open(); }) && posts(root)==before && !client.progressConflict(),
                "changed profile, reader position, account or same-size file blocks a stale sync write");
        profile="default"; position="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
        if(race==3) mode(path,original);
    }
    {
        WorkerGate gate(executor,&client);
        require(until([&] { return gate.entered->load(); }),"occupy worker before verification");
        client.verifyLibrary();
        require(client.busy() && !wait(client,[&] { client.cancelLibraryVerification(); }),"preflight verification cancels immediately");
        require(wait(client,[&] { client.openFile(101); QTimer::singleShot(20,&client,[&] { gate.open(); }); }),
                "cancelled verification cannot finish a newer queued opening");
    }
    {
        WorkerGate gate(executor,&client);
        require(until([&] { return gate.entered->load(); }),"occupy worker during post-download validation");
        client.download(1);
        require(until([&] { return bool(ClientWorkerCheck::task(client)); }) && client.downloading(),
                "download remains cancellable while its worker validation is pending");
        require(!wait(client,[&] { client.cancelDownload(); }) && !client.downloading(),"post-download preparation cancels immediately");
        require(wait(client,[&] { client.openFile(101); QTimer::singleShot(20,&client,[&] { gate.open(); }); }) && bytes(path)==original,
                "cancelled download result cannot register a file or finish a newer operation");
    }
    {
        mode(fault,"changed");
        WorkerGate first(executor,&client);
        require(until([&] { return first.entered->load(); }),"hold download before old-file hash");
        client.download(0);
        require(until([&] { return bool(ClientWorkerCheck::task(client)); }),"old-file hash is queued");
        const auto oldTask=ClientWorkerCheck::task(client);
        WorkerGate validation(executor,&client); first.open();
        require(until([&] { return validation.entered->load() && ClientWorkerCheck::task(client)!=oldTask; }),
                "hold download between old-file hash and EPUB validation");
        auto modified=original; modified[modified.size()-1]^=1; mode(path,modified);
        require(!wait(client,[&] { validation.open(); }) && client.localFile(0)==path && bytes(path)==modified,
                "old file changed during staging prevents publishing a replacement");
        mode(path,original); mode(fault,"");
    }
    // Hold the second worker pass after GET, then replace the local file at the same size.
    mode(fault,"slow_progress");
    const int before=posts(root); client.syncFile(101);
    const auto localTask=ClientWorkerCheck::task(client);
    // Poll the fixture request count without changing client state.
    const auto gets=[&] { QFile file(root+"/requests.json"); if(!file.open(QIODevice::ReadOnly)) return 0; return QJsonDocument::fromJson(file.readAll()).object()["GET /api/v1/books/files/101/progress"].toInt(); };
    const int initialGets=gets();
    require(until([&] { return gets()>initialGets; }),"server GET begins after the first hash");
    WorkerGate remoteGate(executor,&client);
    require(until([&] { return remoteGate.entered->load(); }),"hold worker during remote response");
    require(until([&] { return ClientWorkerCheck::task(client)!=localTask; }),"second integrity pass is queued after network response");
    auto modified=original; modified[modified.size()-1]^=1; mode(path,modified);
    require(!wait(client,[&] { remoteGate.open(); }) && posts(root)==before,"second full hash rejects a same-size rewrite after GET");
    mode(path,original); mode(fault,"");
    for (int race=0;race<5;++race) {
        mode(fault,"slow_progress"); position="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
        const auto oldRecord=ClientWorkerCheck::record(client,"101"); const int beforePost=posts(root),beforeGet=gets();
        client.syncFile(101); const auto localTask=ClientWorkerCheck::task(client);
        require(until([&] { return gets()>beforeGet; }),"action race reaches server GET");
        WorkerGate analysisGate(executor,&client);
        require(until([&] { return analysisGate.entered->load() && ClientWorkerCheck::task(client)!=localTask; }),"hold decision validation after GET");
        const auto analysisTask=ClientWorkerCheck::task(client); WorkerGate actionGate(executor,&client); analysisGate.open();
        require(until([&] { return actionGate.entered->load() && ClientWorkerCheck::task(client)!=analysisTask; }),"chosen action is queued after the decision");
        if (race==0) profile="changed-during-preparation";
        if (race==1) position="pbr:/webkit?##epubcfi(/6/2!/4/8/1)";
        if (race==2) ClientWorkerCheck::changeAccount(client);
        if (race==3) { auto modified=original; modified[modified.size()-1]^=1; mode(path,modified); }
        if (race==4) { auto record=oldRecord; record["progress"]=QJsonObject{{"profile",profile},{"localBase","changed"}}; ClientWorkerCheck::store(client,"101",record); }
        require(!wait(client,[&] { actionGate.open(); }) && posts(root)==beforePost,
                "late prepared action cannot write after profile, reader, account, file or journal changes");
        profile="default"; position="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
        if (race==3) mode(path,original);
        if (race==4) ClientWorkerCheck::store(client,"101",oldRecord);
        mode(fault,"");
    }
}

void registryChecks(const QUrl &endpoint,const QString &root) {
    Client client(endpoint,root+"/registry-race"); require(waitForLibrary(client),"registry race initial state");
    auto &executor=ClientWorkerCheck::executor(client);
    WorkerGate gate(executor,&client); require(until([&] { return gate.entered->load(); }),"hold registry worker");
    ClientWorkerCheck::reload(client);
    require(client.libraryLoading() && client.busy() && client.books().isEmpty(),"registry loading is visible before its scan starts");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("client",&client);
    engine.rootContext()->setContextProperty("screenWidth",600); engine.rootContext()->setContextProperty("screenHeight",800);
    engine.load(QUrl("qrc:/Main.qml")); require(!engine.rootObjects().isEmpty(),"loading QML creates a window while worker waits");
    auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first()); bool frame=false;
    QObject::connect(window,&QQuickWindow::frameSwapped,&client,[&] { frame=true; }); window->show(); window->requestUpdate();
    require(until([&] { return frame; }) && client.libraryLoading(),"first QML frame renders before full registry scan completes");
    const auto splash=window->findChild<QObject *>("startupScreen");
    const auto splashIcon=window->findChild<QObject *>("startupIcon");
    const auto splashTitle=window->findChild<QObject *>("startupTitle");
    require(splash && splash->property("visible").toBool() && splashIcon && splashIcon->property("status").toInt()==1
            && splashTitle && splashTitle->property("text").toString()=="BookOrbit client", "first frame shows startup icon and title while registry waits");
    const auto message=window->findChild<QObject *>("operationMessage");
    require(message && message->property("text").toString().contains("Загрузка"),"first frame displays downloaded-library loading state");
    // A second generation must remain loading when the first cancelled callback arrives.
    WorkerGate next(executor,&client); ClientWorkerCheck::reload(client); gate.open();
    require(until([&] { return next.entered->load(); }),"old registry drains before newer generation");
    QCoreApplication::processEvents(); require(client.libraryLoading(),"old account-generation callback cannot publish or clear loading");
    next.open(); require(waitForLibrary(client) && waitForRecents(client),"new registry generation publishes successfully");
    require(!splash->property("visible").toBool(),"startup screen disappears when local library is ready");
    WorkerGate reload(executor,&client); require(until([&] { return reload.entered->load(); }),"hold later registry reload");
    ClientWorkerCheck::reload(client);
    require(client.libraryLoading() && !splash->property("visible").toBool(),"later registry reload does not reopen startup screen");
    reload.open(); require(waitForLibrary(client) && waitForRecents(client),"later registry reload completes");
    client.resetPresentationWork(); client.refreshRecents();
    require(waitForRecents(client) && client.presentationWork().localFileChecks==0,"full recents preparation performs no GUI localFile checks");
}

void modelChecks() {
    for (int size:{100,1000,5000}) {
        LibraryModel model; QVariantList values;
        for (int i=0;i<size;++i) values.append(QVariantMap{{"bookId",i+1},{"title",QString::number(i)}});
        model.replace(values,true); model.sequenceVisits=model.indexVisits=model.lookups=0;
        model.replace(values,false);
        require(model.sequenceVisits==quint64(size) && !model.lookups && !model.indexVisits,
            "unchanged model reconciliation visits each typed ID once without lookup or reindexing");
        model.move(0,size-1); model.move(size-1,1); model.remove(0); model.insert(0,{{"bookId",size+1}});
        bool indexed=true; for (int i=0;i<model.rowCount();++i) indexed=indexed && model.rowFor(model.idAt(i))==i;
        require(indexed,"model index follows moves, insertions and removals");
        std::printf("MEASURE: same_order_rows=%d sequence_visits=%d lookups=0 index_visits=0\n",size,size);
    }
}

void presentationChecks(QGuiApplication &app, QTranslator &russian, const QUrl &endpoint, const QString &root) {
    const QString data=root+"/presentation", source=root+"/plain.epub";
    QString scope;
    { Client seed(endpoint,data); require(waitForLibrary(seed),"registry snapshot loaded"); require((seed.configure(endpoint.toString(),"demo") && waitForLibrary(seed)),"configure presentation account"); scope=seed.downloadDirectory(); }
    const auto content=bytes(source);
    const QString digest=QString::fromLatin1(QCryptographicHash::hash(content,QCryptographicHash::Sha256).toHex());
    QJsonObject choices;
    for (int i=0; i<1000; ++i) {
        const int bookId=10000+i, fileId=50000+i;
        const QJsonObject file{{"id",fileId},{"format","epub"},{"role","content"},{"sizeBytes",content.size()}};
        const QJsonObject book{{"id",bookId},{"title",QString("Книга %1").arg(i,4,10,QChar('0'))},
            {"authors",QJsonArray{QString("Автор %1").arg(999-i,4,10,QChar('0'))}},
            {"files",QJsonArray{file}},{"selectedFile",file}};
        QJsonObject record{{"bookId",bookId},{"fileId",fileId},{"sha256",digest},{"bytes",content.size()},
            {"format","epub"},{"filename",QString::number(fileId)+".epub"},{"book",book}};
        if (!i) record["progress"]=QJsonObject{{"profile",profile},{"pending",QJsonObject{{"cfi","epubcfi(/6/2!/4/2/1:3)"}}}};
        mode(scope+"/records/"+QString::number(fileId)+".json",QJsonDocument(record).toJson());
        require(QFile::copy(source,scope+"/"+QString::number(fileId)+".epub"),"copy isolated library file");
        choices[QString::number(bookId)]=fileId;
    }
    mode(scope+"/file-choices.json",QJsonDocument(choices).toJson());
    history.available=true; history.files[scope+"/50007.epub"]={10007,100};
    Client client(endpoint,data); require(waitForLibrary(client),"registry snapshot loaded");
    require(waitForRecents(client),"initial history snapshot completes");
    client.resetPresentationWork();
    QElapsedTimer clock; clock.start();
    const auto first=client.books();
    const auto elapsed=clock.nsecsElapsed();
    const auto initial=client.presentationWork();
    require(first.size()==1000 && initial.libraryBuilds==1 && initial.librarySorts==1 && initial.localFileChecks==0,
        "1000-book GUI snapshot uses worker metadata and groups/sorts once");
    std::printf("MEASURE: books=1000 build_ms=%.3f file_checks=%llu builds=%llu sorts=%llu\n",
        elapsed/1000000.0,static_cast<unsigned long long>(initial.localFileChecks),
        static_cast<unsigned long long>(initial.libraryBuilds),static_cast<unsigned long long>(initial.librarySorts));
    for (int i=0; i<10; ++i)
        require(client.books()==first && client.recentBook()["fileId"].toInt()==50007,"repeated getters share the library snapshot");
    require(client.presentationWork()==initial,"repeated getters perform no presentation work");
    const auto summary=client.syncSummary();
    const auto syncWork=client.presentationWork();
    require(summary["total"].toInt()==1000 && syncWork.syncBuilds==1 && syncWork.syncSorts==1 &&
            syncWork.localFileChecks==0,"sync list and counters build one separate snapshot");
    require(client.syncBooks().size()==1000 && !summary.contains("books") && client.syncSummary()==summary,"sync aggregates exclude snapshots and the explicit C++ snapshot remains available");
    client.searchDownloaded("КНИГА 0007");
    require(client.books().size()==1 && client.books().first().toMap()["bookId"].toInt()==10007 &&
            client.recentBook().isEmpty() && client.presentationWork().localFileChecks==syncWork.localFileChecks &&
            client.presentationWork().libraryBuilds==syncWork.libraryBuilds,
        "case-folded search only filters prepared rows and hides recents");
    require(client.localFile(0)==scope+"/50007.epub","indexed actions select the filtered snapshot row");
    opened=false;
    require(wait(client,[&] { client.open(0); }) && opened,"filtered index opens its freshly checked file");
    client.showDetail(10000);
    require(client.detailVisible() && client.detail()["bookId"].toInt()==10000,
        "unfiltered snapshot can open details outside search results");
    client.closeDetail(); client.searchDownloaded("");
    client.resetPresentationWork();
    client.setLocalSort("author");
    require(client.books().first().toMap()["bookId"].toInt()==10999 &&
            client.presentationWork().librarySorts==1 && client.presentationWork().localFileChecks==0 &&
            client.presentationWork().libraryBuilds==0,"author sort reuses prepared rows without checking files");
    client.setLocalSort("title");
    require(client.books().first().toMap()["bookId"].toInt()==10000,"title sort preserves original ordering semantics");
    client.setLocalSort("recent");
    require(client.books().first().toMap()["bookId"].toInt()==10007,"native recents determine recent ordering");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("client",&client);
    engine.rootContext()->setContextProperty("screenWidth",600);
    engine.rootContext()->setContextProperty("screenHeight",800);
    int warnings=0;
    QObject::connect(&engine,&QQmlApplicationEngine::warnings,&engine,[&](const QList<QQmlError> &errors) { warnings+=errors.size(); });
    engine.load(QUrl("qrc:/Main.qml"));
    require(!engine.rootObjects().isEmpty(),"load production QML for 1000-book presentation checks");
    QObject *window=engine.rootObjects().first();
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
    const auto catalog=window->findChild<QObject *>("catalog"), grid=window->findChild<QObject *>("coverCatalog");
    require(catalog && grid && catalog->property("count").toInt()==1000 && grid->property("count").toInt()==1000,
        "list and grid consume the shared QML snapshot");
    int librarySignals=0, syncSignals=0;
    QObject::connect(&client,&Client::libraryChanged,&engine,[&] { ++librarySignals; });
    QObject::connect(&client,&Client::syncSummaryChanged,&engine,[&] { ++syncSignals; });
    client.books(); client.syncSummary(); client.recentBook(); client.resetPresentationWork();
    client.connectForSync(); client.setUiContext("settings");
    require(client.setDiagnosticLogging(true),"toggle diagnostics during notification isolation check");
    const QString log=client.diagnosticLogPath();
    require(QFile::rename(log,log+".held") && QFile::link(log+".held",log),"inject unsafe diagnostic log path");
    client.logDiagnostic("presentation check");
    require(!client.diagnosticError().isEmpty(),"diagnostic error publishes settings notification");
    require(QFile::remove(log) && QFile::rename(log+".held",log),"restore diagnostic log path");
    client.logDiagnostic("presentation restored");
    require(client.diagnosticError().isEmpty(),"diagnostic recovery clears settings error");
    require(!client.configure("http://not-allowed",""),"validation error changes only connection feedback");
    require(client.prepareUpdate() && client.busy(),"prepare OTA changes operation state");
    std::function<QQuickItem *(QQuickItem *)> findAction=[&](QQuickItem *parent) -> QQuickItem * {
        if (parent->objectName()=="bookAction-10007") return parent;
        for (auto child : parent->childItems()) if (auto found=findAction(child)) return found;
        return nullptr;
    };
    const auto action=findAction(qobject_cast<QQuickWindow *>(window)->contentItem());
    require(action && !action->property("enabled").toBool(),"busy disables the existing QML read action");
    client.cancelUpdate();
    require(!client.busy() && action->property("enabled").toBool(),"cancel OTA restores the QML action");
    window->setProperty("coverGrid",true); window->setProperty("coverGrid",false);
    QCoreApplication::processEvents();
    require(client.presentationWork()==Client::PresentationWork{} && librarySignals==0 && syncSignals==0,
        "feedback, diagnostics, UI context, view toggle and busy do zero library or sync work");
    require(window->property("uiMessage").value<QJSValue>().toVariant().toMap()["text"]==client.feedback()["text"],
        "isolated feedback notification updates the production QML message");
    std::printf("MEASURE: message_busy_context file_checks=0 library_builds=0 library_sorts=0 sync_builds=0 sync_sorts=0\n");

    {
        auto *model=client.bookModel(); int resets=0,updates=0;
        const int affected=model==nullptr ? -1 : static_cast<LibraryModel *>(model)->rowFor(10007);
        const auto resetConnection=QObject::connect(model,&QAbstractItemModel::modelReset,&engine,[&] { ++resets; });
        auto changed=QObject::connect(model,&QAbstractItemModel::dataChanged,&engine,[&](const QModelIndex &first,const QModelIndex &last,const QList<int> &roles) {
            require(first.row()==affected && last.row()==affected && !roles.isEmpty(),"status notification addresses one row and explicit roles"); ++updates;
        });
        const auto oldY=catalog->property("contentY");
        client.resetPresentationWork();
        ClientWorkerCheck::saveSyncMessage(client,"50007",uiMessage("previous_result_unavailable"));
        require(waitForRecents(client),"single-book result history completes");
        client.books(); client.syncSummary(); client.recentBook();
        const auto work=client.presentationWork();
        require(client.bookModel()==model && !resets && updates==1 && catalog->property("contentY")==oldY,
                "single-book status retains model, other delegates and scroll position");
        require(!work.libraryBuilds && !work.librarySorts && !work.syncBuilds && !work.syncSorts && work.localFileChecks==0 && work.bookUpdates==1 && work.syncUpdates==1,
                "single-book completion uses worker metadata and updates only its group among 1000 books");
        require(!work.indexBuilds && !work.indexVisits && !work.recentPasses && !work.recentVisits && !work.modelIndexVisits &&
            !work.modelSequenceVisits && !work.syncSnapshots && work.syncReconciliations==1,
            "status-only completion performs no whole-library index, recent, reconciliation or snapshot pass");
        QObject::disconnect(changed);

        auto record=ClientWorkerCheck::record(client,"50007"); client.resetPresentationWork();
        require(ClientWorkerCheck::save(client,"50007",record) && !client.presentationWork().recordWrites,"confirmed identical JSON skips writing");
        const auto path=scope+"/records/50007.json";
        require(QFile::rename(path,path+".held") && QDir().mkdir(path),"inject record commit failure");
        record["invalid"]=true;
        require(!ClientWorkerCheck::save(client,"50007",record),"record commit fails without a saved proof");
        ClientWorkerCheck::store(client,"50007",record);
        require(QDir().rmdir(path) && QFile::rename(path+".held",path),"restore record storage");
        require(ClientWorkerCheck::save(client,"50007",record) && client.presentationWork().recordWrites==2 &&
                QJsonDocument::fromJson(bytes(path)).object()==record,"memory equality after failed commit still writes and persists");
        record.remove("invalid"); require(ClientWorkerCheck::save(client,"50007",record),"restore valid record");
        require(QFile::remove(path) && ClientWorkerCheck::save(client,"50007",record),"missing confirmed JSON is recreated");
        auto moved=record; moved["bookId"]=10008; auto book=moved["book"].toObject(); book["id"]=10008; moved["book"]=book;
        ClientWorkerCheck::store(client,"50007",moved);
        require(ClientWorkerCheck::bookFiles(client,10007).isEmpty() && ClientWorkerCheck::bookFiles(client,10008).size()==2,"book index moves a file without duplicate entries");
        ClientWorkerCheck::publish(client);
        require(client.books().size()==999 && catalog->property("count").toInt()==999 && grid->property("count").toInt()==999,"removing a book group notifies both shared views");
        ClientWorkerCheck::store(client,"50007",record); ClientWorkerCheck::publish(client);
        require(client.books().size()==1000 && ClientWorkerCheck::bookFiles(client,10007)==QStringList{"50007"},"book index and model recover original membership");
        client.setLocalSort("title"); client.searchDownloaded("Книга 0007");
        auto renamed=record; book=renamed["book"].toObject(); book["title"]=""; renamed["book"]=book;
        client.resetPresentationWork(); ClientWorkerCheck::store(client,"50007",renamed); ClientWorkerCheck::publish(client);
        require(client.books().isEmpty() && catalog->property("count").toInt()==0,"changing the title removes only the affected search result");
        client.searchDownloaded("");
        std::printf("MEASURE: title_move first_book=%d builds=%llu sorts=%llu\n",client.books().first().toMap()["bookId"].toInt(),
                     static_cast<unsigned long long>(client.presentationWork().libraryBuilds),static_cast<unsigned long long>(client.presentationWork().librarySorts));
        require(client.books().first().toMap()["bookId"].toInt()==10007 && !client.presentationWork().libraryBuilds && !client.presentationWork().librarySorts,
                "changed sort key moves one prepared book without sorting the library");
        ClientWorkerCheck::store(client,"50007",record); ClientWorkerCheck::publish(client); client.setLocalSort("recent");
        require(resets>0,"bulk filtering and sort changes use an anchored reset");
        // The model outlives this local signal counter.
        QObject::disconnect(resetConnection);
        const auto incremental=client.books(); const auto incrementalSync=client.syncSummary();
        ClientWorkerCheck::rebuild(client);
        require(client.books()==incremental && client.syncSummary()==incrementalSync,"incremental rows, order and counters match a fresh complete build");
    }

    {
        client.setLocalSort("title"); client.searchDownloaded("");
        const auto originalA=ClientWorkerCheck::record(client,"50000"),originalC=ClientWorkerCheck::record(client,"50002");
        auto a=originalA,c=originalC; auto ab=a["book"].toObject(),cb=c["book"].toObject();
        ab["title"]="000 B"; cb["title"]="000 A"; a["book"]=ab; c["book"]=cb;
        for (bool reverse:{false,true}) {
            if (reverse) { ClientWorkerCheck::store(client,"50002",c); ClientWorkerCheck::store(client,"50000",a); }
            else { ClientWorkerCheck::store(client,"50000",a); ClientWorkerCheck::store(client,"50002",c); }
            ClientWorkerCheck::publish(client);
            const auto incremental=client.books();
            require(incremental[0].toMap().value("bookId").toInt()==10002 && incremental[1].toMap().value("bookId").toInt()==10000,
                "two changed books reach their common final order independently of insertion order");
            ClientWorkerCheck::rebuild(client);
            require(client.books()==incremental,"batched final rows and roles equal a fresh complete build");
            ClientWorkerCheck::store(client,"50000",originalA); ClientWorkerCheck::store(client,"50002",originalC); ClientWorkerCheck::publish(client);
        }
        for (bool gridView:{false,true}) {
            window->setProperty("coverGrid",gridView); QCoreApplication::processEvents();
            auto *view=gridView ? grid : catalog;
            // Downloads use the list; give the hidden grid a viewport for its anchor check.
            if (gridView) { view->setProperty("width",600); view->setProperty("height",600); }
            require(QMetaObject::invokeMethod(view,"positionViewAtIndex",Q_ARG(int,400),Q_ARG(int,0)),"scroll before a bulk model reset");
            QCoreApplication::processEvents();
            const auto previousY=view->property("contentY").toDouble();
            ClientWorkerCheck::store(client,"50000",a); ClientWorkerCheck::store(client,"50002",c); ClientWorkerCheck::publish(client);
            QCoreApplication::processEvents(); QCoreApplication::processEvents();
            std::printf("ANCHOR: grid=%d before=%.3f after=%.3f\n",gridView,previousY,view->property("contentY").toDouble());
            require(std::abs(view->property("contentY").toDouble()-previousY)<1,"bulk reset keeps the visible library anchor in list and grid");
            ClientWorkerCheck::store(client,"50000",originalA); ClientWorkerCheck::store(client,"50002",originalC); ClientWorkerCheck::publish(client);
            QCoreApplication::processEvents(); QCoreApplication::processEvents();
        }
        window->setProperty("coverGrid",false); client.setLocalSort("recent");
        window->setProperty("syncBookKey","book:10007");
        client.bookModel(); client.syncModel(); client.resetPresentationWork();
        for (int i=0;i<1000;++i) {
            const auto id=QString::number(50000+i); auto record=ClientWorkerCheck::record(client,id);
            auto status=record["syncStatus"].toObject(); status["profile"]=profile;
            status["at"]="2026-10-16T12:34:56Z"; record["syncStatus"]=status;
            ClientWorkerCheck::store(client,id,record); ClientWorkerCheck::publish(client);
        }
        const auto series=client.presentationWork();
        require(series.bookUpdates==1000 && series.syncReconciliations==1000 && !series.indexBuilds && !series.recentPasses &&
            !series.modelIndexVisits && !series.modelSequenceVisits && !series.modelSnapshots && !series.syncSnapshots,
            "1000 content-only updates reconcile 1000 groups without whole-library passes or list snapshots");
        std::printf("MEASURE: status_updates=1000 reconciliations=%llu index_visits=%llu recent_visits=%llu snapshots=%llu\n",
            static_cast<unsigned long long>(series.syncReconciliations),static_cast<unsigned long long>(series.indexVisits),
            static_cast<unsigned long long>(series.recentVisits),static_cast<unsigned long long>(series.syncSnapshots));
        require(window->property("selectedSyncBook").value<QJSValue>().toVariant().toMap()==client.syncBookDetails("book:10007"),
            "selected sync details refresh when content changes without changing aggregate counts");
        window->setProperty("syncBookKey","");
        client.setSyncFilter("waiting"); require(client.syncModel()->rowCount()>0,"sync proxy exposes the waiting category");
        client.setSyncFilter("invalid"); require(client.syncFilter()=="waiting","invalid sync filter is rejected");
        client.setSyncFilter("all"); require(client.syncModel()->rowCount()==1000,"sync proxy restores all groups");
        require(!client.syncBookDetails("book:10007").isEmpty() && client.syncBookDetails("missing").isEmpty(),"sync details use the group index");
    }
    client.showSyncFile(50000);
    require(client.detail()["pendingProgress"].toBool(),"original profile exposes its pending position");
    require(client.prepareUpdate(),"defer native recents while busy");
    profile="presentation-other"; client.refreshRecents();
    require(!client.historyAvailable() && client.recentBook().isEmpty() && !client.detail()["pendingProgress"].toBool() &&
            window->property("detailData").value<QJSValue>().toVariant().toMap()["pendingProgress"]==false,
        "profile change immediately clears old profile presentation even while busy");
    client.cancelUpdate(); client.refreshRecents(); require(waitForRecents(client),"history refresh completes");
    profile="default"; client.refreshRecents(); require(waitForRecents(client),"history refresh completes");
    client.closeDetail();
    const QString removed=scope+"/50007.epub";
    require(QFile::rename(removed,removed+".held"),"temporarily remove a cached test file");
    opened=false; client.openFile(50007);
    require(!opened,"fresh action checks reject a missing file before external refresh");
    client.refreshRecents(); require(waitForRecents(client),"history refresh completes");
    require(client.recentBook().isEmpty() && client.syncSummary()["attention"].toInt()>0,
        "external refresh observes missing files even with identical native recents");
    client.searchDownloaded("Книга 0007");
    require(client.books().size()==1 && client.books().first().toMap()["needsRepair"].toBool(),
        "missing book remains in the library with repair action");
    require(QFile::rename(removed+".held",removed),"restore cached test file");
    client.refreshRecents(); require(waitForRecents(client),"history refresh completes"); client.searchDownloaded("");
    require(client.recentBook()["fileId"].toInt()==50007,"external refresh restores available recent file");
    mode(removed,content+"changed size");
    client.refreshRecents(); require(waitForRecents(client),"history refresh completes"); client.searchDownloaded("Книга 0007");
    require(client.books().first().toMap()["needsRepair"].toBool(),"external size change invalidates cached file availability");
    mode(removed,content); client.refreshRecents(); require(waitForRecents(client),"history refresh completes"); client.searchDownloaded("");
    QObject::connect(&client,&Client::languageChanged,&engine,[&] {
        if (client.language()=="en") app.removeTranslator(&russian); else app.installTranslator(&russian);
        QLocale::setDefault(QLocale(client.language()=="en" ? "en" : "ru"));
        engine.retranslate();
    });
    const QString russianLabel=client.syncBooks().first().toMap()["label"].toString();
    require(client.setLanguage("en") && client.syncBooks().first().toMap()["label"].toString()!=russianLabel &&
            window->property("syncData").value<QJSValue>().toVariant().toMap()==client.syncSummary() && client.syncModel()->rowCount()==1000,
        "live language switch retranslates sync labels and the QML model");
    require(client.setLanguage("ru"),"restore Russian after cached language check");
    mode(root+"/fault.txt","");
    for (int i=0;i<3;++i) {
        const auto id=QString::number(50000+i); auto record=ClientWorkerCheck::record(client,id);
        record["bookId"]=i+1; auto book=record["book"].toObject(); book["id"]=i+1; record["book"]=book;
        require(ClientWorkerCheck::save(client,id,record),"bind three fixture records to catalog books");
    }
    client.resetPresentationWork();
    require(wait(client,[&] { client.login(endpoint.toString(),"demo","demo"); }) &&
            client.presentationWork().metadataVisits==3 && client.presentationWork().recordWrites==3,
            "initial catalog refresh visits and writes only three linked records among 1000 files");
    client.resetPresentationWork();
    require(wait(client,[&] { client.refresh(); }) && client.presentationWork().metadataVisits==3 && !client.presentationWork().recordWrites,
            "identical catalog metadata performs zero record writes");
    require(wait(client,[&] { client.showDetail(1); }),"load detailed metadata through the same file index");
    client.resetPresentationWork();
    require(wait(client,[&] { client.refreshDetail(); }) && client.presentationWork().metadataVisits==1 && !client.presentationWork().recordWrites,
            "identical detailed metadata also skips JSON writing");
    require(!warnings,"presentation checks produce no QML warnings");
    history={};
}

int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);
    if (argc==4 && QString::fromLocal8Bit(argv[1])=="--epub-memory") {
        auto rss=[](const QByteArray &name) -> qint64 {
#ifdef Q_OS_LINUX
            QFile status("/proc/self/status"); if (!status.open(QIODevice::ReadOnly)) return -1;
            for (const auto &line:status.readAll().split('\n'))
                if (line.startsWith(name+':')) return line.simplified().split(' ')[1].toLongLong();
            return -1;
#else
            Q_UNUSED(name)
            struct rusage usage{}; getrusage(RUSAGE_SELF,&usage); return usage.ru_maxrss/1024;
#endif
        };
        const auto before=rss("VmRSS");
        const auto path=QString::fromLocal8Bit(argv[2]); double percent=0;
        const bool ok=epubPosition(path,"epubcfi(/6/2!/4/2/1:3)",&percent);
        const auto peak=rss("VmHWM");
        std::printf("MEASURE: %s accepted=%d rss_start_kib=%lld peak_rss_kib=%lld delta_peak_kib=%lld\n",
            qPrintable(QFileInfo(path).fileName()),ok,static_cast<long long>(before),static_cast<long long>(peak),static_cast<long long>(peak-before));
        require(ok==(QString::fromLocal8Bit(argv[3])=="1"),"working budget result matches expected coordinate/estimate support");
        require(before>0 && peak>=before && peak-before<64*1024,"transient RSS stays below the host regression ceiling");
        return 0;
    }
    QTranslator russian;
    require(russian.load(":/translations/bookorbit_ru.qm"), "Russian translation catalog loaded");
    app.installTranslator(&russian);
    QLocale::setDefault(QLocale("ru"));
    require(argc==3,"data directory and fault file supplied");
    require(QImageReader::supportedImageFormats().contains("svg"),"SVG icon decoder is installed");
    const QString root=QString::fromLocal8Bit(argv[1]),fault=QString::fromLocal8Bit(argv[2]);
    const auto fixture=[&](const QString &name) { return root+"/"+name+".epub"; };
    const QString point="epubcfi(/6/2!/4/2/1:3)";
    for (const auto &name:QStringList{"plain","doctype","comment","cdata","unicode","whitespace"})
        require(epubPosition(fixture(name),point,nullptr),"resolve logical text chunk including safe DOCTYPE and UTF-16");
    for (const auto &name:QStringList{"plain","comment","cdata","unicode"}) {
        require(epubPosition(fixture(name),"epubcfi(/6/2!/4/2/1:4)",nullptr),"accept end of logical text chunk");
        require(!epubPosition(fixture(name),"epubcfi(/6/2!/4/2/1:5)",nullptr),"reject offset beyond complete chunk");
    }
    require(epubPosition(fixture("empty_chunk"),"epubcfi(/6/2!/4/2/1:0)",nullptr) &&
            epubPosition(fixture("empty_chunk"),"epubcfi(/6/2!/4/2/3:0)",nullptr),"resolve empty chunks on both sides of an element");
    require(!epubPosition(fixture("empty_chunk"),"epubcfi(/6/2!/4/2/5:0)",nullptr),"reject nonexistent logical chunk");
    for (const auto &name:QStringList{"external","entities","internal","deep"})
        require(!epubPosition(fixture(name),point,nullptr),"reject external identifiers, subsets and excessive depth");
    const QString asserted="epubcfi(/6/2[chapter-ref]!/4/2[p]/1:3)";
    require(sameEpubPosition(fixture("plain"),point,asserted),"validated ID assertions preserve position identity");
    require(!sameEpubPosition(fixture("plain"),point,"epubcfi(/6/2[wrong]!/4/2/1:3)"),"wrong assertions never compare equal");
    require(sameEpubPosition(fixture("plain"),"epubcfi(/6/2!/4/2/1)","epubcfi(/6/2!/4/2/1:0)"),"implicit offset equals explicit zero");
    const QString range="epubcfi(/6/2!/4/2/1,:0,:3)",otherRange="epubcfi(/6/2!/4/2/1,:0,:4)";
    require(sameEpubPosition(fixture("plain"),range,otherRange) &&
            !sameEpubPosition(fixture("plain"),range,otherRange,true),"position comparison and complete range comparison stay distinct");
    double percentage=0;
    require(epubPosition(fixture("unrelated"),point,nullptr) && !epubPosition(fixture("unrelated"),point,&percentage),
            "unrelated malformed chapter does not invalidate a target locator");
    require(validEpub(fixture("plain")) && validEpub(fixture("doctype")),"validate EPUB container and resources");
    for (const auto &name:QStringList{"missing_container","missing_opf","missing_spine","ordinary_zip","truncated_zip"})
        require(!validEpub(fixture(name)),"reject invalid EPUB structure before replacement");
    resetProgressWork(); PositionInfo resolved;
    require(epubPosition(fixture("cache"),point,nullptr,nullptr,nullptr,&resolved) && progressWork().lengthPasses==0,
            "coordinate validation parses only the target chapter before choosing an action");
    require(positionEstimate(fixture("cache"),resolved,&percentage),"chosen action calculates chapter offsets once");
    const auto cacheWork=progressWork();
    require(cacheWork.packages==1 && cacheWork.chapters==1 && cacheWork.lengthReads==11 && cacheWork.lengthPasses==1,"first estimate builds one package and streams eleven remaining chapter lengths");
    require(epubPosition(fixture("cache"),"epubcfi(/6/24!/4/2/1:100)",&percentage) &&
            progressWork().packages==1 && progressWork().lengthPasses==1 && progressWork().chapters==2,
            "second position reuses the package and lengths, parsing only its target DOM");
    const auto cachePath=fixture("cache"); const auto cacheBytes=bytes(cachePath);
    mode(cachePath,cacheBytes);
    require(!positionEstimate(cachePath,resolved,&percentage),"same-size rewrite rejects coordinates resolved against an older identity");
    require(epubPosition(cachePath,point,&percentage) && progressWork().packages==2 && progressWork().lengthPasses==2,
            "changed file rebuilds the EPUB index");
    require(QFile::rename(cachePath,cachePath+".held"),"move the indexed inode aside");
    mode(cachePath,cacheBytes);
    require(epubPosition(cachePath,point,&percentage) && progressWork().packages==3,"atomic replacement also invalidates the index");
    require(QFile::remove(cachePath+".held"),"remove superseded cache fixture");
    for (const auto &name:QStringList{"plain","doctype","comment","cdata","unicode"})
        require(epubPosition(fixture(name),point,&percentage),"exercise bounded multi-book cache");
    require(progressWork().cachedBooks<=4 && progressWork().cachedBytes<=16*1024*1024 && progressWork().cachedNodes<=100000 && progressWork().evictions>0,
            "cache enforces book, memory and DOM-node limits");
    expireEpubCache(); require(!progressWork().cachedBooks,"idle lifetime releases cached books");
    {
        FileCancellationScope cancelled(std::make_shared<std::atomic_bool>(true));
        require(!epubPosition(cachePath,point,&percentage),"cancelled index construction cannot produce a position");
    }
    clearEpubCache();
    for (bool known:{false,true}) for (bool local:{false,true}) for (bool remote:{false,true}) {
        const auto expected=local!=remote ? (local ? SyncDecision::Upload : SyncDecision::ApplyRemote) :
            known && !local ? SyncDecision::Unchanged : SyncDecision::Conflict;
        require(decideSync(known,local,remote)==expected,"pure three-way decision matrix");
    }

    const QUrl endpoint(qEnvironmentVariable("BOOKORBIT_TEST_ENDPOINT"));
    require(endpoint.isValid() && !endpoint.host().isEmpty(),"isolated endpoint provided");
    const QString firstRunRoot=root+"/first-install/applications/bookorbit";
    require(!QFileInfo::exists(firstRunRoot),"first-install data directory is absent");
    Client firstRun(endpoint,firstRunRoot); require(waitForLibrary(firstRun),"registry snapshot loaded");
    require(QFileInfo(firstRunRoot).isDir() &&
            QFileInfo(firstRun.downloadDirectory()+"/records").isDir() && firstRun.status().isEmpty(),
            "first launch creates data directory and account records recursively");
    require(firstRun.setDiagnosticLogging(true),"first launch can save preferences");
    const auto savedPreferences=bytes(firstRunRoot+"/preferences.json");
    Client nextRun(endpoint,firstRunRoot); require(waitForLibrary(nextRun),"registry snapshot loaded");
    require(nextRun.diagnosticLogging() && bytes(firstRunRoot+"/preferences.json")==savedPreferences &&
            nextRun.status().isEmpty(),"existing data directory and preferences survive restart");
    const QString blockedRoot=root+"/blocked-data";
    mode(blockedRoot,"keep existing file");
    for (const QString &path:QStringList{blockedRoot,blockedRoot+"/bookorbit"}) {
        Client blocked(endpoint,path); require(waitForLibrary(blocked),"registry snapshot loaded");
        blocked.restoreSession();
        require(blocked.status().contains("Не удалось создать папку данных приложения") &&
                blocked.status().contains(path) && blocked.feedback()["result"]=="error" &&
                bytes(blockedRoot)=="keep existing file",
                "failed directory creation reports its path and preserves blocking file");
    }
    registryChecks(endpoint,root);
    Client client(endpoint,root+"/client"); require(waitForLibrary(client),"registry snapshot loaded");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("client",&client);
    engine.rootContext()->setContextProperty("screenWidth",600);
    engine.rootContext()->setContextProperty("screenHeight",800);
    engine.load(QUrl("qrc:/Main.qml"));
    require(!engine.rootObjects().isEmpty(),"load actual application QML offscreen");
    QObject *window=engine.rootObjects().first();
    const auto shown=[&](const char *name) {
        const auto item=window->findChild<QObject *>(QString::fromLatin1(name));
        return item && item->property("visible").toBool();
    };
    QCoreApplication::processEvents();
    require(wait(client,[&] { client.login(endpoint.toString(),"demo","demo"); }),"login and fetch catalog");
    require(wait(client,[&] { client.download(0); }),"download initial EPUB");
    cleanupRaces(endpoint,root,bytes(client.localFile(0)));
    workerRaces(client,root,fault);
    mode(fault,"error");
    require(!wait(client,[&] { client.download(1); }),"inline download returns server error");
    require(until([&] { return shown("operationMessage") && shown("retryOperation"); }) &&
            client.feedback()["context"]=="catalog","inline download error and retry are visible in actual QML");
    mode(fault,"");
    require(wait(client,[&] { client.retry(); }),"retry inline download");
    indexed=false;
    require(!wait(client,[&] { client.open(0); }) && shown("operationMessage"),"quick-open indexing error stays visible");
    indexed=true;

    window->setProperty("settings",true);
    window->setProperty("addingConnection",true);
    QCoreApplication::processEvents();
    mode(fault,"audit_catalog_error");
    require(!wait(client,[&] { client.login(endpoint.toString(),"demo","demo"); }) && client.authenticated(),
            "successful authentication survives initial catalog failure");
    require(until([&] { return shown("operationMessage") && shown("retryOperation"); }) &&
            client.feedback()["context"]=="connection","catalog failure and retry remain visible on connection screen");
    mode(fault,"");
    require(wait(client,[&] { client.retry(); }),"retry catalog without entering credentials again");
    require(!window->property("settings").toBool(),"successful catalog retry completes connection navigation");
    require(until([&] { return !client.coverUrl(1).isEmpty() && !client.coverUrl(2).isEmpty(); }),"load both covers");
    coverCancellationChecks(client,fault);
    window->setProperty("coverGrid",true);
    auto *quickWindow=qobject_cast<QQuickWindow *>(window);
    auto findCover=[&]() -> QQuickItem * {
        std::function<QQuickItem *(QQuickItem *)> find=[&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName()=="gridCover-1") return item;
            for (auto child:item->childItems()) if (auto value=find(child)) return value;
            return nullptr;
        };
        return find(quickWindow->contentItem());
    };
    require(until([&] { const auto cover=findCover(); return cover && cover->property("status").toInt()==1; }),"asynchronous cached cover reaches Ready");
    auto cover=findCover();
    require(cover->property("asynchronous").toBool() && cover->property("cache").toBool() &&
        cover->property("sourceSize").toSize()==QSize(240,320),"book cover decoding is asynchronous, cached and size-limited");
    const auto beforeCover=quickWindow->grabWindow().copy(cover->mapRectToScene(cover->boundingRect()).toAlignedRect());
    const QString firstCover=client.coverUrl(1),secondCover=client.coverUrl(2);
    mode(fault,"cover_changed");
    require(wait(client,[&] { client.refresh(); }),"refresh a changed cover");
    require(until([&] { return !client.coverUrl(1).isEmpty() && client.coverUrl(1)!=firstCover; }) &&
            client.coverUrl(2)==secondCover,"one changed cover leaves other source URLs unchanged");
    require(until([&] { const auto image=findCover(); return image && image->property("status").toInt()==1 && image->property("source").toUrl().toString()==client.coverUrl(1); }),
        "revised cover URL finishes asynchronous loading");
    cover=findCover();
    require(beforeCover!=quickWindow->grabWindow().copy(cover->mapRectToScene(cover->boundingRect()).toAlignedRect()),
        "cover cache displays new pixels after URL revision");
    auto rss=[]() -> qint64 {
        QFile status("/proc/self/status"); if (!status.open(QIODevice::ReadOnly)) return -1;
        const auto lines=status.readAll().split('\n');
        for (const auto &line:lines) if (line.startsWith("VmRSS:")) return line.simplified().split(' ')[1].toLongLong();
        return -1;
    };
    const auto memoryBefore=rss(); auto peak=memoryBefore;
    for (int i=0;i<20;++i) {
        window->setProperty("coverGrid",false); QCoreApplication::processEvents();
        window->setProperty("coverGrid",true); QCoreApplication::processEvents(); peak=qMax(peak,rss());
    }
    std::printf("MEASURE: cover_view_cycles=20 rss_before_kib=%lld rss_after_kib=%lld rss_peak_kib=%lld\n",
        static_cast<long long>(memoryBefore),static_cast<long long>(rss()),static_cast<long long>(peak));
    window->setProperty("coverGrid",false);
    mode(fault,"");
    client.showDownloaded(true);
    client.showSyncFile(101);
    position="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
    require(wait(client,[&] { client.syncFile(101); }),"establish agreed sync baseline");
    const auto bar=window->findChild<QObject *>("detailProgress"),label=window->findChild<QObject *>("detailReading");
    require(bar && label && until([&] {
        return bar->property("visible").toBool() && bar->property("value")==client.detail()["readingProgress"] &&
            label->property("text").toString().contains('%');
    }),"successful sync updates the visible QML bar and percentage without reopening details");
    position="pbr:/webkit?##epubcfi(/6/2!/4/8/1)";
    const int before=posts(root);
    mode(fault,"audit_post401");
    require(!wait(client,[&] { client.syncFile(101); }) && client.authenticated() && posts(root)==before+1,
            "POST 401 renews session but never replays an old write");
    require(!wait(client,[&] { client.syncFile(101); }) && client.progressConflict() && posts(root)==before+1,
            "fresh sync detects progress changed on another device during token refresh");
    require(wait(client,[&] { client.resolveProgress(true); }),"explicit fresh choice resolves conflict");
    require(until([&] { return bar->property("value")==client.detail()["readingProgress"]; }),
            "conflict resolution refreshes the existing QML progress bar");
    const int agreedPosts=posts(root);
    profile="equivalent-profile";
    position="pbr:/webkit?##epubcfi(/6/2!/4/8[p3]/1:0)";
    require(wait(client,[&] { client.syncFile(101); }) && !client.progressConflict() && posts(root)==agreedPosts,
            "equivalent native and server CFIs acknowledge without conflict or extra POST");
    profile="default";
    position="pbr:/webkit?##epubcfi(/6/2!/4/8/1)";
    require(wait(client,[&] { client.syncFile(101); }),"restore baseline for original profile");

    client.showSyncFile(101);
    const QString oldFile=client.localFile(0);
    require(!oldFile.isEmpty(),"find original local file");
    const qint64 oldSize=QFileInfo(oldFile).size();
    const QString recordPath=QFileInfo(oldFile).absolutePath()+"/records/101.json";
    mode(fault,"audit_new_file");
    require(!wait(client,[&] { client.verifyLibrary(); }) && client.detail()["remoteFileChanged"].toBool(),
            "verification marks replacement with a different byte count");
    require(wait(client,[&] { client.downloadSelected(); }),"repair from cached card refreshes file metadata");
    const QString replacement=client.localFile(0);
    require(replacement!=oldFile && QFileInfo(replacement).size()>oldSize && !QFile::exists(oldFile),
            "valid larger EPUB replaces old version only after validation");
    const QByteArray replacementBytes=bytes(replacement);
    mode(fault,"audit_zip_file");
    require(!wait(client,[&] { client.verifyLibrary(); }),"mark non-EPUB server replacement");
    const QByteArray recordBefore=bytes(recordPath);
    require(!wait(client,[&] { client.downloadSelected(); }) && client.localFile(0)==replacement &&
            bytes(replacement)==replacementBytes && bytes(recordPath)==recordBefore,
            "ordinary ZIP cannot replace a valid EPUB or its persisted record");
    storageFailureChecks(endpoint,root,fault);
    commitBoundaryChecks(endpoint,root,fault);
    modelChecks();
    presentationChecks(app,russian,endpoint,root);
    std::puts("PASS: audit regressions");
    return 0;
}
