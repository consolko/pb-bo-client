#include "update.h"
#include "client.h"
#include "device.h"
#include "ota.h"
#include "zip.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QLocale>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QSaveFile>
#include <QStorageInfo>
#include <QRegularExpression>
#include <QTimer>
#include <memory>
#include <QUuid>
#include <tuple>
#include <utility>
#include <cstdio>

// Query strings may contain GitHub signed asset credentials. Never record them.
static QString logUrl(const QUrl &url) { return url.adjusted(QUrl::RemoveUserInfo|QUrl::RemoveQuery|QUrl::RemoveFragment).toString().left(512); }
static QJsonObject storageDetails(const QString &path) {
    const QStorageInfo storage(path);
    return {{"path",path},{"filesystem",QString::fromLatin1(storage.fileSystemType())},{"valid",storage.isValid()},
            {"readonly",storage.isReadOnly()},{"freeBytes",storage.bytesAvailable()}};
}
constexpr qint64 maxUpdateSettings=64*1024,maxReleaseCache=128*1024,maxReleaseResponse=1024*1024,maxLegacySettings=2*1024*1024;
static QString trUpdate(const char *text) { return QCoreApplication::translate("BookOrbit",text); }
UpdateManager::UpdateManager(Client *c,QString data,QString path,QObject *parent)
    : QObject(parent),client(c),root(std::move(data)),executable(std::move(path)),apiBase("https://api.github.com") {
    network.setTransferTimeout(30000);
    const bool directoryReady=QDir().mkpath(root+"/update");
    auto preferences=QJsonDocument::fromJson(Ota::read(root+"/update/preferences.json",maxUpdateSettings)).object();
    if(preferences.isEmpty()) {
        const auto legacy=QJsonDocument::fromJson(Ota::read(root+"/update/preferences.json",maxLegacySettings)).object();
        if(!legacy.contains("schema")) preferences=legacy;
    }
    settings={{"schema",1},{"automatic",preferences["automatic"].toBool(true)}};
    cache=normalizeCache(QJsonDocument::fromJson(Ota::read(root+"/update/release-cache.json",maxReleaseCache)).object());
    if(preferences.contains("release") && cache.isEmpty()) cache=normalizeCache(preferences);
    if(!preferences.isEmpty() && preferences!=settings && persist()) persistCache();
    const auto abandoned=QDir(root+"/update").entryInfoList({"attempt-*"},QDir::Dirs|QDir::NoDotAndDotDot);
    client->files.submit(this,[abandoned](const FileCancellation &cancel) {
        for(const auto &entry:abandoned) {
            if(fileCancelled(cancel)) break;
            if(entry.isSymLink() || QUuid::fromString(entry.fileName().mid(8)).isNull()) continue;
            // A staged file may be the only remaining copy after a replacement.
            if(QFileInfo::exists(entry.absoluteFilePath()+"/bookorbit.next") ||
               QFileInfo::exists(entry.absoluteFilePath()+"/release.json") ||
               QFileInfo::exists(entry.absoluteFilePath()+"/install.json")) continue;
            QFile::remove(entry.absoluteFilePath()+"/archive.zip");
            QDir().rmdir(entry.absoluteFilePath());
        }
        return true;
    },[](bool,bool) {});
    recoverAttempts();
    traceContext();
    if(!directoryReady) trace("directory.failed",{{"path",root+"/update"}});
    connect(client,&Client::diagnosticsEnabled,this,&UpdateManager::traceContext);
    totalDeadline.setSingleShot(true);
    connect(&totalDeadline,&QTimer::timeout,this,[this] { fail(trUpdate("The update request timed out.")); });

}
UpdateManager::~UpdateManager() {
    if(fileTask) fileTask->store(true);
    discardAttempt();
}
void UpdateManager::discardAttempt() {
    if(attemptDir.isEmpty()) return;
    const auto directory=std::exchange(attemptDir,{});
    client->files.submit(this,[directory](const FileCancellation &) {
        if(QFileInfo::exists(directory+"/release.json") || QFileInfo::exists(directory+"/install.json")) return false;
        QFile::remove(directory+"/archive.zip"); QFile::remove(directory+"/bookorbit.next");
        return QDir().rmdir(directory);
    },[](bool,bool) {});
}
QString UpdateManager::version() const { return Ota::version(); }
bool UpdateManager::canInstall() const {
#ifdef POCKETBOOK_DEVICE
    return phase=="ready" || phase=="unconfirmed" || phase=="cleanup_pending";
#else
    return false;
#endif
}
QString UpdateManager::lastChecked() const {
    const auto value=cache["lastChecked"].toInteger();
    return value>0 ? QLocale().toString(QDateTime::fromSecsSinceEpoch(value),QLocale::ShortFormat) : QString{};
}
void UpdateManager::trace(const QString &event,QJsonObject fields) {
    fields["state"]=phase; fields["candidate"]=candidateVersion; fields["attempt"]=generation;
    client->logDiagnostic("ota."+event,fields);
}
void UpdateManager::traceContext() {
    QJsonObject device; const auto blocker=updateDeviceError(&device);
    trace("context",{{"installed",version()},{"executable",executable},{"source",logUrl(apiBase)},
          {"device",device},{"installBlocker",blocker},
          {"storage",storageDetails(root)},{"stagedBytes",attemptDir.isEmpty() ? 0 : QFileInfo(attemptDir+"/bookorbit.next").size()}});
}
bool UpdateManager::persist() {
    QString error; const bool ok=QJsonDocument(settings).toJson(QJsonDocument::Compact).size()<=maxUpdateSettings && Ota::saveJson(root+"/update/preferences.json",settings,&error);
    if(!ok) trace("settings.failed",{{"detail",error}});
    return ok;
}
bool UpdateManager::persistCache() {
    QString error;
    const bool ok=QJsonDocument(cache).toJson(QJsonDocument::Compact).size()<=maxReleaseCache &&
        Ota::saveJson(root+"/update/release-cache.json",cache,&error);
    if(!ok) trace("cache.failed",{{"detail",error}});
    return ok;
}
QJsonObject UpdateManager::normalizeRelease(const QJsonObject &release) const {
    const auto tag=release["tag_name"].toString();
    if(!release["draft"].isBool() || !release["prerelease"].isBool() || release["draft"].toBool() ||
       release["prerelease"].toBool() || !tag.startsWith('v') || !Ota::validVersion(tag.mid(1)) || !release["assets"].isArray()) return {};
    QJsonArray assets; QSet<QString> names;
    for(const auto &value:release["assets"].toArray()) {
        const auto asset=value.toObject(); const auto name=asset["name"].toString();
        if(name!="bookorbit-pb634.zip" && name!="SHA256SUMS") continue;
        const auto url=asset["browser_download_url"].toString(); const auto bytes=asset["size"];
        if(names.contains(name) || url.size()>4096 || !allowed(QUrl(url)) || !bytes.isDouble() ||
           bytes.toDouble()!=bytes.toInteger() || bytes.toInteger()<1 ||
           bytes.toInteger()>(name=="SHA256SUMS" ? 4096 : Ota::maxArchive)) return {};
        names.insert(name);
        assets.append(QJsonObject{{"name",name},{"size",bytes},{"browser_download_url",url}});
    }
    if(Ota::newer(tag.mid(1),version()) && names.size()!=2) return {};
    return {{"tag_name",tag},{"draft",false},{"prerelease",false},{"body",release["body"].toString().left(12000)},{"assets",assets}};
}
QJsonObject UpdateManager::normalizeCache(const QJsonObject &value) const {
    if(value["source"]!=apiBase.toString()) return {};
    QJsonObject normalized{{"schema",1},{"source",apiBase.toString()}};
    for(const auto *key:{"lastChecked","lastAutomatic","retryAfter"}) {
        const auto field=value[key];
        if(field.isDouble() && field.toDouble()==field.toInteger() && field.toInteger()>=0)
            normalized[key]=qMin(field.toInteger(),QDateTime::currentSecsSinceEpoch()+86400);
    }
    if(value.contains("release")) {
        const auto release=normalizeRelease(value["release"].toObject());
        if(release.isEmpty()) return {};
        normalized["release"]=release;
        const auto etag=value["etag"].toString();
        if(etag.toUtf8().size()<=1024 && !etag.contains(QRegularExpression("[\\x00-\\x20\\x7f]"))) normalized["etag"]=etag;
    }
    return QJsonDocument(normalized).toJson(QJsonDocument::Compact).size()<=maxReleaseCache ? normalized : QJsonObject{};
}
void UpdateManager::setAutomatic(bool value) {
    const auto previous=settings; settings["automatic"]=value;
    if(!persist()) { settings=previous; notice=trUpdate("Could not save update settings."); }
    trace("automatic.setting",{{"enabled",automatic()}}); emit changed();
}
void UpdateManager::windowReady() {
    if(startupDone) return;
    startupDone=true; trace("ui.ready"); emit startupCommitted();
    QTimer::singleShot(2000,this,&UpdateManager::automaticCheck);
}
void UpdateManager::automaticCheck() {
    if(automaticAttempted || !automatic() || !networkConnected() || !startupDone) { trace("automatic.skipped",{{"enabled",automatic()},{"connected",networkConnected()},{"startupDone",startupDone}}); return; }
    automaticAttempted=true;
    const auto now=QDateTime::currentSecsSinceEpoch(),last=cache["lastAutomatic"].toInteger();
    if(last>0 && now-last<86400) { trace("automatic.skipped",{{"reason","24h interval"},{"last",last},{"now",now}}); return; }
    cache["source"]=apiBase.toString(); cache["lastAutomatic"]=now; persistCache();
    checkRelease(false);
}
void UpdateManager::check() { checkRelease(true); }
void UpdateManager::fail(const QString &error) {
    trace("failed",{{"reason",error}});
    ++generation; if(fileTask) fileTask->store(true); fileTask.reset(); totalDeadline.stop(); if(active) active->abort(); active.clear();
    discardAttempt();
    phase="error"; notice=error; emit changed();
}
bool UpdateManager::allowed(const QUrl &url) const {
#ifdef BOOKORBIT_TEST_HTTP
    if(url.scheme()=="http" && url.host()=="127.0.0.1" && apiBase.host()==url.host() &&
       url.port()==apiBase.port() && url.userInfo().isEmpty() && !url.hasFragment()) return true;
#endif
    if(!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    return url.scheme()=="https" && url.port(443)==443 &&
        QStringList{"api.github.com","github.com","release-assets.githubusercontent.com","objects.githubusercontent.com"}.contains(url.host());
}
void UpdateManager::fetch(QUrl url,qint64 limit,std::function<void(QByteArray,int,QString)> done,int redirects,bool conditional) {
    if(!allowed(url)||redirects<0) { trace("url.rejected",{{"url",logUrl(url)},{"redirectsLeft",redirects}}); fail(trUpdate("The update server returned an unsafe download address.")); return; }
    QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("User-Agent",("BookOrbit/"+version()).toUtf8());
    request.setRawHeader("Accept","application/vnd.github+json"); request.setRawHeader("X-GitHub-Api-Version","2022-11-28");
    const bool latest=url.path().endsWith("/releases/latest");
    if(latest && conditional && !cache["release"].toObject().isEmpty() && !cache["etag"].toString().isEmpty()) request.setRawHeader("If-None-Match",cache["etag"].toString().toUtf8());
    trace("http.request",{{"url",logUrl(url)},{"redirectsLeft",redirects}});
    auto *reply=network.get(request); active=reply; const int run=generation;
    auto data=std::make_shared<QByteArray>();
    connect(reply,&QNetworkReply::readyRead,this,[=,this] {
        if(run!=generation) return;
        if(data->size()+reply->bytesAvailable()>limit) { fail(trUpdate("The update response is too large.")); return; }
        data->append(reply->readAll());
    });
    connect(reply,&QNetworkReply::finished,this,[=,this] {
        reply->deleteLater(); if(run!=generation) return; active.clear();
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        trace("http.finished",{{"url",logUrl(url)},{"http",status},{"networkError",int(reply->error())}});
        if(status>=300 && status<400 && status!=304) { fetch(url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl()),limit,done,redirects-1,conditional); return; }
        if(status==403 || status==429) {
            bool ok=false; const auto seconds=reply->rawHeader("Retry-After").toLongLong(&ok);
            const auto now=QDateTime::currentSecsSinceEpoch();
            cache["source"]=apiBase.toString(); cache["retryAfter"]=ok ? now+qBound(qint64(60),seconds,qint64(86400)) : qMax(now+60,qMin(now+86400,reply->rawHeader("X-RateLimit-Reset").toLongLong())); persistCache();
        }
        if(status!=200 && status!=304) { fail(status==404 ? trUpdate("The published release is unavailable.") : trUpdate("Could not check updates. Try again later.")); return; }
        if(reply->error()!=QNetworkReply::NoError) { fail(trUpdate("The update connection failed.")); return; }
        const auto remaining=reply->readAll();
        if(data->size()+remaining.size()>limit) { fail(trUpdate("The update response is too large.")); return; }
        data->append(remaining);
        if(status==304 && (!latest || cache["release"].toObject().isEmpty())) {
            if(latest && conditional) { fetch(url,limit,done,redirects,false); return; }
            fail(trUpdate("The release information is invalid.")); return;
        }
        done(*data,status,QString::fromUtf8(reply->rawHeader("ETag")));
    });
}
void UpdateManager::checkRelease(bool manual) {
    if(active || phase=="checking" || phase=="downloading" || phase=="verifying" || phase=="installing" || phase=="ready" || phase=="unconfirmed" || phase=="cleanup_pending" || phase=="recovering" || !startupDone) { trace("check.ignored"); return; }
    if(QDateTime::currentSecsSinceEpoch()<cache["retryAfter"].toInteger()) { if(manual) fail(trUpdate("The update server asked to wait. Try again later.")); return; }
    if(manual && !connectNetwork()) { fail(trUpdate("Connect to the network to check updates.")); return; }
    trace("check.requested",{{"manual",manual}});
    ++generation; phase="checking"; notice.clear(); prepared={}; candidateVersion.clear(); releaseNotes.clear(); emit changed(); totalDeadline.start(30000);
    QUrl endpoint(apiBase.toString()+"/repos/consolko/pb-bo-client/releases/latest");
    fetch(endpoint,maxReleaseResponse,[this](QByteArray data,int code,QString etag) { receiveRelease(data,code,etag); });
}
void UpdateManager::receiveRelease(QByteArray data,int code,const QString &etag) {
    totalDeadline.stop();
    const auto release=normalizeRelease(code==304 ? cache["release"].toObject() : QJsonDocument::fromJson(data).object());
    if(release.isEmpty()) { fail(trUpdate("The release information is invalid.")); return; }
    QJsonObject next=cache; next["source"]=apiBase.toString(); next["release"]=release;
    next["lastChecked"]=QDateTime::currentSecsSinceEpoch();
    if(code==200) next["etag"]=etag;
    cache=normalizeCache(next); persistCache();
    const auto tag=release["tag_name"].toString(); trace("release.received",{{"tag",tag},{"http",code}});
    if(!Ota::newer(tag.mid(1),version())) { trace("release.current"); phase="idle"; notice=trUpdate("You have the latest version."); emit changed(); return; }
    for(const auto &value:release["assets"].toArray()) {
        const auto asset=value.toObject();
        if(asset["name"]=="bookorbit-pb634.zip") { archiveSize=asset["size"].toInteger(); zipUrl=QUrl(asset["browser_download_url"].toString()); }
        else checksumsUrl=QUrl(asset["browser_download_url"].toString());
    }
    candidateVersion=tag.mid(1); releaseNotes=release["body"].toString(); phase="available";
    trace("release.available",{{"archiveBytes",archiveSize}});
    notice=trUpdate("An application update is available."); emit changed();
}

