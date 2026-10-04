#include "ota.h"
#include "zip.h"
#include "client.h"
#include "update.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

// Client device stubs keep this check independent of reader hardware.
bool connectNetwork() { return false; }
bool networkConnected() { return false; }
QString updateDeviceError(QJsonObject *details) { if(details) *details={{"firmware","test"}}; return {}; }
bool openReader(const QString&) { return false; }
void scanBook(const QString&) {}
bool readerBookIndexed(const QString&) { return false; }
ReaderFileState readerFileState(const QString&) { return ReaderFileState::Closed; }
bool readerPosition(const QString&,QString*) { return false; }
QString readerProfile() { return {}; }
ReaderRecents readerRecents(const QStringList&) { return {}; }
bool saveReaderPosition(const QString&,const QString&,const QString&,const QString&,QString*) { return false; }

static void check(bool ok,const char *label) { std::printf("%s: %s\n",ok?"PASS":"FAIL",label); if(!ok) std::exit(1); }
static void put16(QByteArray &b,quint16 v) { b.append(char(v)); b.append(char(v>>8)); }
static void put32(QByteArray &b,quint32 v) { put16(b,v); put16(b,v>>16); }
static QByteArray archive(const QByteArray &name,const QByteArray &data) {
    QByteArray b; const auto crc=crc32(0,reinterpret_cast<const Bytef*>(data.constData()),data.size());
    put32(b,0x04034b50); put16(b,20); put16(b,0); put16(b,0); put32(b,0); put32(b,crc); put32(b,data.size()); put32(b,data.size()); put16(b,name.size()); put16(b,0); b+=name; b+=data;
    const auto offset=b.size();
    put32(b,0x02014b50); put16(b,20); put16(b,20); put16(b,0); put16(b,0); put32(b,0); put32(b,crc); put32(b,data.size()); put32(b,data.size()); put16(b,name.size()); put16(b,0); put16(b,0); put16(b,0); put16(b,0); put32(b,0); put32(b,0); b+=name;
    const auto central=b.size()-offset;
    put32(b,0x06054b50); put16(b,0); put16(b,0); put16(b,1); put16(b,1); put32(b,central); put32(b,offset); put16(b,0); return b;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); QTemporaryDir temp; check(temp.isValid(),"temporary root");
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
    Client client(QUrl("https://example.test"),root,nullptr,false);
    check(client.prepareUpdate() && client.busy(),"installation locks client operations");
    client.cancelUpdate(); check(!client.busy(),"cancel releases installation lock");
    UpdateManager updater(&client,root,target);
    check(!QFile::exists(client.diagnosticLogPath()),"diagnostics off writes no log");
    check(client.setDiagnosticLogging(true),"enable diagnostics");
    updater.windowReady(); updater.install(); updater.check();
    auto log=Ota::read(client.diagnosticLogPath(),128*1024);
    check(log.contains("ota.context") && log.contains("ota.install.requested") && log.contains("ota.install.ignored") && log.contains("ota.failed"),"OTA context, user action and failure reach the enabled log");
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
