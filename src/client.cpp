#include "i18n.h"
#include "client.h"
#include <cstdio>
#include "device.h"
#include "progress.h"
#include "sync_decision.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStorageInfo>
#include <algorithm>
#include <QTimer>
#include <QBuffer>
#include <QImageReader>
#include <QImage>
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <QUuid>
#include <QDateTime>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QTextDocument>
#include <memory>

namespace {
constexpr qint64 maxBook = 100 * 1024 * 1024;
constexpr qint64 maxJson = 2 * 1024 * 1024;
constexpr int pageSize = 10;
QJsonObject readObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxJson) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeObject(const QString &path, const QJsonObject &object, QString *error = nullptr) {
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error) *error = QCoreApplication::translate("BookOrbit", "could not create folder ") + directory;
        return false;
    }
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (bytes.size() > maxJson) { if (error) *error = QCoreApplication::translate("BookOrbit", "file too large"); return false; }
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString()+QCoreApplication::translate("BookOrbit", " (code ")+QString::number(int(file.error()))+")";
        return false;
    }
    return true;
}
constexpr auto sessionPermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
bool privateSessionPermissions(QFileDevice::Permissions permissions) {
    constexpr auto shared = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther | QFileDevice::ExeOwner;
    return (permissions & sessionPermissions) == sessionPermissions && !(permissions & shared);
}
bool validServer(const QUrl &url) {
#ifdef BOOKORBIT_TEST_HTTP
    const bool local = url.scheme() == "http" &&
        (url.host() == "localhost" || url.host() == "127.0.0.1" || url.host() == "host.containers.internal") && url.path().isEmpty();
#else
    const bool local = false;
#endif
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
        !url.hasQuery() && !url.hasFragment() && (url.scheme() == "https" || local);
}
bool positiveId(const QJsonValue &value) {
    return value.isDouble() && value.toDouble() > 0 &&
           value.toDouble() <= 2147483647 && value.toDouble() == value.toInt();
}
QJsonObject smallBook(const QJsonObject &book) {
    QJsonObject result;
    for (const auto key : {"id", "title", "subtitle", "authors", "selectedFile", "files", "hasCover", "coverVersion",
             "description", "seriesName", "seriesIndex", "seriesMemberships", "publisher", "publishedDate", "publishedYear",
             "language", "pageCount", "isbn10", "isbn13", "libraryName", "genres", "tags", "rating", "personalNote",
             "readStatus", "readingProgress", "collections", "communityRatings", "detailed"})
        if (book.contains(key)) result[key] = book[key];
    return result;
}
QString fileFormat(const QJsonObject &file) {
    const QString format = file["format"].toString().trimmed().toLower();
    static const QSet<QString> formats{"epub", "kepub", "fb2", "pdf", "djvu", "mobi", "azw", "azw3", "txt", "rtf",
        "doc", "docx", "html", "htm", "chm", "cbz", "cbr", "cb7", "cbx", "m4b", "mp3", "m4a", "opus", "ogg", "flac"};
    return formats.contains(format) ? format : QString{};
}
QJsonArray bookFiles(const QJsonObject &book) {
    QJsonArray result;
    QSet<int> seen;
    for (const auto value : book["files"].toArray()) {
        const auto file = value.toObject();
        if (positiveId(file["id"]) && !seen.contains(file["id"].toInt()) && !fileFormat(file).isEmpty() &&
            (file["role"] == "primary" || file["role"] == "content")) {
            result.append(file); seen.insert(file["id"].toInt());
        }
    }
    return result;
}
bool readableFormat(const QString &format) {
    return QStringList{"epub", "fb2", "pdf", "txt", "djvu"}.contains(format);
}
QString authorNames(const QJsonArray &authors) {
    QStringList names;
    for (const auto value : authors) names << (value.isObject() ? value.toObject()["name"].toString() : value.toString());
    return names.join(", ");
}
QString plainText(const QString &text) {
    if (!Qt::mightBeRichText(text)) return text;
    QTextDocument document;
    document.setHtml(text);
    return document.toPlainText();
}
bool validId(const QString &id) {
    static const QRegularExpression pattern("^[1-9][0-9]*$");
    bool ok = false;
    const qlonglong number = id.toLongLong(&ok);
    return pattern.match(id).hasMatch() && ok && number <= 2147483647;
}
bool storagePath(const QString &path) {
    if (path != QDir::cleanPath(path) || path.contains(QChar::Null)) return false;
    const auto parts = path.split('/');
    if (parts.size() < 3 || parts[0] != "" || parts[1] != "mnt" || parts[2] != "ext1") return false;
    if (parts.size() > 3 && (parts[3] == "system" || parts[3] == "applications")) return false;
    for (const auto &part : parts) if (part.startsWith('.')) return false;
    return true;
}
bool accessibleDirectory(const QString &path) {
    const QFileInfo dir(path);
    return dir.isDir() && dir.isWritable() && dir.canonicalFilePath() == path;
}
bool sameStorage(const QString &first, const QString &second) {
    const QStorageInfo a(first), b(second);
    return a.isValid() && b.isValid() && a.isReady() && b.isReady() && !a.device().isEmpty() &&
           a.device() == b.device() && a.rootPath() == b.rootPath();
}
}

Client::Client(QUrl server, QString root, QObject *parent, bool restoreAccount)
    : QObject(parent), endpoint(server), rootDir(root) {
    diagnosticClock.start();
    network.setTransferTimeout(15000);
    const bool dataDirectoryCreated = QDir().mkpath(rootDir);
    const auto preferences = readObject(rootDir+"/preferences.json");
    const auto language = preferences["language"].toString();
    if (interfaceLanguages().contains(language)) languagePreference = language;
    bookDirectory = preferences["downloadDirectory"].toString();
    diagnostics = preferences["diagnosticLogging"].toBool();
    const auto sort=preferences["localSort"].toString("recent");
    if (QStringList{"recent","title","author"}.contains(sort)) librarySort=sort;
    if (!storagePath(bookDirectory)) bookDirectory.clear();
    const auto settings = readObject(rootDir+"/accounts.json");
    for (const auto value : settings["accounts"].toArray()) {
        const auto account = value.toObject();
        if (validServer(QUrl(account["server"].toString())) &&
            !account["username"].toString().isEmpty()) savedAccounts.append(account);
    }
    const int selected = settings["selected"].toInt(-1);
    if (restoreAccount && selected >= 0 && selected < savedAccounts.size()) {
        const auto account = savedAccounts[selected].toObject();
        endpoint = QUrl(account["server"].toString());
        user = account["username"].toString();
    }
    loadRecords();
    localView = !downloads.isEmpty();
    if (!dataDirectoryCreated)
        finish(QCoreApplication::translate("BookOrbit", "Could not create the app data folder: ") + rootDir +
               QCoreApplication::translate("BookOrbit", ". Check free space and access to device storage."), false, "storage");
}

void Client::startFileOperation() {
    ++fileGeneration;
    if (fileTask) fileTask->store(true);
    fileTask.reset(); working=true;
}
Client::FileContext Client::fileContext(const QString &id,const QString &path) const {
    return {fileGeneration,accountGeneration,id,path,readerProfile(),downloads.value(id).toObject()};
}
bool Client::acceptFileContext(const FileContext &context,const FileStamp &stamp) {
    if (context.generation!=fileGeneration) return false;
    const auto record=downloads.value(context.id).toObject();
    bool same=context.account==accountGeneration && readerProfile()==context.profile;
    for (const auto key:{"bookId","fileId","filename","format","sha256","bytes","directory"})
        same=same && record[key]==context.record[key];
    if (stamp.valid) same=same && fileStamp(context.path)==stamp;
    if (!same) {
        finish(QCoreApplication::translate("BookOrbit", "The file, profile or account changed. Retry the operation."),false,"file");
        return false;
    }
    return true;
}

bool Client::prepareUpdate() {
    if(busy() || downloading() || checkingLibrary || syncingAll) return false;
    updateLocked=true; working=true; stopCovers(); notifyOperation(); return true;
}
void Client::cancelUpdate() {
    if(updateLocked) { updateLocked=false; working=false; notifyOperation(); }
}

bool Client::setLanguage(const QString &language) {
    if (busy() || (language != "system" && !interfaceLanguages().contains(language))) return false;
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["language"] = language;
    beginFeedback("settings");
    if (!writeObject(rootDir+"/preferences.json", preferences)) {
        finish(QCoreApplication::translate("BookOrbit", "Could not save the language setting."), false, "settings");
        return false;
    }
    languagePreference = language;
    invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible();
    emit languageChanged();
    if (!conflictId.isEmpty()) prepareConflict(conflictId, conflictLocal, conflictRemote, conflictProfile);
    finish(QCoreApplication::translate("BookOrbit", "Language saved."), true, "settings");
    return true;
}

void Client::loadRecords() {
    ++accountGeneration; ++recentsGeneration;
    if (recentsTask) recentsTask->store(true);
    if (cleanupTask) cleanupTask->store(true);
    recentsScheduled=false; cleanupScheduled=false; cleanupPending=false; cleanupProofs.clear();
    const auto key = QCryptographicHash::hash((endpoint.toString()+"\n"+user).toUtf8(), QCryptographicHash::Sha256).toHex();
    scopeDir = rootDir + "/" + QString::fromLatin1(key.left(24));
    QDir().mkpath(scopeDir);
    canCleanupBooks = false;
    refreshToken.clear(); sessionStored = false; sessionNotice.clear();
    const QString sessionFile = scopeDir+"/session.json";
    const QFileInfo sessionInfo(sessionFile);
    if (sessionInfo.exists() || sessionInfo.isSymLink()) {
        const bool protectedFile = sessionInfo.isFile() && !sessionInfo.isSymLink() &&
            sessionInfo.canonicalPath() == sessionInfo.absolutePath() &&
            (privateSessionPermissions(sessionInfo.permissions()) ||
             (QFile::setPermissions(sessionFile, sessionPermissions) &&
              privateSessionPermissions(QFileInfo(sessionFile).permissions())));
        if (!protectedFile) {
            sessionNotice = QFile::remove(sessionFile) ? QCoreApplication::translate("BookOrbit", "Could not protect the saved session. Sign in again.")
                                                       : QCoreApplication::translate("BookOrbit", "Could not protect or remove sign-in data. Check device storage.");
        } else {
            const auto session = readObject(sessionFile);
            static const QRegularExpression nativeRefresh("^[a-f0-9]{64}$");
            refreshToken = session["refreshToken"].toString();
            if (!nativeRefresh.match(refreshToken).hasMatch()) {
                refreshToken.clear();
                if (!QFile::remove(sessionFile)) sessionNotice = QCoreApplication::translate("BookOrbit", "Could not remove old sign-in data.");
            } else if (session.contains("password")) {
                if (!saveSession()) refreshToken.clear();
            } else sessionStored = true;
        }
    }
    invalidateLibrary(); invalidateSync(); detailPending=true;
    nativeRecents={}; libraryQuery.clear(); committedQuery.clear();
    items = {}; collectionItems = {}; detailBook = {}; browseHistory.clear();
    ++detailGeneration; detailMessage.clear();
    activeCollection = 0; activeCollectionName.clear(); collectionList = false;
    fileChoices = readObject(scopeDir+"/file-choices.json");
    coverVersions = readObject(scopeDir+"/cover-index.json");
    count = currentPage = 0;
    downloads = {};
    syncResults = {};
    bool recordsReadable = true;
    const QString recordsDir = scopeDir+"/records";
    const bool createdRecords = QDir().mkpath(recordsDir);
    const QFileInfo recordsInfo(recordsDir);
    recordsReadable = createdRecords && recordsInfo.isDir() && recordsInfo.isReadable() &&
                      recordsInfo.canonicalFilePath() == recordsDir;
    for (const auto &name : QDir(recordsDir).entryList({"*.json"}, QDir::AllEntries | QDir::NoDotAndDotDot)) {
        const QString id = name.left(name.size()-5);
        if (!validId(id)) continue;
        const QFileInfo info(recordsDir+"/"+name);
        if (!info.isFile() || info.isSymLink()) { recordsReadable = false; continue; }
        const auto record = readObject(recordsDir+"/"+name);
        if (!recordFile(id, record).isEmpty()) downloads[id] = record;
        else recordsReadable = false;
    }
    const QString oldPath = scopeDir+"/downloads.json";
    QJsonObject old;
    bool migrated = true;
    if (QFile legacy(oldPath); legacy.exists()) {
        if (!legacy.open(QIODevice::ReadOnly) || legacy.size() > maxJson) migrated = false;
        else {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(legacy.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) migrated = false;
            else old = document.object();
        }
    }
    // Read the old catalog only to preserve metadata when migrating legacy downloads.
    const auto legacyItems = old.isEmpty() ? QJsonArray{} : readObject(scopeDir+"/catalog.json")["items"].toArray();
    for (auto it=old.begin(); it!=old.end(); ++it) {
        const QString id = it.key();
        if (!validId(id) || downloads.contains(id)) continue;
        auto record = it.value().toObject();
        if (record["book"].toObject().isEmpty()) {
            for (auto value : legacyItems) {
                const auto book = value.toObject();
                if (book["selectedFile"].toObject()["id"].toInt() == id.toInt()) {
                    record["book"] = smallBook(book); break;
                }
            }
        } else record["book"] = smallBook(record["book"].toObject());
        record["filename"] = id+".epub";
        if (recordFile(id, record).isEmpty()) { migrated = false; continue; }
        const QString destination = recordsDir+"/"+id+".json";
        if (!writeObject(destination, record) || readObject(destination) != record) migrated = false;
        downloads[id] = record;
    }
    if (QFile::exists(oldPath) && migrated)
        migrated = QFile::rename(oldPath, scopeDir+"/downloads-v1.json");
    if (migrated) QFile::remove(scopeDir+"/catalog.json");
    refreshRecents();
    canCleanupBooks = migrated && recordsReadable;
    if (canCleanupBooks) {
        cleanupBooks();
        cleanupCovers();
    }
}

