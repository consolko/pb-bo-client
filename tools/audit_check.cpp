// Regression checks use the real Client and QML, but never a native reader.
#include "client.h"
#include "device.h"
#include "progress.h"
#include "sync_decision.h"
#include "check_wait.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

QString position;
QString profile="default";
bool indexed=true;
bool connectNetwork() { return true; }
QString readerProfile() { return profile; }
bool readerPosition(const QString &,QString *value) { *value=position; return true; }
ReaderFileState readerFileState(const QString &) { return ReaderFileState::Closed; }
ReaderRecents readerRecents(const QStringList &) { return {profile,false,{}}; }
bool readerBookIndexed(const QString &) { return indexed; }
void scanBook(const QString &) {}
bool openReader(const QString &) { return true; }
bool saveReaderPosition(const QString &,const QString &expected,const QString &cfi,const QString &expectedProfile,QString *error) {
    if (expected!=position || expectedProfile!=profile) { *error="Changed fixture"; return false; }
    position="pbr:/webkit?##"+cfi;
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

int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);
    require(argc==3,"data directory and fault file supplied");
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
    std::puts("PASS: audit regressions");
    return 0;
}
