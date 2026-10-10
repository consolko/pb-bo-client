#include "ota.h"
#include "zip.h"
#include "client.h"
#include "update.h"
#include "check_wait.h"
#include "worker_gate.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QUuid>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>

struct UpdateManagerCheck {
    static void ready(UpdateManager &updater, const QJsonObject &manifest) {
        updater.prepared=manifest; updater.phase="ready";
    }
    static void verify(UpdateManager &updater,const QString &directory) {
        ++updater.generation; updater.attemptDir=directory; updater.candidateVersion=Ota::version(); updater.verifyArchive();
    }
    static void install(UpdateManager &updater,const QString &directory,const QJsonObject &manifest,bool confirm=false) {
        updater.attemptDir=directory; updater.prepared=manifest;
        if(!QFile::exists(directory+"/release.json")) {
            QFile metadata(directory+"/release.json"); metadata.open(QIODevice::WriteOnly); metadata.write("test metadata");
        }
        updater.client->prepareUpdate();
        updater.installPrepared(confirm);
    }
    static void source(UpdateManager &updater,const QUrl &url) {
        updater.apiBase=url; updater.cache=updater.normalizeCache(updater.cache); updater.startupDone=true;
    }
    static void loseCache(UpdateManager &updater) { updater.cache={}; }
    static void timeout(UpdateManager &updater) { QMetaObject::invokeMethod(&updater.totalDeadline,"timeout",Qt::DirectConnection); }
};

#ifdef BOOKORBIT_FSYNC_CHECK
enum class SyncFailure { None, File, Directory };
static SyncFailure syncFailure=SyncFailure::None;
static QStringList syncCalls;
static QString syncTarget,syncStaged;
static QByteArray targetAtDirectorySync;
static bool stagedAtDirectorySync=false;
static bool cleanupFailure=false;
extern "C" int __real_unlink(const char *path);
extern "C" int __wrap_unlink(const char *path) {
    if(cleanupFailure && QFile::decodeName(path)==syncStaged) { errno=EACCES; return -1; }
    return __real_unlink(path);
}
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    struct stat info{};
    if(::fstat(fd,&info)!=0) return -1;
    const bool directory=S_ISDIR(info.st_mode);
    syncCalls << (directory ? "directory" : "file");
    if(directory && !syncTarget.isEmpty()) {
        targetAtDirectorySync=Ota::read(syncTarget,100);
        stagedAtDirectorySync=QFile::exists(syncStaged);
    }
    struct stat destinationDir{};
    const bool destinationSync=syncTarget.isEmpty() || (::stat(QFile::encodeName(QFileInfo(syncTarget).absolutePath()),&destinationDir)==0 &&
        destinationDir.st_dev==info.st_dev && destinationDir.st_ino==info.st_ino);
    if((directory && destinationSync && syncFailure==SyncFailure::Directory) || (!directory && syncFailure==SyncFailure::File)) {
        errno=EIO; return -1;
    }
    return __real_fsync(fd);
}
#endif