void Client::cleanupBooks(const QString &verifiedFile) {
    if (!verifiedFile.isEmpty()) cleanupProofs[verifiedFile]={fileStamp(verifiedFile),true};
    if (busy() || cleanupScheduled) { cleanupPending=true; return; }
    cleanupPending=false;
    if (!canCleanupBooks || (activeDownload && !activeDownload->isFinished())) return;
    QSet<QString> referenced;
    for (auto it = downloads.begin(); it != downloads.end(); ++it)
        referenced.insert(recordFile(it.key(), it.value().toObject()));
    auto removeUnused = [&referenced](const QString &path) {
        if (path.isEmpty()) return true;
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) return true;
        if (!info.isFile() || info.isSymLink() || info.canonicalPath() != info.absolutePath()) return false;
        if (referenced.contains(path)) return false;
        return readerFileState(path) == ReaderFileState::Closed && QFile::remove(path);
    };
    QSet<QString> retained;
    const QString transfers = scopeDir+"/transfers";
    const QFileInfo transfersInfo(transfers);
    if ((transfersInfo.exists() || transfersInfo.isSymLink()) &&
        (!transfersInfo.isDir() || !transfersInfo.isReadable() || transfersInfo.isSymLink() ||
         transfersInfo.canonicalFilePath() != transfers)) return;
    for (const auto &name : QDir(transfers).entryList({"*.json"}, QDir::AllEntries | QDir::NoDotAndDotDot)) {
        const QFileInfo journalInfo(transfers+"/"+name);
        if (!journalInfo.isFile() || journalInfo.isSymLink()) return;
        const auto journal = readObject(transfers+"/"+name);
        const QString id = QString::number(journal["fileId"].toInt());
        const auto next = journal["next"].toObject(), previous = journal["previous"].toObject();
        const QString newFile = recordFile(id, next), oldFile = previous.isEmpty() ? QString{} : recordFile(id, previous);
        const QString stem = name.chopped(5);
        if (!positiveId(journal["fileId"]) || !journal["previous"].isObject() || !journal["next"].isObject() || newFile.isEmpty() || (!previous.isEmpty() && oldFile.isEmpty()) ||
            !stem.startsWith(id+"-") || QUuid::fromString(stem.mid(id.size()+1)).isNull() ||
            next["filename"].toString() != stem+"."+next["format"].toString()) return;
        const QString staging = scopeDir+"/"+stem+".part";
        const auto current = downloads.value(id).toObject();
        const bool registered = recordFile(id, current) == newFile;
        const QFileInfo info(newFile);
        const bool candidate=registered && current["sha256"]==next["sha256"] && current["bytes"]==next["bytes"] &&
            !current["invalid"].toBool() && info.isFile() && !info.isSymLink() &&
            info.canonicalPath()==info.absolutePath() && info.size()==next["bytes"].toDouble();
        auto proof=cleanupProofs.constFind(newFile);
        if (candidate && (proof==cleanupProofs.cend() || proof->stamp!=fileStamp(newFile))) {
            cleanupScheduled=true;
            const auto account=accountGeneration;
            cleanupTask=files.submit(this,[newFile,next](const FileCancellation &cancel) {
                const auto before=fileStamp(newFile);
                const bool valid=validContent(newFile,next["format"].toString(),cancel) &&
                    digestFile(newFile,cancel)==next["sha256"].toString().toLatin1() && fileStamp(newFile)==before;
                return CleanupProof{before,valid};
            },[this,newFile,account](const CleanupProof &result,bool cancelled) {
                if (account!=accountGeneration) return;
                cleanupScheduled=false;
                if (!cancelled) cleanupProofs[newFile]=result;
                cleanupBooks();
            });
            return;
        }
        const bool confirmed=candidate && proof!=cleanupProofs.cend() && proof->valid;
        bool removed = false;
        if (!registered || confirmed) {
            const bool newRemoved = registered || removeUnused(newFile);
            const bool oldRemoved = recordFile(id, current) == oldFile || removeUnused(oldFile);
            const bool partRemoved = removeUnused(staging);
            removed = newRemoved && oldRemoved && partRemoved && QFile::remove(transfers+"/"+name);
        }
        if (!removed) { retained.insert(newFile); retained.insert(oldFile); retained.insert(staging); }
    }
    static const QRegularExpression owned("^([1-9][0-9]*)(?:-([0-9a-f-]{36}))?\\.([a-z0-9]+)(?:\\.[A-Za-z0-9]+)?$");
    const QDir dir(scopeDir);
    for (const auto &name : dir.entryList(QDir::Files | QDir::NoSymLinks)) {
        const auto match = owned.match(name);
        if (!match.hasMatch() || !validId(match.captured(1))) continue;
        const QString uuid = match.captured(2), format = match.captured(3), path = dir.filePath(name);
        if (retained.contains(path) || (!uuid.isEmpty() && QUuid::fromString(uuid).isNull())) continue;
        if (format != "part") {
            if (fileFormat({{"format", format}}).isEmpty() || !name.endsWith("."+format)) continue;
            // Legacy names without a record have no proof of ownership.
            if (uuid.isEmpty() && (format != "epub" || !downloads.contains(match.captured(1)))) continue;
        }
        removeUnused(path);
    }
    cleanupProofs.clear();
}

void Client::removeCover(int bookId) {
    const QString key = QString::number(bookId);
    if (coverVersions.contains(key)) {
        coverVersions.remove(key);
        writeObject(scopeDir+"/cover-index.json", coverVersions);
    }
    if (QFile::remove(scopeDir+"/cover-"+key+".png")) {
        coverRevisions[scopeDir+"/cover-"+key+".png"]=++coverVersion;
        emit coversChanged();
    }
}

void Client::cleanupCovers() {
    QSet<int> downloaded, visible;
    for (auto value : items) if (positiveId(value.toObject()["id"])) visible.insert(value.toObject()["id"].toInt());
    for (auto it=downloads.begin(); it!=downloads.end(); ++it) {
        const auto book = it.value().toObject()["book"].toObject();
        if (positiveId(book["id"]) && book["hasCover"] != false) downloaded.insert(book["id"].toInt());
    }
    static const QRegularExpression owned("^cover-([1-9][0-9]*)\\.png$");
    QList<QFileInfo> transient;
    for (const auto &name : QDir(scopeDir).entryList({"cover-*.png"}, QDir::Files)) {
        const auto match = owned.match(name);
        if (!match.hasMatch()) continue;
        const int id = match.captured(1).toInt();
        if (downloaded.contains(id)) continue;
        if (!coverVersions.contains(QString::number(id)) && !visible.contains(id)) removeCover(id);
        else transient.append(QFileInfo(scopeDir+"/"+name));
    }
    std::sort(transient.begin(), transient.end(), [&visible](const QFileInfo &a, const QFileInfo &b) {
        const int left=a.baseName().mid(6).toInt(), right=b.baseName().mid(6).toInt();
        if (visible.contains(left) != visible.contains(right)) return !visible.contains(left);
        return a.lastModified() < b.lastModified();
    });
    for (int i=0; i<transient.size()-64; ++i) removeCover(transient[i].baseName().mid(6).toInt());
}

void Client::invalidateLibrary(bool data) {
    libraryDirty = libraryDirty || data;
    libraryOrderDirty = libraryOrderDirty || data;
    libraryFilterDirty = true;
    libraryPending = true;
}

void Client::invalidateSync() {
    syncDirty = true;
    syncPending = true;
}

void Client::publishDataChanges() {
    // Reading a dirty snapshot must not consume the notification still owed to QML.
    if (libraryPending) { libraryPending=false; emit libraryChanged(); }
    if (syncPending) { syncPending=false; emit syncSummaryChanged(); }
    if (detailPending) { detailPending=false; emit detailChanged(); }
}

void Client::notifyOperation() {
    const QVariantList state{busy(), downloading(), verifyingLibrary(), canRetry()};
    if (state == lastOperation) return;
    lastOperation = state;
    emit operationChanged();
}

void Client::storeRecord(const QString &id, const QJsonObject &record) {
    if (downloads.value(id).toObject() == record) return;
    downloads[id] = record;
    invalidateLibrary();
    invalidateSync();
    if (detailBook.value("id") == record["bookId"]) detailPending=true;
}

void Client::setSyncResult(const QString &id, const QString &text) {
    if (syncResults.value(id) == text) return;
    syncResults[id] = text;
    invalidateLibrary();
    if (detailBook.value("id") == downloads.value(id).toObject()["bookId"] ||
        detailBook.value("selectedFile").toObject()["id"].toInt() == id.toInt()) detailPending=true;
}

void Client::clearSyncResult(const QString &id) {
    if (!syncResults.contains(id)) return;
    syncResults.remove(id);
    invalidateLibrary();
    if (detailBook.value("id") == downloads.value(id).toObject()["bookId"]) detailPending=true;
}

bool Client::saveRecord(const QString &id, const QJsonObject &record) {
    if (recordFile(id, record).isEmpty() ||
        !writeObject(scopeDir+"/records/"+id+".json", record)) return false;
    storeRecord(id, record);
    return true;
}

QString Client::recordFile(const QString &id, const QJsonObject &record) const {
    static const QRegularExpression versioned("^[1-9][0-9]*-[0-9a-f-]{36}\\.[a-z0-9]+$");
    static const QRegularExpression sha256("^[0-9a-f]{64}$");
    const QString format = fileFormat(QJsonObject{{"format", record["format"].toString("epub")}});
    const QString name = record["filename"].toString(id+".epub");
    const auto bytes = record["bytes"];
    if (format.isEmpty() || !name.endsWith("."+format) || !validId(id) || record["fileId"].toInt() != id.toInt() ||
        !sha256.match(record["sha256"].toString()).hasMatch() ||
        !bytes.isDouble() || bytes.toDouble() < 0 || bytes.toDouble() > maxBook ||
        bytes.toDouble() != qint64(bytes.toDouble()) ||
        (name != id+".epub" && (!versioned.match(name).hasMatch() || !name.startsWith(id+"-"))) ||
        QFileInfo(name).fileName() != name) return {};
    const QString directory = record["directory"].toString();
    if (!directory.isEmpty() && !storagePath(directory)) return {};
    return (directory.isEmpty() ? scopeDir : directory)+"/"+name;
}

QString Client::pathFor(const QJsonObject &book) const {
    const auto file = book["selectedFile"].toObject();
    const QString format = fileFormat(file);
    if (!positiveId(file["id"]) || format.isEmpty()) return {};
    return scopeDir+"/"+QString::number(file["id"].toInt())+"."+format;
}

QString Client::localFile(int index) const {
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return {};
    return localFile(visible[index].toObject());
}

QString Client::localFile(const QJsonObject &book, LocalFiles *files) const {
    const QString id = QString::number(book["selectedFile"].toObject()["id"].toInt());
    if (files && files->contains(id.toInt())) return files->value(id.toInt());
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.localFileChecks;
#endif
    const auto record = downloads.value(id).toObject();
    const QString path = recordFile(id, record);
    const QFileInfo info(path);
    const QString result = !record["invalid"].toBool() && !path.isEmpty() && info.canonicalPath() == info.absolutePath() && record["sha256"].toString().size() == 64 && info.isFile() && !info.isSymLink() &&
           info.size() == record["bytes"].toDouble() ? path : QString{};
    if (files) files->insert(id.toInt(), result);
    return result;
}

QString Client::downloadDirectory() const {
    if (!bookDirectory.isEmpty()) return bookDirectory;
    if (storagePath(rootDir)) return rootDir;
    if (rootDir.startsWith("/mnt/ext1/applications/"))
        return QDir("/mnt/ext1/Books").exists() ? "/mnt/ext1/Books" : "/mnt/ext1/books";
    return scopeDir;
}

QString Client::diagnosticLogPath() const {
    return rootDir+"/diagnostic.log";
}

QJsonObject Client::selectBookFile(QJsonObject book, LocalFiles *localFiles) const {
    auto files = bookFiles(book);
    if (files.isEmpty() && positiveId(book["selectedFile"].toObject()["id"])) {
        auto legacy = book["selectedFile"].toObject();
        if (!legacy.contains("format")) legacy["format"] = "epub";
        if (!legacy.contains("role")) legacy["role"] = "content";
        if (!fileFormat(legacy).isEmpty()) files.append(legacy);
    }
    book["files"] = files;
    QJsonObject selected;
    const int preferred = fileChoices[QString::number(book["id"].toInt())].toInt();
    for (const auto value : files) {
        const auto file = value.toObject();
        if (selected.isEmpty() || file["role"] == "primary") selected = file;
        if (file["id"].toInt() == preferred) { selected = file; break; }
    }
    // An already downloaded variant is the useful default when no choice was saved.
    if (!preferred) {
        for (const auto value : files) {
            auto candidate = book;
            candidate["selectedFile"] = value;
            if (!localFile(candidate, localFiles).isEmpty()) { selected = value.toObject(); break; }
        }
    }
    book["selectedFile"] = selected;
    return book;
}

QVariantMap Client::fileSummary(const QJsonObject &book, LocalFiles *files) const {
    const auto file = book["selectedFile"].toObject();
    const QString id = QString::number(file["id"].toInt()), format = fileFormat(file);
    const auto record = downloads.value(id).toObject(), progress = record["progress"].toObject();
    const bool downloaded = !localFile(book, files).isEmpty();
    const bool sameProfile=progress.contains("profile") && progress["profile"].toString()==readerProfile();
    return {{"fileId", file["id"].toInt()}, {"format", format.toUpper()},
        {"downloaded", downloaded}, {"readable", readableFormat(format)}, {"canSync", downloaded && syncFormat(format)},
        {"syncResult", translatedText(syncResults[id].toString())}, {"pendingProgress", sameProfile && progress["pending"].isObject()},
        {"remoteFileChanged", record["remoteFileChanged"].toBool()}, {"hasConflict", sameProfile && progress["conflictRemote"].isObject()},
        {"supported", !pathFor(book).isEmpty() && file["sizeBytes"].toDouble() <= maxBook},
        {"needsRepair", !record.isEmpty() && !downloaded}};
}

QVariantMap Client::bookSummary(const QJsonObject &book, LocalFiles *files) const {
    QStringList localFormats, formats;
    for (const auto value : book["files"].toArray()) {
        auto variant = book;
        variant["selectedFile"] = value;
        const auto label = fileFormat(value.toObject()).toUpper();
        if (!formats.contains(label)) formats << label;
        if (!localFile(variant, files).isEmpty() && !localFormats.contains(label)) localFormats << label;
    }
    const auto readingProgress = book["readingProgress"];
    auto result = fileSummary(book, files);
    result.insert(QVariantMap{{"bookId", book["id"].toInt()}, {"title", book["title"].toString().isEmpty() ? QCoreApplication::translate("BookOrbit", "Untitled") : book["title"].toString()},
        {"readStatus", book["readStatus"].toVariant()},
        {"readingProgress", readingProgress.isDouble() && readingProgress.toDouble() >= 0 && readingProgress.toDouble() <= 100 ? readingProgress.toVariant() : QVariant{}},
        {"author", authorNames(book["authors"].toArray())}, {"seriesName", book["seriesName"].toString()},
        {"seriesIndex", book["seriesIndex"].toVariant()}, {"fileCount", book["files"].toArray().size()},
        {"localFormats", localFormats.join(", ")}, {"formats", formats.join(", ")}});
    return result;
}

void Client::beginFeedback(const QString &context,int bookId,int fileId) {
    // Entity identity is independent from the screen that initiated the action.
    // Inline downloads/opens stay in the catalog; login's initial catalog request
    // remains visible on the connection screen if that request fails.
    feedbackContext=context;
    if ((context=="book" && !detailVisible() && (uiContext=="catalog" || uiContext=="sync")) ||
        (context=="catalog" && (uiContext=="connection" || uiContext=="settings")))
        feedbackContext=uiContext;
    feedbackBookId=bookId; feedbackFileId=fileId;
    feedbackResult="running"; feedbackHidden=false; message.clear();
    emit feedbackChanged();
}

QVariantMap Client::feedback() const {
    return {{"text",feedbackHidden ? QString{} : translatedText(message)},{"context",feedbackContext},
        {"result",feedbackResult},{"bookId",feedbackBookId},{"fileId",feedbackFileId}};
}

void Client::setUiContext(const QString &context) {
    if (uiContext==context) return;
    const bool hide=!uiContext.isEmpty() && feedbackResult=="success" && !feedbackHidden;
    uiContext=context;
    if (hide) { feedbackHidden=true; emit feedbackChanged(); }
}

QVariantMap Client::syncBatch() const {
    return {{"running",syncingAll},{"completed",syncCompleted},{"total",syncCount},{"stopRequested",syncStopRequested}};
}

void Client::stopSyncAfterCurrent() {
    if (!syncingAll || syncStopRequested) return;
    syncStopRequested=true; emit syncBatchChanged();
}

void Client::searchDownloaded(const QString &query) {
    if (busy() || query.size()>500) return;
    if (libraryQuery==query) return;
    libraryQuery=query; invalidateLibrary(false); publishDataChanges();
}

