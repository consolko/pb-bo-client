#include "ota.h"
#include "zip.h"
#include "client.h"
#include "update.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>

struct UpdateManagerCheck {
    static void ready(UpdateManager &updater, const QJsonObject &manifest) {
        updater.prepared=manifest; updater.phase="ready";
    }
};

#ifdef BOOKORBIT_FSYNC_CHECK
enum class SyncFailure { None, File, Directory };
static SyncFailure syncFailure=SyncFailure::None;
static QStringList syncCalls;
static QString syncTarget,syncStaged;
static QByteArray targetAtDirectorySync;
static bool stagedAtDirectorySync=false;
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
    if((directory && syncFailure==SyncFailure::Directory) || (!directory && syncFailure==SyncFailure::File)) {
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
        check(Ota::install(root+"/release.next",root+"/release.app",release,&error),"signed release installs into isolated storage");
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
    check(!Ota::install(staged,root+"/link",record,&error),"installation rejects symlink destination");
    auto corrupted=record; corrupted["sha256"]=QString(64,'0');
    check(!Ota::install(staged,target,corrupted,&error) && Ota::read(target,100)=="old version","recheck before replacement preserves old executable on corruption");
#ifdef BOOKORBIT_FSYNC_CHECK
    syncCalls.clear(); syncFailure=SyncFailure::File;
    check(!Ota::install(staged,target,record,&error) && error.startsWith("install.sync_file:") && Ota::read(target,100)=="old version" && QFile::exists(staged),"file fsync failure preserves installed and staged executables");
    check(syncCalls==QStringList{"file"},"failed file sync never advances to directory sync");
    syncCalls.clear(); syncFailure=SyncFailure::Directory; syncTarget=target; syncStaged=staged;
    check(!Ota::install(staged,target,record,&error) && error.startsWith("install.sync_directory:") && Ota::read(target,100)==executable && QFile::exists(staged),"directory fsync failure reports unconfirmed replacement and retains staged executable");
    check(syncCalls==QStringList{"file","directory"} && targetAtDirectorySync==executable && stagedAtDirectorySync,"sync directory after replacement and before staged cleanup");
    syncFailure=SyncFailure::None; syncTarget.clear();
    check(Ota::save(target,"old version",&error),"restore atomic replacement fixture");
#endif
    QFile running(target); check(running.open(QIODevice::ReadOnly),"hold running inode open");
    check(Ota::install(staged,target,record,&error),"replace application atomically");
    check(running.readAll()=="old version" && Ota::read(target,100)==executable,"open inode remains valid while new path has new content");
    check(!QFile::exists(staged) && QFileInfo(target).isExecutable(),"installed executable permissions and staged cleanup");
    const auto ownerOnly=QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner;
    check(QFile::setPermissions(target,ownerOnly) && Ota::save(staged,executable,&error),"existing executable with owner-only permissions");
    check(Ota::install(staged,target,record,&error),"replace an already executable application");
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
    UpdateManager updater(&client,root,target);
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