void UpdateManager::cancel() {
    if(phase!="checking" && phase!="downloading" && phase!="verifying") return;
    trace("cancelled",{{"progress",fraction}});
    ++generation; if(fileTask) fileTask->store(true); fileTask.reset(); totalDeadline.stop(); if(active) active->abort(); active.clear();
    discardAttempt();
    phase=candidateVersion.isEmpty()?"idle":"available"; notice=trUpdate("Update download cancelled."); emit changed();
}
void UpdateManager::download() {
    if(phase!="available" || active) { trace("download.ignored"); return; }
    trace("download.requested",{{"storage",storageDetails(root)}});
    const QStorageInfo storage(root);
    if(!storage.isValid()||storage.isReadOnly()||storage.bytesAvailable()<archiveSize+2*Ota::maxClient+8*1024*1024) { fail(trUpdate("Not enough writable space for the update.")); return; }
    if(!connectNetwork()) { fail(trUpdate("Connect to the network to download the update.")); return; }
    attemptDir=root+"/update/attempt-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!QDir().mkpath(attemptDir)) { fail(trUpdate("Could not save the update download.")); return; }
    ++generation; phase="downloading"; fraction=0; notice.clear(); emit changed(); totalDeadline.start(600000);
    fetch(checksumsUrl,4096,[this](QByteArray data,int,QString) {
        zipDigest.clear();
        for(const auto &line:data.split('\n')) {
            const auto fields=QString::fromLatin1(line).simplified().split(' ');
            if(fields.size()==2 && fields[1]=="bookorbit-pb634.zip") {
                if(!zipDigest.isEmpty()) { fail(trUpdate("Invalid archive checksum.")); return; }
                zipDigest=fields[0];
            }
        }
        if(!QRegularExpression("^[a-f0-9]{64}$").match(zipDigest).hasMatch()) { fail(trUpdate("Invalid archive checksum.")); return; }
        trace("checksum.received",{{"sha256",zipDigest}}); downloadZip(zipUrl);
    });
}
void UpdateManager::downloadZip(QUrl url,int redirects) {
    if(!allowed(url)||redirects<0) { trace("url.rejected",{{"url",logUrl(url)},{"redirectsLeft",redirects}}); fail(trUpdate("The update server returned an unsafe download address.")); return; }
    auto out=std::make_shared<QSaveFile>(attemptDir+"/archive.zip"); out->setDirectWriteFallback(false);
    if(!out->open(QIODevice::WriteOnly)) { trace("archive.open.failed",{{"detail",out->errorString()}}); fail(out->errorString()); return; }
    auto hash=std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto size=std::make_shared<qint64>(0);
    QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("User-Agent",("BookOrbit/"+version()).toUtf8());
    trace("http.request",{{"url",logUrl(url)},{"redirectsLeft",redirects}});
    auto *reply=network.get(request); active=reply; const int run=generation;
    reply->setReadBufferSize(65536);
    connect(reply,&QNetworkReply::readyRead,this,[=,this] {
        if(run!=generation) return;
        const auto data=reply->readAll();
        if(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()!=200) return;
        *size+=data.size();
        if(*size>archiveSize || out->write(data)!=data.size()) { trace("archive.write.failed",{{"bytes",*size},{"expectedBytes",archiveSize},{"detail",out->errorString()}}); fail(trUpdate("Could not save the update download.")); return; }
        hash->addData(data); const double next=double(*size)/archiveSize;
        if(int(next*4)>int(fraction*4)) trace("download.progress",{{"bytes",*size},{"expectedBytes",archiveSize}});
        if(next-fraction>=0.01 || next==1) { fraction=next; emit changed(); }
    });
    connect(reply,&QNetworkReply::finished,this,[=,this] {
        reply->deleteLater(); if(run!=generation) return; active.clear();
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        trace("http.finished",{{"url",logUrl(url)},{"http",status},{"networkError",int(reply->error())}});
        if(status>=300 && status<400) { out->cancelWriting(); downloadZip(url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl()),redirects-1); return; }
        const auto actual=QString::fromLatin1(hash->result().toHex());
        trace("archive.received",{{"bytes",*size},{"expectedBytes",archiveSize},{"sha256",actual},{"expectedSha256",zipDigest}});
        if(reply->error()!=QNetworkReply::NoError || status!=200 || *size!=archiveSize || actual!=zipDigest) { fail(trUpdate("The update archive is incomplete or damaged.")); return; }
        if(!out->commit()) { trace("archive.commit.failed",{{"detail",out->errorString()}}); fail(trUpdate("Could not save the update download.")); return; }
        verifyArchive();
    });
}
void UpdateManager::verifyArchive() {
    const auto run=generation;
    phase="verifying"; notice=trUpdate("Verifying the update package…"); trace("verify.started"); emit changed();
    const auto directory=attemptDir,version=candidateVersion;
    fileTask=client->files.submit(this,[directory,version](const FileCancellation &cancel) {
        QJsonObject prepared; QString error;
        bool ok=Ota::stageArchive(directory+"/archive.zip",directory+"/bookorbit.next",version,&prepared,&error);
        if(ok && !fileCancelled(cancel)) {
            Zip archive(directory+"/archive.zip");
            ok=Ota::save(directory+"/release.json",archive.read("release.json",65536),&error) &&
               Ota::save(directory+"/release.sig",archive.read("release.sig",64),&error);
        }
        QFile::remove(directory+"/archive.zip");
        if(!ok || fileCancelled(cancel)) {
            QFile::remove(directory+"/bookorbit.next"); QFile::remove(directory+"/release.json");
            QFile::remove(directory+"/release.sig"); QDir().rmdir(directory);
        }
        return std::make_tuple(ok,prepared,error);
    },[this,run](const auto &result,bool cancelled) {
        if(run!=generation || cancelled) return;
        fileTask.reset(); const auto &[ok,manifest,error]=result;
        if(!ok) {
            trace("verify.failed",{{"detail",error}});
            fail(trUpdate("The update package could not be verified.")); return;
        }
        prepared=manifest; totalDeadline.stop(); phase="ready";
        trace("verify.ready",{{"sha256",prepared["sha256"]},{"bytes",prepared["bytes"]}});
        notice=canInstall() ? trUpdate("The update is verified. Press Install and close to apply it.") : updateDeviceError(); emit changed();
    });
}