void Client::setLocalSort(const QString &sort) {
    if (busy() || !QStringList{"recent","title","author"}.contains(sort)) return;
    auto preferences=readObject(rootDir+"/preferences.json"); preferences["localSort"]=sort;
    beginFeedback("catalog");
    if (!writeObject(rootDir+"/preferences.json",preferences)) {
        finish(QCoreApplication::translate("BookOrbit", "Could not save the book order."),false,"settings"); return;
    }
    if (librarySort!=sort) {
        librarySort=sort; libraryOrderDirty=true; invalidateLibrary(false);
    }
    feedbackResult="success"; emit feedbackChanged(); publishDataChanges();
}

void Client::refreshRecents() {
    const auto profile=readerProfile();
    if (nativeRecents.profile!=profile) { nativeRecents={}; nativeRecents.profile=profile; invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible(); publishDataChanges(); }
    recentsRequested=true;
    if (busy() || recentsScheduled) return;
    recentsScheduled=true; recentsRequested=false;
    QStringList paths;
    for (const auto value:downloads) {
        const auto book=value.toObject()["book"].toObject();
        const QString path=localFile(book);
        if (!path.isEmpty() && readableFormat(fileFormat(book["selectedFile"].toObject()))) paths << path;
    }
    paths.removeDuplicates();
    const auto generation=++recentsGeneration,account=accountGeneration;
    recentsTask=files.submit(this,readerRecentsTask(paths,profile),[this,generation,account,profile](ReaderRecents result,bool cancelled) {
        if (generation!=recentsGeneration) return;
        recentsScheduled=false;
        if (!cancelled && account==accountGeneration && profile==readerProfile() && !busy()) {
            nativeRecents=std::move(result);
            invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible(); publishDataChanges();
        } else recentsRequested=true;
        if (recentsRequested && !busy()) refreshRecents();
    });
}

QJsonObject Client::recentFile(QJsonObject book, LocalFiles *files) const {
    QJsonObject best; qint64 latest=0;
    const int preferred=fileChoices[QString::number(book["id"].toInt())].toInt();
    for (const auto value:book["files"].toArray()) {
        const auto file=value.toObject(); book["selectedFile"]=file;
        if (!readableFormat(fileFormat(file))) continue;
        const QString path=localFile(book, files);
        const qint64 time=nativeRecents.available && !path.isEmpty() ? nativeRecents.files.value(path).openTime : 0;
        const int id=file["id"].toInt(),old=best["id"].toInt();
        if (time>0 && (time>latest || (time==latest && ((id==preferred && old!=preferred) ||
            (old!=preferred && id!=preferred && id<old))))) { best=file; latest=time; }
    }
    book["selectedFile"]=best; book["nativeOpenTime"]=double(latest);
    return book;
}

QVariantMap Client::recentBook() const {
    if (!libraryQuery.isEmpty() || !nativeRecents.available) return {};
    ensureLibrary();
    return libraryRecent;
}

void Client::openFile(int fileId,bool applyIncoming) {
    if (busy()) return;
    const auto record=downloads.value(QString::number(fileId)).toObject();
    if (!record.isEmpty()) openBook(record["book"].toObject(),applyIncoming);
}

QVariantList Client::books() const {
    ensureLibrary();
    return libraryBooks;
}

QVariantMap Client::syncFileStatus(const QString &id, const QString &profile) const {
    const auto record=downloads.value(id).toObject(), progress=record["progress"].toObject();
    const auto saved=record["syncStatus"].toObject();
    const bool sameProfile=saved["profile"].toString()==profile;
    const bool progressProfile=progress.contains("profile") && progress["profile"].toString()==profile;
    QString state=sameProfile ? saved["state"].toString("unknown") : "unknown";
    QString reason=sameProfile ? translatedText(saved["text"].toString()) : QString{};
    const auto book=record["book"].toObject();
    const bool available=!localFile(book).isEmpty();
    if (record["remoteFileChanged"].toBool()) {
        state="file"; reason=QCoreApplication::translate("BookOrbit", "The server file differs. Download the new version before syncing positions.");
    } else if (!available) {
        state="file"; reason=QCoreApplication::translate("BookOrbit", "The local book file is missing or damaged. Download it again.");
    } else if (progressProfile && progress["conflictRemote"].isObject()) state="conflict";
    else if (progressProfile && progress["outgoing"].isObject()) state="uncertain";
    else if (progressProfile && progress["pending"].isObject() && (state=="synced" || state=="unknown")) state="pending";
    const QMap<QString,QString> labels{
        {"synced",QCoreApplication::translate("BookOrbit", "Synced")},{"conflict",QCoreApplication::translate("BookOrbit", "Choose a position")},{"reader",QCoreApplication::translate("BookOrbit", "Close the reader")},
        {"reader_unknown",QCoreApplication::translate("BookOrbit", "Could not check the reader")},{"network",QCoreApplication::translate("BookOrbit", "Waiting for network")},
        {"pending",QCoreApplication::translate("BookOrbit", "Position not applied yet")},{"uncertain",QCoreApplication::translate("BookOrbit", "Result needs verification")},
        {"unknown",QCoreApplication::translate("BookOrbit", "Not checked yet")},{"auth",QCoreApplication::translate("BookOrbit", "Sign in to BookOrbit")},{"file",QCoreApplication::translate("BookOrbit", "Check the book file")},
        {"position",QCoreApplication::translate("BookOrbit", "Could not match the position")},{"error",QCoreApplication::translate("BookOrbit", "Could not sync")}};
    if (!labels.contains(state)) state="unknown";
    if (state=="network") reason=QCoreApplication::translate("BookOrbit", "Connect to a network and retry sync");
    if (state=="unknown") reason=QCoreApplication::translate("BookOrbit", "Run sync to check the position status.");
    const auto at=QDateTime::fromString(saved["at"].toString(),Qt::ISODate).toLocalTime();
    return {{"fileId",id.toInt()},{"filename",book["selectedFile"].toObject()["filename"].toString(record["filename"].toString())},
        {"format",record["format"].toString("epub").toUpper()},
        {"state",state},{"label",labels[state]},{"reason",reason},{"available",available},
        {"checkedAt",sameProfile && at.isValid() ? at.toString("dd.MM.yyyy HH:mm") : QString{}}};
}

QVariantList Client::syncBooks() const {
    return syncSummary()["books"].toList();
}

QVariantList Client::buildSyncBooks() const {
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.syncBuilds;
#endif
    QMap<QString,QVariantMap> grouped;
    const QString profile=readerProfile();
    const QStringList priority{"conflict","file","position","reader","reader_unknown","error","auth","pending","uncertain","network","unknown","synced"};
    for (auto it=downloads.begin(); it!=downloads.end(); ++it) {
        const auto record=it.value().toObject();
        if (!syncFormat(record["format"].toString("epub"))) continue;
        const auto book=record["book"].toObject();
        const QString key=positiveId(book["id"]) ? "book:"+QString::number(book["id"].toInt()) : "file:"+it.key();
        const auto file=syncFileStatus(it.key(),profile);
        auto &entry=grouped[key];
        if (entry.isEmpty()) entry={{"key",key},{"title",book["title"].toString(QCoreApplication::translate("BookOrbit", "Untitled"))},
            {"author",authorNames(book["authors"].toArray())},{"state","synced"},{"label",QCoreApplication::translate("BookOrbit", "Synced")}};
        auto files=entry["files"].toList(); files.append(file); entry["files"]=files;
        if (priority.indexOf(file["state"].toString())<=priority.indexOf(entry["state"].toString())) {
            entry["state"]=file["state"]; entry["label"]=file["label"];
        }
    }
    QVariantList result;
    for (auto entry : grouped) {
        const QString state=entry["state"].toString();
        entry["group"]=state=="synced" ? "synced" : (state=="unknown" || state=="network") ? "waiting" : "attention";
        result.append(entry);
    }
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.syncSorts;
#endif
    std::stable_sort(result.begin(),result.end(),[&priority](const QVariant &a,const QVariant &b) {
        const auto left=a.toMap(),right=b.toMap();
        const int l=priority.indexOf(left["state"].toString()),r=priority.indexOf(right["state"].toString());
        return l!=r ? l<r : QString::localeAwareCompare(left["title"].toString(),right["title"].toString())<0;
    });
    return result;
}

QVariantMap Client::syncSummary() const {
    if (!syncDirty) return cachedSyncSummary;
    const auto books=buildSyncBooks();
    int synced=0,waiting=0,attention=0;
    for (const auto &book:books) {
        const auto group=book.toMap()["group"];
        if (group=="synced") ++synced; else if (group=="waiting") ++waiting; else ++attention;
    }
    QSet<int> unsupported,supported;
    for (const auto value : downloads) {
        const auto record=value.toObject(); const int id=record["book"].toObject()["id"].toInt();
        if (syncFormat(record["format"].toString("epub"))) supported.insert(id); else unsupported.insert(id);
    }
    unsupported.subtract(supported);
    cachedSyncSummary={{"books",books},{"total",books.size()},{"synced",synced},{"waiting",waiting},{"attention",attention},{"unsupported",unsupported.size()}};
    syncDirty=false;
    return cachedSyncSummary;
}

void Client::syncFile(int fileId) {
    if (busy() || !downloads.contains(QString::number(fileId))) return;
    beginFeedback("sync",0,fileId);
    dismissConflict(); syncBook(QString::number(fileId));
}

void Client::showSyncFile(int fileId) {
    if (busy() || !downloads.contains(QString::number(fileId))) return;
    ++detailGeneration;
    detailBook=downloads.value(QString::number(fileId)).toObject()["book"].toObject();
    detailMessage=QCoreApplication::translate("BookOrbit", "Details saved on this device");
    emit detailChanged();
}

void Client::connectForSync() {
    if (busy()) return;
    beginFeedback("sync");
    if (ensureNetwork()) { feedbackResult="success"; message=QCoreApplication::translate("BookOrbit", "Network connected. Retry sync now."); emit feedbackChanged(); }
}

void Client::prepareConflict(const QString &id,const QString &local,const QJsonObject &remote,const QString &profile) {
    const bool different=conflictId!=id || conflictLocal!=local || conflictRemote!=remote || conflictProfile!=profile;
    conflictId=id; conflictLocal=local; conflictRemote=remote; conflictProfile=profile;
    conflictMessage=downloads.value(id).toObject()["book"].toObject()["title"].toString();
    const auto record=downloads.value(id).toObject();
    const QString path=localFile(record["book"].toObject());
    if (conflictTask) conflictTask->store(true);
    const auto generation=++conflictGeneration,account=accountGeneration;
    conflictPreparing=true; positionChoices.clear();
    if (different) ++positionRevision;
    notifyOperation(); emit conflictChanged();
    conflictTask=files.submit(this,[path,local,remote,record](const FileCancellation &cancel) {
        QVariantList choices;
        const auto before=fileStamp(path);
        for (bool useLocal:{true,false}) {
            QVariantMap context; double percentage=0;
            const auto cfi=useLocal ? local : remote["cfi"].toString();
            const bool valid=!fileCancelled(cancel) && bookPosition(path,cfi,&percentage,nullptr,&context);
            context["local"]=useLocal; context["valid"]=valid; context["percentageValue"]=percentage; context["empty"]=cfi.isEmpty();
            choices.append(context);
        }
        const bool same=before.valid && digestFile(path,cancel)==record["sha256"].toString().toLatin1() && fileStamp(path)==before;
        if (!same) for (auto &choice:choices) {
            auto context=choice.toMap(); context["valid"]=false; context.remove("chapter"); context.remove("excerpt"); choice=context;
        }
        return std::make_pair(choices,before);
    },[this,id,local,remote,profile,record,path,generation,account](const auto &result,bool cancelled) {
        if (generation!=conflictGeneration) return;
        conflictPreparing=false; conflictTask.reset();
        if (!cancelled && account==accountGeneration && profile==readerProfile() && conflictId==id && conflictLocal==local &&
            conflictRemote==remote && downloads.value(id).toObject()["sha256"]==record["sha256"] &&
            downloads.value(id).toObject()["progress"]==record["progress"] && fileStamp(path)==result.second) {
            bool other=false;
            for (const auto key:{"pageNumber","positionSeconds","mediaOverlayFragment","mediaOverlaySectionIndex","koboLocationSource",
                "koboLocationType","koboLocationValue","koboContentSourceProgressPercent","koreaderProgress","narrationPercentage","narrationUpdatedAt"})
                if (remote.contains(key) && !remote[key].isNull()) other=true;
            positionChoices=result.first;
            for (auto &choice:positionChoices) {
                auto context=choice.toMap(); const bool useLocal=context.take("local").toBool(),valid=context.take("valid").toBool();
                const double percentage=context.take("percentageValue").toDouble(); const bool empty=context.take("empty").toBool();
                context["local"]=useLocal;
                context["source"]=useLocal ? QCoreApplication::translate("BookOrbit", "On this PocketBook") : QCoreApplication::translate("BookOrbit", "In BookOrbit");
                context["percentage"]=valid ? QCoreApplication::translate("BookOrbit", "About %1% of the book").arg(QLocale().toString(percentage,'f',1)) : empty ? QCoreApplication::translate("BookOrbit", "No position saved yet") : QCoreApplication::translate("BookOrbit", "The position does not match this book");
                context["canUse"]=valid && !(useLocal && other) && !record["remoteFileChanged"].toBool();
                context["notice"]=useLocal && other ? QCoreApplication::translate("BookOrbit", "Upload stopped to preserve other server position data.") : QString{};
                choice=context;
            }
        }
        notifyOperation(); emit conflictChanged();
        if (!busy() && recentsRequested) refreshRecents();
        if (!busy() && cleanupPending) cleanupBooks();
    });
}

void Client::inspectConflict(int fileId) {
    if (busy()) return;
    const QString id=QString::number(fileId);
    const auto progress=downloads.value(id).toObject()["progress"].toObject();
    if (!progress["conflictRemote"].isObject() || progress["profile"].toString()!=readerProfile()) return;
    prepareConflict(id,progress["conflictLocal"].toString(),progress["conflictRemote"].toObject(),progress["profile"].toString());
}

QVariantMap Client::detail() const {
    if (detailBook.isEmpty()) return {};
    auto result = detailBook.toVariantMap();
    const auto summary = bookSummary(detailBook);
    for (auto it = summary.begin(); it != summary.end(); ++it) result[it.key()] = it.value();
    result["notice"] = translatedText(detailMessage);
    QVariantList files;
    for (const auto value : detailBook["files"].toArray()) {
        auto book = detailBook;
        book["selectedFile"] = value;
        auto file = value.toObject().toVariantMap();
        const auto info = fileSummary(book);
        for (const auto key : {"downloaded", "readable", "supported", "needsRepair", "remoteFileChanged", "syncResult"}) file[key] = info[key];
        file["format"] = fileFormat(value.toObject()).toUpper();
        const double bytes = value.toObject()["sizeBytes"].toDouble(-1);
        file["sizeLabel"] = bytes < 0 ? QString{} : bytes < 1024 ? QString::number(bytes, 'f', 0)+QCoreApplication::translate("BookOrbit", " B") : bytes < 1024*1024 ? QString::number(bytes/1024, 'f', 0)+QCoreApplication::translate("BookOrbit", " KiB") : QString::number(bytes/(1024*1024), 'f', 1)+QCoreApplication::translate("BookOrbit", " MiB");
        files.append(file);
    }
    result["files"] = files;
    return result;
}