static void check(bool ok,const char *label) { std::printf("%s: %s\n",ok?"PASS":"FAIL",label); if(!ok) std::exit(1); }
static void put16(QByteArray &b,quint16 v) { b.append(char(v)); b.append(char(v>>8)); }
static void put32(QByteArray &b,quint32 v) { put16(b,v); put16(b,v>>16); }
static QByteArray archive(const QByteArray &name,const QByteArray &data,const QByteArray &deflated={}) {
    const auto &packed=deflated.isEmpty() ? data : deflated;
    const quint16 method=deflated.isEmpty() ? 0 : 8;
    QByteArray b; const auto crc=crc32(0,reinterpret_cast<const Bytef*>(data.constData()),data.size());
    put32(b,0x04034b50); put16(b,20); put16(b,0); put16(b,method); put32(b,0); put32(b,crc); put32(b,packed.size()); put32(b,data.size()); put16(b,name.size()); put16(b,0); b+=name; b+=packed;
    const auto offset=b.size();
    put32(b,0x02014b50); put16(b,20); put16(b,20); put16(b,0); put16(b,method); put32(b,0); put32(b,crc); put32(b,packed.size()); put32(b,data.size()); put16(b,name.size()); put16(b,0); put16(b,0); put16(b,0); put16(b,0); put32(b,0); put32(b,0); b+=name;
    const auto central=b.size()-offset;
    put32(b,0x06054b50); put16(b,0); put16(b,0); put16(b,1); put16(b,1); put32(b,central); put32(b,offset); put16(b,0); return b;
}
static QByteArray deflateData(const QByteArray &data,int flush=Z_FINISH) {
    z_stream z{};
    check(deflateInit2(&z,6,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)==Z_OK,"initialize fixture compressor");
    QByteArray packed(deflateBound(&z,data.size())+16,0);
    z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(data.constData())); z.avail_in=data.size();
    z.next_out=reinterpret_cast<Bytef*>(packed.data()); z.avail_out=packed.size();
    const int rc=deflate(&z,flush);
    check(rc==(flush==Z_FINISH ? Z_STREAM_END : Z_OK) && !z.avail_in && z.avail_out,"compress fixture");
    packed.resize(z.total_out); deflateEnd(&z); return packed;
}
static void cacheRegressions(Client &client,const QString &root) {
    QTcpServer server; check(server.listen(QHostAddress::LocalHost,0),"local update fixture listens");
    const auto endpoint=QUrl("http://127.0.0.1:"+QString::number(server.serverPort()));
    QJsonArray assets;
    for(const auto *name:{"bookorbit-pb634.zip","SHA256SUMS"}) assets.append(QJsonObject{
        {"name",name},{"size",100},{"browser_download_url","https://github.com/consolko/pb-bo-client/releases/download/v2.0.0/"+QString(name)}});
    QJsonObject release{{"tag_name","v2.0.0"},{"draft",false},{"prerelease",false},{"assets",assets},
                        {"body",QString(20000,'n')},{"ignored",QString(900000,'x')}};
    QByteArray response=QJsonDocument(release).toJson(QJsonDocument::Compact);
    int status=200; QList<QByteArray> requests;
    QObject::connect(&server,&QTcpServer::newConnection,&server,[&] {
        while(server.hasPendingConnections()) {
            auto *socket=server.nextPendingConnection(); auto request=std::make_shared<QByteArray>();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,request] {
                request->append(socket->readAll());
                if(socket->property("served").toBool() || !request->contains("\r\n\r\n")) return;
                socket->setProperty("served",true); requests.append(*request);
                const auto body=status==304 ? QByteArray{} : response;
                socket->write("HTTP/1.1 "+QByteArray::number(status)+" OK\r\nETag: \"release-v1\"\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
                socket->disconnectFromHost();
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        }
    });
    QString error; const auto data=root+"/cache-regression";
    {
        UpdateManager updater(&client,data,root+"/bookorbit.app");
        check(waitUntil([&] { return updater.state()!="recovering"; }),"cache test recovery settles");
        updater.setAutomatic(false); UpdateManagerCheck::source(updater,endpoint);
        updater.check(); check(waitUntil([&] { return updater.state()!="checking"; }) && updater.state()=="available","large release response passes through real HTTP handling");
        const auto preferences=Ota::read(data+"/update/preferences.json",65536),cached=Ota::read(data+"/update/release-cache.json",128*1024);
        check(preferences.size()<100 && QJsonDocument::fromJson(preferences).object()["automatic"]==false && !cached.isEmpty() && !cached.contains("ignored"),"network cache stores only release fields separately from settings");
        check(updater.notes().size()==12000,"release notes are bounded before caching");
        status=304; updater.check();
        check(waitUntil([&] { return updater.state()!="checking"; }) && updater.state()=="available" && requests.last().toLower().contains("if-none-match: \"release-v1\""),"304 reuses the matching validated release and ETag");
        UpdateManagerCheck::loseCache(updater); const int before=requests.size(); updater.check();
        check(waitUntil([&] { return updater.state()!="checking"; }) && updater.state()=="error" && requests.size()==before+2 && !requests.last().toLower().contains("if-none-match"),"304 without cache retries unconditionally once and stops");
        status=200; response="{"; updater.check();
        check(waitUntil([&] { return updater.state()!="checking"; }) && updater.state()=="error" && !updater.automatic(),"malformed response never changes automatic preference");
        response=QByteArray(1024*1024+1,'x'); updater.check();
        check(waitUntil([&] { return updater.state()!="checking"; }) && updater.state()=="error" && !updater.automatic(),"oversized response never changes automatic preference");
        auto duplicate=release; auto extra=assets; extra.append(assets.last()); duplicate["assets"]=extra;
        response=QJsonDocument(duplicate).toJson(QJsonDocument::Compact); updater.check();
        check(waitUntil([&] { return updater.state()!="checking"; }) && updater.state()=="error","duplicate checksum asset is rejected before caching");
    }
    for(const auto &damage:{QByteArray("{"),QByteArray(128*1024+1,'x')}) {
        check(Ota::save(data+"/update/release-cache.json",damage,&error),"damage only the network cache");
        UpdateManager restarted(&client,data,root+"/bookorbit.app");
        check(!restarted.automatic() && waitUntil([&] { return restarted.state()!="recovering"; }),"automatic=false survives restart with invalid cache");
        UpdateManagerCheck::source(restarted,endpoint); status=200; response=QJsonDocument(release).toJson(QJsonDocument::Compact);
        restarted.check();
        check(waitUntil([&] { return restarted.state()!="checking"; }) && restarted.state()=="available" && !requests.last().toLower().contains("if-none-match"),"invalid cache causes an unconditional HTTP check");
    }
    check(QFile::remove(data+"/update/release-cache.json") && QDir().mkdir(data+"/update/release-cache.json"),"simulate cache persistence failure");
    {
        UpdateManager restarted(&client,data,root+"/bookorbit.app");
        check(waitUntil([&] { return restarted.state()!="recovering"; }),"unwritable cache recovery settles");
        UpdateManagerCheck::source(restarted,endpoint); restarted.check();
        check(waitUntil([&] { return restarted.state()!="checking"; }) && restarted.state()=="available" && !restarted.automatic(),"cache write failure does not fail the valid release or reset settings");
    }
    const auto legacy=root+"/legacy-update";
    check(QDir().mkpath(legacy+"/update") && Ota::saveJson(legacy+"/update/preferences.json",{{"automatic",false},{"source","https://api.github.com"},{"release",release},{"etag","\"legacy\""}},&error),"large legacy preferences fixture");
    {
        UpdateManager migrated(&client,legacy,root+"/bookorbit.app");
        check(!migrated.automatic() && QFileInfo(legacy+"/update/preferences.json").size()<100 && !Ota::read(legacy+"/update/release-cache.json",128*1024).isEmpty(),"legacy preferences larger than 64 KiB migrate without losing automatic=false");
    }
    UpdateManager migratedAgain(&client,legacy,root+"/bookorbit.app");
    check(!migratedAgain.automatic(),"migrated preference survives another restart");
}

int main(int argc,char **argv) {
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM","offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc,argv); QTemporaryDir temp; check(temp.isValid(),"temporary root");
    const auto root=temp.path(); QString error;
    // Optionally validate a release artifact using the compiled production trust key.
    if(argc==2) {
        const QString path=QString::fromLocal8Bit(argv[1]);
        QJsonObject release;
        check(Ota::stageArchive(path,root+"/release.next",Ota::version(),&release,&error),"signed release stages with the compiled key");
        check(Ota::save(root+"/release.app","previous executable",&error),"release installation fixture");
        check(Ota::install(root+"/release.next",root+"/release.app",release).state==Ota::InstallState::Complete,"signed release installs into isolated storage");
        check(Ota::hashFile(root+"/release.app")==release["sha256"].toString(),"installed release matches signed checksum");
        Zip zip(path);
        auto signature=zip.read("release.sig",64); signature[0]^=1;
        check(!Ota::verifyManifest(zip.read("release.json",65536),signature,&release,&error),"modified release signature rejected");
    }
    check(Ota::newer("1.10.0","1.9.0") && !Ota::newer("1.0.0","1.0.0") && !Ota::newer("1.0.0","2.0.0"),"numeric version ordering");
    for(const auto *v:{"v1.0.0","1.0","01.0.0","1.0.0-rc.1","1.0.0\n","9999999.0.0"}) check(!Ota::validVersion(v),"reject invalid version");
    check(Ota::save(root+"/valid.zip",archive("client.app","payload"),&error),"write fixture");
    Zip valid(root+"/valid.zip");
    check(valid.extract("client.app",root+"/out",7,&error)&&Ota::read(root+"/out",7)=="payload","bounded ZIP extraction");
    check(!valid.extract("client.app",root+"/out",8,&error)&&Ota::read(root+"/out",7)=="payload","size mismatch preserves output");
    const QByteArray boundary=QByteArray(15,'a')+QByteArray(65521,'b')+'c';
    auto packed=deflateData(QByteArray(15,'a'),Z_SYNC_FLUSH);
    check(packed.size()==10,"fixture prefix saves five bytes before stored block");
    packed+=char(0); put16(packed,65521); put16(packed,quint16(~65521)); packed+=QByteArray(65521,'b');
    packed+=char(1); put16(packed,1); put16(packed,quint16(~1)); packed+='c';
    check(boundary.size()==65537 && packed.size()==65542,"fixture exhausts both 64 KiB buffers with six compressed bytes remaining");
    check(Ota::save(root+"/deflated.zip",archive("client.app",boundary,packed),&error),"write DEFLATE boundary fixture");
    Zip deflated(root+"/deflated.zip");
    check(deflated.read("client.app",boundary.size())==boundary,"single-buffer decoder validates boundary fixture and CRC");
    check(deflated.extract("client.app",root+"/inflated",boundary.size(),&error) && Ota::read(root+"/inflated",boundary.size())==boundary,"streaming DEFLATE crosses exhausted input/output boundary");
    auto rejectDeflate=[&](const QByteArray &data,const QByteArray &compressed,const char *label) {
        check(Ota::save(root+"/invalid-deflate.zip",archive("client.app",data,compressed),&error),"write invalid DEFLATE fixture");
        Zip zip(root+"/invalid-deflate.zip");
        check(!zip.extract("client.app",root+"/inflated",data.size(),&error) && Ota::read(root+"/inflated",boundary.size())==boundary,label);
    };
    rejectDeflate(boundary,packed.left(packed.size()-1),"truncated DEFLATE preserves output");
    rejectDeflate(boundary,packed+char(0),"trailing compressed data rejected");
    auto invalidBlock=packed; invalidBlock[13]^=1;
    rejectDeflate(boundary,invalidBlock,"invalid DEFLATE block rejected");
    auto badCrc=packed; badCrc[badCrc.size()-1]='d';
    rejectDeflate(boundary,badCrc,"DEFLATE CRC mismatch preserves output");
    rejectDeflate(boundary.left(boundary.size()-1),packed,"DEFLATE expansion past declared size rejected");
    const QByteArray expanded(3*65536,'x');
    check(Ota::save(root+"/expanded.zip",archive("client.app",expanded,deflateData(expanded)),&error),"write compressed multi-buffer fixture");
    Zip expanding(root+"/expanded.zip");
    check(expanding.extract("client.app",root+"/expanded",expanded.size(),&error) && Ota::read(root+"/expanded",expanded.size())==expanded,"drain multiple output buffers from one compressed block");
    auto broken=archive("client.app","payload"); broken[40]^=1;
    check(Ota::save(root+"/broken.zip",broken,&error),"write corrupt fixture");
    Zip damaged(root+"/broken.zip");
    check(!damaged.extract("client.app",root+"/out",7,&error)&&Ota::read(root+"/out",7)=="payload","CRC mismatch preserves output");
    Ota::save(root+"/escape.zip",archive("../escape","payload"),&error); Zip escape(root+"/escape.zip");
    check(!escape.extract("../escape",root+"/out",7,&error),"ZIP traversal rejected");
    check(!valid.extract("client.app",root+"/out",33*1024*1024,&error),"ZIP expansion bound");
    check(QFile::link(root+"/out",root+"/link"),"create symlink fixture");
    check(!valid.extract("client.app",root+"/link",7,&error) && !Ota::save(root+"/link","bad",&error),"symlink destination rejected");
    QJsonObject manifest;
    check(!Ota::verifyManifest("{}",QByteArray(64,'x'),&manifest,&error),"untrusted manifest rejected");
    QByteArray executable(52,0); executable.replace(0,6,QByteArray("\177ELF\1\1",6)); executable[18]=40;
    const auto target=root+"/bookorbit.app", staged=root+"/bookorbit.next";
    check(Ota::save(target,"old version",&error) && Ota::save(staged,executable,&error),"installation fixture");
    QJsonObject record{{"bytes",executable.size()},{"sha256",Ota::hashFile(staged)}};
    check(!Ota::stageArchive(root+"/valid.zip",staged,"1.1.5",&manifest,&error) && Ota::read(target,100)=="old version","unsigned ZIP never replaces installed application");
    check(Ota::install(staged,root+"/link",record).state==Ota::InstallState::FailedBeforeReplace,"installation rejects symlink destination");
    auto corrupted=record; corrupted["sha256"]=QString(64,'0');
    check(Ota::install(staged,target,corrupted).state==Ota::InstallState::FailedBeforeReplace && Ota::read(target,100)=="old version","recheck before replacement preserves old executable on corruption");
#ifdef BOOKORBIT_FSYNC_CHECK
    syncCalls.clear(); syncFailure=SyncFailure::File;
    check(Ota::install(staged,target,record).state==Ota::InstallState::FailedBeforeReplace && Ota::read(target,100)=="old version" && QFile::exists(staged),"file fsync failure preserves installed and staged executables");
    check(syncCalls==QStringList{"file"},"failed file sync never advances to directory sync");
    syncCalls.clear(); syncFailure=SyncFailure::Directory; syncTarget=target; syncStaged=staged;
    check(Ota::install(staged,target,record).state==Ota::InstallState::ReplacedUnconfirmed && Ota::read(target,100)==executable && QFile::exists(staged),"directory fsync failure reports unconfirmed replacement and retains staged executable");
    check(syncCalls==QStringList{"file","directory"} && targetAtDirectorySync==executable && stagedAtDirectorySync,"sync directory after replacement and before staged cleanup");
    syncFailure=SyncFailure::None; syncTarget.clear();
    check(Ota::save(target,"old version",&error),"restore atomic replacement fixture");
#endif
    QFile running(target); check(running.open(QIODevice::ReadOnly),"hold running inode open");
    check(Ota::install(staged,target,record).state==Ota::InstallState::Complete,"replace application atomically");
    check(running.readAll()=="old version" && Ota::read(target,100)==executable,"open inode remains valid while new path has new content");
    check(!QFile::exists(staged) && QFileInfo(target).isExecutable(),"installed executable permissions and staged cleanup");
    const auto ownerOnly=QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner;
    check(QFile::setPermissions(target,ownerOnly) && Ota::save(staged,executable,&error),"existing executable with owner-only permissions");
    check(Ota::install(staged,target,record).state==Ota::InstallState::Complete,"replace an already executable application");
    const auto mode=QFileInfo(target).permissions();
    check((mode&QFileDevice::ExeOwner) && !(mode&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::WriteOther|QFileDevice::ExeOther)),"preserve existing execute permissions instead of forcing chmod 0755");
    check(!QFile::exists(root+"/runtime") && !QFile::exists(root+"/update/state.json"),"no slots or rollback state created");