void UpdateManager::install() {
    trace("install.requested",{{"canInstall",canInstall()},{"busy",client->busy()},{"destination",executable},{"permissions",int(QFileInfo(executable).permissions())},{"storage",storageDetails(QFileInfo(executable).absolutePath())}});
    QJsonObject device; const auto deviceError=updateDeviceError(&device); trace("install.device",device);
    if(!deviceError.isEmpty()) { trace("install.blocked",{{"reason",deviceError}}); notice=deviceError; emit changed(); return; }
    if(!canInstall()) { trace("install.ignored"); return; }
    const bool confirmOnly=phase=="unconfirmed" || phase=="cleanup_pending";
    if(!confirmOnly && !Ota::isRunningExecutable(executable)) {
        trace("install.blocked",{{"reason","unverified executable path"}});
        notice=trUpdate("Cannot safely identify the running application. Update installation is blocked."); emit changed(); return;
    }
    if(active || !client->prepareUpdate()) { trace("install.blocked",{{"reason","client busy"}}); notice=trUpdate("Finish the current operation before updating."); emit changed(); return; }
    installPrepared(confirmOnly);
}
void UpdateManager::recoverAttempts() {
    phase="recovering";
    const auto run=generation;
    const auto destination=executable;
    const auto entries=QDir(root+"/update").entryInfoList({"attempt-*"},QDir::Dirs|QDir::NoDotAndDotDot);
    client->files.submit(this,[entries,destination](const FileCancellation &cancel) {
        for(const auto &entry:entries) {
            if(fileCancelled(cancel)) break;
            const auto directory=entry.absoluteFilePath();
            if(QUuid::fromString(entry.fileName().mid(8)).isNull() || !Ota::safeDirectory(directory)) continue;
            QJsonObject manifest; QString error;
            if(!Ota::verifyManifest(Ota::read(directory+"/release.json",65536),Ota::read(directory+"/release.sig",64),&manifest,&error)) continue;
            const auto marker=QJsonDocument::fromJson(Ota::read(directory+"/install.json",4096)).object();
            const bool installed=marker["destination"]==destination && Ota::hashFile(destination)==manifest["sha256"].toString();
            const QFileInfo staged(directory+"/bookorbit.next");
            const bool verified=staged.isFile() && !staged.isSymLink() && staged.size()==manifest["bytes"].toInteger() &&
                Ota::hashFile(staged.absoluteFilePath())==manifest["sha256"].toString();
            if(installed || (verified && Ota::newer(manifest["version"].toString(),Ota::version())))
                return std::make_tuple(directory,manifest,installed);
        }
        return std::make_tuple(QString{},QJsonObject{},false);
    },[this,run](const auto &result,bool cancelled) {
        if(cancelled || run!=generation) return;
        const auto &[directory,manifest,installed]=result;
        if(!directory.isEmpty()) {
            attemptDir=directory; prepared=manifest; candidateVersion=manifest["version"].toString();
            phase=installed ? "unconfirmed" : "ready";
            notice=installed ? trUpdate("The application was replaced. Confirm saving the update before removing the prepared file.") :
                              trUpdate("A verified update was retained. Retry installation when ready.");
        } else phase="idle";
        emit changed();
    });
}
void UpdateManager::installPrepared(bool confirmOnly) {
    phase="installing"; notice=trUpdate("Installing the update…"); trace("install.started"); emit changed();
    const auto directory=attemptDir,destination=executable; const auto manifest=prepared;
    fileTask=client->files.submit(this,[directory,destination,manifest,confirmOnly](const FileCancellation &) {
        // Finish the atomic replacement even when the application is closing.
        FileCancellationScope uninterruptible({});
        QString error;
        if(!confirmOnly && !Ota::saveJson(directory+"/install.json",{{"destination",destination}},&error))
            return Ota::InstallResult{Ota::InstallState::FailedBeforeReplace,error};
        auto result=confirmOnly ? Ota::finishInstall(directory+"/bookorbit.next",destination,manifest) :
                                 Ota::install(directory+"/bookorbit.next",destination,manifest);
        if(result.state==Ota::InstallState::Complete) {
            bool clean=true;
            for(const auto *name:{"release.json","release.sig","install.json"})
                if(QFileInfo::exists(directory+"/"+name) && !QFile::remove(directory+"/"+name)) clean=false;
            if(!clean || !QDir().rmdir(directory)) result={Ota::InstallState::InstalledCleanupPending,"install.cleanup: cannot remove attempt"};
        }
        return result;
    },[this](const Ota::InstallResult &result,bool) {
        fileTask.reset(); client->cancelUpdate();
        trace("install.result",{{"result",int(result.state)},{"detail",result.detail}});
        switch(result.state) {
        case Ota::InstallState::FailedBeforeReplace:
            phase="ready"; notice=trUpdate("Installation stopped before replacement. The prepared update was kept; retry installation."); break;
        case Ota::InstallState::ReplacedUnconfirmed:
            phase="unconfirmed"; notice=trUpdate("The application was replaced, but saving was not confirmed. The prepared update was kept; retry confirmation."); break;
        case Ota::InstallState::InstalledCleanupPending:
            phase="cleanup_pending"; notice=trUpdate("Update installed, but temporary files could not be removed. Retry cleanup."); break;
        case Ota::InstallState::Complete:
            attemptDir.clear(); phase="installed";
            notice=trUpdate("Update installed. Open BookOrbit again from the applications menu.");
#ifdef POCKETBOOK_DEVICE
            QTimer::singleShot(2000,qApp,&QCoreApplication::quit);
#endif
            break;
        }
        emit changed();
    });
}
