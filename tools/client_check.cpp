// Executes the real Client against the local fixture; never launches the reader.
#include "client.h"
#include "device.h"
#include "progress.h"
#include "check_wait.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <cstdio>
#include <cstdlib>

bool networkAvailable = true;
bool connectNetwork() { return networkAvailable; }
QString fakePosition;
QString fakeProfile="default";
QString readerProfile() { return fakeProfile; }
bool readerPosition(const QString &, QString *position) { *position=fakePosition; return true; }
bool historyReadable=true;
QMap<QString,ReaderRecent> fakeRecents;
ReaderRecents readerRecents(const QStringList &) { return {fakeProfile,historyReadable,fakeRecents}; }
QString appliedCfi;
bool applyAllowed=true;
bool saveReaderPosition(const QString &, const QString &expected, const QString &cfi, const QString &profile, QString *error) {
    *error="Тестовый отказ сохранения";
    if (!applyAllowed || expected!=fakePosition || profile!=fakeProfile) return false;
    appliedCfi=cfi; fakePosition="pbr:/webkit?##"+cfi; return true;
}
bool opened = false, indexed = true;
QString scanned;
bool openReader(const QString &) { opened = true; return true; }
void scanBook(const QString &path) { scanned = path; }
bool readerBookIndexed(const QString &) { return indexed; }
ReaderFileState fakeReader = ReaderFileState::Closed;
QString blockedPath;
ReaderFileState readerFileState(const QString &path) { return path==blockedPath ? ReaderFileState::Open : fakeReader; }
void require(bool value, const char *label) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
    std::printf("PASS: %s\n", label); std::fflush(stdout);
}
bool wait(Client &client, const std::function<void()> &action) {
    bool done = false;
    const bool ok = waitForClient(client, action, &done);
    require(done, "operation completed within timeout");
    return ok;
}
QByteArray contents(const QString &path) { QFile f(path); require(f.open(QIODevice::ReadOnly), "read test file"); return f.readAll(); }
void write(const QString &path, const QByteArray &data) { QFile f(path); require(f.open(QIODevice::WriteOnly) && f.write(data) == data.size(), "write test data"); }
int requests(const QString &root, const QString &route) {
    return QJsonDocument::fromJson(contents(root+"/requests.json")).object()[route].toInt();
}
int rssKiB() {
    QFile status("/proc/self/status");
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const auto &line : status.readAll().split('\n'))
        if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ').first().toInt();
    return -1;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc==4 && QString::fromLocal8Bit(argv[1])=="--resolve-cfi") {
        QString point; double percentage=0;
        require(epubPosition(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3]),&percentage,&point),
                "resolve supplied EPUB and CFI without mutation");
        std::printf("point=%s approximatePercentage=%.8f\n",qPrintable(point),percentage);
        return 0;
    }
    require(argc == 3, "data directory and fixture fault file supplied");
    const QString root = QString::fromLocal8Bit(argv[1]), fault = QString::fromLocal8Bit(argv[2]);
    const QUrl endpoint("http://host.containers.internal:8766");
    Client c(endpoint, root);
    write(fault, "");
    require(!c.configure("http://example.com", "demo"), "reject non-local HTTP");
    require(!c.configure("https://user:secret@example.com", "a"), "reject URL credentials");
    require(!c.configure("https://example.com?token=secret", "a"), "reject URL query");
    int configured = 0;
    const auto settingsConnection = QObject::connect(&c, &Client::completed, &c, [&](const QString &operation, bool) {
        if (operation == "settings") ++configured;
    });
    require(wait(c, [&] { c.login(c.server(), "demo", "demo"); }) && configured == 1, "login configures account exactly once");
    QObject::disconnect(settingsConnection);
    const QString sessionFile = root+"/"+QString::fromLatin1(QCryptographicHash::hash((endpoint.toString()+"\ndemo").toUtf8(), QCryptographicHash::Sha256).toHex().left(24))+"/session.json";
    require(c.hasSavedSession() && contents(sessionFile).contains("refreshToken") && !contents(sessionFile).contains("password"),
            "successful login saves refresh token without password");
    Client resumed(endpoint, root);
    int restoredSettings = 0;
    QObject::connect(&resumed, &Client::completed, &resumed, [&](const QString &operation, bool) {
        if (operation == "settings") ++restoredSettings;
    });
    require(resumed.hasSavedSession() && wait(resumed, [&] { resumed.login(resumed.server()+"/", "demo", ""); }) &&
            resumed.authenticated() && restoredSettings == 0,
            "restart restores the session without entering a password");
    Client pending(endpoint,root);
    const int refreshBefore = requests(root,"POST /api/v1/auth/refresh");
    require(!wait(pending,[&] { pending.login("https://other.example.test","demo",""); }) &&
            !wait(pending,[&] { pending.login(pending.server(),"other-user",""); }) &&
            pending.hasSavedSession() && !pending.authenticated() && pending.server()==endpoint.toString() &&
            requests(root,"POST /api/v1/auth/refresh")==refreshBefore, "blank password cannot restore another server or user");
    write(fault,"error");
    require(!wait(pending,[&] { pending.restoreSession(); }) && pending.hasSavedSession() && QFile::exists(sessionFile),
            "refresh 503 retains saved session for a later retry");
    write(fault,"");
    require(wait(pending,[&] { pending.restoreSession(); }) && pending.authenticated(), "saved session can be restored after transient refresh failure");
    write(sessionFile, QJsonDocument(QJsonObject{{"refreshToken", QString(64, '0')}, {"password", "legacy secret"}}).toJson());
    require(QFile::setPermissions(sessionFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther), "seed permissive legacy session");
    Client migrated(endpoint, root);
    require(migrated.hasSavedSession() && !contents(sessionFile).contains("password") &&
            !(QFileInfo(sessionFile).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)),
            "legacy password file is rewritten as private token-only session");
    const QString unprotectedRoot=root+"/session-storage-failure";
    const QString accountKey=QString::fromLatin1(QCryptographicHash::hash((endpoint.toString()+"\ndemo").toUtf8(),QCryptographicHash::Sha256).toHex().left(24));
    require(QDir().mkpath(unprotectedRoot+"/"+accountKey+"/session.json"), "inject session storage failure");
    Client unprotected(endpoint,unprotectedRoot,nullptr,false);
    require(wait(unprotected,[&] { unprotected.login(unprotected.server(), "demo","demo"); }) && unprotected.authenticated() &&
            !unprotected.hasSavedSession() && !unprotected.sessionWarning().isEmpty(),
            "unsafe session storage does not undo successful login");
    write(fault, "always401");
    require(!wait(unprotected, [&] { unprotected.refresh(); }) && !unprotected.authenticated() &&
            !unprotected.hasSavedSession() && !unprotected.sessionWarning().isEmpty(),
            "failed session deletion still invalidates in-memory state and leaves a warning");
    write(fault, "");
    require(wait(c, [&] { c.showDownloaded(false); }), "opening catalog fetches online");
    require(c.books().size() == 3, "catalog contains three books");
    const QString catalogPath = QFileInfo(sessionFile).absolutePath()+"/catalog.json";
    require(!QFile::exists(catalogPath), "online catalog is never saved to disk");
    write(catalogPath, "{\"items\":[{\"id\":999,\"title\":\"stale\"}],\"page\":0,\"total\":1}");
    Client noCache(endpoint, root);
    require(noCache.books().isEmpty() && noCache.status().isEmpty() && !QFile::exists(catalogPath),
            "restart discards obsolete catalog and duplicate sign-in prompt");
    c.showDownloaded(true);
    write(fault, "error");
    require(!wait(c, [&] { c.showDownloaded(false); }) && c.books().isEmpty(),
            "reopening catalog uses network and never falls back to stale books");
    write(fault, "");
    require(wait(c, [&] { c.retry(); }) && c.books().size() == 3, "catalog retry restores fresh online books");
    write(fault, "renew");
    require(wait(c, [&] { c.refresh(); }) && c.authenticated(), "catalog 401 renews session and retries once");
    require(!c.books()[1].toMap()["needsRepair"].toBool(), "new book offers first download, not repair");
    write(fault, "renew");
    require(wait(c, [&] { c.download(0); }) && c.authenticated(), "download 401 renews session and retries once");
    const auto file = c.localFile(0), original = QString::fromLatin1(QCryptographicHash::hash(contents(file), QCryptographicHash::Sha256).toHex());
    require(scanned == file, "completed download notifies native scanner");
    indexed = false; scanned.clear();
    require(!wait(c, [&] { c.open(0); }) && !opened && scanned == file, "unindexed book cannot open before native registration");
    indexed = true;
    require(wait(c, [&] { c.open(0); }) && opened, "indexed book can open");
    require(wait(c, [&] { c.download(1); }), "download second book");
    const auto scope = QFileInfo(file).absolutePath();
    require(QFile::exists(scope+"/records/101.json") && !QFile::exists(scope+"/downloads.json"), "per-book records replace shared registry");
    double percent=0;
    require(c.syncSummary()["total"].toInt()==2 && c.syncSummary()["synced"].toInt()==0, "downloaded EPUBs start unverified in the independent sync list");
    QVariantMap preview;
    require(epubPosition(file,"epubcfi(/6/2!/4/14/1:0)",nullptr,nullptr,&preview) &&
            preview["chapter"].toString()=="Тестовая книга 1" && preview["excerpt"].toString().startsWith("Книга 1, абзац 006"),
            "position preview resolves the exact paragraph and preceding heading");
    require(!epubPosition(file,"epubcfi(/6/2!/4/9999/1)",nullptr,nullptr,&preview) && preview.isEmpty(),
            "invalid coordinate clears preview instead of retaining stale text");
    require(epubPosition(file,"epubcfi(/6/2!/4/14/1:65)",&percent) && percent>0 && percent<100, "resolve CFI and estimate text percentage");
    require(!epubPosition(file,"epubcfi(/6/2!/4/9999/1)",nullptr) &&
            !epubPosition(file,"epubcfi(/6/2!/4/14/1:99999)",nullptr), "reject out-of-book positions");
    QString point;
    require(epubPosition(file,"epubcfi(/6/2!/4,/14/1:0,/16/1:1)",&percent,&point) &&
            point=="epubcfi(/6/2!/4/14/1:0)", "validate both range endpoints and select its start");
    require(epubPosition(file,"epubcfi(/6/2!/4/14/1,:0,:20)",nullptr,&point) &&
            point=="epubcfi(/6/2!/4/14/1:0)", "resolve offsets relative to shared text node");
    for (const auto *cfi : {"epubcfi(/6/2!/4,/14/1:0,/16/1:99999)",
                            "epubcfi(/6/2!/4,/16/1:1,/14/1:0)",
                            "epubcfi(/6/2!/4/14/1,:20,:10)",
                            "epubcfi(/6/2!/4,/14/1:0,/9999/1)",
                            "epubcfi(/6/2!/4,2,/8/1)",
                            "epubcfi(/6/2!/4,/14/1:0,)",
                            "epubcfi(/6/2!/4,/14/1:0,/16/1:1,/18)"})
        require(!epubPosition(file,QString::fromLatin1(cfi),nullptr), "reject invalid or reversed range without applying its start");
    require(epubPosition(root+"/compressed.epub","epubcfi(/6/2!/4/14/1:65)",&percent), "resolve deflated ZIP member with CRC verification");
    require(epubPosition(root+"/multi.epub","epubcfi(/6/4[second]!/4/14[p6]/1:65)",&percent) && percent>50,
            "resolve second spine and validate element ID assertions");
    require(!epubPosition(root+"/multi.epub","epubcfi(/6/4[wrong]!/4/14/1)",nullptr), "reject wrong CFI ID assertion");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/14/1:65)";
    const int syncDownloads=requests(root,"GET /api/v1/books/files/101/download")+requests(root,"GET /api/v1/books/files/102/download");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }), "upload native reading position to empty server");
    require(c.syncSummary()["synced"].toInt()==1, "only confirmed sync contributes to success count");
    Client statusRestart(endpoint,root);
    require(statusRestart.syncSummary()["synced"].toInt()==1, "confirmed status survives restart without a network session");
    networkAvailable=false;
    require(!wait(c,[&] { c.syncFile(101); }) && c.syncBooks()[0].toMap()["state"]=="network", "explicit network refusal becomes waiting state");
    networkAvailable=true;
    require(wait(c,[&] { c.syncFile(101); }), "retry restores the confirmed state");
    require(QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"].toObject()["localBase"]==nativeCfi(fakePosition), "persist agreed baseline");
    const int firstDownloads = requests(root, "GET /api/v1/books/files/101/download");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && requests(root, "GET /api/v1/books/files/101/download") == firstDownloads,
            "unchanged progress uses GET without downloading EPUB again");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
    const int beforeMovedDownload=requests(root,"GET /api/v1/books/files/101/download");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }), "sync deliberate backwards reading without percentage ordering");
    require(requests(root,"GET /api/v1/books/files/101/download")==beforeMovedDownload,
            "changed local position uploads without downloading EPUB");
    write(fault,"progress_remote");
    applyAllowed=false; opened=false;
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }), "native write failure is not reported as synchronization success");
    Client pendingRestart(endpoint,root);
    pendingRestart.showDownloaded(true);
    require(pendingRestart.books()[0].toMap()["pendingProgress"].toBool(), "incoming position survives restart");
    fakeReader=ReaderFileState::Open;
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && appliedCfi.isEmpty(), "never apply incoming position to an open reader");
    require(c.syncBooks()[0].toMap()["state"]=="reader", "open reader has an actionable status");
    fakeReader=ReaderFileState::Unknown;
    require(!wait(c,[&] { c.syncFile(101); }) && c.syncBooks()[0].toMap()["state"]=="reader_unknown", "unknown reader is not described as definitely open");
    fakeReader=ReaderFileState::Closed;
    applyAllowed=true;
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && appliedCfi=="epubcfi(/6/2!/4/62/1)" && !opened,
            "retry saves pending incoming CFI without opening any book");
    require(!c.books()[0].toMap()["pendingProgress"].toBool(), "confirmed native write clears pending state");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }), "acknowledge applied position without echo upload");
    write(fault,"progress_range");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && appliedCfi=="epubcfi(/6/2!/4/42/1:0)" && !opened,
            "incoming server range saves a point without launching reader");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) &&
            QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"].toObject()["remoteBase"].toObject()["cfi"].toString().contains(','),
            "repeat sync preserves original server range without echo upload");
    fakeProfile="range-first-sync";
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/6/1)";
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.progressConflict(), "first sync with local point and remote range requires explicit choice");
    require(wait(c,[&] { c.resolveProgress(false); }) && appliedCfi=="epubcfi(/6/2!/4/42/1:0)",
            "explicit range choice saves validated start");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/8/1)";
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && !c.progressConflict(), "reading after incoming range uploads against original server baseline");
    fakeProfile="default";
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }), "same positions acknowledge current default profile");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/10/1)";
    write(fault,"progress_other");
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.progressConflict(), "two changed sides create explicit conflict");
    const int conflictGets=requests(root,"GET /api/v1/books/files/101/progress");
    c.dismissConflict(); networkAvailable=false;
    c.inspectConflict(101);
    require(c.progressConflict() && c.conflictPositions().size()==2 && !c.conflictPositions()[0].toMap()["excerpt"].toString().isEmpty() &&
            requests(root,"GET /api/v1/books/files/101/progress")==conflictGets, "saved conflict opens offline with local text context and no GET");
    networkAvailable=true;
    const int revision=c.conflictRevision(), postsBeforeChoice=requests(root,"POST /api/v1/books/files/101/progress");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/12/1)";
    require(!wait(c,[&] { c.resolveProgress(true); }) && c.progressConflict() && c.conflictRevision()>revision &&
            requests(root,"POST /api/v1/books/files/101/progress")==postsBeforeChoice, "changed explicit choice refreshes both variants without an automatic write");
    require(wait(c,[&] { c.resolveProgress(true); }) && !c.progressConflict(), "explicit local conflict choice rechecks and uploads");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/12/1)";
    write(fault,"progress_metadata");
    require(wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && !c.progressConflict(), "server timestamp alone is not a competing reading position");
    fakeProfile="another-profile";
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/16/1)";
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.progressConflict(), "another native profile cannot reuse an agreed baseline");
    require(wait(c,[&] { c.resolveProgress(false); }) && !opened, "explicit server choice saves without launching reader");
    fakeProfile="default";
    blockedPath=file;
    require(!wait(c,[&] { c.syncAll(); }) && !c.progressConflict(), "batch continues after an open book without showing a blocking dialog");
    require(!c.books()[0].toMap()["syncResult"].toString().isEmpty() &&
            c.books()[1].toMap()["syncResult"].toString().contains("BookOrbit"), "batch exposes an independent result for each book");
    blockedPath.clear();
    require(wait(c,[&] { c.syncAll(); }), "batch retry reconciles both downloaded books");
    const QString untouched=QString::fromUtf8(contents(scope+"/records/102.json"));
    int otherGets=requests(root,"GET /api/v1/books/files/102/progress");
    write(fault,"slow_progress");
    require(!wait(c,[&] {
        c.syncAll(); QTimer::singleShot(100,&c,[&] { c.stopSyncAfterCurrent(); });
    }) && c.syncBatch()["completed"].toInt()==1 && !c.busy() &&
        requests(root,"GET /api/v1/books/files/102/progress")==otherGets &&
        QString::fromUtf8(contents(scope+"/records/102.json"))==untouched,
        "stop during GET finishes one file and preserves every unprocessed record");
    write(fault,"");
    auto stopBetween=QObject::connect(&c,&Client::changed,&c,[&] {
        if (c.syncBatch()["running"].toBool() && c.syncBatch()["completed"].toInt()==1) c.stopSyncAfterCurrent();
    });
    require(!wait(c,[&] { c.syncAll(); }) && requests(root,"GET /api/v1/books/files/102/progress")==otherGets,
        "stop at the queued transition never starts the next file");
    QObject::disconnect(stopBetween);
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/20/1)";
    const int posts=requests(root,"POST /api/v1/books/files/101/progress");
    write(fault,"slow_confirm");
    require(!wait(c,[&] {
        c.syncAll(); QTimer::singleShot(100,&c,[&] { c.stopSyncAfterCurrent(); });
    }) && requests(root,"POST /api/v1/books/files/101/progress")==posts+1 &&
        requests(root,"GET /api/v1/books/files/102/progress")==otherGets &&
        !QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"].toObject().contains("outgoing"),
        "stop after POST still confirms the current file before returning");
    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/22/1)";
    write(fault,"uncertain_post");
    require(!wait(c,[&] {
        c.syncAll(); QTimer::singleShot(0,&c,[&] { c.stopSyncAfterCurrent(); });
    }) && QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"].toObject().contains("outgoing") &&
        c.syncBooks()[0].toMap()["state"]=="uncertain", "unknown send outcome remains journaled after stopping");
    write(fault,"");
    require(wait(c,[&] { c.syncAll(); }), "a fresh batch reconciles a stopped uncertain send");
    auto stopLast=QObject::connect(&c,&Client::changed,&c,[&] {
        if (c.syncBatch()["running"].toBool() && c.syncBatch()["completed"].toInt()==2) c.stopSyncAfterCurrent();
    });
    require(!wait(c,[&] { c.syncAll(); }) && c.syncBatch()["completed"].toInt()==2 && c.status().contains("пользователем"),
        "stop on the last file keeps its confirmed result and reports the requested stop");
    QObject::disconnect(stopLast);
    otherGets=requests(root,"GET /api/v1/books/files/102/progress"); write(fault,"relogin");
    require(!wait(c,[&] { c.syncAll(); }) && !c.authenticated() && requests(root,"GET /api/v1/books/files/102/progress")==otherGets &&
        c.status().contains("войдите снова"),"authorization loss stops the sync queue without starting the next file");
    require(wait(c,[&] { c.login(c.server(),"demo","demo"); }),"restore session after sync authorization stop");
    c.setUiContext("sync"); require(wait(c,[&] { c.syncAll(); }),"prepare successful sync feedback");
    require(c.feedback()["context"]=="sync" && !c.feedback()["text"].toString().isEmpty(),"structured feedback carries its operation context");
    c.setUiContext("connection"); require(c.feedback()["text"].toString().isEmpty(),"successful sync feedback clears on leaving its context");


    fakePosition="pbr:/webkit?##epubcfi(/6/2!/4/18/1)";
    const int beforePagePosts = requests(root, "POST /api/v1/books/files/101/progress");
    write(fault,"progress_page");
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.status().contains("страниц") &&
            requests(root, "POST /api/v1/books/files/101/progress") == beforePagePosts,
            "server page number blocks outgoing POST and remains intact on retry");
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.status().contains("страниц") &&
            requests(root, "POST /api/v1/books/files/101/progress") == beforePagePosts,
            "server page number survives blocked upload");
    write(fault,"progress_page_conflict");
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.progressConflict(), "page-number record can still require explicit conflict choice");
    require(!wait(c,[&] { c.resolveProgress(true); }) && c.status().contains("страниц") &&
            requests(root, "POST /api/v1/books/files/101/progress") == beforePagePosts,
            "explicit local choice cannot erase remote page number");
    write(fault,"changed");
    const int beforeChangedPosts=requests(root,"POST /api/v1/books/files/101/progress");
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.progressConflict(), "ordinary sync trusts downloaded identity until a full check");
    require(requests(root,"GET /api/v1/books/files/101/download")+requests(root,"GET /api/v1/books/files/102/download")==syncDownloads,
            "first, incoming, changed, conflict and batch progress checks never download books");
    const QByteArray beforeCheck=contents(scope+"/records/101.json"), beforeFile=contents(file);
    const auto beforeRecord=QJsonDocument::fromJson(beforeCheck).object();
    const QString beforeApplied=appliedCfi;
    const int beforeProgressGets=requests(root,"GET /api/v1/books/files/101/progress")+requests(root,"GET /api/v1/books/files/102/progress");
    require(!wait(c,[&] { c.verifyLibrary(); }) && c.status().contains("совпало: 1; отличается: 1; ошибок: 0"), "full check counts files and detects changed server content");
    auto checkedRecord=QJsonDocument::fromJson(contents(scope+"/records/101.json")).object();
    require(checkedRecord["remoteFileChanged"].toBool() && checkedRecord["progress"]==beforeRecord["progress"] &&
            checkedRecord["sha256"]==beforeRecord["sha256"] && contents(file)==beforeFile && appliedCfi==beforeApplied &&
            requests(root,"GET /api/v1/books/files/101/progress")+requests(root,"GET /api/v1/books/files/102/progress")==beforeProgressGets &&
            requests(root,"POST /api/v1/books/files/101/progress")==beforeChangedPosts,
            "verification preserves books and progress and never calls progress API");
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && c.status().contains("приостановлен"), "known mismatch blocks progress before network access");
    Client mismatchRestart(endpoint,root);
    require(mismatchRestart.books()[0].toMap()["remoteFileChanged"].toBool() &&
            wait(mismatchRestart,[&] { mismatchRestart.restoreSession(); }), "mismatch survives restart and login");
    opened=false;
    require(wait(mismatchRestart,[&] { mismatchRestart.open(0); }) && opened, "known server mismatch still permits local reading");
    require(!wait(mismatchRestart,[&] { mismatchRestart.syncFile(mismatchRestart.books().first().toMap()["fileId"].toInt()); }) && mismatchRestart.status().contains("приостановлен"), "restarted client blocks incoming and outgoing progress for mismatched file");
    write(fault,"");
    require(wait(c,[&] { c.verifyLibrary(); }) && !c.books()[0].toMap()["remoteFileChanged"].toBool() &&
            c.status().contains("совпало: 2"), "matching full check clears durable mismatch");
    write(fault,"changed");
    require(!wait(c,[&] { c.verifyLibrary(); }), "mark mismatch for explicit download repair");
    write(fault,"");
    require(wait(c,[&] { c.download(0); }) && c.localFile(0)==file &&
            !c.books()[0].toMap()["remoteFileChanged"].toBool() &&
            QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"]==beforeRecord["progress"],
            "already-downloaded early return clears mismatch and preserves progress");
    for (const auto mode : {"missing_remote","truncated","html_error"}) {
        write(fault,mode);
        require(!wait(c,[&] { c.verifyLibrary(); }) && !c.books()[0].toMap()["remoteFileChanged"].toBool(), "HTTP and incomplete or invalid responses are errors, not mismatches");
        require(!c.books()[1].toMap()["syncResult"].toString().isEmpty(), "verification continues to next file after errors");
    }
    write(fault,"");
    const int missingDownloads=requests(root,"GET /api/v1/books/files/101/download");
    require(QFile::rename(file,file+".saved"), "temporarily remove local book");
    require(!wait(c,[&] { c.verifyLibrary(); }) && requests(root,"GET /api/v1/books/files/101/download")==missingDownloads,
            "missing local file is reported without remote download");
    require(QFile::rename(file+".saved",file), "restore local book");
    auto corrupt=beforeFile; corrupt[corrupt.size()-1]=char(corrupt.at(corrupt.size()-1)^1); write(file,corrupt);
    require(!wait(c,[&] { c.verifyLibrary(); }) && requests(root,"GET /api/v1/books/files/101/download")==missingDownloads,
            "same-size local corruption is reported without remote download");
    write(file,beforeFile);
    const QString verificationRecordPath=scope+"/records/101.json";
    require(QFile::rename(verificationRecordPath,verificationRecordPath+".saved") && QDir().mkdir(verificationRecordPath), "inject verification record save failure");
    write(fault,"changed");
    require(!wait(c,[&] { c.verifyLibrary(); }) && c.status().contains("ошибок: 1") &&
            c.books()[0].toMap()["remoteFileChanged"].toBool(), "failed mismatch persistence reports error and blocks in-memory progress");
    require(QDir().rmdir(verificationRecordPath) && QFile::rename(verificationRecordPath+".saved",verificationRecordPath), "restore verification record storage");
    write(fault,"");
    require(wait(c,[&] { c.verifyLibrary(); }), "verification recovers after storage failure");
    for (const auto mode : {"slow","slow_refresh"}) {
        write(fault,mode);
        const auto previousResult=c.books()[0].toMap()["syncResult"];
        const int nextDownloads=requests(root,"GET /api/v1/books/files/102/download");
        require(!wait(c,[&] { c.verifyLibrary(); QTimer::singleShot(150,&c,&Client::cancelLibraryVerification); }) &&
                !c.busy() && !c.verifyingLibrary() && c.status().contains("не проверено: 2") &&
                c.books()[0].toMap()["syncResult"]==previousResult &&
                requests(root,"GET /api/v1/books/files/102/download")==nextDownloads,
                "cancel transfer or token refresh leaves current and remaining files unchecked");
        QEventLoop settle; QTimer::singleShot(600,&settle,&QEventLoop::quit); settle.exec();
    }
    write(fault,"renew");
    require(wait(c,[&] { c.verifyLibrary(); }) && c.authenticated(), "full verification renews token and resumes the same file");
    write(fault,"relogin");
    const int authNextDownloads=requests(root,"GET /api/v1/books/files/102/download");
    require(!wait(c,[&] { c.verifyLibrary(); }) && !c.authenticated() && c.status().contains("не проверено: 1") &&
            requests(root,"GET /api/v1/books/files/102/download")==authNextDownloads, "lost authorization stops remaining verification queue");
    require(wait(c,[&] { c.login(c.server(), "demo","demo"); }), "restore synthetic login after verification auth failure");
    write(fault,"");
    networkAvailable=false;
    require(!wait(c,[&] { c.syncFile(c.books().first().toMap()["fileId"].toInt()); }) && !c.busy(), "network failure leaves durable progress available for retry");
    networkAvailable=true;
    require(c.setDiagnosticLogging(true) && c.diagnosticLogPath()==root+"/diagnostic.log", "diagnostics stay in application directory");
    fakePosition.clear();
    fakeReader = ReaderFileState::Open;
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == file, "open reader blocks replacement");
    fakeReader = ReaderFileState::Unknown;
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == file, "unknown reader state blocks replacement");
    fakeReader = ReaderFileState::Closed;
    QEventLoop covers;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &covers, [&] { if (!c.coverUrl(1).isEmpty() && !c.coverUrl(2).isEmpty()) covers.quit(); });
    poll.start(20); QTimer::singleShot(10000, &covers, &QEventLoop::quit); covers.exec();
    require(!c.coverUrl(1).isEmpty() && !c.coverUrl(2).isEmpty() && c.coverUrl(3).isEmpty(), "covers cached; absent cover stays empty");
    const int cachedCoverRequests=requests(root,"GET /api/v1/books/1/thumbnail");
    require(wait(c,[&] { c.refresh(1); }) && wait(c,[&] { c.refresh(0); }) &&
            requests(root,"GET /api/v1/books/1/thumbnail")==cachedCoverRequests,
            "catalog page change reuses versioned cover without a second GET");
    write(fault,"cover_unknown");
    require(wait(c,[&] { c.refresh(0); }), "catalog with unknown cover version");
    QEventLoop unknownCover;
    QTimer unknownPoll;
    QObject::connect(&unknownPoll,&QTimer::timeout,&unknownCover,[&] {
        if (requests(root,"GET /api/v1/books/1/thumbnail")>cachedCoverRequests) unknownCover.quit();
    });
    unknownPoll.start(20); QTimer::singleShot(10000,&unknownCover,&QEventLoop::quit); unknownCover.exec();
    require(requests(root,"GET /api/v1/books/1/thumbnail")>cachedCoverRequests,
            "unknown cover version triggers online recheck");
    write(fault, "cover_removed");
    require(wait(c, [&] { c.refresh(0); }) && c.coverUrl(1).isEmpty() && !QFile::exists(scope+"/cover-1.png"), "removed cover deletes cached image");
    write(fault, "cover_changed");
    require(wait(c, [&] { c.refresh(0); }), "changed cover catalog");
    QEventLoop changedCover;
    QTimer changedPoll;
    QObject::connect(&changedPoll, &QTimer::timeout, &changedCover, [&] { if (!c.coverUrl(1).isEmpty()) changedCover.quit(); });
    changedPoll.start(20); QTimer::singleShot(10000, &changedCover, &QEventLoop::quit); changedCover.exec();
    require(!c.coverUrl(1).isEmpty(), "changed cover fetched after invalidation");
    const int before404=requests(root,"GET /api/v1/books/1/thumbnail");
    write(fault,"cover_404");
    require(wait(c,[&] { c.refresh(0); }), "catalog still advertises unavailable cover");
    QEventLoop missingCover;
    QTimer missingPoll;
    QObject::connect(&missingPoll,&QTimer::timeout,&missingCover,[&] { if (c.coverUrl(1).isEmpty()) missingCover.quit(); });
    missingPoll.start(20); QTimer::singleShot(10000,&missingCover,&QEventLoop::quit); missingCover.exec();
    require(c.coverUrl(1).isEmpty() && !QFile::exists(scope+"/cover-1.png") &&
            requests(root,"GET /api/v1/books/1/thumbnail")>before404,
            "thumbnail 404 deletes cached cover");
    write(fault, "");
    require(wait(c, [&] { c.refresh(0, "no such title"); }), "empty search");
    require(c.books().isEmpty(), "empty catalog shown");
    c.showDownloaded(true);
    require(c.books().size() == 2 && !c.localFile(0).isEmpty(), "downloads independent of search");
    write(scope+"/cover-999.png", "stale");
    write(scope+"/cover-other.png", "keep");
    require(wait(c, [&] { c.refresh(0, "no such title"); }) &&
            !QFile::exists(scope+"/cover-999.png") && QFile::exists(scope+"/cover-other.png"), "cover cleanup touches only owned unreferenced names");
    c.showDownloaded(true);
    write(fault, "slow");
    require(!wait(c, [&] { c.download(0); QTimer::singleShot(200, &c, [&] { c.cancelDownload(); }); }), "cancel active download");
    require(QString::fromLatin1(QCryptographicHash::hash(contents(file), QCryptographicHash::Sha256).toHex()) == original, "cancel preserves previous EPUB");
    fakeReader = ReaderFileState::Closed;
    require(!wait(c, [&] { c.download(0); QTimer::singleShot(200, &c, [] { fakeReader = ReaderFileState::Open; }); }) &&
            c.localFile(0) == file, "reader opening during transfer blocks promotion");
    fakeReader = ReaderFileState::Closed;
    write(fault, "truncated");
    require(!wait(c, [&] { c.download(0); }) && c.canRetry(), "failed download offers retry");
    write(fault, "");
    require(wait(c, [&] { c.retry(); }), "retry succeeds");
    require(c.localFile(0) == file, "identical redownload keeps file identity");
    const auto progressBeforeRepair = QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"].toObject();
    write(file, QByteArray(contents(file).size(), 'x'));
    c.open(0);
    require(c.localFile(0).isEmpty() && c.books().size() == 2 && c.books()[0].toMap()["needsRepair"].toBool(), "corruption preserves library entry with repair action");
    require(wait(c, [&] { c.download(0); }), "redownload repairs corruption");
    const auto repaired = c.localFile(0);
    require(!repaired.isEmpty() && repaired != file, "repair switches to a new EPUB path");
    require(QJsonDocument::fromJson(contents(scope+"/records/101.json")).object()["progress"].toObject()==progressBeforeRepair,
            "same-hash repair retains durable progress journal");
    const auto recordPath = scope+"/records/101.json";
    require(QFile::rename(recordPath,recordPath+".saved") && QDir().mkdir(recordPath),
            "inject record failure before changed-content replacement");
    write(fault, "changed");
    require(!wait(c,[&] { c.download(0); }) && c.localFile(0)==repaired,
            "failed changed-content record write keeps previous EPUB");
    require(QDir().rmdir(recordPath) && QFile::rename(recordPath+".saved",recordPath),
            "restore record before restart checkpoint");
    Client beforeReplacement(endpoint,root);
    require(QJsonDocument::fromJson(contents(recordPath)).object()["progress"].toObject()==progressBeforeRepair &&
            !beforeReplacement.localFile(0).isEmpty(),
            "restart between failed and completed replacement keeps same-EPUB progress");
    require(wait(c, [&] { c.download(0); }) && c.localFile(0) != repaired, "changed EPUB under same ID gets a new path");
    const auto changedFile = c.localFile(0);
    require(!QJsonDocument::fromJson(contents(scope+"/records/101.json")).object().contains("progress"),
            "changed EPUB starts without previous progress journal");
    require(QFile::exists(changedFile) && !QFile::exists(repaired), "old closed version cleaned after switch");
    require(QFile::rename(recordPath, recordPath+".saved") && QDir().mkdir(recordPath), "inject metadata write failure");
    write(fault, "");
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == changedFile, "failed metadata write preserves current EPUB");
    require(QDir().rmdir(recordPath) && QFile::rename(recordPath+".saved", recordPath), "restore metadata record");
    Client afterFailure(endpoint, root);
    afterFailure.showDownloaded(true);
    require(!afterFailure.localFile(0).isEmpty(), "restart after metadata failure keeps previous book");
    const QString orphan = scope+"/101-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".epub";
    write(orphan, "orphan");
    const QString orphanPart = scope+"/101-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".part";
    write(orphanPart, "partial");
    write(orphanPart+".ABC123", "partial temporary");
    const QString firstOrphan = scope+"/999-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".epub";
    const QString pdfOrphan = scope+"/998-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".pdf";
    const QString legacyOrphan = scope+"/997.epub";
    write(firstOrphan, "first interrupted download"); write(pdfOrphan, "%PDF-old"); write(legacyOrphan, "unproven legacy file");
    Client afterCrash(endpoint, root);
    require(!QFile::exists(orphan) && !QFile::exists(orphanPart) && !QFile::exists(orphanPart+".ABC123"),
            "restart removes only owned unreferenced versions and partial downloads");
    require(!QFile::exists(firstOrphan) && !QFile::exists(pdfOrphan) && QFile::exists(legacyOrphan),
            "cleanup removes first-download and non-EPUB UUID orphans but preserves unproven legacy files");
    require(QFile::rename(recordPath, recordPath+".saved"), "save record before corruption test");
    write(recordPath, "{");
    Client badRecord(endpoint, root);
    require(QFile::exists(changedFile), "invalid record never deletes an EPUB");
    auto incomplete = QJsonDocument::fromJson(contents(recordPath+".saved")).object();
    incomplete.remove("sha256");
    write(recordPath, QJsonDocument(incomplete).toJson());
    Client incompleteRecord(endpoint, root);
    require(QFile::exists(changedFile), "incomplete record never deletes an EPUB");
    require(QFile::remove(recordPath) && QFile::rename(recordPath+".saved", recordPath), "restore record after corruption test");
    const QString transfers = scope+"/transfers";
    require(QDir().rename(transfers, transfers+".saved"), "save transfer directory before failure test");
    write(transfers, "blocked"); write(fault, "");
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == changedFile,
            "journal persistence failure keeps previous book and blocks promotion");
    require(QFile::remove(transfers) && QDir().rename(transfers+".saved", transfers), "restore transfer directory");
    write(fault, "expired");
    require(!wait(c, [&] { c.download(0); }) && !c.authenticated(), "download 401 expires session");
    write(fault, "");
    require(wait(c, [&] { c.login(c.server(), "demo", "demo"); }), "login after expiry");
    write(fault, "error");
    require(!wait(c, [&] { c.refresh(0, ""); }) && c.canRetry() && c.books().size()==3, "catalog outage preserves previous page and offers retry");
    write(fault, "");
    require(wait(c, [&] { c.retry(); }), "catalog retry succeeds");
    require(wait(c, [&] { c.logout(); }), "server logout completes");
    require(!c.authenticated() && !c.hasSavedSession() && !QFile::exists(sessionFile) && c.books().size() == 2,
            "logout clears saved credentials and keeps downloads");
    require(wait(c, [&] { c.login(c.server(), "demo", "demo"); }), "new session after logout");
    write(fault, "always401");
    require(!wait(c, [&] { c.refresh(); }) && !c.authenticated(), "persistent 401 stops after one renewal");
    write(fault, "");
    require(wait(c, [&] { c.login(c.server(), "demo", "demo"); }), "new session after rejected renewal");
    for (int operation = 0; operation < 3; ++operation) {
        const int renewals = requests(root, "POST /api/v1/auth/refresh");
        write(fault, "always401");
        require(!wait(c, [&] {
            if (operation == 0) c.refresh();
            else if (operation == 1) c.download(0);
            else c.verifyLibrary();
        }) && !c.authenticated() && !c.hasSavedSession() && !QFile::exists(sessionFile) &&
            requests(root, "POST /api/v1/auth/refresh") == renewals+1,
            "final 401 clears stored session after one successful renewal for every request path");
        write(fault, "");
        require(wait(c, [&] { c.login(c.server(), "demo", "demo"); }), "manual login works after final 401");
    }
    write(fault, "bad_auth");
    require(!wait(c, [&] { c.login(c.server(), "demo", "demo"); }) && !c.authenticated() &&
            !c.hasSavedSession() && !QFile::exists(sessionFile), "invalid credentials response removes stale saved session");
    write(fault, "");
    require(wait(c, [&] { c.login(c.server(), "demo", "demo"); }), "manual login works after malformed credentials");
    QEventLoop finalCover;
    auto coverConnection = QObject::connect(&c, &Client::coversChanged, &finalCover, [&] {
        if (!c.coverUrl(1).isEmpty()) finalCover.quit();
    });
    QTimer::singleShot(10000, &finalCover, &QEventLoop::quit);
    if (c.coverUrl(1).isEmpty()) finalCover.exec();
    QObject::disconnect(coverConnection);
    require(!c.coverUrl(1).isEmpty(), "cover finishes before testing logout persistence");
    write(fault, "error");
    require(!wait(c, [&] { c.logout(); }) && !c.authenticated() && c.status().contains("не подтверждён"), "failed server logout still clears local credentials");
    write(fault, "");
    require(c.configure("https://books.example.test/", "alice"), "save non-demo account without network");
    require(c.books().isEmpty() && c.coverUrl(1).isEmpty(), "account isolation includes covers");
    Client restarted(endpoint, root);
    require(restarted.username() == "alice" && restarted.server() == "https://books.example.test" && !restarted.authenticated(), "restart restores selected account without token");
    restarted.selectAccount(0);
    require(restarted.books().size() == 2 && !restarted.localFile(0).isEmpty() && !restarted.coverUrl(1).isEmpty(), "offline account selection restores books and covers");
    const auto settings = contents(root+"/accounts.json");
    require(!settings.contains("password") && !settings.contains("Token") && !settings.contains("local-development-only"), "settings contain no credentials");
    // A partial migration keeps the old registry and resumes on the next launch.
    const QString legacyRoot = root+"/legacy-check";
    const QString legacyScope = legacyRoot+"/"+QFileInfo(scope).fileName();
    require(QDir().mkpath(legacyScope+"/records"), "create isolated legacy account");
    QJsonObject legacyRecords;
    for (int id : {101, 102}) {
        const auto saved = QJsonDocument::fromJson(contents(scope+"/records/"+QString::number(id)+".json")).object();
        auto old = saved;
        old.remove("filename");
        if (id == 102) old.remove("book");
        legacyRecords[QString::number(id)] = old;
        require(QFile::copy(scope+"/"+saved["filename"].toString(), legacyScope+"/"+QString::number(id)+".epub"), "copy legacy EPUB");
    }
    write(legacyScope+"/downloads.json", QJsonDocument(legacyRecords).toJson());
    require(QDir().mkdir(legacyScope+"/records/102.json"), "inject interrupted migration");
    write(legacyRoot+"/accounts.json", QJsonDocument(QJsonObject{{"accounts", QJsonArray{QJsonObject{{"server",endpoint.toString()},{"username","demo"}}}},{"selected",0}}).toJson());
    Client partial(endpoint, legacyRoot);
    partial.showDownloaded(true);
    require(partial.books().size() == 2 && !partial.localFile(0).isEmpty() && QFile::exists(legacyScope+"/downloads.json"), "partial migration keeps legacy data");
    require(QDir().rmdir(legacyScope+"/records/102.json"), "clear migration fault");
    require(QDir().mkdir(legacyScope+"/downloads-v1.json"), "inject backup rename failure");
    const QString migrationOrphan = legacyScope+"/999-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".epub";
    write(migrationOrphan,"defer cleanup until migration backup succeeds");
    Client beforeBackup(endpoint, legacyRoot);
    beforeBackup.showDownloaded(true);
    require(beforeBackup.books().size() == 2 && QFile::exists(legacyScope+"/records/101.json") &&
            QFile::exists(legacyScope+"/records/102.json") && QFile::exists(legacyScope+"/downloads.json"),
            "restart after all records written keeps old registry until backup succeeds");
    require(QFile::exists(migrationOrphan), "failed migration backup blocks cleanup");
    require(QDir().rmdir(legacyScope+"/downloads-v1.json"), "clear backup rename fault");
    Client legacy(endpoint, legacyRoot);
    legacy.showDownloaded(true);
    require(legacy.books().size() == 2 && !legacy.localFile(0).isEmpty() &&
            QFile::exists(legacyScope+"/downloads-v1.json") && !QFile::exists(legacyScope+"/downloads.json"), "migration resumes and keeps backup");
    require(!QFile::exists(migrationOrphan), "completed migration permits owned orphan cleanup");
    const QString manyRoot = root+"/many-check";
    const QString manyScope = manyRoot+"/"+QFileInfo(scope).fileName();
    require(QDir().mkpath(manyScope+"/records"), "create large isolated library");
    qint64 aggregate = 0;
    bool allWrote = true;
    for (int id=1000; id<2200; ++id) {
        const QJsonObject book{{"id", id}, {"title", QString(2000, 'T')},
                               {"selectedFile", QJsonObject{{"id", id}, {"sizeBytes", QJsonValue::Null}}}};
        const QJsonObject record{{"bookId", id}, {"fileId", id}, {"filename", QString::number(id)+".epub"},
                                 {"sha256", QString(64, 'a')}, {"bytes", 1}, {"book", book}};
        const QString name = manyScope+"/records/"+QString::number(id)+".json";
        const auto data = QJsonDocument(record).toJson();
        QFile output(name);
        if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size()) { allWrote = false; break; }
        output.close();
        aggregate += QFileInfo(name).size();
    }
    require(allWrote, "write large library records");
    require(aggregate > 2*1024*1024, "per-book records exceed old aggregate limit");
    write(manyRoot+"/accounts.json", QJsonDocument(QJsonObject{{"accounts", QJsonArray{QJsonObject{{"server",endpoint.toString()},{"username","demo"}}}},{"selected",0}}).toJson());
    Client many(endpoint, manyRoot);
    many.showDownloaded(true);
    require(many.books().size() == 1200, "large downloaded list remains available");
    const QString searchRoot=root+"/search-check",searchScope=searchRoot+"/"+QFileInfo(scope).fileName();
    require(QDir().mkpath(searchScope+"/records"),"create isolated search library");
    const QString searchEpub=root+"/compressed.epub";
    const QString searchDigest=QString::fromLatin1(QCryptographicHash::hash(contents(searchEpub),QCryptographicHash::Sha256).toHex());
    for (int i=1;i<=100;++i) {
        const int id=3000+i;
        const QJsonObject variant{{"id",id},{"format","epub"},{"role","content"},{"sizeBytes",QFileInfo(searchEpub).size()}};
        const QJsonObject book{{"id",i},{"title",i==1 ? "Ёлка C++ % ? e\u0301" : QString("Книга %1").arg(i,3,10,QChar('0'))},
            {"authors",QJsonArray{QJsonObject{{"name",i==2 ? "Анна ТЕСТ" : "Автор"}}}},
            {"seriesName",i==3 ? "Лунная СЕРИЯ" : ""},{"files",QJsonArray{variant}},{"selectedFile",variant}};
        require(QFile::copy(searchEpub,searchScope+"/"+QString::number(id)+".epub"),"copy controlled local EPUB");
        write(searchScope+"/records/"+QString::number(id)+".json",QJsonDocument(QJsonObject{{"fileId",id},{"bookId",i},
            {"bytes",QFileInfo(searchEpub).size()},{"sha256",searchDigest},{"book",book}}).toJson());
    }
    write(searchRoot+"/accounts.json",contents(root+"/accounts.json"));
    Client searchClient(endpoint,searchRoot); searchClient.showDownloaded(true);
    const int beforeLocal=requests(root,"POST /api/v1/books/query");
    for (const auto &query:QStringList{"ёлКа","C++","%","?","é"}) {
        searchClient.searchDownloaded(query); require(searchClient.books().size()==1,"Unicode and literal punctuation search");
    }
    searchClient.searchDownloaded("анна тест"); require(searchClient.books().size()==1,"local author search folds Cyrillic case");
    searchClient.searchDownloaded("лунная серия"); require(searchClient.books().size()==1,"local series search");
    searchClient.searchDownloaded("missing"); require(searchClient.books().isEmpty(),"empty local search result");
    searchClient.searchDownloaded(""); searchClient.setLocalSort("author");
    require(searchClient.books().size()==100 && requests(root,"POST /api/v1/books/query")==beforeLocal,"100-book search and sort never request network");
    opened=false; networkAvailable=false;
    require(wait(searchClient,[&] { searchClient.openFile(3001); }) && opened,"stable file ID opens after sorting without network");
    networkAvailable=true;
    searchClient.setDiagnosticLogging(true); searchClient.setLocalSort("title");
    Client searchRestart(endpoint,searchRoot);
    require(searchRestart.localSort()=="title" && searchRestart.diagnosticLogging(),"sort persists without overwriting other settings");
    searchClient.searchDownloaded("Ёлка"); searchClient.showDownloaded(true);
    require(searchClient.localQuery()=="Ёлка","query survives tab transitions within session");
    searchClient.configure(endpoint.toString(),"other");
    require(searchClient.localQuery().isEmpty() && searchClient.recentBook().isEmpty() && searchClient.books().isEmpty(),"account switch resets query and native recents");
    const QString folderRoot = root+"/folder-check";
    const QString destination = "/mnt/ext1/books/folder-check-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString first = destination+"/Первая папка", second = destination+"/Вторая папка";
    require(QDir().mkpath(first) && QDir().mkpath(second), "create download destinations with Cyrillic and spaces");
    const QString publicRoot = "/mnt/ext1/books/default-check-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    Client defaultLocation(endpoint, publicRoot, nullptr, false);
    require(defaultLocation.downloadDirectory() == publicRoot, "device data folder is the readable default destination");
    const QString appRoot = "/mnt/ext1/applications/bookorbit-check-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    Client appLocation(endpoint, appRoot, nullptr, false);
    require(appLocation.downloadDirectory() == "/mnt/ext1/books" &&
            appLocation.configure("https://books.example.test", "demo") &&
            appLocation.setDownloadDirectory(first) &&
            QFile::exists(appRoot+"/accounts.json") && QFile::exists(appRoot+"/preferences.json"),
            "application settings are saved outside the book directory");
    Client reopenedApp(endpoint, appRoot);
    require(reopenedApp.downloadDirectory() == first && reopenedApp.accounts().size() == 1,
            "application settings survive restart");
    Client folders(endpoint, folderRoot, nullptr, false);
    require(folders.setDiagnosticLogging(true) && folders.diagnosticLogging() &&
            contents(folders.diagnosticLogPath()).contains("diagnostics enabled"), "diagnostic logging is opt-in");
    require(wait(folders, [&] { folders.login(folders.server(), "demo", "demo"); }), "folder check login");
    require(wait(folders, [&] { folders.download(0); }), "download before choosing folder");
    const QString beforeChoice = folders.localFile(0);
    require(!folders.setDownloadDirectory("/mnt/ext1/system") &&
            !folders.setDownloadDirectory("/mnt/ext1/applications") &&
            !folders.setDownloadDirectory("/mnt/ext1/books/../system") &&
            !folders.setDownloadDirectory("relative") &&
            !folders.setDownloadDirectory(first+"/absent"), "reject system, traversal, relative and absent destinations");
    require(QFile::link(first, destination+"/link"), "create symlink destination");
    require(!folders.setDownloadDirectory(destination+"/link"), "reject symlink destination");
    require(folders.directories(destination).size() == 2 && folders.directories("/mnt/ext1/system").isEmpty(),
            "folder browser lists real children and hides links and service directories");
    require(!folders.setDownloadDirectory(QFileInfo(beforeChoice).absolutePath()), "reject account metadata directory");
    require(folders.setDownloadDirectory(first) && folders.localFile(0) == beforeChoice, "folder selection preserves old download path");
    require(folders.diagnosticLogging() && !contents(folders.diagnosticLogPath()).contains("demo"),
            "folder settings preserve logging and diagnostic log omits credentials");
    require(wait(folders, [&] { folders.download(1); }) && QFileInfo(folders.localFile(1)).absolutePath() == first, "new download uses chosen folder");
    const QString inFirst = folders.localFile(1);
    const QString foreign = first+"/102-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".epub";
    write(foreign, "another account's book");
    require(folders.setDownloadDirectory(second), "change destination again");
    require(wait(folders, [&] { folders.download(1); }) && folders.localFile(1) == inFirst, "identical book keeps identity in previous folder");
    write(fault, "changed");
    require(wait(folders, [&] { folders.download(0); }) && QFileInfo(folders.localFile(0)).absolutePath() == second, "changed EPUB uses new destination");
    write(fault, "");
    Client savedFolders(endpoint, folderRoot);
    savedFolders.showDownloaded(true);
    require(savedFolders.downloadDirectory() == second && savedFolders.books().size() == 2 &&
            !savedFolders.localFile(0).isEmpty() && !savedFolders.localFile(1).isEmpty(), "restart restores destination and books in different folders");
    require(QFile::exists(foreign), "shared download directory never swept for orphan names");
    require(QDir().rename(second, second+"-away"), "simulate missing destination");
    Client missingFolder(endpoint, folderRoot);
    missingFolder.showDownloaded(true);
    require(missingFolder.downloadDirectory() == second && missingFolder.books().size() == 2, "missing directory preserves settings and repair entries");
    require(!wait(folders, [&] { folders.download(1); }) && folders.status().contains("недоступна"), "unavailable destination blocks download without fallback");
    require(QDir().rename(second+"-away", second), "restore destination");
    const QString preferences = folderRoot+"/preferences.json";
    require(QFile::rename(preferences, preferences+".saved") && QDir().mkdir(preferences), "inject folder settings save failure");
    require(!folders.setDownloadDirectory(first) && folders.downloadDirectory() == second, "failed folder save preserves current setting");
    require(!folders.setDiagnosticLogging(true) && folders.diagnosticLogging() &&
            contents(folders.diagnosticLogPath()).contains("folder save failed"),
            "diagnostic logging remains usable when preferences cannot be saved");
    require(QDir().rmdir(preferences) && QFile::rename(preferences+".saved", preferences), "restore folder settings");
    const QString accountsFile = folderRoot+"/accounts.json";
    require(QFile::rename(accountsFile, accountsFile+".saved") && QDir().mkdir(accountsFile), "inject account settings save failure");
    require(!folders.configure("https://books.example.test", "other") && folders.status().contains("код") &&
            folders.username() == "demo", "failed account save reports file error and preserves connection");
    require(QDir().rmdir(accountsFile) && QFile::rename(accountsFile+".saved", accountsFile), "restore account settings");
    require(savedFolders.configure("https://books.example.test", "other") && savedFolders.books().isEmpty(), "shared folder does not expose another account's books");

    const QByteArray recoveryEpub = contents(changedFile);
    for (const auto &format : {QString("epub"), QString("pdf")}) {
        for (bool shared : {false, true}) for (bool replacement : {false, true}) for (int phase = 0; phase < 3; ++phase) {
            const QString caseName = "bookorbit-check-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString caseRoot = root+"/"+caseName;
            Client setup(endpoint, caseRoot, nullptr, false);
            require(setup.configure(endpoint.toString(), "demo"), "prepare isolated transfer recovery account");
            const QString caseScope = caseRoot+"/"+accountKey;
            const QString directory = shared ? "/mnt/ext1/books/"+caseName : caseScope;
            require(QDir().mkpath(directory) && QDir().mkpath(caseScope+"/transfers"), "prepare recovery paths");
            const QString nextStem = "700-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString previousStem = "700-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString nextPath = directory+"/"+nextStem+"."+format, previousPath = directory+"/"+previousStem+"."+format;
            const QString part = caseScope+"/"+nextStem+".part", journal = caseScope+"/transfers/"+nextStem+".json";
            const QByteArray nextBytes = format == "epub" ? recoveryEpub : QByteArray("%PDF-1.4\nsynthetic recovery\n");
            const QByteArray previousBytes = nextBytes+"previous";
            auto recordFor = [&](const QString &stem, const QByteArray &bytes) {
                QJsonObject file{{"id",700},{"format",format},{"role","primary"},{"sizeBytes",double(bytes.size())}};
                QJsonObject book{{"id",700},{"title","Recovery"},{"selectedFile",file},{"files",QJsonArray{file}}};
                QJsonObject record{{"bookId",700},{"fileId",700},{"filename",stem+"."+format},{"format",format},
                    {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
                    {"bytes",double(bytes.size())},{"book",book}};
                if (shared) record["directory"]=directory;
                return record;
            };
            const auto next = recordFor(nextStem,nextBytes);
            const auto previous = replacement ? recordFor(previousStem,previousBytes) : QJsonObject{};
            if (replacement) write(previousPath,previousBytes);
            if (phase == 0) write(part,nextBytes); else write(nextPath,nextBytes);
            const QString recordPath = caseScope+"/records/700.json";
            if (phase == 2 || replacement) write(recordPath,QJsonDocument(phase == 2 ? next : previous).toJson());
            write(journal,QJsonDocument(QJsonObject{{"fileId",700},{"previous",previous},{"next",next}}).toJson());
            // Confirmed replacement keeps both versions while the reader state is uncertain.
            if (phase == 2 && replacement) {
                const auto savedJournal = contents(journal);
                write(journal,"{");
                Client malformedJournal(endpoint,caseRoot);
                require(QFile::exists(previousPath) && QFile::exists(nextPath), "malformed transfer journal blocks private fallback cleanup");
                write(journal,savedJournal);
                const QString transfers = caseScope+"/transfers";
                require(QDir().rename(transfers,transfers+".saved"), "save journal directory before substitution");
                write(transfers,"not a directory");
                Client blockedJournals(endpoint,caseRoot);
                require(QFile::exists(previousPath) && QFile::exists(nextPath), "unreadable journal directory blocks private fallback cleanup");
                require(QFile::remove(transfers) && QDir().rename(transfers+".saved",transfers), "restore journal directory");
                for (auto state : {ReaderFileState::Open,ReaderFileState::Unknown}) {
                    fakeReader=state;
                    Client deferred(endpoint,caseRoot);
                    require(QFile::exists(previousPath) && QFile::exists(nextPath) && QFile::exists(journal),
                            "open or unknown reader retains transfer journal and both versions");
                }
                fakeReader=ReaderFileState::Closed;
                write(nextPath,nextBytes+"damaged");
                Client damaged(endpoint,caseRoot);
                require(QFile::exists(previousPath) && QFile::exists(journal), "unconfirmed current file preserves previous version");
                write(nextPath,nextBytes);
                const QString foreignPath=directory+"/foreign."+format;
                write(foreignPath,previousBytes);
                require(QFile::remove(previousPath) && QFile::link(foreignPath,previousPath), "inject symlink instead of retired version");
                Client linked(endpoint,caseRoot);
                require(QFileInfo(previousPath).isSymLink() && QFile::exists(journal) && contents(foreignPath)==previousBytes,
                        "recovery preserves symlinks and their unrelated targets");
                require(QFile::remove(previousPath), "remove test symlink");
            }
            Client recovered(endpoint,caseRoot);
            require(!QFile::exists(journal) && !QFile::exists(part), "restart resolves durable transfer journal and staging file");
            require(phase == 2 ? QFile::exists(nextPath) && !QFile::exists(previousPath) && contents(nextPath)==nextBytes :
                    !QFile::exists(nextPath) && (!replacement || (QFile::exists(previousPath) && contents(previousPath)==previousBytes)),
                    "recovery follows committed record for initial and replacement downloads in every format and directory");
            Client repeated(endpoint,caseRoot);
            require(repeated.books().size() == ((phase == 2 || replacement) ? 1 : 0), "recovery is idempotent and preserves registered books");
        }
    }
    const QString guardedRoot=root+"/guarded-cleanup";
    Client guarded(endpoint,guardedRoot,nullptr,false);
    require(guarded.configure(endpoint.toString(),"demo"), "prepare cleanup guard account");
    const QString guardedScope=guardedRoot+"/"+accountKey;
    write(guardedScope+"/records/999.json","{");
    const QString guardedOrphan=guardedScope+"/999-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".pdf";
    write(guardedOrphan,"%PDF-retain");
    require(wait(guarded,[&] { guarded.login(guarded.server(),"demo","demo"); }) &&
            wait(guarded,[&] { guarded.download(0); }) && QFile::exists(guardedOrphan),
            "invalid registry blocks cleanup after successful downloads as well as startup");

    const auto guardedJournals=QDir(guardedScope+"/transfers").entryList({"*.json"},QDir::Files);
    require(guardedJournals.size()==1, "deferred transfer keeps a durable journal");
    const auto journalBytes=contents(guardedScope+"/transfers/"+guardedJournals.first());
    const auto guardedJournal=QJsonDocument::fromJson(journalBytes).object();
    require(journalBytes.size()<2048 && !guardedJournal["next"].toObject().contains("book") &&
            guardedJournal["next"].toObject()["fileId"]==101, "transfer journal stores bounded file identity without duplicate book metadata");
    // New screens use the same downloader and durable records, with an isolated library.
    const QString featureRoot = root+"/features";
    Client features(endpoint, featureRoot, nullptr, false);
    write(fault, "features");
    require(wait(features, [&] { features.login(features.server(), "demo", "demo"); }) && features.total() == 23, "multi-format paginated catalog");
    require(features.books()[0].toMap()["readStatus"].toMap()["status"] == "reading" &&
            features.books()[0].toMap()["readingProgress"].toDouble() == 37.5, "catalog exposes server reading status and percentage");
    require(!features.books()[3].toMap()["readingProgress"].isNull() &&
            features.books()[3].toMap()["readingProgress"].toDouble() == 0, "zero reading progress is retained");
    for (const int i : {2, 4, 5, 6})
        require(features.books()[i].toMap()["readingProgress"].isNull(), "missing or invalid percentage is not shown as zero");
    require(wait(features, [&] { features.showDetail(1); }), "load book details by stable book ID");
    require(features.detail()["readingProgress"].toDouble() == 37.5, "detail without percentage preserves catalog reading progress");
    require(features.detail()["author"].toString() == "Локальный стенд" &&
            features.detail()["description"].toString().contains("Второй абзац") &&
            !features.detail()["description"].toString().contains("<p>"), "detail author objects and HTML become readable text");
    require(features.detail()["files"].toList().size() == 5, "variants include duplicate formats but exclude sidecars");
    features.selectFile(900);
    require(features.detail()["fileId"].toInt() == 101, "cannot select a sidecar or an unknown file ID");
    features.selectFile(201);
    require(wait(features, [&] { features.downloadSelected(); }), "download chosen PDF");
    const QString pdfPath = features.localFile(0);
    require(pdfPath.endsWith(".pdf") && contents(pdfPath).startsWith("%PDF-"), "PDF retains real extension and contents");
    features.selectFile(202);
    require(wait(features, [&] { features.downloadSelected(); }), "download second format without replacing PDF");
    const QString fb2Path = features.localFile(0);
    require(fb2Path.endsWith(".fb2") && contents(fb2Path).contains("FictionBook") && QFile::exists(pdfPath), "FB2 and PDF coexist");
    features.selectFile(101);
    require(wait(features, [&] { features.downloadSelected(); }), "EPUB remains available beside other formats");
    const QString epubPath = features.localFile(0);
    features.selectFile(203);
    require(wait(features, [&] { features.downloadSelected(); }) && features.localFile(0) != epubPath && QFile::exists(epubPath),
            "two EPUB files of one book keep distinct file identities");
    require(features.syncSummary()["total"].toInt()==1 && features.syncBooks()[0].toMap()["files"].toList().size()==2 &&
            features.syncSummary()["unsupported"].toInt()==0, "two EPUBs plus PDF and FB2 count as one book and keep separate file actions");
    const QString categoriesRoot=root+"/category-check", categoriesScope=categoriesRoot+"/"+QFileInfo(scope).fileName();
    require(QDir().mkpath(categoriesScope+"/records"),"create isolated sync category fixture");
    write(categoriesRoot+"/accounts.json",contents(featureRoot+"/accounts.json"));
    auto categoryRecord=QJsonDocument::fromJson(contents(featureRoot+"/"+QFileInfo(scope).fileName()+"/records/101.json")).object();
    categoryRecord.remove("directory");
    require(QFile::copy(epubPath,categoriesScope+"/"+categoryRecord["filename"].toString()),"copy category fixture EPUB into its own scope");
    for (const auto &state:QStringList{"unknown","network","conflict","reader","reader_unknown","pending","uncertain","auth","file","position","error","synced"}) {
        categoryRecord["syncStatus"]=QJsonObject{{"state",state},{"profile",fakeProfile}};
        write(categoriesScope+"/records/101.json",QJsonDocument(categoryRecord).toJson());
        Client category(endpoint,categoriesRoot);
        const QString group=state=="synced" ? "synced" : (state=="unknown" || state=="network") ? "waiting" : "attention";
        require(!category.syncBooks().isEmpty() && category.syncBooks()[0].toMap()["group"]==group && category.syncSummary()["waiting"].toInt()+category.syncSummary()["attention"].toInt()+category.syncSummary()["synced"].toInt()==1,
            "every persisted sync state maps to one book category");
    }
    fakeRecents={{epubPath,{10,100}},{features.localFile(0),{10,100}},{pdfPath,{11,90}}};
    features.refreshRecents(); QCoreApplication::processEvents();
    require(features.historyAvailable() && features.recentBook()["fileId"].toInt()==203,
        "native aliases with equal opentime prefer the user's selected variant");
    features.selectFile(101); features.refreshRecents(); QCoreApplication::processEvents();
    require(features.recentBook()["fileId"].toInt()==101, "equal native times honor a changed preferred file");
    features.selectFile(201); features.refreshRecents(); QCoreApplication::processEvents();
    require(features.recentBook()["fileId"].toInt()==101,"equal aliases without a recent preferred format choose the smaller file ID");
    require(QFile::rename(epubPath,epubPath+".recent-test"),"temporarily remove only the isolated recent candidate");
    features.refreshRecents(); QCoreApplication::processEvents();
    require(features.recentBook()["fileId"].toInt()==203,"missing native recent file never receives a read action");
    require(QFile::rename(epubPath+".recent-test",epubPath),"restore isolated recent candidate");
    fakeProfile="empty-profile"; fakeRecents.clear(); features.refreshRecents();
    require(features.recentBook().isEmpty(), "profile switch clears the previous recent snapshot immediately");
    QCoreApplication::processEvents(); fakeProfile="default";
    historyReadable=false; features.refreshRecents(); QCoreApplication::processEvents();
    require(!features.historyAvailable() && features.recentBook().isEmpty(), "unavailable native history has no client-only fallback");
    historyReadable=true; fakeRecents.clear(); features.refreshRecents(); QCoreApplication::processEvents();
    features.selectFile(201);
    require(!wait(features, [&] { features.syncSelected(); }) && features.status().contains("только для EPUB"), "PDF never enters EPUB synchronization");
    opened = false;
    require(wait(features, [&] { features.openSelected(); }) && opened, "selected PDF uses the native opener boundary");
    require(features.localFile(0) == pdfPath, "successful open remembers the chosen variant");
    write(fault, "invalid_content");
    require(!wait(features, [&] { features.downloadSelected(); }) && QFile::exists(pdfPath), "invalid PDF body cannot replace downloaded file");
    features.selectFile(202);
    require(!wait(features, [&] { features.downloadSelected(); }) && QFile::exists(fb2Path), "invalid FB2 body cannot replace downloaded file");
    write(fault, "features");
    features.selectFile(205);
    require(wait(features, [&] { features.downloadSelected(); }) && !features.detail()["readable"].toBool(), "other book formats can be saved without promising native opening");
    require(!wait(features, [&] { features.openSelected(); }), "unsupported opener is explicit");
    features.showDownloaded(true);
    require(features.books().size() == 1, "downloaded variants share one book row");
    Client featureRestart(endpoint, featureRoot);
    require(featureRestart.books().size() == 1 && featureRestart.books()[0].toMap()["fileCount"].toInt() == 5,
            "all format records survive restart");
    featureRestart.showDetail(1);
    require(featureRestart.detail()["description"].toString().contains("Аннотация") && !featureRestart.authenticated(),
            "downloaded detail metadata remains readable offline");
    featureRestart.selectFile(201);
    networkAvailable = false;
    opened = false;
    require(wait(featureRestart, [&] { featureRestart.openSelected(); }) && opened, "cached file opens without metadata network access");
    networkAvailable = true;
    write(fault,"features");
    const QString featureScope=featureRoot+"/"+QFileInfo(scope).fileName();
    const auto pdfBeforeCheck=contents(pdfPath), fb2BeforeCheck=contents(fb2Path);
    require(wait(features,[&] { features.verifyLibrary(); }) && features.status().contains("Файлов: 5; совпало: 5"),
            "verification counts all five format files of one book, not one book row");
    require(contents(pdfPath)==pdfBeforeCheck && contents(fb2Path)==fb2BeforeCheck &&
            QFile::exists(featureScope+"/records/205.json"), "full check keeps all downloaded variants unchanged");
    auto cancelBetween=QObject::connect(&features,&Client::changed,&features,[&] {
        if (features.verifyingLibrary() && features.status().contains("Локальный файл совпадает"))
            features.cancelLibraryVerification();
    });
    require(!wait(features,[&] { features.verifyLibrary(); }) && features.status().contains("совпало: 1") &&
            features.status().contains("не проверено: 4"), "cancellation between files preserves completed result and skips remaining files");
    QObject::disconnect(cancelBetween);
    require(wait(features, [&] { features.showCollections(); }) && features.collections().size() == 3,
            "collection array includes own and public books, excludes podcasts");
    require(wait(features, [&] { features.openCollection(12, "Общая библиотека"); }) && features.total() == 23 && features.books().size() == 10,
            "collection uses paginated server membership");
    require(wait(features, [&] { features.refresh(2); }) && features.books().size() == 3 && features.collectionId() == 12,
            "last page stays in selected collection");
    require(wait(features, [&] { features.refresh(0, "Автор коллекции"); }) && features.total() == 20,
            "Cyrillic collection search is URL encoded and scoped");
    write(fault, "error");
    require(!wait(features, [&] { features.refresh(0, "Автор коллекции"); }) && features.books().size()==10 && features.catalogQuery()=="Автор коллекции",
            "collection failure preserves the validated page and committed query");
    write(fault, "features");
    require(wait(features, [&] { features.retry(); }) && features.collectionId() == 12 && features.total() == 20,
            "retry preserves collection and search");
    require(wait(features, [&] { features.openCollection(13, "Пустая коллекция"); }) && features.books().isEmpty(), "empty collection is a successful empty response");
    require(!wait(features, [&] { features.openCollection(99, "Удалённая коллекция"); }) && features.books().isEmpty(), "deleted collection does not fall back to global catalog");
    write(fault, "renew");
    require(wait(features, [&] { features.showCollections(); }) && features.authenticated(), "collection 401 renews session and retries array response");
    require(wait(features, [&] { features.openCollection(12, "Общая библиотека"); }), "collection before token expiry");
    write(fault, "relogin");
    require(!wait(features, [&] { features.refresh(1); }) && !features.authenticated() && !features.hasSavedSession(),
            "rejected refresh token clears saved session and requests a password");
    require(wait(features, [&] { features.login(features.server(), "demo", "demo"); }) &&
            wait(features, [&] { features.openCollection(12, "Общая библиотека"); }) &&
            wait(features, [&] { features.refresh(1); }) &&
            features.collectionId() == 12 && features.page() == 1 && features.total() == 23,
            "explicit login restores access to collection page");
    write(fault, "encoded_search");
    for (const QString &query : {QString("C++"),QString("+"),QString(" "),QString("пробел"),QString("%"),
                               QString("%2B"),QString("&"),QString("?"),QString("#"),QString("=")}) {
        require(wait(features, [&] { features.refresh(0, query); }) && features.total() == 23 &&
                features.books().first().toMap()["title"].toString() == query,
                "collection search round-trips literal reserved characters and Cyrillic");
    }
    require(wait(features, [&] { features.refresh(2, "C++"); }) && features.books().size() == 3 &&
            features.books().first().toMap()["title"].toString() == "C++",
            "encoded search survives pagination");
    write(fault, "error");
    require(!wait(features, [&] { features.refresh(0, "%2B"); }) && features.hasSavedSession(),
            "server failure retains saved session and encoded query");
    write(fault, "encoded_search");
    require(wait(features, [&] { features.retry(); }) && features.total() == 23 &&
            features.books().first().toMap()["title"].toString() == "%2B",
            "retry preserves literal percent encoding in collection search");
    write(fault, "bad_collections");
    require(!wait(features, [&] { features.showCollections(); }) && features.collections().isEmpty(), "reject wrong collection response shape");
    write(fault, "features");
    require(wait(features, [&] { features.retry(); }), "retry collection list after malformed response");
    require(wait(features, [&] { features.openCollection(11, "К прочтению"); }) && features.total() == 2, "own collection membership");
    require(wait(features, [&] { features.showDetail(1); features.closeDetail(); }) && !features.detailVisible() && features.collectionId() == 11,
            "late detail response cannot reopen a closed card or leave collection");
    require(wait(features, [&] { features.showDetail(1); }), "reopen details in collection");
    require(wait(features, [&] { features.openCollection(12, "Общая библиотека", 120, 250); }), "follow collection link from book details");
    const auto restoredView = features.backFromCollection();
    require(features.detailVisible() && features.detail()["bookId"].toInt() == 1 && features.collectionId() == 11 &&
            restoredView["catalogY"].toDouble() == 120 && restoredView["detailY"].toDouble() == 250,
            "back from linked collection restores the previous book and navigation context");
    const int rssBefore=rssKiB();
    for (int step=0; step<100; ++step) {
        require(wait(features,[&] { features.showDetail(1); }), "navigation detail loads");
        require(wait(features,[&] { features.openCollection(step%2 ? 11 : 12, "Коллекция"); }), "navigation collection loads");
    }
    bool bounded=true;
    for (int step=0; step<16; ++step) bounded &= !features.backFromCollection().isEmpty();
    bounded &= features.backFromCollection().isEmpty();
    require(bounded, "100 collection transitions retain at most 16 history entries");
    const int rssAfter=rssKiB();
    std::printf("RSS navigation KiB: before=%d after=%d delta=%d\n",rssBefore,rssAfter,rssAfter-rssBefore);
    write(fault, "error");
    require(!wait(features, [&] { features.refreshDetail(); }) && features.detail()["description"].toString().contains("Аннотация"),
            "failed detail refresh preserves known metadata and downloaded actions");
    write(fault, "");
    require(features.configure("https://books.example.test", "other") && !features.detailVisible() && features.collections().isEmpty() && features.books().isEmpty(),
            "account change clears details, collections and file selection context");
    const QString coverRoot=root+"/cover-limit-check";
    const QString coverKey=QString::fromLatin1(QCryptographicHash::hash((endpoint.toString()+"\n").toUtf8(),QCryptographicHash::Sha256).toHex().left(24));
    const QString coverScope=coverRoot+"/"+coverKey;
    require(QDir().mkpath(coverScope), "create isolated cover cache");
    QJsonObject manyVersions;
    for (int id=1000; id<1070; ++id) {
        write(coverScope+"/cover-"+QString::number(id)+".png", "transient");
        manyVersions[QString::number(id)]="v1";
    }
    write(coverScope+"/cover-index.json",QJsonDocument(manyVersions).toJson());
    Client boundedCache(endpoint,coverRoot,nullptr,false);
    require(QDir(coverScope).entryList({"cover-*.png"},QDir::Files).size()==64,
            "restart bounds transient cover cache to 64 files");
    std::puts("PASS: stage 3 client checks");
}