#ifdef Q_OS_LINUX
    check(Ota::isRunningExecutable(QCoreApplication::applicationFilePath()),"running executable confirmed through procfs");
#endif
    check(!Ota::isRunningExecutable(target) && !Ota::isRunningExecutable(root+"/link") && !Ota::isRunningExecutable({}),"unrelated, symlink and missing executable paths rejected");
    Client client(QUrl("https://example.test"),root,nullptr,false);
    check(client.prepareUpdate() && client.busy(),"installation locks client operations");
    client.cancelUpdate(); check(!client.busy(),"cancel releases installation lock");
    const auto abandoned=root+"/update/attempt-01234567-89ab-4cde-8abc-0123456789ab",unknown=root+"/update/attempt-foreign";
    check(QDir().mkpath(abandoned) && QDir().mkpath(unknown) && Ota::save(abandoned+"/archive.zip","owned stale file",&error) &&
          Ota::save(unknown+"/archive.zip","unrelated file",&error),"create owned and unknown update directories");
    UpdateManager updater(&client,root,target);
    bool cleanupDone=false;
    ClientWorkerCheck::executor(client).submit(&app,[](const FileCancellation &) { return true; },[&](bool,bool) { cleanupDone=true; });
    check(waitUntil([&] { return cleanupDone; }) && !QFile::exists(abandoned+"/archive.zip") && Ota::read(unknown+"/archive.zip",100)=="unrelated file",
          "startup cleanup removes owned UUID attempts and preserves unknown directories");
