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
#include <QQuickWindow>
#include <QQuickItem>
#include <cstdio>
#include <cstdlib>

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
bool saveReaderPosition(const QString &,const QString &expected,const PreparedReaderPosition &prepared,const QString &expectedProfile,QString *error) {
    if (expected!=position || expectedProfile!=profile) { *error="Changed fixture"; return false; }
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
}

void presentationChecks(QGuiApplication &app, QTranslator &russian, const QUrl &endpoint, const QString &root) {
    const QString data=root+"/presentation", source=root+"/plain.epub";
    QString scope;
    { Client seed(endpoint,data); scope=seed.downloadDirectory(); }
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
    Client client(endpoint,data);
    require(waitForRecents(client),"initial history snapshot completes");
    client.resetPresentationWork();
    QElapsedTimer clock; clock.start();
    const auto first=client.books();
    const auto elapsed=clock.nsecsElapsed();
    const auto initial=client.presentationWork();
    require(first.size()==1000 && initial.libraryBuilds==1 && initial.librarySorts==1 && initial.localFileChecks==1000,
        "1000-book snapshot checks each file once and groups/sorts once");
    std::printf("MEASURE: books=1000 build_ms=%.3f file_checks=%llu builds=%llu sorts=%llu\n",
        elapsed/1000000.0,static_cast<unsigned long long>(initial.localFileChecks),
        static_cast<unsigned long long>(initial.libraryBuilds),static_cast<unsigned long long>(initial.librarySorts));
    for (int i=0; i<10; ++i)
        require(client.books()==first && client.recentBook()["fileId"].toInt()==50007,"repeated getters share the library snapshot");
    require(client.presentationWork()==initial,"repeated getters perform no presentation work");
    const auto summary=client.syncSummary();
    const auto syncWork=client.presentationWork();
    require(summary["total"].toInt()==1000 && syncWork.syncBuilds==1 && syncWork.syncSorts==1 &&
            syncWork.localFileChecks==2000,"sync list and counters build one separate snapshot");
    require(client.syncBooks()==summary["books"].toList() && client.syncSummary()==summary &&
            client.presentationWork()==syncWork,"sync getters share the same snapshot");
    client.searchDownloaded("КНИГА 0007");
    require(client.books().size()==1 && client.books().first().toMap()["bookId"].toInt()==10007 &&
            client.recentBook().isEmpty() && client.presentationWork()==syncWork,
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
            window->property("syncData").value<QJSValue>().toVariant().toMap()["books"]==client.syncSummary()["books"],
        "live language switch retranslates cached sync labels and QML snapshot");
    require(client.setLanguage("ru"),"restore Russian after cached language check");
    require(!warnings,"presentation checks produce no QML warnings");
    history={};
}

int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);
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
    for (bool known:{false,true}) for (bool local:{false,true}) for (bool remote:{false,true}) {
        const auto expected=local!=remote ? (local ? SyncDecision::Upload : SyncDecision::ApplyRemote) :
            known && !local ? SyncDecision::Unchanged : SyncDecision::Conflict;
        require(decideSync(known,local,remote)==expected,"pure three-way decision matrix");
    }

    const QUrl endpoint(qEnvironmentVariable("BOOKORBIT_TEST_ENDPOINT"));
    require(endpoint.isValid() && !endpoint.host().isEmpty(),"isolated endpoint provided");
    const QString firstRunRoot=root+"/first-install/applications/bookorbit";
    require(!QFileInfo::exists(firstRunRoot),"first-install data directory is absent");
    Client firstRun(endpoint,firstRunRoot);
    require(QFileInfo(firstRunRoot).isDir() &&
            QFileInfo(firstRun.downloadDirectory()+"/records").isDir() && firstRun.status().isEmpty(),
            "first launch creates data directory and account records recursively");
    require(firstRun.setDiagnosticLogging(true),"first launch can save preferences");
    const auto savedPreferences=bytes(firstRunRoot+"/preferences.json");
    Client nextRun(endpoint,firstRunRoot);
    require(nextRun.diagnosticLogging() && bytes(firstRunRoot+"/preferences.json")==savedPreferences &&
            nextRun.status().isEmpty(),"existing data directory and preferences survive restart");
    const QString blockedRoot=root+"/blocked-data";
    mode(blockedRoot,"keep existing file");
    for (const QString &path:QStringList{blockedRoot,blockedRoot+"/bookorbit"}) {
        Client blocked(endpoint,path);
        blocked.restoreSession();
        require(blocked.status().contains("Не удалось создать папку данных приложения") &&
                blocked.status().contains(path) && blocked.feedback()["result"]=="error" &&
                bytes(blockedRoot)=="keep existing file",
                "failed directory creation reports its path and preserves blocking file");
    }
    Client client(endpoint,root+"/client");
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
    const QString firstCover=client.coverUrl(1),secondCover=client.coverUrl(2);
    mode(fault,"cover_changed");
    require(wait(client,[&] { client.refresh(); }),"refresh a changed cover");
    require(until([&] { return !client.coverUrl(1).isEmpty() && client.coverUrl(1)!=firstCover; }) &&
            client.coverUrl(2)==secondCover,"one changed cover leaves other source URLs unchanged");
    mode(fault,"");
    client.showDownloaded(true);
    position="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
    require(wait(client,[&] { client.syncFile(101); }),"establish agreed sync baseline");
    position="pbr:/webkit?##epubcfi(/6/2!/4/8/1)";
    const int before=posts(root);
    mode(fault,"audit_post401");
    require(!wait(client,[&] { client.syncFile(101); }) && client.authenticated() && posts(root)==before+1,
            "POST 401 renews session but never replays an old write");
    require(!wait(client,[&] { client.syncFile(101); }) && client.progressConflict() && posts(root)==before+1,
            "fresh sync detects progress changed on another device during token refresh");
    require(wait(client,[&] { client.resolveProgress(true); }),"explicit fresh choice resolves conflict");
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
    presentationChecks(app,russian,endpoint,root);
    std::puts("PASS: audit regressions");
    return 0;
}