void Client::rememberFile(const QJsonObject &book) {
    const auto file = book["selectedFile"].toObject();
    if (!positiveId(book["id"]) || !positiveId(file["id"])) return;
    auto choices = fileChoices;
    choices[QString::number(book["id"].toInt())] = file["id"];
    if (choices != fileChoices && writeObject(scopeDir+"/file-choices.json", choices)) {
        fileChoices = choices; invalidateLibrary();
    }
}

void Client::showDetail(int bookId) {
    if (busy() || bookId <= 0) return;
    for (const auto value : visibleItems(false)) {
        const auto book = value.toObject();
        if (book["id"].toInt() != bookId) continue;
        detailBook = book;
        for (auto it = downloads.begin(); it != downloads.end(); ++it) {
            const auto saved = it.value().toObject()["book"].toObject();
            if (saved["id"].toInt() == bookId && saved["detailed"].toBool()) {
                detailBook = saved;
                auto files = saved["files"].toArray();
                QSet<int> known;
                for (const auto file : files) known.insert(file.toObject()["id"].toInt());
                for (const auto file : book["files"].toArray())
                    if (!known.contains(file.toObject()["id"].toInt())) files.append(file);
                detailBook["files"] = localView ? files : book["files"].toArray();
                detailBook["selectedFile"] = book["selectedFile"];
                if (!localView) {
                    detailBook["readStatus"] = book["readStatus"];
                    detailBook["readingProgress"] = book["readingProgress"];
                }
                break;
            }
        }
        detailMessage = detailBook["detailed"].toBool() ? QCoreApplication::translate("BookOrbit", "Details saved on this device") : "";
        ++detailGeneration;
        emit detailChanged();
        if (authenticated() && !localView && !(retryKind==2 && retryBook["id"].toInt()==bookId)) refreshDetail();
        else if (!detailBook["detailed"].toBool()) {
            detailMessage = QCoreApplication::translate("BookOrbit", "Full details not loaded yet");
            emit detailChanged();
        }
        return;
    }
}

void Client::refreshDetail() {
    if (busy() || detailBook.isEmpty()) return;
    if (!authenticated()) { detailMessage = QCoreApplication::translate("BookOrbit", "Sign in to load full details"); notifyOperation(); emit feedbackChanged(); emit detailChanged(); return; }
    beginFeedback("book",detailBook["id"].toInt(),detailBook["selectedFile"].toObject()["id"].toInt());
    const int id = detailBook["id"].toInt(), generation = detailGeneration;
    const auto selectedFile = detailBook["selectedFile"];
    retryKind = 3; working = true; message.clear(); detailMessage = QCoreApplication::translate("BookOrbit", "Loading details…"); notifyOperation(); emit feedbackChanged(); emit detailChanged();
    jsonRequest("/api/v1/books/"+QString::number(id), {}, [this, id, generation, selectedFile](const QJsonObject &response) {
        if (generation != detailGeneration) { finish("", true, "details"); return; }
        if (response["id"].toInt() != id || !response["files"].isArray() || !response["authors"].isArray()) {
            finish(QCoreApplication::translate("BookOrbit", "The server returned invalid book details"), false, "details"); return;
        }
        auto book = smallBook(response);
        // The detail endpoint omits the book-level percentage returned by the catalog.
        if (!book.contains("readingProgress")) book["readingProgress"] = detailBook["readingProgress"];
        book["files"] = bookFiles(response);
        book["description"] = plainText(response["description"].toString());
        book["detailed"] = true;
        book["hasCover"] = detailBook["hasCover"];
        book = selectBookFile(book);
        for (const auto file : book["files"].toArray())
            if (file.toObject()["id"] == selectedFile.toObject()["id"]) book["selectedFile"] = file;
        detailBook = book; detailPending=true;
        bool saved = true;
        const auto ids = downloads.keys();
        for (const auto &key : ids) {
            auto record = downloads.value(key).toObject();
            if (record["bookId"].toInt() != id) continue;
            auto metadata = book;
            metadata["selectedFile"] = record["book"].toObject()["selectedFile"];
            record["book"] = smallBook(metadata);
            if (!saveRecord(key, record)) saved = false;
        }
        detailMessage = saved ? "" : QCoreApplication::translate("BookOrbit", "Details received but not saved for offline use");
        finish("", true, "details");
    }, true, true);
}

void Client::closeDetail() {
    ++detailGeneration; detailBook = {}; detailMessage.clear();
    if (retryKind == 3) retryKind = 0;
    notifyOperation(); emit detailChanged();
}

void Client::selectFile(int fileId) {
    if (busy() || detailBook.isEmpty()) return;
    for (const auto value : detailBook["files"].toArray()) {
        if (value.toObject()["id"].toInt() != fileId) continue;
        if (feedbackContext=="book" && feedbackFileId!=fileId && feedbackResult=="success") feedbackHidden=true;
        detailBook["selectedFile"] = value;
        rememberFile(detailBook);
        emit detailChanged(); emit feedbackChanged(); publishDataChanges();
        return;
    }
}

void Client::downloadSelected() {
    if (busy() || detailBook.isEmpty()) return;
    if (!authenticated()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to download"), false, "download"); return; }
    downloadBook(detailBook);
}

void Client::openSelected(bool applyIncoming) {
    if (!working && !detailBook.isEmpty()) openBook(detailBook, applyIncoming);
}

void Client::syncSelected() {
    if (busy() || detailBook.isEmpty()) return;
    dismissConflict();
    syncBook(QString::number(detailBook["selectedFile"].toObject()["id"].toInt()));
}

void Client::showCollections() {
    if (busy()) return;
    beginFeedback("catalog");
    closeDetail(); stopCovers(); browseHistory.clear();
    collectionList = true; localView = false; activeCollection = 0; activeCollectionName.clear();
    collectionItems = {}; items = {}; count = currentPage = 0;
    invalidateLibrary(); emit catalogChanged(); publishDataChanges();
    retryKind = 4;
    if (!authenticated()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to load collections"), false, "collections"); return; }
    working = true; message = QCoreApplication::translate("BookOrbit", "Loading collections…"); notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/collections", {}, [this](const QJsonObject &response) {
        QJsonArray parsed;
        QSet<int> seen;
        for (const auto value : response["items"].toArray()) {
            const auto collection = value.toObject();
            if (!positiveId(collection["id"]) || !collection["name"].isString() || !collection["bookCount"].isDouble() || collection["bookCount"].toDouble() < 0 || seen.contains(collection["id"].toInt())) {
                finish(QCoreApplication::translate("BookOrbit", "The server returned an invalid collection list"), false, "collections"); return;
            }
            seen.insert(collection["id"].toInt());
            if (collection["mediaType"] == "books") parsed.append(collection);
        }
        collectionItems = parsed;
        emit catalogChanged();
        finish("", true, "collections");
    }, true, true, true);
}

void Client::openCollection(int id, const QString &name, double catalogOffset, double detailOffset) {
    if (busy() || id <= 0) return;
    if (detailVisible() && id == activeCollection) { closeDetail(); return; }
    if (detailVisible()) {
        if (browseHistory.size() == 16) browseHistory.removeFirst();
        browseHistory.append(QJsonObject{{"detail", detailBook}, {"items", items}, {"page", currentPage},
        {"total", count}, {"local", localView}, {"collectionId", activeCollection}, {"collectionName", activeCollectionName},
        {"query", committedQuery}, {"catalogY", catalogOffset}, {"detailY", detailOffset}, {"notice", detailMessage}});
    }
    closeDetail();
    items={}; currentPage=count=0; committedQuery.clear();
    activeCollection = id; activeCollectionName = name;
    invalidateLibrary();
    refresh();
}

QVariantMap Client::backFromCollection() {
    if (busy() || browseHistory.isEmpty()) return {};
    const auto saved = browseHistory.takeLast();
    stopCovers(); ++detailGeneration;
    detailBook = saved["detail"].toObject(); items = saved["items"].toArray();
    currentPage = saved["page"].toInt(); count = saved["total"].toInt(); localView = saved["local"].toBool();
    activeCollection = saved["collectionId"].toInt(); activeCollectionName = saved["collectionName"].toString();
    committedQuery = saved["query"].toString(); retryQuery=committedQuery; detailMessage = saved["notice"].toString();
    collectionList = false; retryKind = 0; message.clear();
    invalidateLibrary(); emit catalogChanged(); emit detailChanged(); notifyOperation(); emit feedbackChanged(); publishDataChanges();
    return {{"query", committedQuery}, {"catalogY", saved["catalogY"].toDouble()}, {"detailY", saved["detailY"].toDouble()}};
}

void Client::finish(QString text, bool success, const QString &operation, const QString &syncState) {
    struct Maintenance {
        Client *client;
        ~Maintenance() { if (!client->busy() && client->cleanupPending) client->cleanupBooks(); }
    } maintenance{this};
    auto previousBatch=syncBatch();
    auto publish=[this,&previousBatch] {
        notifyOperation(); emit feedbackChanged();
        if (syncBatch()!=previousBatch) emit syncBatchChanged();
        publishDataChanges();
    };
    ++fileGeneration;
    if (fileTask) fileTask->store(true);
    fileTask.reset(); fileDownloading=false;
    working = syncingAll || checkingLibrary;
    activeDownload.clear();
    if (!success && retryKind == 3 && detailVisible()) { detailMessage = text; detailPending=true; }
    if (success) retryKind = 0;
    const bool hasFile = !syncingId.isEmpty();
    if (hasFile && !checkingLibrary) {
        auto record=downloads.value(syncingId).toObject();
        record["syncStatus"]=QJsonObject{{"state",syncState.isEmpty() ? (success ? "synced" : "error") : syncState},
            {"text",text},{"at",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"profile",readerProfile()}};
        if (!saveRecord(syncingId,record)) {
            success=false;
            text=QCoreApplication::translate("BookOrbit", "Could not save the sync result. Resolve the storage error and try again.");
            record["syncStatus"]=QJsonObject{{"state","error"},{"text",text},{"profile",readerProfile()}};
            storeRecord(syncingId,record);
        }
    }
    if (!success && feedbackFileId>0 && !hasFile) setSyncResult(QString::number(feedbackFileId),text);
    message = text; feedbackResult=success ? "success" : "error"; feedbackHidden=false;
    logEvent(operation, success ? 0 : 1);
    if (hasFile) {
        if (!checkingLibrary || !verificationCancelled) setSyncResult(syncingId,text);
        syncingId.clear();
    }
    if (syncingAll || checkingLibrary) {
        if (checkingLibrary) {
            if (hasFile && !verificationCancelled) {
                if (success) ++syncSucceeded;
                else if (operation == "verify-different") ++verificationDifferent;
                else ++verificationErrors;
            }
            if (verificationCancelled || !authenticated()) syncQueue.clear();
        } else {
            if (hasFile) { ++syncCompleted; if (success) ++syncSucceeded; }
            if (syncStopRequested || !authenticated()) syncQueue.clear();
        }
        if (syncBatch()!=previousBatch) { emit syncBatchChanged(); previousBatch=syncBatch(); }
        dismissConflict(); // Each unresolved conflict remains in its book record for individual retry.
        if (!syncQueue.isEmpty()) {
            working=true;
            publish();
            QTimer::singleShot(0,this,[this] {
                working=false;
                if (checkingLibrary && verificationCancelled) {
                    finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify"); return;
                }
                if (syncingAll && (syncStopRequested || !authenticated())) { syncQueue.clear(); finish("",false,"sync"); return; }
                const auto id=syncQueue.takeFirst();
                if (checkingLibrary) verifyBook(id);
                else syncBook(id);
            });
            return;
        }
        if (checkingLibrary) {
            checkingLibrary=false;
            working=false;
            const int unchecked=syncCount-syncSucceeded-verificationDifferent-verificationErrors;
            message=QCoreApplication::translate("BookOrbit", "%1. Files: %2; matching: %3; different: %4; errors: %5; unchecked: %6. See book details for results.")
                .arg(verificationCancelled ? QCoreApplication::translate("BookOrbit", "Verification cancelled") : !authenticated() ? QCoreApplication::translate("BookOrbit", "Verification stopped: sign in again") : QCoreApplication::translate("BookOrbit", "Verification complete"))
                .arg(syncCount).arg(syncSucceeded).arg(verificationDifferent).arg(verificationErrors).arg(unchecked);
            if (recentsRequested) refreshRecents();
            publish();
            emit completed("verify",!verificationCancelled && syncSucceeded==syncCount);
            return;
        }
        syncingAll=false; emit conflictChanged();
        working=false;
        const auto summary=syncSummary();
        const bool allOk=!syncStopRequested && authenticated() && syncSucceeded==syncCount;
        message=QCoreApplication::translate("BookOrbit", "%1. Files completed: %2 of %3. Books synced: %4 of %5; needs attention: %6; waiting: %7.")
            .arg(!authenticated() ? QCoreApplication::translate("BookOrbit", "Sync stopped: sign in again") : syncStopRequested ? QCoreApplication::translate("BookOrbit", "Sync stopped by user") : allOk ? QCoreApplication::translate("BookOrbit", "Sync complete") : QCoreApplication::translate("BookOrbit", "Sync completed with issues"))
            .arg(syncCompleted).arg(syncCount).arg(summary["synced"].toInt()).arg(summary["total"].toInt())
            .arg(summary["attention"].toInt()).arg(summary["waiting"].toInt());
        feedbackResult=allOk ? "success" : "error";
        refreshRecents();
        publish();
        emit completed("sync",allOk);
        return;
    }
    if (operation=="progress" || operation=="download" || operation=="open" || recentsRequested) refreshRecents();
    publish();
    emit completed(operation, success);
}

QNetworkRequest Client::request(const QString &path) const {
    QNetworkRequest request(QUrl(endpoint.toString()+path));
    if (endpoint.scheme() == "https") {
        auto ssl = request.sslConfiguration();
        auto certificates = ssl.caCertificates();
        if (endpoint.host() == "books.lan")
            certificates.append(QSslCertificate::fromPath(":/books-lan-ca.pem", QSsl::Pem, QSslCertificate::PatternSyntax::FixedString));
        // Public LAN CA, scoped to this host and this client's data directory.
        certificates.append(QSslCertificate::fromPath(rootDir+"/certificates/"+endpoint.host()+".pem", QSsl::Pem, QSslCertificate::PatternSyntax::FixedString));
        ssl.setCaCertificates(certificates);
        request.setSslConfiguration(ssl);
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer "+token);
    return request;
}

bool Client::setCredentials(const QJsonObject &response) {
    const QString access = response["accessToken"].toString();
    const QString refresh = response["refreshToken"].toString();
    static const QRegularExpression nativeRefresh("^[a-f0-9]{64}$");
    if (access.isEmpty() || access.size() > 8192 || access.contains('\r') || access.contains('\n') ||
        !nativeRefresh.match(refresh).hasMatch()) {
        invalidateSession();
        finish(QCoreApplication::translate("BookOrbit", "The server response contains no valid session"), false, "login");
        return false;
    }
    token = access.toUtf8(); refreshToken = refresh;
    saveSession(); // A storage failure leaves this login usable until the application closes.
    return true;
}

bool Client::saveSession() {
    const QString path = scopeDir+"/session.json";
    QSaveFile file(path);
    const auto bytes = QJsonDocument(QJsonObject{{"refreshToken", refreshToken}}).toJson();
    const QFileInfo info(path);
    sessionStored = !info.isSymLink() && QFileInfo(scopeDir).canonicalFilePath() == scopeDir &&
        file.open(QIODevice::WriteOnly) && file.setPermissions(sessionPermissions) &&
        privateSessionPermissions(file.permissions()) && file.write(bytes) == bytes.size() && file.commit() &&
        privateSessionPermissions(QFileInfo(path).permissions());
    if (!sessionStored) {
        file.cancelWriting();
        const bool removed = (!QFile::exists(path) && !QFileInfo(path).isSymLink()) || QFile::remove(path);
        sessionNotice = removed ? QCoreApplication::translate("BookOrbit", "Could not protect the saved session. You will need to sign in again after closing the app.")
                                : QCoreApplication::translate("BookOrbit", "Could not protect or remove sign-in data. Check device storage.");
    } else sessionNotice.clear();
    logEvent(sessionStored ? "session saved" : "session save failed");
    emit connectionChanged(); notifyOperation();
    return sessionStored;
}

void Client::invalidateSession() {
    ++accountGeneration;
    token.clear(); refreshToken.clear(); sessionStored = false; sessionNotice.clear();
    stopCovers();
    const QString path = scopeDir+"/session.json";
    if ((QFile::exists(path) || QFileInfo(path).isSymLink()) && !QFile::remove(path))
        sessionNotice = QCoreApplication::translate("BookOrbit", "Could not remove the invalid session from this device.");
    emit connectionChanged(); notifyOperation();
}

void Client::restoreSession() {
    if (busy() || authenticated() || refreshToken.isEmpty()) return;
    beginFeedback("connection");
    working = true;
    message = QCoreApplication::translate("BookOrbit", "Restoring session…");
    notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/auth/refresh", {{"refreshToken", refreshToken}}, [this](const QJsonObject &response) {
        if (!setCredentials(response)) return;
        working = false;
        refresh();
    }, false);
}