#ifdef BOOKORBIT_FSYNC_CHECK
    for(const auto state:{Ota::InstallState::FailedBeforeReplace,Ota::InstallState::ReplacedUnconfirmed,
                          Ota::InstallState::InstalledCleanupPending,Ota::InstallState::Complete}) {
        const auto directory=root+"/update/attempt-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        check(QDir().mkpath(directory) && Ota::save(directory+"/bookorbit.next",executable,&error) && Ota::save(target,"old version",&error),"manager install fixture");
        syncTarget=target; syncStaged=directory+"/bookorbit.next";
        syncFailure=state==Ota::InstallState::FailedBeforeReplace ? SyncFailure::File :
                    state==Ota::InstallState::ReplacedUnconfirmed ? SyncFailure::Directory : SyncFailure::None;
        cleanupFailure=state==Ota::InstallState::InstalledCleanupPending;
        {
            UpdateManager manager(&client,root,target);
            bool drained=false;
            ClientWorkerCheck::executor(client).submit(&app,[](const FileCancellation &) { return true; },[&](bool,bool) { drained=true; });
            check(waitUntil([&] { return drained; }),"manager recovery settles");
            UpdateManagerCheck::install(manager,directory,record);
            check(waitUntil([&] { return manager.state()!="installing"; }),"real installer result reaches manager");
            const auto expected=state==Ota::InstallState::FailedBeforeReplace ? "ready" :
                                state==Ota::InstallState::ReplacedUnconfirmed ? "unconfirmed" :
                                state==Ota::InstallState::InstalledCleanupPending ? "cleanup_pending" : "installed";
            check(manager.state()==expected && !client.busy(),"manager publishes correct result and releases client");
            check(Ota::read(target,100)==(state==Ota::InstallState::FailedBeforeReplace ? QByteArray("old version") : executable),"manager preserves actual replacement outcome");
            check(QFile::exists(syncStaged)==(state!=Ota::InstallState::Complete),"manager never discards staged executable on an incomplete install");
            syncFailure=SyncFailure::None; cleanupFailure=false;
            if(state==Ota::InstallState::ReplacedUnconfirmed || state==Ota::InstallState::InstalledCleanupPending) {
                if(state==Ota::InstallState::ReplacedUnconfirmed) {
                    check(Ota::save(target,"unexpected executable",&error),"change destination before confirmation");
                    UpdateManagerCheck::install(manager,directory,record,true);
                    check(waitUntil([&] { return manager.state()!="installing"; }) && manager.state()=="unconfirmed" && QFile::exists(syncStaged),"mismatched destination blocks cleanup");
                    check(Ota::save(target,executable,&error),"restore destination for confirmation");
                }
                UpdateManagerCheck::install(manager,directory,record,true);
                check(waitUntil([&] { return manager.state()=="installed"; }) && !QFile::exists(syncStaged),"confirmation or cleanup retry finishes without replacing again");
            }
        }
        bool drained=false;
        ClientWorkerCheck::executor(client).submit(&app,[](const FileCancellation &) { return true; },[&](bool,bool) { drained=true; });
        check(waitUntil([&] { return drained; }),"destructor cleanup settles");
        if(state==Ota::InstallState::FailedBeforeReplace) {
            // A verified attempt remains protected even if the intent write failed.
            check(QFile::exists(syncStaged),"failed-before-replace attempt survives manager destruction");
        }
    }
    syncTarget.clear(); syncStaged.clear();
