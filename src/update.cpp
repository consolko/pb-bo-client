#include "update.h"
#include "client.h"
#include "device.h"
#include "ota.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QLocale>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>
#include <QStorageInfo>
#include <QRegularExpression>
#include <QTimer>
#include <memory>
#include <cstdio>

// Query strings may contain GitHub signed asset credentials. Never record them.
static QString logUrl(const QUrl &url) { return url.adjusted(QUrl::RemoveUserInfo|QUrl::RemoveQuery|QUrl::RemoveFragment).toString().left(512); }
static QJsonObject storageDetails(const QString &path) {
    const QStorageInfo storage(path);
    return {{"path",path},{"filesystem",QString::fromLatin1(storage.fileSystemType())},{"valid",storage.isValid()},
            {"readonly",storage.isReadOnly()},{"freeBytes",storage.bytesAvailable()}};
}
static QString trUpdate(const char *text) { return QCoreApplication::translate("BookOrbit",text); }
UpdateManager::UpdateManager(Client *c,QString data,QString path,QObject *parent)
    : QObject(parent),client(c),root(std::move(data)),executable(std::move(path)),apiBase("https://api.github.com") {
    network.setTransferTimeout(30000);
    const bool directoryReady=QDir().mkpath(root+"/update");
    settings=QJsonDocument::fromJson(Ota::read(root+"/update/preferences.json",65536)).object();
    if(settings["source"].toString()!=apiBase.toString()) settings={{"automatic",settings["automatic"].toBool(true)},{"source",apiBase.toString()}};
    traceContext();
    if(!directoryReady) trace("directory.failed",{{"path",root+"/update"}});
    connect(client,&Client::diagnosticsEnabled,this,&UpdateManager::traceContext);
    totalDeadline.setSingleShot(true);
    connect(&totalDeadline,&QTimer::timeout,this,[this] { fail(trUpdate("The update request timed out.")); });

}
QString UpdateManager::version() const { return Ota::version(); }
bool UpdateManager::canInstall() const {
#ifdef POCKETBOOK_DEVICE
    return phase=="ready";
#else
    return false;
#endif
}
QString UpdateManager::lastChecked() const {
    const auto value=settings["lastChecked"].toInteger();
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
          {"storage",storageDetails(root)},{"stagedBytes",QFileInfo(root+"/update/bookorbit.next").size()}});
}
bool UpdateManager::persist() {
    QString error; const bool ok=Ota::saveJson(root+"/update/preferences.json",settings,&error);
    if(!ok) trace("settings.failed",{{"detail",error}});
    return ok;
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
    const auto now=QDateTime::currentSecsSinceEpoch(),last=settings["lastAutomatic"].toInteger();
    if(last>0 && now-last<86400) { trace("automatic.skipped",{{"reason","24h interval"},{"last",last},{"now",now}}); return; }
    settings["lastAutomatic"]=now; if(!persist()) return;
    checkRelease(false);
}
void UpdateManager::check() { checkRelease(true); }
void UpdateManager::fail(const QString &error) {
    trace("failed",{{"reason",error}});
    ++generation; totalDeadline.stop(); if(active) active->abort(); active.clear();
    phase="error"; notice=error; emit changed();
}
bool UpdateManager::allowed(const QUrl &url) const {
    if(!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    return url.scheme()=="https" && url.port(443)==443 &&
        QStringList{"api.github.com","github.com","release-assets.githubusercontent.com","objects.githubusercontent.com"}.contains(url.host());
}
void UpdateManager::fetch(QUrl url,qint64 limit,std::function<void(QByteArray,int)> done,int redirects) {
    if(!allowed(url)||redirects<0) { trace("url.rejected",{{"url",logUrl(url)},{"redirectsLeft",redirects}}); fail(trUpdate("The update server returned an unsafe download address.")); return; }
    QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("User-Agent",("BookOrbit/"+version()).toUtf8());
    request.setRawHeader("Accept","application/vnd.github+json"); request.setRawHeader("X-GitHub-Api-Version","2022-11-28");
    const bool latest=url.path().endsWith("/releases/latest");
    if(latest && !settings["release"].toObject().isEmpty()) request.setRawHeader("If-None-Match",settings["etag"].toString().toUtf8());
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
        if(status>=300 && status<400 && status!=304) { fetch(url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl()),limit,done,redirects-1); return; }
        if(status==403 || status==429) {
            bool ok=false; const auto seconds=reply->rawHeader("Retry-After").toLongLong(&ok);
            const auto now=QDateTime::currentSecsSinceEpoch();
            settings["retryAfter"]=ok ? now+qBound(qint64(60),seconds,qint64(86400)) : qMax(now+60,reply->rawHeader("X-RateLimit-Reset").toLongLong()); persist();
        }
        if(status!=200 && status!=304) { fail(status==404 ? trUpdate("The published release is unavailable.") : trUpdate("Could not check updates. Try again later.")); return; }
        if(reply->error()!=QNetworkReply::NoError) { fail(trUpdate("The update connection failed.")); return; }
        if(latest && status==200) settings["etag"]=QString::fromUtf8(reply->rawHeader("ETag"));
        done(*data,status);
    });
}
void UpdateManager::checkRelease(bool manual) {
    if(active || phase=="installing" || phase=="ready" || !startupDone) { trace("check.ignored"); return; }
    if(QDateTime::currentSecsSinceEpoch()<settings["retryAfter"].toInteger()) { if(manual) fail(trUpdate("The update server asked to wait. Try again later.")); return; }
    if(manual && !connectNetwork()) { fail(trUpdate("Connect to the network to check updates.")); return; }
    trace("check.requested",{{"manual",manual}});
    ++generation; phase="checking"; notice.clear(); prepared={}; candidateVersion.clear(); releaseNotes.clear(); emit changed(); totalDeadline.start(30000);
    QUrl endpoint(apiBase.toString()+"/repos/consolko/pb-bo-client/releases/latest");
    fetch(endpoint,1024*1024,[this](QByteArray data,int code) {
        totalDeadline.stop();
        const auto r=code==304 ? settings["release"].toObject() : QJsonDocument::fromJson(data).object();
        const auto tag=r["tag_name"].toString();
        if(r.isEmpty() || !r["draft"].isBool() || !r["prerelease"].isBool() || r["draft"].toBool() || r["prerelease"].toBool() || !tag.startsWith('v') || !Ota::validVersion(tag.mid(1))) { fail(trUpdate("The release information is invalid.")); return; }
        settings["release"]=r; settings["lastChecked"]=QDateTime::currentSecsSinceEpoch(); persist();
        trace("release.received",{{"tag",tag},{"http",code}});
        if(!Ota::newer(tag.mid(1),version())) { trace("release.current"); phase="idle"; notice=trUpdate("You have the latest version."); emit changed(); return; }
        QJsonObject asset,checksums;
        for(const auto v:r["assets"].toArray()) {
            const auto a=v.toObject();
            if(a["name"]=="bookorbit-pb634.zip") { if(!asset.isEmpty()) { fail(trUpdate("The release information is invalid.")); return; } asset=a; }
            if(a["name"]=="SHA256SUMS") checksums=a;
        }
        archiveSize=asset["size"].toInteger(); zipUrl=QUrl(asset["browser_download_url"].toString()); checksumsUrl=QUrl(checksums["browser_download_url"].toString());
        if(asset.isEmpty() || archiveSize<1 || archiveSize>Ota::maxArchive || !allowed(zipUrl) || !allowed(checksumsUrl)) { fail(trUpdate("This release has no compatible update package.")); return; }
        candidateVersion=tag.mid(1); releaseNotes=r["body"].toString().left(12000); phase="available";
        trace("release.available",{{"archiveBytes",archiveSize}});
        notice=trUpdate("An application update is available."); emit changed();
    });
}
void UpdateManager::cancel() {
    if(phase!="checking" && phase!="downloading") return;
    trace("cancelled",{{"progress",fraction}});
    ++generation; totalDeadline.stop(); if(active) active->abort(); active.clear();
    phase=candidateVersion.isEmpty()?"idle":"available"; notice=trUpdate("Update download cancelled."); emit changed();
}
void UpdateManager::download() {
    if(phase!="available" || active) { trace("download.ignored"); return; }
    trace("download.requested",{{"storage",storageDetails(root)}});
    const QStorageInfo storage(root);
    if(!storage.isValid()||storage.isReadOnly()||storage.bytesAvailable()<archiveSize+2*Ota::maxClient+8*1024*1024) { fail(trUpdate("Not enough writable space for the update.")); return; }
    if(!connectNetwork()) { fail(trUpdate("Connect to the network to download the update.")); return; }
    ++generation; phase="downloading"; fraction=0; notice.clear(); emit changed(); totalDeadline.start(600000);
    fetch(checksumsUrl,4096,[this](QByteArray data,int) {
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
    auto out=std::make_shared<QSaveFile>(root+"/update/archive.zip"); out->setDirectWriteFallback(false);
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
        phase="verifying"; trace("verify.started"); emit changed();
        QTimer::singleShot(0,this,[this] {
            QString error;
            if(!Ota::stageArchive(root+"/update/archive.zip",root+"/update/bookorbit.next",candidateVersion,&prepared,&error)) {
                trace("verify.failed",{{"detail",error}});
                fail(trUpdate("The update package could not be verified.")); return;
            }
            QFile::remove(root+"/update/archive.zip"); totalDeadline.stop(); phase="ready";
            trace("verify.ready",{{"sha256",prepared["sha256"]},{"bytes",prepared["bytes"]}});
            notice=canInstall() ? trUpdate("The update is verified. Press Install and close to apply it.") : updateDeviceError(); emit changed();
        });
    });
}
void UpdateManager::install() {
    trace("install.requested",{{"canInstall",canInstall()},{"busy",client->busy()},{"destination",executable},{"permissions",int(QFileInfo(executable).permissions())},{"storage",storageDetails(QFileInfo(executable).absolutePath())}});
    QJsonObject device; const auto deviceError=updateDeviceError(&device); trace("install.device",device);
    if(!deviceError.isEmpty()) { trace("install.blocked",{{"reason",deviceError}}); notice=deviceError; emit changed(); return; }
    if(!canInstall()) { trace("install.ignored"); return; }
    if(!Ota::isRunningExecutable(executable)) {
        trace("install.blocked",{{"reason","unverified executable path"}});
        notice=trUpdate("Cannot safely identify the running application. Update installation is blocked."); emit changed(); return;
    }
    if(active || !client->prepareUpdate()) { trace("install.blocked",{{"reason","client busy"}}); notice=trUpdate("Finish the current operation before updating."); emit changed(); return; }
    trace("install.started");
    QString error;
    if(!Ota::install(root+"/update/bookorbit.next",executable,prepared,&error)) {
        trace("install.failed",{{"detail",error}}); client->cancelUpdate();
        fail(trUpdate("The update could not be installed. The current version is still running.")); return;
    }
    trace("install.complete",{{"sha256",Ota::hashFile(executable)},{"expectedSha256",prepared["sha256"]}});
    phase="installing"; notice=trUpdate("Update installed. Open BookOrbit again from the applications menu."); emit changed();
    QTimer::singleShot(2000,qApp,&QCoreApplication::quit);
}