void Client::renewSession(const std::function<void()> &resume) {
    stopCovers();
    message = QCoreApplication::translate("BookOrbit", "Refreshing session…");
    notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/auth/refresh", {{"refreshToken", refreshToken}}, [this, resume](const QJsonObject &response) {
        if (setCredentials(response)) { queueCovers(); resume(); }
    }, false);
}

bool Client::ensureNetwork() {
    if (connectNetwork()) return true;
    finish(QCoreApplication::translate("BookOrbit", "Wi-Fi is disconnected. Connect to a network and try again."), false, "network", "network");
    return false;
}

void Client::jsonRequest(const QString &path, const QJsonObject &payload, const Callback &callback, bool renew, bool get, bool arrayResponse) {
    if (checkingLibrary && verificationCancelled) { finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify"); return; }
    if (!ensureNetwork()) return;
    const auto generation=fileGeneration,account=accountGeneration;
    auto request = this->request(path);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto reply = get ? network.get(request) : network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (checkingLibrary) activeVerification=reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > maxJson) reply->abort();
    });
    // Bound the whole operation as well as inactivity between packets.
    QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, callback, path, payload, renew, get, arrayResponse,generation,account] {
        reply->deleteLater();
        if (generation!=fileGeneration) return;
        if (account!=accountGeneration) { finish(QCoreApplication::translate("BookOrbit", "The file, profile or account changed. Retry the operation."),false,"file"); return; }
        if (activeVerification==reply) activeVerification.clear();
        if (checkingLibrary && verificationCancelled) { finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify"); return; }
        bytes->append(reply->readAll());
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        logEvent(path.section('?', 0, 0), int(reply->error()), code);
        if (code == 401 && renew && (path.startsWith("/api/v1/books/") || path.startsWith("/api/v1/collections")) && !refreshToken.isEmpty()) {
            if (!get && path.endsWith("/progress")) {
                // A refreshed token is not permission to replay an old sync
                // decision. Keep the outgoing journal; the next sync starts with
                // fresh reader/profile/server snapshots, including explicit choices.
                renewSession([this] {
                    finish(QCoreApplication::translate("BookOrbit", "Session restored. Retry sync: the previous position was not sent again."),
                           false,"progress","uncertain");
                });
            } else {
                renewSession([this, path, payload, callback, get, arrayResponse] { jsonRequest(path, payload, callback, false, get, arrayResponse); });
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300 || bytes->size() > maxJson) {
            if (code == 401) invalidateSession();
            finish(path == "/api/v1/auth/logout" ? QCoreApplication::translate("BookOrbit", "Signed out on this device. Server session revocation was not confirmed.") :
                   reply->error() == QNetworkReply::SslHandshakeFailedError ? QCoreApplication::translate("BookOrbit", "Could not verify the server TLS certificate. Check the trusted CA and device date.") :
                   reply->error() == QNetworkReply::HostNotFoundError ? QCoreApplication::translate("BookOrbit", "Server not found: ")+endpoint.host()+QCoreApplication::translate("BookOrbit", ". Check Wi-Fi and DNS.") :
                   reply->error() == QNetworkReply::ConnectionRefusedError ? QCoreApplication::translate("BookOrbit", "The server refused the connection. Check the address and network.") :
                   reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::OperationCanceledError ? QCoreApplication::translate("BookOrbit", "Connection timed out. Check the network and try again.") :
                   code == 401 ? QCoreApplication::translate("BookOrbit", "Not signed in or session expired. Sign in again.") :
                   code >= 400 ? QCoreApplication::translate("BookOrbit", "The server returned HTTP ")+QString::number(code)+QCoreApplication::translate("BookOrbit", ". Check the address and try again.") :
                   QCoreApplication::translate("BookOrbit", "The server is unavailable. Downloaded books can be read offline."), false, path, code==401 ? "auth" : "error");
            return;
        }
        if (!get && path.endsWith("/progress") && bytes->trimmed().isEmpty()) { callback({}); return; }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(*bytes, &error);
        if (error.error != QJsonParseError::NoError || (arrayResponse ? !doc.isArray() : !doc.isObject())) {
            finish(QCoreApplication::translate("BookOrbit", "The server returned invalid data"), false, path);
            return;
        }
        callback(arrayResponse ? QJsonObject{{"items", doc.array()}} : doc.object());
    });
}

void Client::login(const QString &address, const QString &username, const QString &password) {
    if (busy()) return;
    beginFeedback("connection");
    QString normalized = address.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    const QUrl url(normalized, QUrl::StrictMode);
    if (validServer(url) && url == endpoint && password.isEmpty() && username == user && !refreshToken.isEmpty()) {
        restoreSession(); return;
    }
    const QString secret = password;
    if (!validServer(url) || username.trimmed().isEmpty() || secret.isEmpty()) {
        finish(QCoreApplication::translate("BookOrbit", "Enter an HTTPS address, username and password."), false, "login");
        return;
    }
    if (!configure(normalized, username)) return;
    retryKind = 0;
    feedbackResult="running";
    working = true;
    message = QCoreApplication::translate("BookOrbit", "Connecting…");
    notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/auth/login", {{"username", username}, {"password", secret},
                                    {"clientKind", "native"}, {"deviceLabel", "PocketBook prototype"}},
        [this](const QJsonObject &response) {
            if (!setCredentials(response)) return;
            working = false;
            refresh();
        });
}