#endif
    if(argc==2) {
        const auto directory=root+"/update/attempt-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        check(QDir().mkpath(directory),"recovery attempt directory");
        QJsonObject release; Zip archive(QString::fromLocal8Bit(argv[1]));
        check(Ota::stageArchive(QString::fromLocal8Bit(argv[1]),directory+"/bookorbit.next",Ota::version(),&release,&error) &&
              Ota::save(directory+"/release.json",archive.read("release.json",65536),&error) &&
              Ota::save(directory+"/release.sig",archive.read("release.sig",64),&error),"persist production-signed recovery metadata");
#ifdef BOOKORBIT_FSYNC_CHECK
        {
            UpdateManager manager(&client,root,target);
            check(waitUntil([&] { return manager.state()!="recovering"; }),"recovery discovery completes");
            syncTarget=target; syncStaged=directory+"/bookorbit.next"; syncFailure=SyncFailure::Directory;
            UpdateManagerCheck::install(manager,directory,release);
            check(waitUntil([&] { return manager.state()=="unconfirmed"; }),"signed package reaches unconfirmed replacement");
            syncFailure=SyncFailure::None;
        }
        {
            UpdateManager restarted(&client,root,target);
            check(waitUntil([&] { return restarted.state()!="recovering"; }) && restarted.state()=="unconfirmed" && QFile::exists(directory+"/bookorbit.next"),"startup retains and re-verifies signed unconfirmed installation");
            UpdateManagerCheck::install(restarted,directory,release,true);
            check(waitUntil([&] { return restarted.state()=="installed"; }) && !QFile::exists(directory),"restart confirmation cleans only a confirmed installation");
        }
        syncTarget.clear(); syncStaged.clear();
#endif
    }
    const auto package=argc==2 ? QString::fromLocal8Bit(argv[1]) : root+"/valid.zip";
    const auto attempt=[&](const QString &name) {
        const auto directory=root+"/update/attempt-"+name;
        check(QDir().mkpath(directory) && QFile::copy(package,directory+"/archive.zip"),"create isolated verification attempt");
        return directory;
    };
    {
        auto &executor=ClientWorkerCheck::executor(client);
        WorkerGate gate(executor,&app);
        check(waitUntil([&] { return gate.entered->load(); }),"hold OTA worker");
        const auto old=attempt("cancelled"); UpdateManagerCheck::verify(updater,old);
        int ticks=0; QTimer timer; QObject::connect(&timer,&QTimer::timeout,[&] { ++ticks; }); timer.start(5);
        check(updater.state()=="verifying" && waitUntil([&] { return ticks>=5; }),"OTA verification publishes state while GUI remains responsive");
        updater.cancel(); check(updater.state()=="available","verification can be cancelled before worker starts");
        WorkerGate next(executor,&app);
        const auto fresh=attempt("fresh"); UpdateManagerCheck::verify(updater,fresh);
        gate.open(); check(waitUntil([&] { return next.entered->load(); }),"old cancelled result drains");
        QCoreApplication::processEvents();
        check(updater.state()=="verifying" && !QFile::exists(old+"/bookorbit.next") && QFile::exists(fresh+"/archive.zip"),
              "cancelled verification cannot mark a new candidate ready or remove its files");
        next.open(); check(waitUntil([&] { return updater.state()!="verifying"; }),"fresh OTA verification completes");
        check(updater.state()==(argc==2 ? "ready" : "error"),"only the current verified signed package becomes ready");
    }
    {
        auto &executor=ClientWorkerCheck::executor(client);
        WorkerGate gate(executor,&app);
        check(waitUntil([&] { return gate.entered->load(); }),"hold timed-out verification");
        const auto expired=attempt("timeout"); UpdateManagerCheck::verify(updater,expired);
        UpdateManagerCheck::timeout(updater); check(updater.state()=="error","overall timeout cancels package verification");
        bool drained=false;
        executor.submit(&app,[](const FileCancellation &) { return true; },[&](bool,bool) { drained=true; });
        gate.open(); check(waitUntil([&] { return drained; }) && updater.state()=="error" && !QFile::exists(expired+"/bookorbit.next"),
                           "late verification after timeout never publishes ready and removes its own files");
    }
    cacheRegressions(client,root);
    check(!updateDeviceError().isEmpty(),"real desktop backend disallows PocketBook installation");
    check(QDir().mkpath(root+"/update") && Ota::save(root+"/update/bookorbit.next",executable,&error),"verified PocketBook executable staged for desktop guard");
    UpdateManagerCheck::ready(updater,record);
    const auto previous=Ota::hashFile(target), own=Ota::hashFile(QCoreApplication::applicationFilePath());
    check(!updater.canInstall(),"ready PocketBook package exposes no desktop install action");
    updater.install();
    check(updater.message()==updateDeviceError() && !client.busy() && Ota::hashFile(target)==previous && Ota::hashFile(QCoreApplication::applicationFilePath())==own && QFile::exists(root+"/update/bookorbit.next"),"desktop install rejected before touching executable or staged package");
    {
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("client",&client);
        engine.rootContext()->setContextProperty("updateManager",&updater);
        engine.rootContext()->setContextProperty("screenWidth",600);
        engine.rootContext()->setContextProperty("screenHeight",800);
        engine.load(QUrl("qrc:/Main.qml"));
        check(!engine.rootObjects().isEmpty(),"production QML loaded for desktop install guard");
        auto *window=engine.rootObjects().first(); window->setProperty("settings",true);
        auto *button=window->findChild<QObject*>("installUpdateButton");
        check(button && !button->property("visible").toBool(),"install action hidden for ready package on desktop");
    }
    check(!QFile::exists(client.diagnosticLogPath()),"diagnostics off writes no log");
    check(client.setDiagnosticLogging(true),"enable diagnostics");
    updater.windowReady(); updater.install(); updater.check();
    auto log=Ota::read(client.diagnosticLogPath(),128*1024);
    check(log.contains("ota.context") && log.contains("ota.install.requested") && log.contains("ota.install.blocked") && log.contains("ota.check.ignored"),"OTA context, user action and install denial reach the enabled log");
    qint64 sequence=0;
    for(const auto &line:log.split('\n')) {
        if(line.isEmpty()) continue;
        const auto entry=QJsonDocument::fromJson(line).object();
        check(!entry.isEmpty() && entry["seq"].toInteger()>sequence && entry.contains("elapsedMs") && entry.contains("pid") && entry.contains("version"),"diagnostic events have time, identity and monotonic order");
        sequence=entry["seq"].toInteger();
    }
    check(client.setDiagnosticLogging(false),"disable diagnostics");
    log=Ota::read(client.diagnosticLogPath(),128*1024);
    check(log.contains("diagnostics disabled"),"disabling logging is recorded");
    updater.install(); check(Ota::read(client.diagnosticLogPath(),128*1024)==log,"disabled diagnostics do not append OTA events");
    client.setDiagnosticLogging(true);
    check(Ota::save(client.diagnosticLogPath(),QByteArray(129*1024,'x'),&error),"rotation fixture");
    client.logDiagnostic("rotation.check");
    check(QFileInfo(client.diagnosticLogPath()+".1").size()==129*1024 && Ota::read(client.diagnosticLogPath(),4096).contains("rotation.check"),"bounded log rotates and continues writing");
    QFile::remove(client.diagnosticLogPath()); QDir().mkdir(client.diagnosticLogPath());
    client.logDiagnostic("write.failure"); check(!client.diagnosticError().isEmpty(),"unwritable log has visible error");
    QDir().rmdir(client.diagnosticLogPath()); client.logDiagnostic("write.recovered");
    check(client.diagnosticError().isEmpty() && Ota::read(client.diagnosticLogPath(),4096).contains("write.recovered"),"logging resumes after storage recovery");
    return 0;
}