void Client::refresh(int targetPage, const QString &query) {
    if (busy()) return;
    if (targetPage < 0 || query.size() > 500) return;
    if (localView || collectionList) invalidateLibrary();
    collectionList = false;
    stopCovers();
    localView = false;
    beginFeedback("catalog");
    emit catalogChanged();
    if (token.isEmpty()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to the server first"), false, "catalog"); return; }
    retryKind = 1; retryPage = targetPage; retryQuery = query;
    working = true;
    message = QCoreApplication::translate("BookOrbit", "Loading catalog…");
    notifyOperation(); emit feedbackChanged(); publishDataChanges();
    QUrlQuery params;
    params.addQueryItem("page", QString::number(targetPage));
    params.addQueryItem("size", QString::number(pageSize));
    params.addQueryItem("collapseSeries", "false");
    params.addQueryItem("q", QString::fromLatin1(QUrl::toPercentEncoding(query)));
    const QString route = activeCollection > 0 ? "/api/v1/collections/"+QString::number(activeCollection)+"/books?"+params.toString(QUrl::FullyEncoded) : "/api/v1/books/query";
    jsonRequest(route, {{"sort", QJsonArray{}}, {"pagination", QJsonObject{{"page", targetPage}, {"size", pageSize}}}, {"q", query}},
        [this, targetPage, query](const QJsonObject &response) {
            const auto array = response["items"].toArray();
            if (!response["items"].isArray() || !response["total"].isDouble() || response["total"].toDouble() < 0 ||
                response["page"].toInt(-1) != targetPage || array.size() > pageSize) {
                finish(QCoreApplication::translate("BookOrbit", "The server returned an invalid catalog."), false, "catalog"); return;
            }
            QJsonArray parsed;
            for (auto value : array) {
                auto book = value.toObject();
                if (!positiveId(book["id"]) || !book["files"].isArray()) {
                    finish(QCoreApplication::translate("BookOrbit", "Invalid book record"), false, "catalog"); return;
                }
                book["files"] = bookFiles(book);
                parsed.append(selectBookFile(book));
            }
            stopCovers();
            for (auto value : parsed) {
                const auto book = value.toObject();
                const int bookId = book["id"].toInt();
                for (const auto &fileId : downloads.keys()) {
                    auto record = downloads.value(fileId).toObject();
                    if (record["bookId"].toInt() != bookId) continue;
                    auto stored = record["book"].toObject();
                    stored["hasCover"] = book["hasCover"];
                    stored["coverVersion"] = book["coverVersion"];
                    stored["readStatus"] = book["readStatus"];
                    stored["readingProgress"] = book["readingProgress"];
                    record["book"] = stored;
                    saveRecord(fileId, record);
                }
                if (book["hasCover"] == false) removeCover(bookId);
            }
            items = parsed; currentPage = targetPage; count = response["total"].toInt(); committedQuery=query;
            invalidateLibrary(); emit catalogChanged();
            cleanupCovers();
            queueCovers();
            finish(items.isEmpty() ? QCoreApplication::translate("BookOrbit", "No books found") : "", true, "catalog");
        }, true, activeCollection > 0);
}

void Client::download(int index) {
    if (busy()) return;
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return;
    if (token.isEmpty()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to download"), false, "download"); return; }
    downloadBook(visible[index].toObject());
}

void Client::downloadBook(const QJsonObject &book, bool renew) {
    const int bookId=book["id"].toInt(), fileId=book["selectedFile"].toObject()["id"].toInt();
    const auto previous=downloads.value(QString::number(fileId)).toObject();
    if (previous.isEmpty()) { transferBook(book,renew); return; }
    beginFeedback("book",bookId,fileId);
    retryKind=2; retryBook=book;
    if (bookId<=0 || fileId<=0 || previous["bookId"].toInt()!=bookId) {
        finish(QCoreApplication::translate("BookOrbit", "This file is already linked to another book. Refresh the details."),false,"download"); return;
    }
    // Repairs and retries resolve file metadata again. A known replacement may
    // have a different byte count under the same fileId; never use cached size.
    working=true; message=QCoreApplication::translate("BookOrbit", "Checking the current file version…"); notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/books/"+QString::number(bookId),{},
        [this,book,bookId,fileId,renew](const QJsonObject &response) {
            if (response["id"].toInt()!=bookId || !response["files"].isArray()) {
                finish(QCoreApplication::translate("BookOrbit", "Could not get current file details."),false,"download"); return;
            }
            QJsonObject selected;
            for (const auto value:bookFiles(response))
                if (value.toObject()["id"].toInt()==fileId) selected=value.toObject();
            if (selected.isEmpty()) {
                finish(QCoreApplication::translate("BookOrbit", "The selected file is no longer on the server. Refresh the book details."),false,"download"); return;
            }
            if (fileFormat(selected)!=fileFormat(book["selectedFile"].toObject())) {
                finish(QCoreApplication::translate("BookOrbit", "The server file format has changed. Refresh the book details."),false,"download"); return;
            }
            auto refreshed=book;
            refreshed["selectedFile"]=selected;
            auto files=book["files"].toArray();
            for (int i=0;i<files.size();++i)
                if (files[i].toObject()["id"].toInt()==fileId) files[i]=selected;
            refreshed["files"]=files;
            working=false;
            transferBook(refreshed,renew);
        },renew,true);
}

void Client::transferBook(const QJsonObject &book, bool renew) {
    beginFeedback("book",book["id"].toInt(),book["selectedFile"].toObject()["id"].toInt());
    if (!ensureNetwork()) return;
    retryKind = 2; retryBook = book;
    const auto file = book["selectedFile"].toObject();
    const QString path = pathFor(book);
    const QString id = QString::number(file["id"].toInt());
    const QString format = fileFormat(file);
    const double reportedSize = file["sizeBytes"].toDouble(-1);
    if (path.isEmpty() || (!file["sizeBytes"].isNull() && !file["sizeBytes"].isUndefined() &&
        (!file["sizeBytes"].isDouble() || reportedSize < 0 || reportedSize > maxBook || reportedSize != qint64(reportedSize)))) { finish(QCoreApplication::translate("BookOrbit", "The file is unsupported or exceeds 100 MiB"), false, "download"); return; }
    const auto previous = downloads.value(id).toObject();
    if (!previous.isEmpty() && previous["bookId"] != book["id"]) {
        finish(QCoreApplication::translate("BookOrbit", "This file is already linked to another book. Refresh the details."), false, "download"); return;
    }
    const QString oldFile = previous.isEmpty() ? path : recordFile(id, previous);
    if (oldFile.isEmpty()) { finish(QCoreApplication::translate("BookOrbit", "Invalid downloaded book record"), false, "download"); return; }
    if ((QFile::exists(oldFile) || !previous.isEmpty()) && readerFileState(oldFile) != ReaderFileState::Closed) {
        finish(QCoreApplication::translate("BookOrbit", "Close the book in the built-in reader and retry the download"), false, "download"); return;
    }
    const qint64 expected = reportedSize < 0 ? 0 : static_cast<qint64>(reportedSize);
    const QString directory = downloadDirectory();
    if (!accessibleDirectory(directory) || !sameStorage(scopeDir,directory)) {
        finish(QCoreApplication::translate("BookOrbit", "The download folder is unavailable. Choose another folder in Settings."), false, "download"); return;
    }
    const QString stem = id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString staging = scopeDir+"/"+stem+".part";
    const QString finalPath = directory+"/"+stem+"."+format;
    auto output = std::make_shared<QSaveFile>(staging);
    if (!output->open(QIODevice::WriteOnly)) { finish(QCoreApplication::translate("BookOrbit", "Could not create the book file"), false, "download"); return; }
    startFileOperation(); fileDownloading=true; cancelled=false; message=QCoreApplication::translate("BookOrbit", "Downloading book…");
    auto request = this->request("/api/v1/books/files/"+QString::number(file["id"].toInt())+"/download");
    auto reply = network.get(request);
    activeDownload = reply; reply->setReadBufferSize(65536);
    const auto generation=fileGeneration;
    const auto transferContext=fileContext(id,oldFile);
    notifyOperation(); emit feedbackChanged();
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto size = std::make_shared<qint64>(0);
    auto drain = [reply, output, hash, size] {
        const auto chunk = reply->readAll();
        *size += chunk.size();
        if (*size > maxBook || output->write(chunk) != chunk.size()) { output->cancelWriting(); reply->abort(); return; }
        hash->addData(chunk);
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    QTimer::singleShot(120000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, output, hash, size, expected, file, drain, book, id, oldFile, previous, staging, finalPath, stem, directory, renew, format,generation,transferContext] {
        if (generation!=fileGeneration) { output->cancelWriting(); reply->deleteLater(); return; }
        if (reply->bytesAvailable()) drain();
        reply->deleteLater();
        if (!acceptFileContext(transferContext)) { output->cancelWriting(); return; }
        logEvent("download", int(reply->error()), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        if (cancelled) {
            output->cancelWriting(); retryKind = 0;
            finish(QCoreApplication::translate("BookOrbit", "Download cancelled. The previous file was kept."), false, "download"); return;
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401) {
            output->cancelWriting(); activeDownload.clear();
            if (renew && !refreshToken.isEmpty()) {
                renewSession([this, book] { downloadBook(book, false); });
                return;
            }
            invalidateSession();
            finish(QCoreApplication::translate("BookOrbit", "Session expired. Sign in again to download."), false, "download"); return;
        }
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
            reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("application/json") ||
            (reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/html") && format != "html" && format != "htm") ||
            *size == 0 || *size > maxBook || (expected > 0 && *size != expected)) {
            output->cancelWriting();
            finish(QCoreApplication::translate("BookOrbit", "Download incomplete. Try again; the previous file was kept."), false, "download"); return;
        }
        if ((QFile::exists(oldFile) || downloads.contains(id)) && readerFileState(oldFile) != ReaderFileState::Closed) {
            output->cancelWriting();
            finish(QCoreApplication::translate("BookOrbit", "The book is open in the built-in reader. Close it and retry the download."), false, "download"); return;
        }
        const QString newHash = QString::fromLatin1(hash->result().toHex());
        const auto context=transferContext;
        message=QCoreApplication::translate("BookOrbit", "Checking the book file…"); emit feedbackChanged();
        fileTask=files.submit(this,[oldFile](const FileCancellation &cancel) {
            const auto before=fileStamp(oldFile); return std::make_pair(digestFile(oldFile,cancel),before);
        },[this,context,output,book,id,oldFile,previous,staging,finalPath,stem,directory,format,newHash,size,file](const auto &result,bool wasCancelled) {
            if (!acceptFileContext(context,result.second) || wasCancelled) { output->cancelWriting(); return; }
            const auto oldStamp=result.second;
            if ((QFile::exists(oldFile) || !previous.isEmpty()) && readerFileState(oldFile)!=ReaderFileState::Closed) {
                output->cancelWriting(); finish(QCoreApplication::translate("BookOrbit", "Close the book in the built-in reader and retry the download"),false,"download"); return;
            }
            if (downloads.value(id).toObject()["sha256"] == newHash &&
                !localFile(book).isEmpty() && result.first==newHash.toLatin1()) {
                output->cancelWriting();
                auto record=downloads.value(id).toObject();
                if (record.contains("remoteFileChanged")) {
                    record.remove("remoteFileChanged");
                    record.remove("syncStatus");
                    if (!saveRecord(id,record)) {
                        finish(QCoreApplication::translate("BookOrbit", "Could not save the download result. The previous book is still available."),false,"download"); return;
                    }
                }
                clearSyncResult(id);
                rememberFile(book);
                finish(QCoreApplication::translate("BookOrbit", "Book already downloaded"), true, "download"); return;
            }
            QJsonObject record{{"bookId", book["id"]}, {"fileId", file["id"]}, {"filename", stem+"."+format}, {"format", format},
                                     {"sha256", newHash}, {"bytes", double(*size)}, {"book", smallBook(book)}};
            if (previous["sha256"].toString() == newHash && previous["format"].toString("epub") == format &&
                previous["progress"].isObject()) record["progress"] = previous["progress"];
            if (directory != scopeDir) record["directory"] = directory;
            // Recovery needs file identity, not duplicated book metadata or progress.
            auto identity = [](const QJsonObject &saved) {
                QJsonObject result;
                for (const auto key : {"fileId", "filename", "format", "sha256", "bytes", "directory"})
                    if (saved.contains(key)) result[key] = saved[key];
                return result;
            };
            const QString transfers = scopeDir+"/transfers";
            if (!QDir().mkpath(transfers) || !accessibleDirectory(transfers) ||
                !writeObject(transfers+"/"+stem+".json", {{"fileId", file["id"]}, {"previous", identity(previous)}, {"next", identity(record)}})) {
                output->cancelWriting();
                finish(QCoreApplication::translate("BookOrbit", "Could not save the download journal. The previous book is still available."), false, "download"); return;
            }
            if (!accessibleDirectory(directory) || !sameStorage(scopeDir,directory) || !output->commit()) {
                cleanupBooks(); finish(QCoreApplication::translate("BookOrbit", "Could not save the book"),false,"download"); return;
            }
            fileTask=files.submit(this,[staging,format](const FileCancellation &cancel) {
                const auto before=fileStamp(staging);
                return std::make_pair(validContent(staging,format,cancel),before);
            },[this,context,oldStamp,oldFile,staging,finalPath,directory,id,record,book,newHash,format](const auto &validation,bool wasCancelled) {
                if (!acceptFileContext(context,oldStamp) || wasCancelled) { cleanupBooks(); return; }
                if (!validation.first || !validation.second.valid || fileStamp(staging)!=validation.second ||
                    !accessibleDirectory(directory) || !sameStorage(scopeDir,directory) ||
                    ((QFile::exists(oldFile) || !context.record.isEmpty()) && readerFileState(oldFile)!=ReaderFileState::Closed) ||
                    !QFile::rename(staging,finalPath)) {
                    cleanupBooks(); finish(QCoreApplication::translate("BookOrbit", "Could not save the book"),false,"download"); return;
                }
                fileTask=files.submit(this,[finalPath](const FileCancellation &cancel) {
                    const auto before=fileStamp(finalPath); return std::make_pair(digestFile(finalPath,cancel),before);
                },[this,context,oldStamp,oldFile,finalPath,id,record,book,newHash,format](const auto &validation,bool wasCancelled) {
                    if (!acceptFileContext(context,oldStamp) || wasCancelled) { cleanupBooks(); return; }
                    if (validation.first!=newHash.toLatin1() || !validation.second.valid || fileStamp(finalPath)!=validation.second ||
                        ((QFile::exists(oldFile) || !context.record.isEmpty()) && readerFileState(oldFile)!=ReaderFileState::Closed)) {
                        cleanupBooks(); finish(QCoreApplication::translate("BookOrbit", "The saved book is damaged. The previous book is still available."),false,"download"); return;
                    }
                    if (!saveRecord(id, record)) {
                        cleanupBooks();
                        finish(QCoreApplication::translate("BookOrbit", "Download record not saved. The previous book is still available."), false, "download"); return;
                    }
                    clearSyncResult(id);
                    rememberFile(book);
                    cleanupBooks(finalPath); // Its digest was verified immediately before saveRecord().
                    if (readableFormat(format)) scanBook(finalPath);
                    finish(QCoreApplication::translate("BookOrbit", "Book downloaded and available offline"), true, "download");
                });
            });
        });
    });
}

void Client::open(int index, bool applyIncoming) {
    if (busy()) return;
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return;
    openBook(visible[index].toObject(), applyIncoming);
}

void Client::openBook(const QJsonObject &book, bool applyIncoming) {
    beginFeedback("book",book["id"].toInt(),book["selectedFile"].toObject()["id"].toInt());
    const auto path = localFile(book);
    if (path.isEmpty()) { invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible(); finish(QCoreApplication::translate("BookOrbit", "Download the book first"), false, "open"); return; }
    if (!readableFormat(fileFormat(book["selectedFile"].toObject()))) {
        finish(QCoreApplication::translate("BookOrbit", "File downloaded. Opening this format from the app is not supported yet."), false, "open"); return;
    }
    const QString id = QString::number(book["selectedFile"].toObject()["id"].toInt());
    startFileOperation();
    message=QCoreApplication::translate("BookOrbit", "Checking the book file…"); notifyOperation(); emit feedbackChanged();
    const auto context=fileContext(id,path);
    fileTask=files.submit(this,[path](const FileCancellation &cancel) {
        const auto stamp=fileStamp(path); const auto digest=digestFile(path,cancel);
        return std::make_pair(digest,stamp);
    },[this,book,applyIncoming,path,id,context](const auto &result,bool cancelled) {
        if (!acceptFileContext(context,result.second) || cancelled) return;
        if (result.first != context.record["sha256"].toString().toLatin1()) {
            auto record = downloads.value(id).toObject();
            record["invalid"] = true;
            if (!saveRecord(id, record)) storeRecord(id, record);
            finish(QCoreApplication::translate("BookOrbit", "The book file has changed. Download it again."), false, "open"); return;
        }
        if (!readerBookIndexed(path)) {
            scanBook(path);
            finish(QCoreApplication::translate("BookOrbit", "The book is not registered in the library yet. Wait and tap Read again."), false, "open");
            return;
        }
        auto record = downloads.value(id).toObject();
        auto sync = record["progress"].toObject();
        const auto pending = sync["pending"].toObject();
        if (applyIncoming && !pending.isEmpty()) {
            finish(QCoreApplication::translate("BookOrbit", "The incoming position has not been applied yet. Sync the position or choose Read locally."),false,"open");
            return;
        }

        const bool ok = openReader(path);
        if (ok) rememberFile(book);
        finish(ok ? QCoreApplication::translate("BookOrbit", "Opening in the built-in reader…") : QCoreApplication::translate("BookOrbit", "Could not start the reader"), ok, "open");
    });
}

QStringList Client::accounts() const {
    QStringList result;
    for (auto value : savedAccounts) {
        const auto account = value.toObject();
        result << account["username"].toString()+" · "+account["server"].toString();
    }
    return result;
}

bool Client::validDownloadDirectory(const QString &path) const {
    static const QRegularExpression accountFolder("^[0-9a-f]{24}(?:/|$)");
    return storagePath(path) && accessibleDirectory(path) && sameStorage(scopeDir,path) &&
           !accountFolder.match(QDir(rootDir).relativeFilePath(path)).hasMatch();
}

bool Client::setDownloadDirectory(const QString &path) {
    if (busy()) return false;
    beginFeedback("settings");
    if (!validDownloadDirectory(path)) {
        finish(QCoreApplication::translate("BookOrbit", "Choose an accessible folder in internal storage, outside system folders."), false, "settings"); return false;
    }
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["downloadDirectory"] = path;
    QString error;
    if (!writeObject(rootDir+"/preferences.json", preferences, &error)) {
        logEvent("folder save failed: "+error);
        finish(QCoreApplication::translate("BookOrbit", "Could not save the folder: ")+error, false, "settings"); return false;
    }
    bookDirectory = path; emit settingsChanged();
    logEvent("folder saved");
    finish(QCoreApplication::translate("BookOrbit", "Folder saved. Previously downloaded books have stayed where they were."), true, "settings");
    return true;
}

bool Client::setDiagnosticLogging(bool enabled) {
    beginFeedback("settings");
    if(!enabled) logEvent("diagnostics disabled");
    diagnostics = enabled; emit settingsChanged();
    if(enabled) { logEvent("diagnostics enabled"); emit diagnosticsEnabled(); }
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["diagnosticLogging"] = enabled;
    QString error;
    if (!writeObject(rootDir+"/preferences.json", preferences, &error)) {
        finish((enabled ? QCoreApplication::translate("BookOrbit", "Logging is enabled until the app closes. ") : QCoreApplication::translate("BookOrbit", "Logging is disabled. "))+QCoreApplication::translate("BookOrbit", "Could not save the setting: ")+error,
               false, "settings"); return false;
    }
    finish(enabled ? QCoreApplication::translate("BookOrbit", "Diagnostic logging enabled") : QCoreApplication::translate("BookOrbit", "Diagnostic logging disabled"), true, "settings");
    return true;
}

void Client::logEvent(const QString &event, int code, int http) {
    logDiagnostic(event,{{"error",code},{"http",http}});
}
void Client::logDiagnostic(const QString &event, QJsonObject fields) {
    if(!diagnostics) return;
    const QString path=diagnosticLogPath();
    auto failed=[&](const QString &reason) {
        const auto text=QCoreApplication::translate("BookOrbit","Could not write diagnostic log: ")+reason;
        if(logError!=text) { logError=text; emit settingsChanged(); }
        std::fprintf(stderr,"bookorbit diagnostic log: %s\n",qPrintable(reason));
    };
    if(QFileInfo(path).isSymLink() || QFileInfo(path+".1").isSymLink()) { failed("Unsafe log path"); return; }
    if(QFileInfo(path).size()>128*1024) {
        if((QFile::exists(path+".1") && !QFile::remove(path+".1")) || !QFile::rename(path,path+".1")) {
            failed("Cannot rotate log"); return;
        }
    }
    QFile file(path);
    if(!file.open(QIODevice::WriteOnly|QIODevice::Append)) { failed(file.errorString()); return; }
    // Only explicit diagnostic fields: never response bodies, credentials or signed URLs.
    fields["event"]=event; fields["at"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    fields["pid"]=QCoreApplication::applicationPid(); fields["version"]=QCoreApplication::applicationVersion();
    fields["seq"]=++diagnosticSequence; fields["elapsedMs"]=diagnosticClock.elapsed();
    const auto line=QJsonDocument(fields).toJson(QJsonDocument::Compact)+"\n";
    if(file.write(line)!=line.size() || !file.flush()) { failed(file.errorString()); return; }
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    if(!logError.isEmpty()) { logError.clear(); emit settingsChanged(); }
}

QVariantList Client::directories(const QString &path) const {
    QVariantList result;
    if (!validDownloadDirectory(path)) return result;
    for (const auto &entry : QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase)) {
        const QString child = entry.absoluteFilePath();
        if (validDownloadDirectory(child))
            result.append(QVariantMap{{"name", entry.fileName()}, {"path", child}});
    }
    return result;
}

bool Client::configure(const QString &address, const QString &name) {
    if (busy()) return false;
    beginFeedback("connection");
    QString normalized = address.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    const QUrl url(normalized, QUrl::StrictMode);
    if (!validServer(url) || name.trimmed().isEmpty() || name.size() > 200) {
        finish(QCoreApplication::translate("BookOrbit", "Enter an HTTPS address and username."), false, "settings");
        return false;
    }
    QJsonObject account{{"server", url.toString()}, {"username", name}};
    auto next = savedAccounts;
    int selected = -1;
    for (int i=0; i<next.size(); ++i) {
        const auto entry = next[i].toObject();
        if (entry["server"] == account["server"] && entry["username"] == name) { selected = i; break; }
    }
    if (selected < 0) { selected = next.size(); next.append(account); }
    else next[selected] = account;
    QString error;
    if (!writeObject(rootDir+"/accounts.json", {{"accounts", next}, {"selected", selected}}, &error)) {
        logEvent("account save failed: "+error);
        finish(QCoreApplication::translate("BookOrbit", "Could not save the connection: ")+error, false, "settings"); return false;
    }
    dismissConflict();
    stopCovers();
    token.clear(); refreshToken.clear(); retryKind = 0; savedAccounts = next;
    endpoint = url; user = name;
    localView = true; loadRecords();
    emit connectionChanged(); emit catalogChanged(); emit settingsChanged();
    finish(QCoreApplication::translate("BookOrbit", "Connection saved. Downloaded books are available without signing in."), true, "settings");
    return true;
}

void Client::selectAccount(int index) {
    if (busy() || index < 0 || index >= savedAccounts.size()) return;
    const auto account = savedAccounts[index].toObject();
    configure(account["server"].toString(), account["username"].toString());
}

void Client::logout() {
    if (busy()) return;
    ++accountGeneration;
    beginFeedback("connection");
    stopCovers();
    const QString credential = refreshToken;
    const QString sessionFile = scopeDir+"/session.json";
    if (QFile::exists(sessionFile) && !QFile::remove(sessionFile)) {
        sessionNotice=QCoreApplication::translate("BookOrbit", "Could not remove the saved session. Check device storage.");
        finish(sessionNotice, false, "logout"); return;
    }
    sessionStored = false; sessionNotice.clear();
    token.clear(); refreshToken.clear(); retryKind = 0; localView = true;
    items = {}; count = currentPage = 0; collectionItems = {}; detailBook = {};
    ++detailGeneration; browseHistory.clear(); collectionList = false; activeCollection = 0; activeCollectionName.clear();
    invalidateLibrary(); invalidateSync(); detailPending=true;
    emit connectionChanged(); emit catalogChanged(); notifyOperation(); publishDataChanges();
    cleanupCovers();
    logEvent("local logout");
    if (!credential.isEmpty()) {
        working = true; message = QCoreApplication::translate("BookOrbit", "Signing out…");
        notifyOperation(); emit feedbackChanged();
        jsonRequest("/api/v1/auth/logout", {{"refreshToken", credential}}, [this](const QJsonObject &) {
            finish(QCoreApplication::translate("BookOrbit", "Session revoked. Downloaded books were kept."), true, "logout");
        }, false);
        return;
    }
    finish(QCoreApplication::translate("BookOrbit", "Signed out on this device. Downloaded books were kept."), true, "logout");
}

void Client::showDownloaded(bool value) {
    if (busy()) return;
    detailBook = {}; emit detailChanged(); ++detailGeneration; browseHistory.clear(); collectionList = false; activeCollection = 0; activeCollectionName.clear();
    if (!value) { refresh(); return; }
    localView = true;
    refreshRecents();
    stopCovers();
    items = {}; count = currentPage = 0;
    cleanupCovers();
    invalidateLibrary(); emit catalogChanged(); publishDataChanges();
}

void Client::ensureLibrary() const {
    if (libraryDirty) {
#ifdef BOOKORBIT_TEST_HTTP
        ++presentationCounters.libraryBuilds;
#endif
        LocalFiles files;
        QJsonArray source=items;
        if (localView) {
            QMap<int, QJsonObject> grouped;
            for (auto it = downloads.begin(); it != downloads.end(); ++it) {
                const auto record = it.value().toObject();
                auto book = record["book"].toObject();
                if (book.isEmpty()) book = {{"id", record["bookId"]}, {"title", QCoreApplication::translate("BookOrbit", "Book · file ")+it.key()}};
                auto file = book["selectedFile"].toObject();
                file["id"] = record["fileId"];
                file["format"] = record["format"].toString("epub");
                file["role"] = "content";
                if (!file.contains("sizeBytes")) file["sizeBytes"] = record["bytes"];
                book["selectedFile"] = file;
                const int id = book["id"].toInt();
                auto merged = grouped.value(id);
                auto files = merged.value("files").toArray();
                if (merged.isEmpty() || book["detailed"].toBool()) merged = book;
                files.append(file);
                merged["files"] = files;
                grouped[id] = merged;
            }
            source={};
            for (const auto &book : grouped) source.append(book);
        }
        libraryRows.clear();
        libraryRecent.clear();
        QJsonObject latest;
        for (const auto value : source) {
            auto book=selectBookFile(value.toObject(), &files);
            const auto recent=recentFile(book, &files);
            book["nativeOpenTime"]=recent["nativeOpenTime"];
            const QString text=(book["title"].toString()+"\n"+authorNames(book["authors"].toArray())+"\n"+book["seriesName"].toString())
                .normalized(QString::NormalizationForm_KC).toCaseFolded();
            libraryRows.append({book, bookSummary(book, &files), text});
            if (nativeRecents.available && recent["nativeOpenTime"].toDouble()>0 &&
                (latest.isEmpty() || recent["nativeOpenTime"].toDouble()>latest["nativeOpenTime"].toDouble() ||
                 (recent["nativeOpenTime"]==latest["nativeOpenTime"] &&
                  (QString::localeAwareCompare(recent["title"].toString(),latest["title"].toString())<0 ||
                   (recent["title"]==latest["title"] && recent["id"].toInt()<latest["id"].toInt()))))) latest=recent;
        }
        if (!latest.isEmpty()) libraryRecent=bookSummary(latest, &files);
        libraryDirty=false;
        libraryOrderDirty=true;
    }
    if (libraryOrderDirty) {
        if (localView) {
#ifdef BOOKORBIT_TEST_HTTP
            ++presentationCounters.librarySorts;
#endif
            std::sort(libraryRows.begin(),libraryRows.end(),[this](const LibraryRow &left,const LibraryRow &right) {
                const auto &a=left.book, &b=right.book;
                if (librarySort=="recent" && nativeRecents.available) {
                    const double l=a["nativeOpenTime"].toDouble(),r=b["nativeOpenTime"].toDouble();
                    if (l!=r) return l>r;
                }
                if (librarySort=="author") {
                    const int cmp=QString::localeAwareCompare(left.summary["author"].toString(),right.summary["author"].toString());
                    if (cmp) return cmp<0;
                }
                const int cmp=QString::localeAwareCompare(a["title"].toString(),b["title"].toString());
                return cmp ? cmp<0 : a["id"].toInt()<b["id"].toInt();
            });
        }
        libraryAll={};
        for (const auto &row : libraryRows) libraryAll.append(row.book);
        libraryOrderDirty=false;
        libraryFilterDirty=true;
    }
    if (libraryFilterDirty) {
        const QString query=libraryQuery.normalized(QString::NormalizationForm_KC).toCaseFolded();
        libraryVisible={}; libraryBooks.clear();
        for (const auto &row : libraryRows) {
            if (localView && !query.isEmpty() && !row.search.contains(query)) continue;
            libraryVisible.append(row.book); libraryBooks.append(row.summary);
        }
        libraryFilterDirty=false;
    }
}

QJsonArray Client::visibleItems(bool applyQuery) const {
    ensureLibrary();
    return applyQuery ? libraryVisible : libraryAll;
}

void Client::cancelDownload() {
    if (!downloading()) return;
    cancelled=true;
    if (fileTask) {
        fileTask->store(true); cleanupBooks(); retryKind=0;
        finish(QCoreApplication::translate("BookOrbit", "Download cancelled. The previous file was kept."),false,"download");
    } else if (activeDownload) activeDownload->abort();
    else { retryKind=0; finish(QCoreApplication::translate("BookOrbit", "Download cancelled. The previous file was kept."),false,"download"); }
}

void Client::retry() {
    if (!canRetry()) return;
    if (retryKind == 1) refresh(retryPage, retryQuery);
    else if (retryKind == 2) downloadBook(retryBook);
    else if (retryKind == 3) refreshDetail();
    else if (retryKind == 4) showCollections();
}

QString Client::coverUrl(int bookId) const {
    const QString path = scopeDir+"/cover-"+QString::number(bookId)+".png";
    return bookId > 0 && QFileInfo::exists(path) ? QUrl::fromLocalFile(path).toString()+"?v="+QString::number(coverRevisions.value(path).toInt()) : QString{};
}

void Client::stopCovers() {
    ++coverGeneration;
    coverQueue.clear();
    if (activeCover) activeCover->abort();
    activeCover.clear();
}

void Client::queueCovers() {
    stopCovers();
    for (auto value : items) {
        const auto book = value.toObject();
        if (!positiveId(book["id"])) continue;
        const int id=book["id"].toInt();
        if (book["hasCover"] == false) { removeCover(id); continue; }
        const QString version=book["coverVersion"].toString();
        const QString path=scopeDir+"/cover-"+QString::number(id)+".png";
        if (!version.isEmpty() && coverVersions[QString::number(id)].toString()==version && QFile::exists(path)) continue;
        if (!version.isEmpty() && coverVersions.contains(QString::number(id)) &&
            coverVersions[QString::number(id)].toString()!=version) removeCover(id);
        coverQueue.append(id);
    }
    fetchNextCover();
}

void Client::fetchNextCover() {
    if (coverQueue.isEmpty() || token.isEmpty()) return;
    if (!connectNetwork()) { stopCovers(); logEvent("cover network unavailable"); return; }
    const int id = coverQueue.takeFirst(), generation = coverGeneration;
    const QString path = scopeDir+"/cover-"+QString::number(id)+".png";
    QString version;
    for (auto value : items) if (value.toObject()["id"].toInt()==id) { version=value.toObject()["coverVersion"].toString(); break; }
    auto request = this->request("/api/v1/books/"+QString::number(id)+"/thumbnail");
    auto reply = network.get(request);
    activeCover = reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > maxJson) reply->abort();
    });
    QTimer::singleShot(15000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, path, id, version, generation] {
        reply->deleteLater();
        if (generation != coverGeneration) return;
        activeCover.clear();
        bytes->append(reply->readAll());
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) removeCover(id);
        if (reply->error() == QNetworkReply::NoError && bytes->size() <= maxJson &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            const auto account=accountGeneration;
            files.submit(this,[data=*bytes](const FileCancellation &cancel) {
                QByteArray source=data,encoded;
                QBuffer buffer(&source); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
                const auto format=reader.format(); const auto size=reader.size();
                if (!fileCancelled(cancel) && (format=="png" || format=="jpeg" || format=="webp") &&
                    size.width()>0 && size.height()>0 && size.width()<=2048 && size.height()<=2048) {
                    reader.setScaledSize(size.scaled(240,320,Qt::KeepAspectRatio)); const auto cover=reader.read();
                    QBuffer output(&encoded); output.open(QIODevice::WriteOnly);
                    if (fileCancelled(cancel) || cover.isNull() || !cover.save(&output,"PNG")) encoded.clear();
                }
                return encoded;
            },[this,path,id,version,generation,account](const QByteArray &encoded,bool cancelled) {
                if (generation!=coverGeneration || account!=accountGeneration) return;
                QSaveFile file(path);
                if (!cancelled && !encoded.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(encoded)==encoded.size() && file.commit()) {
                    if (version.isEmpty()) coverVersions.remove(QString::number(id)); else coverVersions[QString::number(id)]=version;
                    writeObject(scopeDir+"/cover-index.json",coverVersions);
                    coverRevisions[path]=++coverVersion; emit coversChanged(); cleanupCovers();
                }
                fetchNextCover();
            });
            return;
        }
        fetchNextCover();
    });
}

void Client::dismissConflict() {
    ++conflictGeneration;
    if (conflictTask) conflictTask->store(true);
    conflictTask.reset(); conflictPreparing=false; notifyOperation();
    if (conflictId.isEmpty() && conflictMessage.isEmpty() && positionChoices.isEmpty()) return;
    conflictId.clear(); conflictProfile.clear(); conflictLocal.clear(); conflictRemote={}; conflictMessage.clear();
    positionChoices.clear();
    emit conflictChanged();
}

void Client::syncAll() {
    if (busy()) return;
    beginFeedback("sync");
    if (!authenticated()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to sync"),false,"sync"); return; }
    syncQueue.clear();
    for (auto it=downloads.begin(); it!=downloads.end(); ++it)
        if (syncFormat(it.value().toObject()["format"].toString("epub"))) syncQueue.append(it.key());
    if (syncQueue.isEmpty()) { finish(QCoreApplication::translate("BookOrbit", "No downloaded books to sync"),true,"sync"); return; }
    dismissConflict();
    syncStopRequested=false; syncCompleted=0;
    syncingAll=true; syncCount=syncQueue.size(); syncSucceeded=0;
    emit syncBatchChanged(); emit conflictChanged();
    syncBook(syncQueue.takeFirst());
}

void Client::resolveProgress(bool useLocal) {
    if (!busy() && !conflictId.isEmpty()) syncBook(conflictId,useLocal?1:2);
}

void Client::verifyLibrary() {
    if (busy()) return;
    beginFeedback("settings");
    if (!authenticated()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to verify library files"),false,"verify"); return; }
    syncQueue=downloads.keys();
    if (syncQueue.isEmpty()) { finish(QCoreApplication::translate("BookOrbit", "No downloaded files to verify"),true,"verify"); return; }
    dismissConflict(); stopCovers(); retryKind=0;
    checkingLibrary=true; verificationCancelled=false;
    syncCount=syncQueue.size(); syncSucceeded=verificationDifferent=verificationErrors=0;
    emit syncBatchChanged();
    verifyBook(syncQueue.takeFirst());
}

void Client::cancelLibraryVerification() {
    if (!checkingLibrary) return;
    verificationCancelled=true;
    if (fileTask) {
        fileTask->store(true);
        finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify");
        return;
    }
    if (activeVerification) activeVerification->abort();
    // Between files the queued continuation completes cancellation before starting another file.
}

void Client::verifyBook(const QString &id) {
    syncingId=id;
    const auto record=downloads.value(id).toObject();
    const auto path=recordFile(id,record);
    const QFileInfo info(path);
    working=true;
    message=QCoreApplication::translate("BookOrbit", "Checking file %1 of %2: %3 (%4, fileId %5)…")
        .arg(syncCount-syncQueue.size()).arg(syncCount)
        .arg(record["book"].toObject()["title"].toString())
        .arg(record["format"].toString("epub").toUpper(),id);
    notifyOperation(); emit feedbackChanged();
    if (verificationCancelled) { finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify"); return; }
    startFileOperation();
    const auto context=fileContext(id,path);
    fileTask=files.submit(this,[path](const FileCancellation &cancel) {
        const auto stamp=fileStamp(path); return std::make_pair(digestFile(path,cancel),stamp);
    },[this,id,path,info,record,context](const auto &result,bool cancelled) {
        if (!acceptFileContext(context,result.second) || cancelled) return;
        if (path.isEmpty() || !info.isFile() || info.isSymLink() || info.canonicalPath()!=info.absolutePath() ||
            info.size()!=record["bytes"].toDouble() || result.first!=record["sha256"].toString().toLatin1()) {
            finish(QCoreApplication::translate("BookOrbit", "Could not verify: the local file is missing or damaged. Download it again."),false,"verify"); return;
        }
        fileTask.reset();
        verifyRemoteFile(id);
    });
}

void Client::verifyRemoteFile(const QString &id, bool renew) {
    if (verificationCancelled) { finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify"); return; }
    if (!ensureNetwork()) return;
    const auto generation=fileGeneration;
    const auto context=fileContext(id,recordFile(id,downloads.value(id).toObject()));
    auto reply=network.get(request("/api/v1/books/files/"+id+"/download"));
    activeVerification=reply; reply->setReadBufferSize(65536);
    auto hash=std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto size=std::make_shared<qint64>(0);
    auto drain=[reply,hash,size] {
        if (!reply->bytesAvailable()) return;
        const auto bytes=reply->readAll(); *size+=bytes.size();
        if (*size>maxBook) { reply->abort(); return; }
        hash->addData(bytes);
    };
    connect(reply,&QNetworkReply::readyRead,this,drain);
    QTimer::singleShot(120000,reply,[reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply,&QNetworkReply::finished,this,[this,reply,id,renew,drain,hash,size,generation,context] {
        drain(); reply->deleteLater();
        if (generation!=fileGeneration) return;
        if (!acceptFileContext(context)) return;
        activeVerification.clear();
        if (verificationCancelled) { finish(QCoreApplication::translate("BookOrbit", "Verification cancelled"),false,"verify"); return; }
        const int code=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        logEvent("library file verification",int(reply->error()),code);
        if (code==401 && renew && !refreshToken.isEmpty()) {
            renewSession([this,id] { verifyRemoteFile(id,false); }); return;
        }
        if (code==401) invalidateSession();
        const auto length=reply->header(QNetworkRequest::ContentLengthHeader);
        const auto type=reply->header(QNetworkRequest::ContentTypeHeader).toString();
        const auto format=downloads.value(id).toObject()["format"].toString("epub");
        if (reply->error()!=QNetworkReply::NoError || code!=200 || *size==0 || *size>maxBook ||
            (length.isValid() && length.toLongLong()!=*size) || type.startsWith("application/json") ||
            (type.startsWith("text/html") && format!="html" && format!="htm")) {
            finish(QCoreApplication::translate("BookOrbit", "Could not verify the server file")+(code>=400 ? QString(" (HTTP %1)").arg(code) : QString{})+".",false,"verify"); return;
        }
        auto record=downloads.value(id).toObject();
        const auto path=recordFile(id,record),remoteDigest=QString::fromLatin1(hash->result().toHex());
        fileTask=files.submit(this,[path](const FileCancellation &cancel) {
            const auto stamp=fileStamp(path); return std::make_pair(digestFile(path,cancel),stamp);
        },[this,id,record,remoteDigest,context](const auto &result,bool cancelled) mutable {
            if (!acceptFileContext(context,result.second) || cancelled) return;
            if (result.first!=record["sha256"].toString().toLatin1()) {
                finish(QCoreApplication::translate("BookOrbit", "Could not verify: the local file changed during verification."),false,"verify"); return;
            }
            const bool different=remoteDigest.toLatin1()!=record["sha256"].toString().toLatin1();
            if (different) record["remoteFileChanged"]=true;
            else record.remove("remoteFileChanged");
            if (record!=downloads.value(id).toObject() && !saveRecord(id,record)) {
                if (different) storeRecord(id,record); // Keep the known mismatch blocked in memory even when storage fails.
                finish(QCoreApplication::translate("BookOrbit", "Could not save the verification result. Resolve the storage error and try again."),false,"verify"); return;
            }
            finish(different ? QCoreApplication::translate("BookOrbit", "The server file differs. Progress sync is paused") : QCoreApplication::translate("BookOrbit", "The local file matches BookOrbit"),
                   !different,different ? "verify-different" : "verify");
        });
    });
}

void Client::syncBook(const QString &id, int choice) {
    if (busy()) return;
    if (!syncingAll) beginFeedback(detailVisible() ? "book" : "sync", downloads.value(id).toObject()["bookId"].toInt(),id.toInt());
    syncingId=id;
    if (!authenticated()) { finish(QCoreApplication::translate("BookOrbit", "Sign in to sync"),false,"progress","auth"); return; }
    auto record=downloads.value(id).toObject();
    if (record["remoteFileChanged"].toBool()) {
        finish(QCoreApplication::translate("BookOrbit", "The server file differs. Progress sync is paused"),false,"progress","file"); return;
    }
    if (!syncFormat(record["format"].toString("epub"))) {
        finish(QCoreApplication::translate("BookOrbit", "Position sync is available for EPUB and supported FB2 books. You can read this file locally."),false,"progress"); return;
    }
    const QString path=localFile(record["book"].toObject());
    QString position;
    const QString profile=readerProfile();
    startFileOperation(); retryKind=0; stopCovers();
    message=syncingAll ? QCoreApplication::translate("BookOrbit", "Checking file %1 of %2: %3…").arg(syncCount-syncQueue.size()).arg(syncCount)
        .arg(record["book"].toObject()["title"].toString()) : QCoreApplication::translate("BookOrbit", "Syncing progress…");
    notifyOperation(); emit feedbackChanged();
    if (path.isEmpty()) {
        finish(QCoreApplication::translate("BookOrbit", "Download a valid book file before syncing"),false,"progress","file"); return;
    }
    const auto reader=readerFileState(path);
    if (reader==ReaderFileState::Open) {
        finish(QCoreApplication::translate("BookOrbit", "Close the book in the built-in reader and retry sync. The Home button leaves the book open."),false,"progress","reader"); return;
    }
    if (reader==ReaderFileState::Unknown || !readerPosition(path,&position)) {
        finish(QCoreApplication::translate("BookOrbit", "Could not check the reader state. Positions were kept unchanged; try again."),false,"progress","reader_unknown"); return;
    }
    struct LocalCheck { QByteArray digest; FileStamp stamp; QString local; bool supported=true,valid=false; };
    const auto context=fileContext(id,path);
    fileTask=files.submit(this,[path,position,format=record["format"].toString()](const FileCancellation &cancel) {
        LocalCheck result; result.stamp=fileStamp(path);
        if (format=="fb2") result.supported=fb2Calibration(cancel).valid && !bookCfi(path,"pbr:/word?page=0").isEmpty();
        if (result.supported && !fileCancelled(cancel)) {
            result.local=bookCfi(path,position);
            result.valid=(position.isEmpty() || !result.local.isEmpty()) &&
                (result.local.isEmpty() || bookPosition(path,result.local,nullptr));
        }
        result.digest=digestFile(path,cancel);
        return result;
    },[this,id,path,position,choice,profile,context](const LocalCheck &result,bool cancelled) {
        if (!acceptFileContext(context,result.stamp) || cancelled) return;
        if (result.digest!=context.record["sha256"].toString().toLatin1()) {
            invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible();
            finish(QCoreApplication::translate("BookOrbit", "Download a valid book file before syncing"),false,"progress","file"); return;
        }
        QString current;
        if (readerFileState(path)!=ReaderFileState::Closed || !readerPosition(path,&current) || current!=position) {
            finish(QCoreApplication::translate("BookOrbit", "The reader state has changed. Retry sync."),false,"progress"); return;
        }
        if (!result.supported) {
            finish(QCoreApplication::translate("BookOrbit", "This FB2 structure is not supported for sync yet. Images, tables, poetry and long sections require additional validation. You can still read the original file."),false,"progress","position"); return;
        }
        if (!result.valid) {
            finish(QCoreApplication::translate("BookOrbit", "The local position format is unsupported. Progress was kept unchanged."),false,"progress","position"); return;
        }
        const QString local=result.local;
        const QString route="/api/v1/books/files/"+id+"/progress";
        // shortcut: trust the downloaded file identity; explicit library verification detects server replacements.
        jsonRequest(route,{},[this,id,path,position,local,choice,route,profile,context](const QJsonObject &remote) {
            if (!acceptFileContext(context)) return;
            QString current;
            if (readerProfile()!=profile || readerFileState(path)!=ReaderFileState::Closed || !readerPosition(path,&current) || current!=position) {
                finish(QCoreApplication::translate("BookOrbit", "The reader state has changed. Retry sync."),false,"progress"); return;
            }
            const QString remoteCfi=remote["cfi"].toString();
            const auto percent=remote["percentage"];
            if (!remote.contains("cfi") || (!remote["cfi"].isNull() && !remote["cfi"].isString()) ||
                !percent.isDouble() || percent.toDouble()<0 || percent.toDouble()>100) {
                finish(QCoreApplication::translate("BookOrbit", "Invalid progress response: expected a CFI and a percentage from 0 to 100. Progress was kept unchanged."),false,"progress"); return;
            }
            struct RemoteCheck {
                QByteArray digest; FileStamp stamp; QString point; bool valid=true,equal=false,localBaseEqual=false,remoteBaseEqual=false,estimate=false;
                double percentage=0; PreparedReaderPosition prepared;
            };
            const auto progress=downloads.value(id).toObject()["progress"].toObject();
            fileTask=files.submit(this,[path,local,remoteCfi,progress,choice](const FileCancellation &cancel) {
                RemoteCheck result; result.stamp=fileStamp(path); result.point=remoteCfi;
                if (!remoteCfi.isEmpty()) result.valid=bookPosition(path,remoteCfi,nullptr,&result.point);
                if (result.valid && !fileCancelled(cancel)) {
                    result.equal=local==result.point || sameBookPosition(path,local,result.point);
                    if (!result.equal) {
                        const auto localBase=progress["localBase"].toString();
                        const auto remoteBase=progress["remoteBase"].toObject()["cfi"].toString();
                        result.localBaseEqual=local==localBase || sameBookPosition(path,local,localBase);
                        result.remoteBaseEqual=remoteCfi==remoteBase || sameBookPosition(path,remoteCfi,remoteBase,true);
                        if (choice!=2 && !local.isEmpty()) result.estimate=bookPosition(path,local,&result.percentage);
                        if (choice!=1 && !result.point.isEmpty()) result.prepared=prepareReaderPosition(path,result.point,cancel);
                    }
                }
                result.digest=digestFile(path,cancel);
                return result;
            },[this,id,path,position,local,choice,route,profile,remote,remoteCfi,percent,context,progress](const RemoteCheck &analysis,bool cancelled) {
                if (!acceptFileContext(context,analysis.stamp) || cancelled) return;
                QString current;
                if (readerFileState(path)!=ReaderFileState::Closed || !readerPosition(path,&current) || current!=position ||
                    downloads.value(id).toObject()["progress"].toObject()!=progress) {
                    finish(QCoreApplication::translate("BookOrbit", "The reader state has changed. Retry sync."),false,"progress"); return;
                }
                if (analysis.digest!=context.record["sha256"].toString().toLatin1()) {
                    finish(QCoreApplication::translate("BookOrbit", "The local book file changed during sync. Progress was kept unchanged."),false,"progress","file"); return;
                }
                if (!analysis.valid) {
                    finish(QCoreApplication::translate("BookOrbit", "Could not match the server CFI to this book. Progress was kept unchanged."),false,"progress","position"); return;
                }
                const QString cfi=analysis.point;
                bool other=false;
                for (const auto key : {"positionSeconds","mediaOverlayFragment","mediaOverlaySectionIndex","koboLocationSource",
                                      "koboLocationType","koboLocationValue","koboContentSourceProgressPercent","koreaderProgress","narrationPercentage","narrationUpdatedAt"})
                    if (remote.contains(key) && !remote[key].isNull()) other=true;
                const bool hasPage=remote.contains("pageNumber") && !remote["pageNumber"].isNull();
                const bool remoteEmpty=cfi.isEmpty() && percent.toDouble()==0 && !hasPage && !other;
                if (cfi.isEmpty() && !remoteEmpty) {
                    finish(QCoreApplication::translate("BookOrbit", "The server has progress without an exact CFI. Automatic sync is not possible."),false,"progress","position"); return;
                }
                auto record=downloads.value(id).toObject(); auto state=record["progress"].toObject();
                if (state.contains("profile") && state["profile"].toString()!=profile) state={};
                const auto pending=state["pending"].toObject();
                auto persist=[this,id,profile,&record,&state] {
                    state["profile"]=profile;
                    record["progress"]=state;
                    if (saveRecord(id,record)) return true;
                    finish(QCoreApplication::translate("BookOrbit", "Could not save progress. Retry sync."),false,"progress"); return false;
                };
                auto acknowledge=[&] {
                    state={{"localBase",local},{"remoteBase",remote}};
                    if (persist()) { dismissConflict(); finish(QCoreApplication::translate("BookOrbit", "Progress synced"),true,"progress"); }
                };
                if (analysis.equal) { acknowledge(); return; }
                const bool known=state.contains("profile") && state["profile"].toString()==profile &&
                    state.contains("localBase") && state["remoteBase"].isObject();
                const bool localChanged=known ? !analysis.localBaseEqual : !local.isEmpty();
                const bool remoteChanged=known ? !analysis.remoteBaseEqual : !remoteEmpty;
                int action=0;
                const bool changedChoice=choice && (conflictId!=id || conflictProfile!=profile || conflictLocal!=local || conflictRemote!=remote);
                if (changedChoice) action=0; // A stale explicit choice must never become an automatic write.
                else if (choice) action=choice;
                else if (state.contains("profile") && !pending.isEmpty() && pending==remote && state["pendingLocal"].toString()==local && state["profile"].toString()==profile) action=2;
                else {
                    switch (decideSync(known,localChanged,remoteChanged)) {
                    case SyncDecision::Upload: action=1; break;
                    case SyncDecision::ApplyRemote: action=2; break;
                    case SyncDecision::Unchanged:
                        if (persist()) finish(QCoreApplication::translate("BookOrbit", "Progress unchanged"),true,"progress");
                        return;
                    case SyncDecision::Conflict: break;
                    }
                }
                if (!action) {
                    // Persist both versions; after restart the next GET rechecks the conflict.
                    state["conflictLocal"]=local; state["conflictRemote"]=remote;
                    if (!persist()) return;
                    if (!syncingAll) prepareConflict(id,local,remote,profile);
                    finish(changedChoice ? QCoreApplication::translate("BookOrbit", "The position changed while you were choosing. Review the choices again.") : QCoreApplication::translate("BookOrbit", "Choose a reading position"),false,"progress","conflict"); return;
                }
                if (action==2) {
                    if (cfi.isEmpty()) { finish(QCoreApplication::translate("BookOrbit", "The server has no exact position to open."),false,"progress"); return; }
                    // Journal before native mutation; failed writes do not advance the agreed baseline.
                    state["pending"]=remote; state["pendingLocal"]=local;
                    if (!persist()) return;
                    QString error;
                    if (!saveReaderPosition(path,position,analysis.prepared,profile,&error)) {
                        finish(error,false,"progress","pending"); return;
                    }
                    state={{"localBase",cfi},{"remoteBase",remote}};
                    if (persist()) { dismissConflict(); finish(QCoreApplication::translate("BookOrbit", "Position saved. Open the book from the built-in library."),true,"progress"); }
                    return;
                }
                if (local.isEmpty() || other || hasPage) {
                    finish(hasPage ? QCoreApplication::translate("BookOrbit", "The server position includes a page number. Upload stopped to preserve it.") :
                           other ? QCoreApplication::translate("BookOrbit", "The server record contains other reading coordinates. Upload stopped to preserve them.") :
                                   QCoreApplication::translate("BookOrbit", "No exact position on the reader yet."),false,"progress"); return;
                }
                const double percentage=analysis.percentage;
                if (!analysis.estimate) {
                    finish(QCoreApplication::translate("BookOrbit", "The position was recognized, but the book percentage could not be estimated for upload. Positions were kept unchanged."),
                           false,"progress","position"); return;
                }
                // Save before POST. An uncertain result is reconciled by the next GET, never blindly retried.
                const QJsonObject outgoing{{"source","text"},{"cfi",local},{"pageNumber",QJsonValue::Null},{"percentage",percentage}};
                state["outgoing"]=outgoing;
                if (!persist()) return;
                const auto sentContext=fileContext(id,path);
                jsonRequest(route,outgoing,[this,id,local,route,profile,sentContext](const QJsonObject &) {
                    if (!acceptFileContext(sentContext)) return;
                    jsonRequest(route,{},[this,id,local,profile,sentContext](const QJsonObject &confirmed) {
                        if (!acceptFileContext(sentContext)) return;
                        if (confirmed["cfi"].toString()!=local) {
                            finish(QCoreApplication::translate("BookOrbit", "The server position changed after upload. Check again."),false,"progress"); return;
                        }
                        auto record=downloads.value(id).toObject();
                        record["progress"]=QJsonObject{{"localBase",local},{"remoteBase",confirmed},{"profile",profile}};
                        if (!saveRecord(id,record)) { finish(QCoreApplication::translate("BookOrbit", "Uploaded, but the result could not be saved. Check again."),false,"progress"); return; }
                        dismissConflict(); finish(QCoreApplication::translate("BookOrbit", "Progress sent to BookOrbit"),true,"progress");
                    },true,true);
                });
            });
        },true,true);
    });
}
