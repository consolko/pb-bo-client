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
#include <utility>
#include <unistd.h>

namespace {
constexpr qint64 maxBook = 100 * 1024 * 1024;
constexpr int pageSize = 10;
QJsonObject catalogPayload(int page, const QString &query) {
    return {{"sort", QJsonArray{}}, {"pagination", QJsonObject{{"page", page}, {"size", pageSize}}}, {"q", query}};
}
bool failedJsonReply(const QNetworkReply &reply, int code, qsizetype size) {
    return reply.error() != QNetworkReply::NoError || code < 200 || code >= 300 || size > maxJson;
}
struct TransferStage { bool journal=false,valid=false; FileStamp stamp; QString error; };
constexpr auto sessionPermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
bool privateSessionPermissions(QFileDevice::Permissions permissions) {
    constexpr auto shared = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther | QFileDevice::ExeOwner;
    return (permissions & sessionPermissions) == sessionPermissions && !(permissions & shared);
}
// FAT permissions come from mount options; chmod cannot make a file private there.
bool permissionlessStorage(const QString &path) {
    const auto type = QStorageInfo(path).fileSystemType();
    return type == "vfat" || type == "msdos" || type == "exfat";
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
QString fileSizeLabel(const QJsonValue &size) {
    const double bytes = size.toDouble(-1);
    if (bytes < 0) return {};
    if (bytes < 1024) return QString::number(bytes, 'f', 0) + QCoreApplication::translate("BookOrbit", " B");
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024, 'f', 0) + QCoreApplication::translate("BookOrbit", " KiB");
    return QString::number(bytes / (1024 * 1024), 'f', 1) + QCoreApplication::translate("BookOrbit", " MiB");
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

static QString recordPath(const QString &scopeDir,const QString &id,const QJsonObject &record) {
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

static QString availableRecord(const QString &scope,const QString &id,const QJsonObject &record) {
    const auto path=recordPath(scope,id,record);
    const QFileInfo info(path);
    return !record["invalid"].toBool() && !path.isEmpty() && info.canonicalPath()==info.absolutePath() &&
        info.isFile() && !info.isSymLink() && info.size()==record["bytes"].toDouble() ? path : QString{};
}

namespace {
const QStringList libraryRoles{"bookId","title","author","seriesName","seriesIndex","readStatus","readingProgress",
    "fileId","format","fileCount","formats","localFormats","downloaded","readable","canSync","supported",
    "needsRepair","syncResult","pendingProgress","remoteFileChanged","hasConflict"};
int libraryBookId(const QJsonObject &record) {
    const auto book=record["book"].toObject();
    return book.isEmpty() ? record["bookId"].toInt() : book["id"].toInt();
}
QString syncGroupKey(const QString &id,const QJsonObject &record) {
    const auto book=record["book"].toObject();
    return positiveId(book["id"]) ? "book:"+QString::number(book["id"].toInt()) : "file:"+id;
}
const QStringList syncPriority{"conflict","file","position","reader","reader_unknown","error","auth","pending","uncertain","network","unknown","synced"};
bool hasPageNumber(const QJsonObject &remote) { return remote.contains("pageNumber") && !remote["pageNumber"].isNull(); }
// Server coordinates besides the CFI; uploading a CFI alone would discard them.
bool hasOtherCoordinates(const QJsonObject &remote) {
    for (const auto key : {"positionSeconds","mediaOverlayFragment","mediaOverlaySectionIndex","koboLocationSource",
                          "koboLocationType","koboLocationValue","koboContentSourceProgressPercent","koreaderProgress","narrationPercentage","narrationUpdatedAt"})
        if (remote.contains(key) && !remote[key].isNull()) return true;
    return false;
}

}
QHash<int,QByteArray> LibraryModel::roleNames() const {
    QHash<int,QByteArray> result;
    for (int i=0;i<libraryRoles.size();++i) result[Qt::UserRole+1+i]=libraryRoles[i].toUtf8();
    return result;
}
QVariant LibraryModel::data(const QModelIndex &index,int role) const {
    const int name=role-Qt::UserRole-1;
    if (!index.isValid() || index.row()<0 || index.row()>=rows.size() || name<0 || name>=libraryRoles.size()) return {};
    return rows[index.row()].value.value(libraryRoles[name]);
}
QVariantList LibraryModel::snapshot() const {
    if (snapshotDirty) {
#ifdef BOOKORBIT_TEST_HTTP
        ++snapshots;
#endif
        cachedSnapshot.clear(); cachedSnapshot.reserve(rows.size());
        for (const auto &row:rows) cachedSnapshot.append(row.value);
        snapshotDirty=false;
    }
    return cachedSnapshot;
}

void LibraryModel::indexRows(int first,int last) {
    for (int i=first;i<=last;++i) {
#ifdef BOOKORBIT_TEST_HTTP
        ++indexVisits;
#endif
        positions[rows[i].id]=i;
    }
}
int LibraryModel::rowFor(int bookId) const {
#ifdef BOOKORBIT_TEST_HTTP
    ++lookups;
#endif
    return positions.value(bookId,-1);
}
void LibraryModel::update(int row,const QVariantMap &value) {
    const auto &before=rows[row].value; QList<int> roles;
    Q_ASSERT(rows[row].id==value.value("bookId").toInt());
    for (int i=0;i<libraryRoles.size();++i) if (before.value(libraryRoles[i])!=value.value(libraryRoles[i])) roles.append(Qt::UserRole+1+i);
    if (roles.isEmpty()) return;
    snapshotDirty=true; rows[row].value=value; emit dataChanged(index(row),index(row),roles);
}
void LibraryModel::insert(int row,const QVariantMap &value) {
    snapshotDirty=true; beginInsertRows({},row,row); rows.insert(row,{value.value("bookId").toInt(),value});
    indexRows(row,rows.size()-1); endInsertRows();
}
void LibraryModel::remove(int row) {
    snapshotDirty=true; beginRemoveRows({},row,row); positions.remove(rows[row].id); rows.removeAt(row);
    indexRows(row,rows.size()-1); endRemoveRows();
}
void LibraryModel::move(int from,int to) {
    if (from==to) return;
    snapshotDirty=true; beginMoveRows({},from,from,{},to>from ? to+1 : to); rows.move(from,to);
    indexRows(std::min(from,to),std::max(from,to)); endMoveRows();
}
void LibraryModel::replace(const QVariantList &values,bool reset) {
    bool same=rows.size()==values.size();
    if (same) for (int i=0;i<values.size();++i) {
#ifdef BOOKORBIT_TEST_HTTP
        ++sequenceVisits;
#endif
        if (rows[i].id!=values[i].toMap().value("bookId").toInt()) { same=false; break; }
    }
    if (same && !reset) {
        for (int i=0;i<values.size();++i) update(i,values[i].toMap());
        return;
    }
    snapshotDirty=true; beginResetModel(); rows.clear(); positions.clear(); rows.reserve(values.size());
    for (const auto &value:values) { const auto map=value.toMap(); rows.append({map.value("bookId").toInt(),map}); }
    indexRows(0,rows.size()-1); endResetModel();
}

QHash<int,QByteArray> SyncModel::roleNames() const {
    static const QHash<int,QByteArray> names{{Key,"key"},{Title,"title"},{Author,"author"},{State,"state"},{Label,"label"},{Group,"group"},{Files,"files"},{SortKey,"sortKey"},{Data,"modelData"}};
    return names;
}
QVariant SyncModel::data(const QModelIndex &index,int role) const {
    if (!index.isValid() || index.row()<0 || index.row()>=rows.size()) return {};
    const auto &row=rows[index.row()];
    if (role==Data) return row;
    if (role==SortKey) return QVariantList{row.value("state"),row.value("title"),row.value("key")};
    return row.value(QString::fromLatin1(roleNames().value(role)));
}
QVariantMap SyncModel::get(const QString &key) const {
    const int row=positions.value(key,-1); return row<0 ? QVariantMap{} : rows[row];
}
void SyncModel::put(const QString &key,const QVariantMap &value) {
    int row=positions.value(key,-1);
    if (row<0) {
        if (value.isEmpty()) return;
        row=rows.size(); beginInsertRows({},row,row); rows.append(value); positions[key]=row; endInsertRows(); return;
    }
    if (value.isEmpty()) {
        beginRemoveRows({},row,row); positions.remove(key); rows.removeAt(row);
        for (int i=row;i<rows.size();++i) positions[rows[i].value("key").toString()]=i;
        endRemoveRows(); return;
    }
    const auto before=rows[row]; if (before==value) return;
    QList<int> roles;
    const auto names=roleNames();
    for (auto it=names.begin();it!=names.end();++it)
        if (it.key()!=SortKey && it.key()!=Data && before.value(QString::fromLatin1(it.value()))!=value.value(QString::fromLatin1(it.value()))) roles.append(it.key());
    if (before.value("state")!=value.value("state") || before.value("title")!=value.value("title")) roles.append(SortKey);
    roles.append(Data); rows[row]=value; emit dataChanged(index(row),index(row),roles);
}
void SyncModel::replace(const QList<QVariantMap> &values) {
    beginResetModel(); rows=values; positions.clear();
    for (int i=0;i<rows.size();++i) positions[rows[i].value("key").toString()]=i;
    endResetModel();
}
SyncProxy::SyncProxy(QObject *parent):QSortFilterProxyModel(parent) {
    setSortRole(SyncModel::SortKey); setFilterRole(SyncModel::Group); setFilterKeyColumn(0); sort(0);
}
bool SyncProxy::lessThan(const QModelIndex &a,const QModelIndex &b) const {
    const auto left=sourceModel()->data(a,SyncModel::SortKey).toList(),right=sourceModel()->data(b,SyncModel::SortKey).toList();
    const int l=syncPriority.indexOf(left[0].toString()),r=syncPriority.indexOf(right[0].toString());
    if (l!=r) return l<r;
    const int title=QString::localeAwareCompare(left[1].toString(),right[1].toString());
    return title ? title<0 : left[2].toString()<right[2].toString();
}
void SyncProxy::setFilter(const QString &value) {
    group=value; setFilterFixedString(value=="all" ? QString{} : value);
}

Client::Client(QUrl server, QString root, QObject *parent, bool restoreAccount)
    : QObject(parent), libraryModel(this), syncRows(this), syncView(this), endpoint(server), rootDir(root) {
    syncView.setSourceModel(&syncRows);
    connect(&covers,&CoverCache::changed,this,&Client::coversChanged);
    connect(&covers,&CoverCache::drained,this,&Client::cleanupCovers);
    connect(&covers,&CoverCache::networkUnavailable,this,[this] { logEvent("cover network unavailable"); });
    connect(&libraryModel,&QAbstractItemModel::modelAboutToBeReset,this,[this] { if (preservingLibraryAnchor) emit libraryAboutToReset(); });
    connect(&libraryModel,&QAbstractItemModel::modelReset,this,[this] { if (preservingLibraryAnchor) emit libraryReset(); });
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
    loadRecords(true);
    localView = true;
    if (!dataDirectoryCreated)
        finish(uiMessage("could_not_create_the_app_data_folder_1_check_free_space_and_acce_fb2fd8").arg(rootDir), false, "storage");
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
        finish(uiMessage("the_file_profile_or_account_changed_retry_the_operation"),false,"file");
        return false;
    }
    return true;
}

bool Client::prepareUpdate() {
    if(busy() || downloading() || checkingLibrary || syncingAll) return false;
    updateLocked=true; working=true; covers.stop(); notifyOperation(); return true;
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
        finish(uiMessage("could_not_save_the_language_setting"), false, "settings");
        return false;
    }
    languagePreference = language;
    invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible();
    emit languageChanged();
    if (!conflictId.isEmpty()) prepareConflict(conflictId, conflictLocal, conflictRemote, conflictProfile);
    finish(uiMessage("language_saved"), true, "settings");
    return true;
}

void Client::retranslate() {
    invalidateLibrary(); invalidateSync(); detailPending=true;
    emit feedbackChanged(); emit connectionChanged(); emit settingsChanged(); emit syncBatchChanged();
    publishDataChanges();
}

void Client::loadRecords(bool startup) {
    recordsReady={};
    resetSessionRestore();
    ++accountGeneration; ++recentsGeneration;
    if (startup) startupAccountGeneration = accountGeneration;
    files.submit(this,[](const FileCancellation &) { clearEpubCache(); return true; },[](bool,bool) {});
    if (recentsTask) recentsTask->store(true);
    cancelCleanup();
    recentsScheduled=false; recentsFull=false; recentsBooks.clear(); recentsRequested=false;
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
            (permissionlessStorage(scopeDir) || privateSessionPermissions(sessionInfo.permissions()) ||
             (QFile::setPermissions(sessionFile, sessionPermissions) &&
              privateSessionPermissions(QFileInfo(sessionFile).permissions())));
        if (!protectedFile) {
            sessionNotice = QFile::remove(sessionFile) ? uiMessage("could_not_protect_the_saved_session_sign_in_again")
                                                       : uiMessage("could_not_protect_or_remove_sign_in_data_check_device_storage");
        } else {
            const auto session = readObject(sessionFile);
            static const QRegularExpression nativeRefresh("^[a-f0-9]{64}$");
            refreshToken = session["refreshToken"].toString();
            if (!nativeRefresh.match(refreshToken).hasMatch()) {
                refreshToken.clear();
                if (!QFile::remove(sessionFile)) sessionNotice = uiMessage("could_not_remove_old_sign_in_data");
            } else if (session.contains("password")) {
                if (!saveSession()) refreshToken.clear();
            } else sessionStored = true;
        }
    }
    invalidateLibrary(); invalidateSync(); detailPending=true;
    nativeRecents={}; clearNavigation();
    fileChoices = readObject(scopeDir+"/file-choices.json");
    covers.setScope(scopeDir);
    downloads = {};
    savedRecordStamps.clear(); durableRecordStamps.clear(); blockedTransfers.clear(); filesByBook.clear();
    syncResults = {};
    libraryFiles.clear(); syncFiles.clear(); knownFiles.clear();
    registryLoading=true; notifyOperation(); emit libraryLoadingChanged();
    const auto account=accountGeneration; const auto scope=scopeDir;
    if (registryTask) registryTask->store(true);
    registryTask=files.submit(this,[scope](const FileCancellation &cancel) {
        struct Snapshot {
            QJsonObject records; QHash<QString,FileStamp> stamps; QMap<int,QStringList> index;
            LocalFiles files; bool cleanup=false;
        } snapshot;
        bool recordsReadable = true;
        const QString recordsDir = scope+"/records";
        const bool createdRecords = QDir().mkpath(recordsDir);
        const QFileInfo recordsInfo(recordsDir);
        recordsReadable = createdRecords && recordsInfo.isDir() && recordsInfo.isReadable() &&
                          recordsInfo.canonicalFilePath() == recordsDir;
        for (const auto &name : QDir(recordsDir).entryList({"*.json"}, QDir::AllEntries | QDir::NoDotAndDotDot)) {
            if (fileCancelled(cancel)) return snapshot;
            const QString id = name.left(name.size()-5);
            if (!validId(id)) continue;
            const QFileInfo info(recordsDir+"/"+name);
            if (!info.isFile() || info.isSymLink()) { recordsReadable = false; continue; }
            const auto stamp=fileStamp(info.absoluteFilePath());
            const auto record = readObject(recordsDir+"/"+name);
            if (!recordPath(scope,id,record).isEmpty()) {
                snapshot.records[id] = record;
                if (stamp.valid && fileStamp(info.absoluteFilePath())==stamp) snapshot.stamps[id]=stamp;
            }
            else recordsReadable = false;
        }
        const QString oldPath = scope+"/downloads.json";
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
        const auto legacyItems = old.isEmpty() ? QJsonArray{} : readObject(scope+"/catalog.json")["items"].toArray();
        for (auto it=old.begin(); it!=old.end(); ++it) {
            if (fileCancelled(cancel)) return snapshot;
            const QString id = it.key();
            if (!validId(id) || snapshot.records.contains(id)) continue;
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
            if (recordPath(scope,id,record).isEmpty()) { migrated = false; continue; }
            const QString destination = recordsDir+"/"+id+".json";
            if (!writeObject(destination, record) || readObject(destination) != record) migrated = false;
            else snapshot.stamps[id]=fileStamp(destination);
            snapshot.records[id] = record;
        }
        for (auto it=snapshot.records.begin(); it!=snapshot.records.end(); ++it) {
            if (fileCancelled(cancel)) return snapshot;
            const auto record=it.value().toObject();
            QSet<int> keys{record["bookId"].toInt(),record["book"].toObject()["id"].toInt()};
            for (int key:keys) snapshot.index[key].append(it.key());
            snapshot.files[it.key().toInt()]=availableRecord(scope,it.key(),record);
        }
        if (QFile::exists(oldPath) && migrated)
            migrated = QFile::rename(oldPath, scope+"/downloads-v1.json");
        if (migrated) QFile::remove(scope+"/catalog.json");
        snapshot.cleanup=migrated && recordsReadable;
        return snapshot;
    },[this,account,startup](auto snapshot,bool cancelled) {
        if (account!=accountGeneration || cancelled) return;
        registryTask.reset();
        downloads=std::move(snapshot.records); savedRecordStamps=std::move(snapshot.stamps);
        filesByBook=std::move(snapshot.index); libraryFiles=std::move(snapshot.files); syncFiles=libraryFiles; knownFiles=libraryFiles;
        canCleanupBooks=snapshot.cleanup; registryLoading=false;
        if (startup) { localView = !downloads.isEmpty(); emit catalogChanged(); }
        invalidateLibrary(); invalidateSync(); detailPending=true;
        notifyOperation(); emit libraryLoadingChanged(); publishDataChanges();
        refreshRecents();
        // Defer maintenance until QML has had a turn to render the loaded state.
        QTimer::singleShot(0,this,[this,account] {
            if (account==accountGeneration && canCleanupBooks) { cleanupBooks(); cleanupCovers(); }
        });
        if (auto ready=std::exchange(recordsReady,{})) ready();
        loadStartupCatalog();
    });
}

void Client::cancelCleanup() {
    ++cleanupGeneration;
    if (cleanupTask) cleanupTask->store(true);
    cleanupTask.reset();
    if (cleanupState==CleanupState::Running) {
        cleanupState=CleanupState::Cancelled;
        logDiagnostic("cleanup.cancelled",{{"task",qint64(cleanupGeneration-1)},{"reason","account or session changed"}});
    }
    cleanupPending=false; cleanupProofs.clear();
}

void Client::cleanupBooks(const QString &verifiedFile) {
    if (!verifiedFile.isEmpty()) cleanupProofs[verifiedFile]={fileStamp(verifiedFile),true};
    if (busy() || cleanupState==CleanupState::Running) { cleanupPending=true; return; }
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
        return readerFileState(path) == ReaderFileState::Closed && ::unlink(QFile::encodeName(path))==0;
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
        const auto recordPath=scopeDir+"/records/"+id+".json",journalPath=transfers+"/"+name;
        const auto selected=recordFile(id,current);
        const bool superseded=!selected.isEmpty() && selected!=newFile && selected!=oldFile;
        if (blockedTransfers.contains(journalPath) || (!previous.isEmpty() && selected.isEmpty())) {
            retained.insert(newFile); retained.insert(oldFile); retained.insert(staging); continue;
        }
        if (!current.isEmpty()) {
            const auto expected=registered ? next : previous;
            if (!superseded && (current["sha256"]!=expected["sha256"] || current["bytes"]!=expected["bytes"])) {
                retained.insert(newFile); retained.insert(oldFile); retained.insert(staging); continue;
            }
            const auto proof=durableRecordStamps.value(recordPath);
            if (!proof.first.valid || proof.first!=fileStamp(recordPath) || proof.second!=fileStamp(selected)) {
                cleanupState=CleanupState::Running;
                const auto task=++cleanupGeneration,account=accountGeneration;
                cleanupTask=files.submit(this,[recordPath,selected,current](const FileCancellation &cancel) {
                    const auto registry=fileStamp(recordPath),file=fileStamp(selected);
                    const bool valid=!fileCancelled(cancel) && !current["invalid"].toBool() &&
                        file.bytes==current["bytes"].toDouble() && readObject(recordPath)==current &&
                        validContent(selected,current["format"].toString("epub"),cancel) &&
                        digestFile(selected,cancel)==current["sha256"].toString().toLatin1() &&
                        confirmFile(selected) && confirmFile(recordPath) && fileStamp(recordPath)==registry && fileStamp(selected)==file;
                    return std::make_pair(valid,std::make_pair(registry,file));
                },[this,recordPath,journalPath,selected,account,task](const auto &result,bool cancelled) {
                    if (task!=cleanupGeneration) return;
                    cleanupTask.reset(); cleanupState=CleanupState::Completed;
                    if (cancelled || account!=accountGeneration) return;
                    if (result.first && fileStamp(recordPath)==result.second.first && fileStamp(selected)==result.second.second)
                        durableRecordStamps[recordPath]=result.second;
                    else blockedTransfers.insert(journalPath);
                    cleanupBooks();
                });
                return;
            }
        }
        const QFileInfo info(newFile);
        const bool candidate=registered && current["sha256"]==next["sha256"] && current["bytes"]==next["bytes"] &&
            !current["invalid"].toBool() && info.isFile() && !info.isSymLink() &&
            info.canonicalPath()==info.absolutePath() && info.size()==next["bytes"].toDouble();
        auto proof=cleanupProofs.constFind(newFile);
        if (candidate && (proof==cleanupProofs.cend() || proof->stamp!=fileStamp(newFile))) {
            cleanupState=CleanupState::Running;
            const auto task=++cleanupGeneration,account=accountGeneration;
            cleanupTask=files.submit(this,[newFile,next](const FileCancellation &cancel) {
                const auto before=fileStamp(newFile);
                const bool valid=validContent(newFile,next["format"].toString(),cancel) &&
                    digestFile(newFile,cancel)==next["sha256"].toString().toLatin1() && fileStamp(newFile)==before;
                return CleanupProof{before,valid};
            },[this,newFile,account,task](const CleanupProof &result,bool cancelled) {
                if (task!=cleanupGeneration) return;
                cleanupTask.reset();
                const bool current=!cancelled && account==accountGeneration;
                cleanupState=current ? CleanupState::Completed : CleanupState::Cancelled;
                logDiagnostic("cleanup.finished",{{"task",qint64(task)},{"result",current ? "completed" : "cancelled"}});
                if (current) cleanupProofs[newFile]=result;
                cleanupBooks();
            });
            return;
        }
        const bool confirmed=candidate && proof!=cleanupProofs.cend() && proof->valid;
        if (!registered || confirmed) {
            const bool newRemoved = registered || removeUnused(newFile);
            const bool oldRemoved = recordFile(id, current) == oldFile || removeUnused(oldFile);
            const bool partRemoved = removeUnused(staging);
            if (newRemoved && oldRemoved && partRemoved) {
                cleanupState=CleanupState::Running;
                const auto task=++cleanupGeneration,account=accountGeneration;
                cleanupTask=files.submit(this,[newFile,oldFile,staging,journalPath,transfers](const FileCancellation &cancel) {
                    QSet<QString> directories;
                    for (const auto &path:{newFile,oldFile,staging}) if (!path.isEmpty()) directories.insert(QFileInfo(path).absolutePath());
                    for (const auto &directory:directories) if (fileCancelled(cancel) || !syncDirectory(directory)) return false;
                    if (fileCancelled(cancel) || ::unlink(QFile::encodeName(journalPath))!=0) return false;
                    return syncDirectory(transfers);
                },[this,journalPath,task,account](bool ok,bool cancelled) {
                    if (task!=cleanupGeneration) return;
                    cleanupTask.reset(); cleanupState=CleanupState::Completed;
                    if (cancelled || account!=accountGeneration) return;
                    if (!ok) blockedTransfers.insert(journalPath);
                    cleanupBooks();
                });
                return;
            }
        }
        retained.insert(newFile); retained.insert(oldFile); retained.insert(staging);
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

void Client::cleanupCovers() {
    QSet<int> downloaded, visible;
    const auto tab = selectedTab();
    auto keepCovers = [&visible](const QJsonArray &page) {
        for (const auto value : page) if (positiveId(value.toObject()["id"])) visible.insert(value.toObject()["id"].toInt());
    };
    keepCovers(items);
    if (tab != CatalogTab) keepCovers(catalogBrowsePage.items);
    if (tab != CollectionsTab) keepCovers(collectionsBrowsePage.items);
    for (auto it=downloads.begin(); it!=downloads.end(); ++it) {
        const auto book = it.value().toObject()["book"].toObject();
        if (positiveId(book["id"]) && book["hasCover"] != false) downloaded.insert(book["id"].toInt());
    }
    covers.cleanup(visible,downloaded);
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
    if (libraryPending) { libraryPending=false; if (modelObserved) ensureLibrary(); emit libraryChanged(); }
    if (syncPending) { syncPending=false; if (syncObserved) syncSummary(); emit syncSummaryChanged(); }
    if (detailPending) { detailPending=false; emit detailChanged(); }
}

void Client::notifyOperation() {
    const QVariantList state{busy(), downloading(), verifyingLibrary(), canRetry(),downloadCancellable()};
    if (state == lastOperation) return;
    lastOperation = state;
    emit operationChanged();
}

void Client::storeRecord(const QString &id, const QJsonObject &record) {
    const auto before=downloads.value(id).toObject();
    if (before == record) return;
    ++recordsRevision;
    savedRecordStamps.remove(id);
    durableRecordStamps.remove(scopeDir+"/records/"+id+".json");
    indexRecord(id,before,record);
    downloads[id] = record;
    for (const auto key:{"filename","directory","bytes","sha256","invalid"}) if (before[key]!=record[key]) {
        libraryFiles.remove(id.toInt()); syncFiles.remove(id.toInt()); knownFiles.remove(id.toInt()); break;
    }
    invalidateBook(libraryBookId(before)); invalidateBook(libraryBookId(record));
    changedSyncGroups.insert(syncGroupKey(id,before)); changedSyncGroups.insert(syncGroupKey(id,record));
    changedUnsupported.insert(before["book"].toObject()["id"].toInt());
    changedUnsupported.insert(record["book"].toObject()["id"].toInt());
    if (detailBook.value("id") == record["bookId"] || detailBook.value("id")==before["bookId"]) detailPending=true;
}

void Client::setSyncResult(const QString &id, const UiMessage &text) {
    if (syncResults.value(id) == text.toJson()) return;
    syncResults[id] = text.toJson();
    invalidateBook(libraryBookId(downloads.value(id).toObject()),false);
    if (detailBook.value("id") == downloads.value(id).toObject()["bookId"] ||
        detailBook.value("selectedFile").toObject()["id"].toInt() == id.toInt()) detailPending=true;
}

void Client::clearSyncResult(const QString &id) {
    if (!syncResults.contains(id)) return;
    syncResults.remove(id);
    invalidateBook(libraryBookId(downloads.value(id).toObject()),false);
    if (detailBook.value("id") == downloads.value(id).toObject()["bookId"]) detailPending=true;
}

bool Client::saveRecord(const QString &id, const QJsonObject &record) {
    if (recordFile(id, record).isEmpty()) return false;
    const auto path=scopeDir+"/records/"+id+".json";
    const auto saved=savedRecordStamps.value(id);
    if (saved.valid && downloads.value(id).toObject()==record && fileStamp(path)==saved) return true;
    savedRecordStamps.remove(id);
    durableRecordStamps.remove(scopeDir+"/records/"+id+".json");
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.recordWrites;
#endif
    const auto bytes=QJsonDocument(record).toJson();
    if (bytes.size()>maxJson) return false;
    const auto result=saveDurable(path,bytes);
    if (result.state==FileCommitState::FailedBeforeReplace) return false;
    // The file was replaced even when its sync is unconfirmed; keep memory equal to it.
    storeRecord(id, record);
    if (result.state!=FileCommitState::Complete) {
        logDiagnostic("record.persistence_failed",{{"state","replaced_unconfirmed"},{"detail",result.detail}});
        return false;
    }
    savedRecordStamps[id]=fileStamp(path);
    return true;
}

QJsonObject Client::progressState(const QString &id,const QString &profile) const {
    const auto state=downloads.value(id).toObject()["progress"].toObject();
    return state.contains("profile") && state["profile"].toString()!=profile ? QJsonObject{} : state;
}

bool Client::persistProgress(const QString &id,const QString &profile,QJsonObject state) {
    auto record=downloads.value(id).toObject(); state["profile"]=profile; record["progress"]=state;
    if (saveRecord(id,record)) return true;
    finish(uiMessage("could_not_save_progress_retry_sync"),false,"progress"); return false;
}

void Client::saveProgressIntent(const QString &id,const QString &profile,QJsonObject state,const std::function<void()> &confirmed) {
    auto record=downloads.value(id).toObject(); state["profile"]=profile; record["progress"]=state;
    const auto path=recordFile(id,record),recordPath=scopeDir+"/records/"+id+".json";
    const auto context=fileContext(id,path); const auto stamp=fileStamp(path);
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.recordWrites;
#endif
    fileTask=files.submit(this,[path,recordPath,record,stamp](const FileCancellation &cancel) {
        const auto bytes=QJsonDocument(record).toJson();
        if (fileCancelled(cancel) || !stamp.valid || fileStamp(path)!=stamp || bytes.size()>maxJson)
            return FileCommitResult{FileCommitState::FailedBeforeReplace,"file changed or invalid record"};
        return saveDurable(recordPath,bytes);
    },[this,id,record,recordPath,context,stamp,confirmed](const FileCommitResult &result,bool cancelled) {
        if (!acceptFileContext(context,stamp) || cancelled) return;
        if (downloads.value(id).toObject()!=context.record) {
            finish(uiMessage("the_file_profile_or_account_changed_retry_the_operation"),false,"progress",{},false); return;
        }
        if (result.state!=FileCommitState::FailedBeforeReplace) storeRecord(id,record);
        if (result.state!=FileCommitState::Complete) {
            logDiagnostic("progress.persistence_failed",{{"state",result.state==FileCommitState::ReplacedUnconfirmed ? "replaced_unconfirmed" : "failed_before_replace"},{"detail",result.detail}});
            finish(uiMessage("could_not_save_progress_retry_sync"),false,"progress",{},false); return;
        }
        savedRecordStamps[id]=fileStamp(recordPath);
        confirmed();
    });
}

void Client::indexRecord(const QString &id,const QJsonObject &before,const QJsonObject &after) {
    auto keys=[](const QJsonObject &record) {
        QSet<int> result;
        if (!record.isEmpty()) {
            result.insert(record["bookId"].toInt());
            result.insert(record["book"].toObject()["id"].toInt());
        }
        return result;
    };
    const auto old=keys(before),next=keys(after);
    for (int key:old-next) {
        filesByBook[key].removeAll(id);
        if (filesByBook[key].isEmpty()) filesByBook.remove(key);
    }
    for (int key:next-old) {
        auto &ids=filesByBook[key];
        ids.insert(std::lower_bound(ids.begin(),ids.end(),id),id);
    }
}

QString Client::recordFile(const QString &id, const QJsonObject &record) const {
    return recordPath(scopeDir,id,record);
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
    if (files && knownFiles.contains(id.toInt())) { files->insert(id.toInt(),knownFiles.value(id.toInt())); return files->value(id.toInt()); }
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.localFileChecks;
#endif
    const auto result=availableRecord(scopeDir,id,downloads.value(id).toObject());
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
        {"syncResult", UiMessage::fromJson(syncResults[id]).text()}, {"pendingProgress", sameProfile && progress["pending"].isObject()},
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
    return {{"text",feedbackHidden ? QString{} : message.text()},{"context",feedbackContext},
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
        finish(uiMessage("could_not_save_the_book_order"),false,"settings"); return;
    }
    if (librarySort!=sort) {
        librarySort=sort; libraryOrderDirty=true; invalidateLibrary(false);
    }
    feedbackResult="success"; emit feedbackChanged(); publishDataChanges();
}

void Client::refreshRecents() {
    const auto profile=readerProfile();
    if (nativeRecents.profile!=profile) {
        nativeRecents={}; nativeRecents.profile=profile;
        invalidateLibrary(); invalidateSync(); detailPending=detailPending || detailVisible(); publishDataChanges();
    }
    recentsFull=true; recentsRequested=true; runRecents();
}
void Client::refreshBookRecents(int bookId) {
    if (nativeRecents.profile!=readerProfile()) { refreshRecents(); return; }
    recentsBooks.insert(bookId); recentsRequested=true; runRecents();
}
void Client::runRecents() {
    if (!recentsRequested || busy() || recentsScheduled) return;
    const bool full=std::exchange(recentsFull,false);
    const auto books=std::exchange(recentsBooks,{});
    recentsScheduled=true; recentsRequested=false;
    QStringList ids;
    if (full) ids=downloads.keys(); else for (int bookId:books) ids.append(filesByBook.value(bookId));
    ids.removeDuplicates();
    const auto generation=++recentsGeneration,account=accountGeneration,revision=recordsRevision;
    const auto profile=readerProfile();
    recentsTask=files.submit(this,[ids,records=downloads,scope=scopeDir](const FileCancellation &cancel) {
        struct Metadata { LocalFiles files; QStringList paths,affectedPaths; QHash<QString,int> owners; } result;
        for (const auto &id:ids) {
            const auto record=records.value(id).toObject(),book=record["book"].toObject();
            if (fileCancelled(cancel)) return result;
            const auto path=availableRecord(scope,id,record);
            result.files[id.toInt()]=path;
            const auto known=recordPath(scope,id,record);
            if (!known.isEmpty()) { result.affectedPaths.append(known); result.owners[known]=libraryBookId(record); }
            if (!path.isEmpty() && readableFormat(fileFormat(book["selectedFile"].toObject()))) result.paths << path;
        }
        result.paths.removeDuplicates(); return result;
    },[this,generation,account,revision,profile,full,books](auto metadata,bool cancelled) {
        if (generation!=recentsGeneration) return;
        if (cancelled || account!=accountGeneration || profile!=readerProfile() || busy() || revision!=recordsRevision) {
            recentsScheduled=false; recentsFull=recentsFull || full; recentsBooks.unite(books); recentsRequested=true; runRecents(); return;
        }
        const auto paths=metadata.paths,affectedPaths=metadata.affectedPaths; const auto owners=metadata.owners;
        recentsTask=files.submit(this,readerRecentsTask(paths,profile),
            [this,generation,account,revision,profile,full,books,affectedPaths,owners,metadata](ReaderRecents result,bool cancelled) {
                if (generation!=recentsGeneration) return;
                recentsScheduled=false;
                if (!cancelled && account==accountGeneration && profile==readerProfile() && !busy() && revision==recordsRevision) {
                    for (auto it=metadata.files.begin();it!=metadata.files.end();++it) {
                        const int id=it.key(); const auto path=it.value();
                        if ((libraryFiles.contains(id) && libraryFiles.value(id)!=path) ||
                            (syncFiles.contains(id) && syncFiles.value(id)!=path)) invalidateBook(libraryBookId(downloads.value(QString::number(id)).toObject()));
                        libraryFiles[id]=path; syncFiles[id]=path; knownFiles[id]=path;
                    }
                    const bool availabilityChanged=nativeRecents.available!=result.available;
                    for (const auto &path:affectedPaths) {
                        const auto a=nativeRecents.files.value(path),b=result.files.value(path);
                        if (a.bookId!=b.bookId || a.openTime!=b.openTime || nativeRecents.files.contains(path)!=result.files.contains(path)) invalidateBook(owners.value(path),false);
                    }
                    if (full || !result.available || availabilityChanged) nativeRecents=std::move(result);
                    else for (const auto &path:affectedPaths) {
                        nativeRecents.files.remove(path);
                        if (result.files.contains(path)) nativeRecents.files[path]=result.files.value(path);
                    }
                    if (availabilityChanged) { invalidateLibrary(); invalidateSync(); }
                    if (detailVisible() && changedBooks.contains(detailBook["id"].toInt())) detailPending=true;
                    publishDataChanges();
                } else {
                    recentsFull=recentsFull || full; recentsBooks.unite(books); recentsRequested=true;
                }
                runRecents();
        });
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
    return libraryModel.snapshot();
}
QAbstractItemModel *Client::bookModel() const { ensureLibrary(); modelObserved=true; return &libraryModel; }

QVariantMap Client::syncFileStatus(const QString &id, const QString &profile,LocalFiles *files) const {
    const auto record=downloads.value(id).toObject(), progress=record["progress"].toObject();
    const auto saved=record["syncStatus"].toObject();
    const bool sameProfile=saved["profile"].toString()==profile;
    const bool progressProfile=progress.contains("profile") && progress["profile"].toString()==profile;
    QString state=sameProfile ? saved["state"].toString("unknown") : "unknown";
    QString reason=sameProfile ? UiMessage::fromJson(saved.contains("message") ? saved["message"] : saved["text"]).text() : QString{};
    const auto book=record["book"].toObject();
    const bool available=!localFile(book,files).isEmpty();
    if (record["remoteFileChanged"].toBool()) {
        state="file"; reason=QCoreApplication::translate("BookOrbit", "The server file differs. Download the new version before syncing positions.");
    } else if (!available) {
        state="file"; reason=QCoreApplication::translate("BookOrbit", "The local book file is missing or damaged. Download it again.");
    } else if (progressProfile && progress["conflictRemote"].isObject()) state="conflict";
    else if (progressProfile && progress["outgoing"].isObject()) state="uncertain";
    else if (progressProfile && progress["pending"].isObject() && (state=="synced" || state=="unknown")) state="pending";
    const QMap<QString,QString> labels{
        {"synced",QCoreApplication::translate("BookOrbit", "Synced")},{"conflict",QCoreApplication::translate("BookOrbit", "Positions differ")},{"reader",QCoreApplication::translate("BookOrbit", "Close the reader")},
        {"reader_unknown",QCoreApplication::translate("BookOrbit", "Could not check the reader")},{"network",QCoreApplication::translate("BookOrbit", "Waiting for network")},
        {"pending",QCoreApplication::translate("BookOrbit", "Position not applied yet")},{"uncertain",QCoreApplication::translate("BookOrbit", "Result needs verification")},
        {"unknown",QCoreApplication::translate("BookOrbit", "Not checked yet")},{"auth",QCoreApplication::translate("BookOrbit", "Sign in to BookOrbit")},{"file",QCoreApplication::translate("BookOrbit", "Check the book file")},
        {"position",QCoreApplication::translate("BookOrbit", "Could not match the position")},{"error",QCoreApplication::translate("BookOrbit", "Could not sync")}};
    if (!labels.contains(state)) state="unknown";
    if (state=="network") reason=QCoreApplication::translate("BookOrbit", "Connect to a network and retry sync");
    if (state=="unknown") reason=QCoreApplication::translate("BookOrbit", "Sync can update the reading position on this PocketBook or in BookOrbit.");
    const auto at=QDateTime::fromString(saved["at"].toString(),Qt::ISODate).toLocalTime();
    return {{"fileId",id.toInt()},{"filename",book["selectedFile"].toObject()["filename"].toString(record["filename"].toString())},
        {"format",record["format"].toString("epub").toUpper()},{"sizeLabel",fileSizeLabel(record["bytes"])},
        {"state",state},{"label",labels[state]},{"reason",reason},{"available",available},
        {"checkedAt",sameProfile && at.isValid() ? at.toString("dd.MM.yyyy HH:mm") : QString{}}};
}

QVariantList Client::syncBooks() const {
    syncSummary();
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.syncSnapshots;
#endif
    SyncProxy all; all.setSourceModel(&syncRows); QVariantList result;
    for (int i=0;i<all.rowCount();++i) result.append(syncRows.get(all.index(i,0).data(SyncModel::Key).toString()));
    return result;
}
QAbstractItemModel *Client::syncModel() const { syncObserved=true; syncSummary(); return &syncView; }
void Client::setSyncFilter(const QString &value) {
    if (value==syncFilter() || !QStringList{"all","waiting","attention","synced"}.contains(value)) return;
    syncView.setFilter(value); emit syncFilterChanged();
}
QVariantMap Client::syncBookDetails(const QString &key) const { syncSummary(); return syncRows.get(key); }

void Client::invalidateSyncBook(int bookId) {
    if (bookId>0) changedSyncGroups.insert("book:"+QString::number(bookId));
    for (const auto &id:filesByBook.value(bookId)) changedSyncGroups.insert(syncGroupKey(id,downloads.value(id).toObject()));
    changedUnsupported.insert(bookId); syncPending=true;
}
QVariantMap Client::syncGroup(const QString &key) const {
    const auto ids=key.startsWith("book:") ? filesByBook.value(key.mid(5).toInt()) : QStringList{key.mid(5)};
    QVariantMap entry; QVariantList variants;
    for (const auto &id:ids) {
        const auto record=downloads.value(id).toObject();
        if (record.isEmpty() || syncGroupKey(id,record)!=key || !syncFormat(record["format"].toString("epub"))) continue;
        const auto book=record["book"].toObject(); const auto file=syncFileStatus(id,readerProfile(),&syncFiles);
        if (entry.isEmpty()) entry={{"key",key},{"title",book["title"].toString(QCoreApplication::translate("BookOrbit", "Untitled"))},
            {"author",authorNames(book["authors"].toArray())},{"state","synced"},{"label",QCoreApplication::translate("BookOrbit", "Synced")}};
        variants.append(file);
        if (syncPriority.indexOf(file["state"].toString())<=syncPriority.indexOf(entry["state"].toString())) {
            entry["state"]=file["state"]; entry["label"]=file["label"];
        }
    }
    if (!entry.isEmpty()) {
        entry["files"]=variants; const auto state=entry["state"].toString();
        entry["group"]=state=="synced" ? "synced" : (state=="unknown" || state=="network") ? "waiting" : "attention";
    }
    return entry;
}
QVariantMap Client::syncSummary() const {
    if (syncDirty) {
#ifdef BOOKORBIT_TEST_HTTP
        ++presentationCounters.syncBuilds; ++presentationCounters.syncSorts;
#endif
        QList<QVariantMap> groups; syncFiles.clear(); QSet<QString> keys;
        for (auto it=downloads.begin();it!=downloads.end();++it) keys.insert(syncGroupKey(it.key(),it.value().toObject()));
        cachedSyncSummary={{"total",0},{"synced",0},{"waiting",0},{"attention",0},{"unsupported",0}};
        for (const auto &key:keys) {
            const auto group=syncGroup(key); if (group.isEmpty()) continue;
            groups.append(group); const auto name=group.value("group").toString();
            cachedSyncSummary[name]=cachedSyncSummary.value(name).toInt()+1;
        }
        syncRows.replace(groups);
        QSet<int> supported; unsupportedBooks.clear();
        for (const auto value:downloads) {
            const auto record=value.toObject(); const int id=record["book"].toObject()["id"].toInt();
            if (syncFormat(record["format"].toString("epub"))) supported.insert(id); else unsupportedBooks.insert(id);
        }
        unsupportedBooks.subtract(supported);
        syncDirty=false; changedSyncGroups.clear(); changedUnsupported.clear();
    } else if (!changedSyncGroups.isEmpty() || !changedUnsupported.isEmpty()) {
        for (const auto &key:std::exchange(changedSyncGroups,{})) {
#ifdef BOOKORBIT_TEST_HTTP
            ++presentationCounters.syncReconciliations;
#endif
            const auto old=syncRows.get(key),next=syncGroup(key);
            if (old==next) continue;
#ifdef BOOKORBIT_TEST_HTTP
            ++presentationCounters.syncUpdates;
#endif
            auto count=[this](const QVariantMap &group,int delta) {
                if (group.isEmpty()) return;
                const auto name=group.value("group").toString(); cachedSyncSummary[name]=cachedSyncSummary.value(name).toInt()+delta;
            };
            count(old,-1); count(next,1); syncRows.put(key,next);
        }
        for (int bookId:std::exchange(changedUnsupported,{})) {
            bool supported=false,unsupported=false;
            for (const auto &id:filesByBook.value(bookId)) {
                const auto record=downloads.value(id).toObject();
                if (record["book"].toObject()["id"].toInt()!=bookId) continue;
                if (syncFormat(record["format"].toString("epub"))) supported=true; else unsupported=true;
            }
            if (unsupported && !supported) unsupportedBooks.insert(bookId); else unsupportedBooks.remove(bookId);
        }
    }
    cachedSyncSummary["total"]=syncRows.rowCount(); cachedSyncSummary["unsupported"]=unsupportedBooks.size();
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
    detailMessage =uiMessage("details_saved_on_this_device");
    emit detailChanged();
}

void Client::connectForSync() {
    if (busy()) return;
    beginFeedback("sync");
    if (ensureNetwork()) { feedbackResult="success"; message =uiMessage("network_connected_retry_sync_now"); emit feedbackChanged(); }
}

void Client::prepareConflict(const QString &id,const QString &local,const QJsonObject &remote,const QString &profile,const QVariantList &prepared,const FileStamp &proof) {
    const bool different=conflictId!=id || conflictLocal!=local || conflictRemote!=remote || conflictProfile!=profile;
    conflictId=id; conflictLocal=local; conflictRemote=remote; conflictProfile=profile;
    conflictMessage=downloads.value(id).toObject()["book"].toObject()["title"].toString();
    const auto record=downloads.value(id).toObject();
    const QString path=localFile(record["book"].toObject());
    if (conflictTask) conflictTask->store(true);
    const auto generation=++conflictGeneration,account=accountGeneration;
    conflictPreparing=true; positionChoices.clear();
    if (different) { ++positionRevision; conflictPreview.clear(); conflictStamp={}; conflictDigest.clear(); }
    notifyOperation(); emit conflictChanged();
    auto done=[this,id,local,remote,profile,record,path,generation,account](const std::pair<QVariantList,FileStamp> &result,bool cancelled) {
        if (generation!=conflictGeneration) return;
        conflictPreparing=false; conflictTask.reset();
        if (!cancelled && account==accountGeneration && profile==readerProfile() && conflictId==id && conflictLocal==local &&
            conflictRemote==remote && downloads.value(id).toObject()["sha256"]==record["sha256"] &&
            downloads.value(id).toObject()["progress"]==record["progress"] && fileStamp(path)==result.second) {
            const bool other=hasPageNumber(remote) || hasOtherCoordinates(remote);
            conflictPreview=result.first; conflictStamp=result.second; conflictDigest=record["sha256"].toString();
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
        if (!busy() && recentsRequested) runRecents();
        if (!busy() && cleanupPending) cleanupBooks();
    };
    if (!prepared.isEmpty()) done({prepared,proof},false);
    else if (!different && !conflictPreview.isEmpty() && fileStamp(path)==conflictStamp && conflictDigest==record["sha256"].toString()) done({conflictPreview,conflictStamp},false);
    else conflictTask=files.submit(this,[path,local,remote,record](const FileCancellation &cancel) {
        QVariantList choices;
        const auto before=fileStamp(path);
        for (bool useLocal:{true,false}) {
            QVariantMap context; double percentage=0;
            const auto cfi=useLocal ? local : remote["cfi"].toString();
            const bool valid=!fileCancelled(cancel) && bookPosition(path,cfi,&percentage,nullptr,&context);
            context["local"]=useLocal; context["valid"]=valid; context["percentageValue"]=percentage; context["empty"]=cfi.isEmpty();
            choices.append(context);
        }
        const bool same=before.valid && cachedDigest(path,cancel)==record["sha256"].toString().toLatin1() && fileStamp(path)==before;
        if (!same) for (auto &choice:choices) {
            auto context=choice.toMap(); context["valid"]=false; context.remove("chapter"); context.remove("excerpt"); choice=context;
        }
        return std::make_pair(choices,before);
    },done);
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
    result["notice"] = detailMessage.text();
    QVariantList files;
    for (const auto value : detailBook["files"].toArray()) {
        auto book = detailBook;
        book["selectedFile"] = value;
        auto file = value.toObject().toVariantMap();
        const auto info = fileSummary(book);
        for (const auto key : {"downloaded", "readable", "supported", "needsRepair", "remoteFileChanged", "syncResult"}) file[key] = info[key];
        file["format"] = fileFormat(value.toObject()).toUpper();
        file["sizeLabel"] = fileSizeLabel(value.toObject()["sizeBytes"]);
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
        fileChoices = choices; invalidateBook(book["id"].toInt(),false);
    }
}

void Client::showDetail(int bookId) {
    if (busy() || bookId <= 0) return;
    for (const auto value : visibleItems(false)) {
        const auto book = value.toObject();
        if (book["id"].toInt() != bookId) continue;
        detailBook = book;
        for (const auto &id:filesByBook.value(bookId)) {
            const auto saved = downloads.value(id).toObject()["book"].toObject();
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
        detailMessage = detailBook["detailed"].toBool() ? uiMessage("details_saved_on_this_device") : UiMessage{};
        ++detailGeneration;
        emit detailChanged();
        if (authenticated() && !localView && !(retryKind==2 && retryBook["id"].toInt()==bookId)) refreshDetail();
        else if (!detailBook["detailed"].toBool()) {
            detailMessage = uiMessage("full_details_not_loaded_yet");
            emit detailChanged();
        }
        return;
    }
}

void Client::refreshDetail() {
    if (busy() || detailBook.isEmpty()) return;
    if (!authenticated()) { detailMessage = uiMessage("sign_in_to_load_full_details"); notifyOperation(); emit feedbackChanged(); emit detailChanged(); return; }
    beginFeedback("book",detailBook["id"].toInt(),detailBook["selectedFile"].toObject()["id"].toInt());
    const int id = detailBook["id"].toInt(), generation = detailGeneration;
    const auto selectedFile = detailBook["selectedFile"];
    retryKind = 3; working = true; message.clear(); detailMessage = uiMessage("loading_details"); notifyOperation(); emit feedbackChanged(); emit detailChanged();
    jsonRequest("/api/v1/books/"+QString::number(id), {}, [this, id, generation, selectedFile](const QJsonObject &response) {
        if (generation != detailGeneration) { finish({}, true, "details"); return; }
        if (response["id"].toInt() != id || !response["files"].isArray() || !response["authors"].isArray()) {
            finish(uiMessage("the_server_returned_invalid_book_details"), false, "details"); return;
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
        const auto ids = filesByBook.value(id);
        for (const auto &key : ids) {
#ifdef BOOKORBIT_TEST_HTTP
            ++presentationCounters.metadataVisits;
#endif
            auto record = downloads.value(key).toObject();
            if (record["bookId"].toInt() != id) continue;
            auto metadata = book;
            metadata["selectedFile"] = record["book"].toObject()["selectedFile"];
            record["book"] = smallBook(metadata);
            if (!saveRecord(key, record)) saved = false;
        }
        detailMessage = saved ? UiMessage{} : uiMessage("details_received_but_not_saved_for_offline_use");
        finish({}, true, "details");
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
    if (!authenticated()) { finish(uiMessage("sign_in_to_download"), false, "download"); return; }
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

Client::BrowseTab Client::selectedTab() const {
    if (localView) return DownloadsTab;
    return collectionList || activeCollection > 0 ? CollectionsTab : CatalogTab;
}

Client::BrowsePage Client::currentBrowsePage() const {
    return {items, currentPage, count, activeCollection, committedQuery, activeCollectionName, pageLoaded};
}

void Client::changeBrowsePage(BrowseTab tab, BrowsePage page, bool restore) {
    emit browseViewAboutToChange();
    const auto previous = selectedTab();
    if (previous == CatalogTab) catalogBrowsePage = currentBrowsePage();
    else if (previous == CollectionsTab) collectionsBrowsePage = currentBrowsePage();
    closeDetail();
    covers.stop();
    localView = tab == DownloadsTab;
    collectionList = tab == CollectionsTab && page.collection == 0;
    items = std::move(page.items);
    currentPage = page.page;
    count = page.total;
    activeCollection = page.collection;
    committedQuery = std::move(page.query);
    activeCollectionName = std::move(page.name);
    pageLoaded = page.loaded;
    retryKind = 0;
    message.clear();
    invalidateLibrary();
    emit catalogChanged(); notifyOperation(); emit feedbackChanged(); publishDataChanges();
    emit browseViewChanged(restore);
}

void Client::clearNavigation() {
    localView = true;
    libraryQuery.clear(); committedQuery.clear();
    items = {}; collectionItems = {}; detailBook = {}; browseHistory.clear();
    catalogBrowsePage = {}; collectionsBrowsePage = {};
    pageLoaded = collectionListLoaded = browseRequested = false;
    retryKind = retryPage = 0; retryQuery.clear(); retryBook = {};
    ++detailGeneration; detailMessage.clear();
    activeCollection = 0; activeCollectionName.clear(); collectionList = false;
    count = currentPage = 0;
    emit browseSessionReset();
}

void Client::selectTab(int tab) {
    if (busy() || tab < DownloadsTab || tab > CollectionsTab) return;
    browseRequested = true;
    const bool same = tab == selectedTab();
    if (!same) {
        changeBrowsePage(BrowseTab(tab), tab == CatalogTab ? catalogBrowsePage : tab == CollectionsTab ? collectionsBrowsePage : BrowsePage{});
    }
    if (tab == DownloadsTab) {
        refreshRecents();
    } else if (collectionList) {
        if (same || !collectionListLoaded) loadCollections();
    } else if (same || !pageLoaded) {
        refresh(currentPage, committedQuery);
    }
}

void Client::showCollections() {
    if (busy()) return;
    if (!collectionList) {
        browseHistory.clear();
        changeBrowsePage(CollectionsTab, {});
    }
    loadCollections();
}

void Client::showCollectionList() {
    if (busy() || collectionList) return;
    browseHistory.clear();
    changeBrowsePage(CollectionsTab, {});
    if (!collectionListLoaded) loadCollections();
}

void Client::loadCollections() {
    beginFeedback("catalog");
    if (!authenticated() && hasSavedSession()) {
        restoreSessionThen([this] { loadCollections(); }, "catalog");
        return;
    }
    retryKind = 4;
    if (!authenticated()) { finish(uiMessage("sign_in_to_load_collections"), false, "collections"); return; }
    working = true; message = uiMessage("loading_collections"); notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/collections", {}, [this](const QJsonObject &response) {
        QJsonArray parsed;
        QSet<int> seen;
        for (const auto value : response["items"].toArray()) {
            const auto collection = value.toObject();
            if (!positiveId(collection["id"]) || !collection["name"].isString() || !collection["bookCount"].isDouble() || collection["bookCount"].toDouble() < 0 || seen.contains(collection["id"].toInt())) {
                finish(uiMessage("the_server_returned_an_invalid_collection_list"), false, "collections"); return;
            }
            seen.insert(collection["id"].toInt());
            // BookOrbit 3.7 removed mediaType along with podcast collections.
            if (!collection.contains("mediaType") || collection["mediaType"] == "books") parsed.append(collection);
        }
        emit browseResultsAboutToChange(true, 0);
        collectionItems = parsed;
        collectionListLoaded = true;
        emit catalogChanged();
        emit browseResultsChanged();
        finish({}, true, "collections");
    }, true, true, true);
}

void Client::openCollection(int id, const QString &name, double catalogOffset, double detailOffset,
                            const QVariantMap &navigation) {
    if (busy() || id <= 0) return;
    if (detailVisible() && id == activeCollection) { closeDetail(); return; }
    if (detailVisible()) {
        if (browseHistory.size() == 16) browseHistory.removeFirst();
        browseHistory.append({selectedTab(), currentBrowsePage(), detailBook, detailMessage, catalogOffset, detailOffset, navigation});
    } else {
        browseHistory.clear();
    }
    BrowsePage page;
    page.collection = id;
    page.name = name;
    changeBrowsePage(CollectionsTab, page, false);
    refresh();
}

QVariantMap Client::backFromCollection() {
    if (busy() || browseHistory.isEmpty()) return {};
    const auto saved = browseHistory.takeLast();
    changeBrowsePage(saved.tab, saved.page);
    detailBook = saved.detail;
    detailMessage = saved.notice;
    emit detailChanged();
    return {{"query", committedQuery}, {"catalogY", saved.catalogY}, {"detailY", saved.detailY}, {"navigation", saved.navigation}};
}

void Client::finish(UiMessage text, bool success, const QString &operation, const QString &syncState,bool persistStatus) {
    const bool downloaded=restoringDownload;
    restoringDownload=false;
    if (downloaded && operation!="download") text=uiMessage("book_downloaded_progress_result").arg(text);
    const int finishedBook=syncingId.isEmpty() ? feedbackBookId : libraryBookId(downloads.value(syncingId).toObject());
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
    fileTask.reset(); fileDownloading=false; committingDownload=false;
    working = syncingAll || checkingLibrary;
    activeDownload.clear();
    if (!success && retryKind == 3 && detailVisible()) { detailMessage = text; detailPending=true; }
    if (success) retryKind = 0;
    const bool hasFile = !syncingId.isEmpty();
    if (hasFile && !checkingLibrary) {
        auto record=downloads.value(syncingId).toObject();
        record["syncStatus"]=QJsonObject{{"state",syncState.isEmpty() ? (success ? "synced" : "error") : syncState},
            {"message",text.toJson()},{"at",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"profile",readerProfile()}};
        if (!persistStatus) storeRecord(syncingId,record);
        else if (!saveRecord(syncingId,record)) {
            success=false;
            text =uiMessage("could_not_save_the_sync_result_resolve_the_storage_error_and_try_again");
            record["syncStatus"]=QJsonObject{{"state","error"},{"message",text.toJson()},{"profile",readerProfile()}};
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
            if (hasFile && !checkingLibrary) refreshBookRecents(finishedBook);
            working=true;
            publish();
            QTimer::singleShot(0,this,[this] {
                working=false;
                if (checkingLibrary && verificationCancelled) {
                    finish(uiMessage("verification_cancelled"),false,"verify"); return;
                }
                if (syncingAll && (syncStopRequested || !authenticated())) { syncQueue.clear(); finish({},false,"sync"); return; }
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
            message =uiMessage("1_files_2_matching_3_different_4_errors_5_unchecked_6_see_book_d_4bf1b3")
                .arg(verificationCancelled ? uiMessage("verification_cancelled") : !authenticated() ? uiMessage("verification_stopped_sign_in_again") : uiMessage("verification_complete"))
                .arg(syncCount).arg(syncSucceeded).arg(verificationDifferent).arg(verificationErrors).arg(unchecked);
            if (recentsRequested) runRecents();
            publish();
            emit completed("verify",!verificationCancelled && syncSucceeded==syncCount);
            return;
        }
        syncingAll=false; emit conflictChanged();
        working=false;
        const auto summary=syncSummary();
        const bool allOk=!syncStopRequested && authenticated() && syncSucceeded==syncCount;
        message =uiMessage("1_files_completed_2_of_3_books_synced_4_of_5_needs_attention_6_waiting_7")
            .arg(!authenticated() ? uiMessage("sync_stopped_sign_in_again") : syncStopRequested ? uiMessage("sync_stopped_by_user") : allOk ? uiMessage("sync_complete") : uiMessage("sync_completed_with_issues"))
            .arg(syncCompleted).arg(syncCount).arg(summary["synced"].toInt()).arg(summary["total"].toInt())
            .arg(summary["attention"].toInt()).arg(summary["waiting"].toInt());
        feedbackResult=allOk ? "success" : "error";
        refreshBookRecents(finishedBook);
        publish();
        emit completed("sync",allOk);
        return;
    }
    if (operation=="progress" || operation=="download" || operation=="open") refreshBookRecents(finishedBook);
    else if (recentsRequested) runRecents();
    publish();
    // A failed restore retains the downloaded file and its separate sync error.
    emit completed(downloaded ? "download" : operation, downloaded || success);
}

QNetworkRequest Client::request(const QString &path) const {
    QNetworkRequest request(QUrl(endpoint.toString()+path));
    if (endpoint.scheme() == "https") {
        if (sslHost != endpoint.host()) {
            ssl = request.sslConfiguration();
            auto certificates = ssl.caCertificates();
            if (endpoint.host() == "books.lan")
                certificates.append(QSslCertificate::fromPath(":/books-lan-ca.pem", QSsl::Pem, QSslCertificate::PatternSyntax::FixedString));
            // Public LAN CA, scoped to this host and this client's data directory.
            certificates.append(QSslCertificate::fromPath(rootDir+"/certificates/"+endpoint.host()+".pem", QSsl::Pem, QSslCertificate::PatternSyntax::FixedString));
            ssl.setCaCertificates(certificates);
            sslHost = endpoint.host();
        }
        request.setSslConfiguration(ssl);
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer "+token);
    return request;
}

bool Client::acceptCredentials(const QJsonObject &response) {
    const QString access = response["accessToken"].toString();
    const QString refresh = response["refreshToken"].toString();
    static const QRegularExpression nativeRefresh("^[a-f0-9]{64}$");
    if (access.isEmpty() || access.size() > 8192 || access.contains('\r') || access.contains('\n') ||
        !nativeRefresh.match(refresh).hasMatch()) return false;
    token = access.toUtf8(); refreshToken = refresh;
    saveSession(); // A storage failure leaves this login usable until the application closes.
    return true;
}

bool Client::setCredentials(const QJsonObject &response) {
    if (acceptCredentials(response)) return true;
    invalidateSession();
    finish(uiMessage("the_server_response_contains_no_valid_session"), false, "login");
    return false;
}

bool Client::saveSession() {
    const QString path = scopeDir+"/session.json";
    QSaveFile file(path);
    const auto bytes = QJsonDocument(QJsonObject{{"refreshToken", refreshToken}}).toJson();
    const QFileInfo info(path);
    const bool permissionless = permissionlessStorage(scopeDir);
    sessionStored = !info.isSymLink() && QFileInfo(scopeDir).canonicalFilePath() == scopeDir &&
        file.open(QIODevice::WriteOnly) &&
        (permissionless || (file.setPermissions(sessionPermissions) && privateSessionPermissions(file.permissions()))) &&
        file.write(bytes) == bytes.size() && file.commit() &&
        (permissionless || privateSessionPermissions(QFileInfo(path).permissions()));
    if (!sessionStored) {
        file.cancelWriting();
        const bool removed = (!QFile::exists(path) && !QFileInfo(path).isSymLink()) || QFile::remove(path);
        sessionNotice = removed ? uiMessage("could_not_protect_the_saved_session_you_will_need_to_sign_in_aga_920b4e")
                                : uiMessage("could_not_protect_or_remove_sign_in_data_check_device_storage");
    } else sessionNotice.clear();
    logEvent(sessionStored ? "session saved" : "session save failed");
    emit connectionChanged(); notifyOperation();
    return sessionStored;
}

void Client::invalidateSession() {
    ++accountGeneration;
    resetSessionRestore();
    cancelCleanup();
    covers.stop();
    clearCredentials();
}

void Client::clearCredentials() {
    token.clear(); refreshToken.clear(); sessionStored = false; sessionNotice.clear();
    const QString path = scopeDir+"/session.json";
    if ((QFile::exists(path) || QFileInfo(path).isSymLink()) && !QFile::remove(path))
        sessionNotice = uiMessage("could_not_remove_the_invalid_session_from_this_device");
    emit connectionChanged(); notifyOperation();
}

void Client::startupRestoreSession() {
    if (accountGeneration != startupAccountGeneration || authenticated() || refreshToken.isEmpty() ||
        sessionRestoreReply || !networkConnected()) return;
    startSessionRestore();
}

void Client::restoreSession() {
    if (authenticated()) return;
    restoreSessionThen([this] { refresh(); });
}

void Client::restoreSessionThen(const std::function<void()> &resume, const QString &context) {
    if (registryLoading) { recordsReady=[this,resume,context] { restoreSessionThen(resume,context); }; return; }
    if (busy()) return;
    if (authenticated()) { resume(); return; }
    if (refreshToken.isEmpty()) return;
    beginFeedback(context);
    working = true;
    message = uiMessage("restoring_session");
    notifyOperation(); emit feedbackChanged();
    if (sessionRestoreReply) {
        sessionRestoreResume = resume;
    } else if (ensureNetwork()) {
        sessionRestoreResume = resume;
        startSessionRestore();
    }
}

void Client::resetSessionRestore() {
    sessionRestoreReply.clear();
    sessionRestoreResume = {};
    startupCatalogPending = false;
}

void Client::startSessionRestore() {
    const auto account = accountGeneration;
    const auto credential = refreshToken;
    sessionRestoreReply = sendJsonRequest("/api/v1/auth/refresh", {{"refreshToken", credential}}, false,
        [this,account,credential](QNetworkReply *reply, const QByteArray &bytes) {
            if (sessionRestoreReply != reply || account != accountGeneration || credential != refreshToken) return;
            sessionRestoreReply.clear();
            auto resume = std::exchange(sessionRestoreResume, {});
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            UiMessage failure;
            if (failedJsonReply(*reply, code, bytes.size())) {
                if (code == 401) clearCredentials();
                failure = requestError(*reply, code);
            } else {
                const auto response = QJsonDocument::fromJson(bytes);
                if (!response.isObject()) {
                    failure = uiMessage("the_server_returned_invalid_data");
                } else if (!acceptCredentials(response.object())) {
                    clearCredentials();
                    failure = uiMessage("the_server_response_contains_no_valid_session");
                }
            }
            if (account != accountGeneration) return;
            if (!resume) {
                if (failure.code.isEmpty()) {
                    startupCatalogPending = true;
                    loadStartupCatalog();
                }
            } else if (!failure.code.isEmpty()) {
                finish(failure, false, "/api/v1/auth/refresh", code == 401 ? "auth" : "error");
            } else {
                working = false;
                resume();
                notifyOperation();
            }
        });
}

void Client::renewSession(const std::function<void()> &resume) {
    covers.stop();
    message = uiMessage("refreshing_session");
    notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/auth/refresh", {{"refreshToken", refreshToken}}, [this, resume](const QJsonObject &response) {
        if (setCredentials(response)) { covers.fetch(items); resume(); }
    }, false);
}

bool Client::ensureNetwork() {
    if (connectNetwork()) return true;
    finish(uiMessage("wi_fi_is_disconnected_connect_to_a_network_and_try_again"), false, "network", "network");
    return false;
}

QNetworkReply *Client::sendJsonRequest(const QString &path, const QJsonObject &payload, bool get, const ReplyCallback &callback) {
    auto request = this->request(path);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto reply = get ? network.get(request) : network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > maxJson) reply->abort();
    });
    // Bound the whole operation as well as inactivity between packets.
    QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, callback, path] {
        reply->deleteLater();
        bytes->append(reply->readAll());
        logEvent(path.section('?', 0, 0), int(reply->error()), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        callback(reply, *bytes);
    });
    return reply;
}

UiMessage Client::requestError(const QNetworkReply &reply, int code) const {
    switch (reply.error()) {
    case QNetworkReply::SslHandshakeFailedError: return uiMessage("could_not_verify_the_server_tls_certificate_check_the_trusted_ca_3413a3");
    case QNetworkReply::HostNotFoundError: return uiMessage("server_not_found_1_check_wi_fi_and_dns").arg(endpoint.host());
    case QNetworkReply::ConnectionRefusedError: return uiMessage("the_server_refused_the_connection_check_the_address_and_network");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError: return uiMessage("connection_timed_out_check_the_network_and_try_again");
    default: break;
    }
    if (code == 401) return uiMessage("not_signed_in_or_session_expired_sign_in_again");
    if (code >= 400) return uiMessage("server_http_error").arg(code);
    return uiMessage("the_server_is_unavailable_downloaded_books_can_be_read_offline");
}

void Client::jsonRequest(const QString &path, const QJsonObject &payload, const Callback &callback, bool renew, bool get, bool arrayResponse) {
    if (checkingLibrary && verificationCancelled) { finish(uiMessage("verification_cancelled"),false,"verify"); return; }
    if (!ensureNetwork()) return;
    const auto generation=fileGeneration,account=accountGeneration;
    auto *reply = sendJsonRequest(path, payload, get,
        [this, callback, path, payload, renew, get, arrayResponse,generation,account](QNetworkReply *reply, const QByteArray &bytes) {
        if (generation!=fileGeneration) return;
        if (account!=accountGeneration) { finish(uiMessage("the_file_profile_or_account_changed_retry_the_operation"),false,"file"); return; }
        if (activeVerification==reply) activeVerification.clear();
        if (checkingLibrary && verificationCancelled) { finish(uiMessage("verification_cancelled"),false,"verify"); return; }
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 401 && renew && (path.startsWith("/api/v1/books/") || path.startsWith("/api/v1/collections")) && !refreshToken.isEmpty()) {
            if (!get && path.endsWith("/progress")) {
                // A refreshed token is not permission to replay an old sync
                // decision. Keep the outgoing journal; the next sync starts with
                // fresh reader/profile/server snapshots, including explicit choices.
                renewSession([this] {
                    finish(uiMessage("session_restored_retry_sync_the_previous_position_was_not_sent_again"),
                           false,"progress","uncertain");
                });
            } else {
                renewSession([this, path, payload, callback, get, arrayResponse] { jsonRequest(path, payload, callback, false, get, arrayResponse); });
            }
            return;
        }
        if (failedJsonReply(*reply, code, bytes.size())) {
            if (code == 401) invalidateSession();
            finish(path == "/api/v1/auth/logout" ? uiMessage("signed_out_on_this_device_server_session_revocation_was_not_confirmed") :
                   requestError(*reply, code), false, path, code==401 ? "auth" : "error");
            return;
        }
        if (!get && path.endsWith("/progress") && bytes.trimmed().isEmpty()) { callback({}); return; }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || (arrayResponse ? !doc.isArray() : !doc.isObject())) {
            finish(uiMessage("the_server_returned_invalid_data"), false, path);
            return;
        }
        callback(arrayResponse ? QJsonObject{{"items", doc.array()}} : doc.object());
    });
    if (checkingLibrary) activeVerification=reply;
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
        finish(uiMessage("enter_an_https_address_username_and_password"), false, "login");
        return;
    }
    if (!configure(normalized, username)) return;
    recordsReady=[this,username,secret] {
        retryKind = 0;
        feedbackResult="running";
        working = true;
        message = uiMessage("connecting");
        notifyOperation(); emit feedbackChanged();
        jsonRequest("/api/v1/auth/login", {{"username", username}, {"password", secret},
                                        {"clientKind", "native"}, {"deviceLabel", "PocketBook prototype"}},
            [this](const QJsonObject &response) {
                if (!setCredentials(response)) return;
                working = false;
                refresh();
            });
    };
}

void Client::refresh(int targetPage, const QString &query) {
    if (busy()) return;
    if (targetPage < 0 || query.size() > 500) return;
    if (localView || collectionList) changeBrowsePage(CatalogTab, catalogBrowsePage);
    browseRequested = true;
    if (!authenticated() && hasSavedSession()) {
        restoreSessionThen([this, targetPage, query] { refresh(targetPage, query); }, "catalog");
        return;
    }
    covers.stop();
    beginFeedback("catalog");
    if (token.isEmpty()) { finish(uiMessage("sign_in_to_the_server_first"), false, "catalog"); return; }
    retryKind = 1; retryPage = targetPage; retryQuery = query;
    working = true;
    message = uiMessage("loading_catalog");
    notifyOperation(); emit feedbackChanged(); publishDataChanges();
    QUrlQuery params;
    params.addQueryItem("page", QString::number(targetPage));
    params.addQueryItem("size", QString::number(pageSize));
    params.addQueryItem("collapseSeries", "false");
    params.addQueryItem("q", QString::fromLatin1(QUrl::toPercentEncoding(query)));
    const QString route = activeCollection > 0 ? "/api/v1/collections/"+QString::number(activeCollection)+"/books?"+params.toString(QUrl::FullyEncoded) : "/api/v1/books/query";
    jsonRequest(route, catalogPayload(targetPage, query),
        [this, targetPage, query](const QJsonObject &response) {
            const auto [message, saved] = applyCatalogResponse(response, targetPage, query, false);
            finish(message, saved, "catalog");
        }, true, activeCollection > 0);
}

void Client::loadStartupCatalog() {
    if (!startupCatalogPending || registryLoading) return;
    startupCatalogPending = false;
    if (busy() || browseRequested || selectedTab() != CatalogTab || pageLoaded || !downloads.isEmpty() ||
        !authenticated()) return;

    const auto account = accountGeneration;
    const auto fileOperation = fileGeneration;
    const auto access = token;
    const auto reportFailure = [this,account,fileOperation](const UiMessage &error) {
        if (account != accountGeneration || fileOperation != fileGeneration || busy() ||
            (!uiContext.isEmpty() && uiContext != "catalog")) return;
        retryKind = authenticated() ? 1 : 0;
        retryPage = 0;
        retryQuery.clear();
        feedbackContext = "catalog";
        feedbackBookId = feedbackFileId = 0;
        feedbackResult = "error";
        feedbackHidden = false;
        message = error;
        notifyOperation();
        emit feedbackChanged();
    };
    if (!networkConnected()) {
        reportFailure(uiMessage("wi_fi_is_disconnected_connect_to_a_network_and_try_again"));
        return;
    }
    sendJsonRequest("/api/v1/books/query", catalogPayload(0, {}), false,
        [this,account,access,reportFailure](QNetworkReply *reply, const QByteArray &bytes) {
            if (account != accountGeneration || token != access || browseRequested || selectedTab() != CatalogTab || pageLoaded) return;
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (failedJsonReply(*reply, code, bytes.size())) {
                if (code == 401) clearCredentials();
                reportFailure(requestError(*reply, code));
                return;
            }
            const auto response = QJsonDocument::fromJson(bytes);
            if (!response.isObject()) {
                reportFailure(uiMessage("the_server_returned_invalid_data"));
                return;
            }
            const auto [error, saved] = applyCatalogResponse(response.object(), 0, {}, true);
            if (!saved) reportFailure(error);
        });
}

std::pair<UiMessage,bool> Client::applyCatalogResponse(const QJsonObject &response, int targetPage, const QString &query, bool background) {
    const auto array = response["items"].toArray();
    if (!response["items"].isArray() || !response["total"].isDouble() || response["total"].toDouble() < 0 ||
        response["page"].toInt(-1) != targetPage || array.size() > pageSize) {
        return {uiMessage("the_server_returned_an_invalid_catalog"), false};
    }
    QJsonArray parsed;
    for (auto value : array) {
        auto book = value.toObject();
        if (!positiveId(book["id"]) || !book["files"].isArray()) {
            return {uiMessage("invalid_book_record"), false};
        }
        book["files"] = bookFiles(book);
        parsed.append(selectBookFile(book));
    }
    covers.stop();
    bool saved=true;
    for (auto value : parsed) {
        const auto book = value.toObject();
        const int bookId = book["id"].toInt();
        for (const auto &fileId : filesByBook.value(bookId)) {
#ifdef BOOKORBIT_TEST_HTTP
            ++presentationCounters.metadataVisits;
#endif
            auto record = downloads.value(fileId).toObject();
            if (record["bookId"].toInt() != bookId) continue;
            auto stored = record["book"].toObject();
            stored["hasCover"] = book["hasCover"];
            stored["coverVersion"] = book["coverVersion"];
            stored["readStatus"] = book["readStatus"];
            stored["readingProgress"] = book["readingProgress"];
            record["book"] = stored;
            if (!saveRecord(fileId, record)) saved=false;
        }
        if (book["hasCover"] == false) covers.remove(bookId);
    }
    const bool sameQuery = query == committedQuery;
    emit browseResultsAboutToChange(sameQuery && targetPage == currentPage, sameQuery && targetPage < currentPage ? -1 : 0);
    pageLoaded = true;
    items = parsed; currentPage = targetPage; count = response["total"].toInt(); committedQuery=query;
    invalidateLibrary(); emit catalogChanged(); publishDataChanges();
    emit browseResultsChanged();
    cleanupCovers();
    covers.fetch(items, !background);
    return {!saved ? uiMessage("details_received_but_not_saved_for_offline_use") : items.isEmpty() ? uiMessage("no_books_found") : UiMessage{}, saved};
}

void Client::download(int index) {
    if (busy()) return;
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return;
    if (token.isEmpty()) { finish(uiMessage("sign_in_to_download"), false, "download"); return; }
    downloadBook(visible[index].toObject());
}

void Client::downloadBook(const QJsonObject &book, bool renew, bool refetch) {
    const int bookId=book["id"].toInt(), fileId=book["selectedFile"].toObject()["id"].toInt();
    const auto previous=downloads.value(QString::number(fileId)).toObject();
    if (previous.isEmpty() && !refetch) { transferBook(book,renew); return; }
    beginFeedback("book",bookId,fileId);
    retryKind=2; retryBook=book;
    if (bookId<=0 || fileId<=0 || (!previous.isEmpty() && previous["bookId"].toInt()!=bookId)) {
        finish(uiMessage("this_file_is_already_linked_to_another_book_refresh_the_details"),false,"download"); return;
    }
    // Repairs and retries resolve file metadata again. A known replacement may
    // have a different byte count under the same fileId; never use cached size.
    working=true; message =uiMessage("checking_the_current_file_version"); notifyOperation(); emit feedbackChanged();
    jsonRequest("/api/v1/books/"+QString::number(bookId),{},
        [this,book,bookId,fileId,renew](const QJsonObject &response) {
            if (response["id"].toInt()!=bookId || !response["files"].isArray()) {
                finish(uiMessage("could_not_get_current_file_details"),false,"download"); return;
            }
            QJsonObject selected;
            for (const auto value:bookFiles(response))
                if (value.toObject()["id"].toInt()==fileId) selected=value.toObject();
            if (selected.isEmpty()) {
                finish(uiMessage("the_selected_file_is_no_longer_on_the_server_refresh_the_book_details"),false,"download"); return;
            }
            if (fileFormat(selected)!=fileFormat(book["selectedFile"].toObject())) {
                finish(uiMessage("the_server_file_format_has_changed_refresh_the_book_details"),false,"download"); return;
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
        (!file["sizeBytes"].isDouble() || reportedSize < 0 || reportedSize > maxBook || reportedSize != qint64(reportedSize)))) { finish(uiMessage("the_file_is_unsupported_or_exceeds_100_mib"), false, "download"); return; }
    const auto previous = downloads.value(id).toObject();
    if (!previous.isEmpty() && previous["bookId"] != book["id"]) {
        finish(uiMessage("this_file_is_already_linked_to_another_book_refresh_the_details"), false, "download"); return;
    }
    const QString oldFile = previous.isEmpty() ? path : recordFile(id, previous);
    if (oldFile.isEmpty()) { finish(uiMessage("invalid_downloaded_book_record"), false, "download"); return; }
    if ((QFile::exists(oldFile) || !previous.isEmpty()) && readerFileState(oldFile) != ReaderFileState::Closed) {
        finish(uiMessage("close_the_book_in_the_built_in_reader_and_retry_the_download"), false, "download"); return;
    }
    const qint64 expected = reportedSize < 0 ? 0 : static_cast<qint64>(reportedSize);
    const QString directory = downloadDirectory();
    if (!accessibleDirectory(directory) || !sameStorage(scopeDir,directory)) {
        finish(uiMessage("the_download_folder_is_unavailable_choose_another_folder_in_settings"), false, "download"); return;
    }
    const QString stem = id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString staging = scopeDir+"/"+stem+".part";
    const QString finalPath = directory+"/"+stem+"."+format;
    auto output = std::make_shared<QSaveFile>(staging);
    if (!output->open(QIODevice::WriteOnly)) { finish(uiMessage("could_not_create_the_book_file"), false, "download"); return; }
    startFileOperation(); fileDownloading=true; cancelled=false; message =uiMessage("downloading_book");
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, output, hash, size, expected, file, drain, book, id, oldFile, previous, staging, finalPath, stem, directory, renew, format,generation,transferContext] {
        if (generation!=fileGeneration) { output->cancelWriting(); reply->deleteLater(); return; }
        if (reply->bytesAvailable()) drain();
        reply->deleteLater();
        if (!acceptFileContext(transferContext)) { output->cancelWriting(); return; }
        logEvent("download", int(reply->error()), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        if (cancelled) {
            output->cancelWriting(); retryKind = 0;
            finish(uiMessage("download_cancelled_the_previous_file_was_kept"), false, "download"); return;
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401) {
            output->cancelWriting(); activeDownload.clear();
            if (renew && !refreshToken.isEmpty()) {
                renewSession([this, book] { downloadBook(book, false); });
                return;
            }
            invalidateSession();
            finish(uiMessage("session_expired_sign_in_again_to_download"), false, "download"); return;
        }
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
            reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("application/json") ||
            (reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/html") && format != "html" && format != "htm") ||
            *size == 0 || *size > maxBook || (expected > 0 && *size != expected)) {
            output->cancelWriting();
            finish(uiMessage("download_incomplete_try_again_the_previous_file_was_kept"), false, "download"); return;
        }
        if ((QFile::exists(oldFile) || downloads.contains(id)) && readerFileState(oldFile) != ReaderFileState::Closed) {
            output->cancelWriting();
            finish(uiMessage("the_book_is_open_in_the_built_in_reader_close_it_and_retry_the_download"), false, "download"); return;
        }
        const QString newHash = QString::fromLatin1(hash->result().toHex());
        const auto context=transferContext;
        message =uiMessage("checking_the_book_file"); emit feedbackChanged();
        fileTask=files.submit(this,[oldFile](const FileCancellation &cancel) {
            const auto before=fileStamp(oldFile); return std::make_pair(cachedDigest(oldFile,cancel),before);
        },[this,context,output,book,id,oldFile,previous,staging,finalPath,stem,directory,format,newHash,size,file](const auto &result,bool wasCancelled) {
            if (!acceptFileContext(context,result.second) || wasCancelled) { output->cancelWriting(); return; }
            const auto oldStamp=result.second;
            if ((QFile::exists(oldFile) || !previous.isEmpty()) && readerFileState(oldFile)!=ReaderFileState::Closed) {
                output->cancelWriting(); finish(uiMessage("close_the_book_in_the_built_in_reader_and_retry_the_download"),false,"download"); return;
            }
            if (downloads.value(id).toObject()["sha256"] == newHash &&
                !localFile(book).isEmpty() && result.first==newHash.toLatin1()) {
                output->cancelWriting();
                auto record=downloads.value(id).toObject();
                if (record.contains("remoteFileChanged")) {
                    record.remove("remoteFileChanged");
                    record.remove("syncStatus");
                    if (!saveRecord(id,record)) {
                        finish(uiMessage("could_not_save_the_download_result_the_previous_book_is_still_available"),false,"download"); return;
                    }
                }
                clearSyncResult(id);
                rememberFile(book);
                finish(uiMessage("book_already_downloaded"), true, "download"); return;
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
            const QString transfers=scopeDir+"/transfers",journal=transfers+"/"+stem+".json",recordPath=scopeDir+"/records/"+id+".json";
            const auto journalBytes=QJsonDocument(QJsonObject{{"fileId",file["id"]},{"previous",identity(previous)},{"next",identity(record)}}).toJson();
            if (!accessibleDirectory(directory) || !sameStorage(scopeDir,directory)) {
                output->cancelWriting(); finish(uiMessage("could_not_save_the_book"),false,"download"); return;
            }
            fileTask=files.submit(this,[output,journal,journalBytes,recordPath,staging,format](const FileCancellation &cancel) {
                TransferStage result;
                if (fileCancelled(cancel) || !makeDurableDirectory(QFileInfo(journal).absolutePath(),&result.error) ||
                    !makeDurableDirectory(QFileInfo(recordPath).absolutePath(),&result.error)) return result;
                const auto saved=saveDurable(journal,journalBytes); result.error=saved.detail;
                result.journal=saved.state==FileCommitState::Complete;
                if (!result.journal || fileCancelled(cancel)) return result;
                const auto staged=commitFile(*output); result.error=staged.detail;
                if (staged.state!=FileCommitState::Complete || fileCancelled(cancel)) return result;
                result.stamp=fileStamp(staging);
                result.valid=validContent(staging,format,cancel) && result.stamp.valid && fileStamp(staging)==result.stamp;
                return result;
            },[this,context,output,oldStamp,oldFile,staging,finalPath,directory,id,record,recordPath,journal,book,newHash,format](const TransferStage &result,bool wasCancelled) {
                if (!acceptFileContext(context,oldStamp) || wasCancelled || !result.valid) {
                    if (output->isOpen()) output->cancelWriting();
                    blockedTransfers.insert(journal);
                    logDiagnostic("transfer.persistence_failed",{{"stage","journal_or_staging"},{"detail",result.error}});
                    if (!wasCancelled && context.generation==fileGeneration)
                        finish(uiMessage(result.journal ? "could_not_save_the_book" : "could_not_save_the_download_journal_the_previous_book_is_still_available"),false,"download");
                    return;
                }
                if (fileStamp(staging)!=result.stamp || !accessibleDirectory(directory) || !sameStorage(scopeDir,directory) ||
                    ((QFile::exists(oldFile) || !context.record.isEmpty()) && readerFileState(oldFile)!=ReaderFileState::Closed)) {
                    blockedTransfers.insert(journal); finish(uiMessage("could_not_save_the_book"),false,"download"); return;
                }
                fileTask=files.submit(this,[staging,finalPath,newHash](const FileCancellation &cancel) {
                    TransferStage result; result.journal=true;
                    if (fileCancelled(cancel)) return result;
                    const auto moved=moveDurable(staging,finalPath); result.error=moved.detail;
                    if (moved.state!=FileCommitState::Complete) return result;
                    result.stamp=fileStamp(finalPath);
                    result.valid=result.stamp.valid && cachedDigest(finalPath,cancel)==newHash.toLatin1() && fileStamp(finalPath)==result.stamp;
                    return result;
                },[this,context,oldStamp,oldFile,finalPath,id,record,recordPath,journal,book,format](const TransferStage &result,bool wasCancelled) {
                    if (!acceptFileContext(context,oldStamp) || wasCancelled || !result.valid || fileStamp(finalPath)!=result.stamp ||
                        ((QFile::exists(oldFile) || !context.record.isEmpty()) && readerFileState(oldFile)!=ReaderFileState::Closed)) {
                        blockedTransfers.insert(journal);
                        if (!wasCancelled && context.generation==fileGeneration) finish(uiMessage("the_saved_book_is_damaged_the_previous_book_is_still_available"),false,"download");
                        return;
                    }
                    const auto stamp=result.stamp;
#ifdef BOOKORBIT_TEST_HTTP
                    ++presentationCounters.recordWrites;
#endif
                    committingDownload=true; notifyOperation();
                    fileTask=files.submit(this,[recordPath,record,finalPath,stamp](const FileCancellation &cancel) {
                        if (fileCancelled(cancel) || fileStamp(finalPath)!=stamp)
                            return FileCommitResult{FileCommitState::FailedBeforeReplace,"file changed"};
                        const auto bytes=QJsonDocument(record).toJson();
                        if (bytes.size()>maxJson) return FileCommitResult{FileCommitState::FailedBeforeReplace,"record too large"};
                        return saveDurable(recordPath,bytes);
                    },[this,context,oldStamp,finalPath,id,record,recordPath,journal,book,format,stamp](const FileCommitResult &result,bool wasCancelled) {
                        if (!acceptFileContext(context,oldStamp) || wasCancelled || result.state!=FileCommitState::Complete || fileStamp(finalPath)!=stamp) {
                            blockedTransfers.insert(journal);
                            logDiagnostic("transfer.persistence_failed",{{"stage","registry"},{"detail",result.detail}});
                            if (!wasCancelled && context.generation==fileGeneration) finish(uiMessage("download_record_not_saved_the_previous_book_is_still_available"),false,"download");
                            return;
                        }
                        storeRecord(id,record); savedRecordStamps[id]=fileStamp(recordPath);
                        durableRecordStamps[recordPath]={fileStamp(recordPath),stamp};
                        // A durable replacement makes earlier failed attempts safe to recheck.
                        const auto prefix=scopeDir+"/transfers/"+id+"-";
                        blockedTransfers.removeIf([&](const QString &path) { return path.startsWith(prefix); });
                        clearSyncResult(id); rememberFile(book); cleanupBooks(finalPath);
                        if (readableFormat(format)) scanBook(finalPath);
                        if (context.record.isEmpty() && syncFormat(format)) {
                            restoringDownload=true; fileDownloading=false; retryKind=0;
                            restoreDownloadedProgress(id); return;
                        }
                        finish(uiMessage("book_downloaded_and_available_offline"),true,"download");
                    });
                });
            });
        });
    });
}

void Client::restoreDownloadedProgress(const QString &id, int attempts) {
    const auto record=downloads.value(id).toObject();
    const auto path=recordFile(id,record);
    if (readerBookIndexed(path)) {
        working=false;
        syncBook(id);
        return;
    }
    if (!attempts) {
        syncingId=id;
        finish(uiMessage("book_registration_pending_sync_later"),false,"progress","reader_unknown");
        return;
    }
    message=uiMessage("waiting_for_book_registration"); notifyOperation(); emit feedbackChanged();
    const auto context=fileContext(id,path);
    QTimer::singleShot(250,this,[this,id,attempts,context] {
        if (acceptFileContext(context)) restoreDownloadedProgress(id,attempts-1);
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
    if (path.isEmpty()) {
        const int id=book["selectedFile"].toObject()["id"].toInt();
        libraryFiles.remove(id); syncFiles.remove(id); invalidateBook(book["id"].toInt());
        detailPending=detailPending || detailVisible(); finish(uiMessage("download_the_book_first"), false, "open"); return;
    }
    if (!readableFormat(fileFormat(book["selectedFile"].toObject()))) {
        finish(uiMessage("file_downloaded_opening_this_format_from_the_app_is_not_supported_yet"), false, "open"); return;
    }
    const QString id = QString::number(book["selectedFile"].toObject()["id"].toInt());
    startFileOperation();
    message =uiMessage("checking_the_book_file"); notifyOperation(); emit feedbackChanged();
    const auto context=fileContext(id,path);
    fileTask=files.submit(this,[path](const FileCancellation &cancel) {
        const auto stamp=fileStamp(path); const auto digest=cachedDigest(path,cancel);
        return std::make_pair(digest,stamp);
    },[this,book,applyIncoming,path,id,context](const auto &result,bool cancelled) {
        if (!acceptFileContext(context,result.second) || cancelled) return;
        if (result.first != context.record["sha256"].toString().toLatin1()) {
            auto record = downloads.value(id).toObject();
            record["invalid"] = true;
            if (!saveRecord(id, record)) storeRecord(id, record);
            finish(uiMessage("the_book_file_has_changed_download_it_again"), false, "open"); return;
        }
        if (!readerBookIndexed(path)) {
            scanBook(path);
            finish(uiMessage("the_book_is_not_registered_in_the_library_yet_wait_and_tap_read_again"), false, "open");
            return;
        }
        auto record = downloads.value(id).toObject();
        auto sync = record["progress"].toObject();
        const auto pending = sync["pending"].toObject();
        if (applyIncoming && !pending.isEmpty()) {
            finish(uiMessage("the_incoming_position_has_not_been_applied_yet_sync_the_position_8547d4"),false,"open");
            return;
        }

        const bool ok = openReader(path);
        if (ok) rememberFile(book);
        finish(ok ? uiMessage("opening_in_the_built_in_reader") : uiMessage("could_not_start_the_reader"), ok, "open");
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

int Client::catalogPageSize() const { return pageSize; }

bool Client::validDownloadDirectory(const QString &path) const {
    static const QRegularExpression accountFolder("^[0-9a-f]{24}(?:/|$)");
    return storagePath(path) && accessibleDirectory(path) && sameStorage(scopeDir,path) &&
           !accountFolder.match(QDir(rootDir).relativeFilePath(path)).hasMatch();
}

bool Client::setDownloadDirectory(const QString &path) {
    if (busy()) return false;
    beginFeedback("settings");
    if (!validDownloadDirectory(path)) {
        finish(uiMessage("choose_an_accessible_folder_in_internal_storage_outside_system_folders"), false, "settings"); return false;
    }
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["downloadDirectory"] = path;
    UiMessage error;
    if (!writeObject(rootDir+"/preferences.json", preferences, &error)) {
        logDiagnostic("folder save failed",{{"reason",error.code}});
        finish(uiMessage("could_not_save_the_folder_1").arg(error), false, "settings"); return false;
    }
    bookDirectory = path; emit settingsChanged();
    logEvent("folder saved");
    finish(uiMessage("folder_saved_previously_downloaded_books_have_stayed_where_they_were"), true, "settings");
    return true;
}

bool Client::setDiagnosticLogging(bool enabled) {
    beginFeedback("settings");
    if(!enabled) logEvent("diagnostics disabled");
    diagnostics = enabled; emit settingsChanged();
    if(enabled) { logEvent("diagnostics enabled"); emit diagnosticsEnabled(); }
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["diagnosticLogging"] = enabled;
    UiMessage error;
    if (!writeObject(rootDir+"/preferences.json", preferences, &error)) {
        finish((enabled ? uiMessage("logging_is_enabled_until_the_app_closes_could_not_save_the_setting_1") : uiMessage("logging_is_disabled_could_not_save_the_setting_1")).arg(error),
               false, "settings"); return false;
    }
    finish(enabled ? uiMessage("diagnostic_logging_enabled") : uiMessage("diagnostic_logging_disabled"), true, "settings");
    return true;
}

void Client::logEvent(const QString &event, int code, int http) {
    logDiagnostic(event,{{"error",code},{"http",http}});
}
void Client::logDiagnostic(const QString &event, QJsonObject fields) {
    if(!diagnostics) return;
    const QString path=diagnosticLogPath();
    auto failed=[&](const QString &reason) {
        const auto text =uiMessage("diagnostic_log_failed");
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
        finish(uiMessage("enter_an_https_address_and_username"), false, "settings");
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
    UiMessage error;
    if (!writeObject(rootDir+"/accounts.json", {{"accounts", next}, {"selected", selected}}, &error)) {
        logDiagnostic("account save failed",{{"reason",error.code}});
        finish(uiMessage("could_not_save_the_connection_1").arg(error), false, "settings"); return false;
    }
    dismissConflict();
    covers.stop();
    token.clear(); refreshToken.clear(); retryKind = 0; savedAccounts = next;
    endpoint = url; user = name; sslHost.clear();
    localView = true; loadRecords();
    emit connectionChanged(); emit catalogChanged(); emit settingsChanged();
    finish(uiMessage("connection_saved_downloaded_books_are_available_without_signing_in"), true, "settings");
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
    resetSessionRestore();
    cancelCleanup();
    beginFeedback("connection");
    covers.stop();
    const QString credential = refreshToken;
    const QString sessionFile = scopeDir+"/session.json";
    if (QFile::exists(sessionFile) && !QFile::remove(sessionFile)) {
        sessionNotice =uiMessage("could_not_remove_the_saved_session_check_device_storage");
        finish(sessionNotice, false, "logout"); return;
    }
    sessionStored = false; sessionNotice.clear();
    token.clear(); refreshToken.clear(); retryKind = 0;
    clearNavigation();
    invalidateLibrary(); invalidateSync(); detailPending=true;
    emit connectionChanged(); emit catalogChanged(); notifyOperation(); publishDataChanges();
    cleanupCovers();
    logEvent("local logout");
    if (!credential.isEmpty()) {
        working = true; message = uiMessage("signing_out");
        notifyOperation(); emit feedbackChanged();
        jsonRequest("/api/v1/auth/logout", {{"refreshToken", credential}}, [this](const QJsonObject &) {
            finish(uiMessage("session_revoked_downloaded_books_were_kept"), true, "logout");
        }, false);
        return;
    }
    finish(uiMessage("signed_out_on_this_device_downloaded_books_were_kept"), true, "logout");
}

void Client::showDownloaded(bool value) {
    if (busy()) return;
    if (value) {
        if (localView && detailVisible()) closeDetail();
        selectTab(DownloadsTab);
    } else {
        if (selectedTab() != CatalogTab) changeBrowsePage(CatalogTab, catalogBrowsePage);
        refresh(currentPage, committedQuery);
    }
}

void Client::invalidateBook(int bookId,bool sync) {
    changedBooks.insert(bookId); libraryPending=true;
    if (sync) invalidateSyncBook(bookId);
}

QJsonObject Client::localBook(int bookId) const {
    QJsonObject merged; QJsonArray variants;
    for (const auto &id:filesByBook.value(bookId)) {
        const auto record=downloads.value(id).toObject();
        if (libraryBookId(record)!=bookId) continue;
        auto book=record["book"].toObject();
        if (book.isEmpty()) book={{"id",record["bookId"]},{"title",QCoreApplication::translate("BookOrbit", "Book · file ")+id}};
        auto file=book["selectedFile"].toObject();
        file["id"]=record["fileId"]; file["format"]=record["format"].toString("epub"); file["role"]="content";
        if (!file.contains("sizeBytes")) file["sizeBytes"]=record["bytes"];
        if (merged.isEmpty() || book["detailed"].toBool()) merged=book;
        variants.append(file);
    }
    if (!merged.isEmpty()) merged["files"]=variants;
    return merged;
}

Client::LibraryRow Client::libraryRow(QJsonObject book) const {
    book=selectBookFile(book,&libraryFiles);
    const auto recent=recentFile(book,&libraryFiles);
    book["nativeOpenTime"]=recent["nativeOpenTime"];
    const auto search=(book["title"].toString()+"\n"+authorNames(book["authors"].toArray())+"\n"+book["seriesName"].toString())
        .normalized(QString::NormalizationForm_KC).toCaseFolded();
    return {book,bookSummary(book,&libraryFiles),nativeRecents.available && recent["nativeOpenTime"].toDouble()>0 ? bookSummary(recent,&libraryFiles) : QVariantMap{},search};
}

bool Client::libraryLess(const LibraryRow &left,const LibraryRow &right) const {
    const auto &a=left.book,&b=right.book;
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
}
bool Client::libraryMatches(const LibraryRow &row) const {
    return !localView || libraryQuery.isEmpty() || row.search.contains(libraryQuery.normalized(QString::NormalizationForm_KC).toCaseFolded());
}
void Client::indexLibrary() const {
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.indexBuilds; presentationCounters.indexVisits+=libraryRows.size();
#endif
    libraryIndex.clear();
    for (int i=0;i<libraryRows.size();++i) libraryIndex[libraryRows[i].book["id"].toInt()]=i;
}
void Client::updateRecent() const {
#ifdef BOOKORBIT_TEST_HTTP
    ++presentationCounters.recentPasses; presentationCounters.recentVisits+=libraryRows.size();
#endif
    int latest=-1;
    for (int i=0;i<libraryRows.size();++i) {
        if (libraryRows[i].recent.isEmpty()) continue;
        const auto &a=libraryRows[i].book;
        if (latest<0) { latest=i; continue; }
        const auto &b=libraryRows[latest].book;
        const int title=QString::localeAwareCompare(a["title"].toString(),b["title"].toString());
        if (a["nativeOpenTime"].toDouble()>b["nativeOpenTime"].toDouble() ||
            (a["nativeOpenTime"]==b["nativeOpenTime"] && (title<0 || (a["title"]==b["title"] && a["id"].toInt()<b["id"].toInt())))) latest=i;
    }
    libraryRecent=latest<0 ? QVariantMap{} : libraryRows[latest].recent;
}

void Client::ensureLibrary() const {
    const bool reset=libraryDirty;
    if (libraryDirty) {
#ifdef BOOKORBIT_TEST_HTTP
        ++presentationCounters.libraryBuilds;
#endif
        libraryFiles.clear(); libraryRows.clear();
        if (localView) {
            for (auto it=filesByBook.begin();it!=filesByBook.end();++it) {
                const auto book=localBook(it.key());
                if (!book.isEmpty()) libraryRows.append(libraryRow(book));
            }
        } else for (const auto &book:items) libraryRows.append(libraryRow(book.toObject()));
        changedBooks.clear(); libraryDirty=false; libraryOrderDirty=true;
    }
    if (libraryOrderDirty) {
        if (localView) {
#ifdef BOOKORBIT_TEST_HTTP
            ++presentationCounters.librarySorts;
#endif
            std::sort(libraryRows.begin(),libraryRows.end(),[this](const LibraryRow &a,const LibraryRow &b) { return libraryLess(a,b); });
        }
        indexLibrary(); libraryOrderDirty=false; libraryFilterDirty=true;
    }
    const auto changed=std::exchange(changedBooks,{});
    QHash<int,LibraryRow> replacements;
    QSet<int> structural,removed;
    bool recentDirty=reset;
    for (int id:changed) {
        QJsonObject book;
        if (localView) book=localBook(id);
        else for (const auto &value:items) if (value.toObject()["id"].toInt()==id) { book=value.toObject(); break; }
        const int old=libraryIndex.value(id,-1);
        if (book.isEmpty()) {
            if (old>=0) { structural.insert(id); removed.insert(id); recentDirty=recentDirty || !libraryRows[old].recent.isEmpty(); }
            continue;
        }
        auto row=libraryRow(book);
#ifdef BOOKORBIT_TEST_HTTP
        ++presentationCounters.bookUpdates;
#endif
        const bool moved=old<0 || (localView && (libraryLess(row,libraryRows[old]) || libraryLess(libraryRows[old],row)));
        const bool history=old<0 || row.book["nativeOpenTime"]!=libraryRows[old].book["nativeOpenTime"] ||
            row.book["title"]!=libraryRows[old].book["title"] || row.recent.isEmpty()!=libraryRows[old].recent.isEmpty();
        recentDirty=recentDirty || history;
        if (!history && libraryRecent.value("bookId").toInt()==id) libraryRecent=row.recent;
        if (moved || (old>=0 && libraryMatches(row)!=libraryMatches(libraryRows[old]))) structural.insert(id);
        if (moved) replacements.insert(id,std::move(row));
        else libraryRows[old]=std::move(row);
    }
    if (!replacements.isEmpty() || !removed.isEmpty()) {
        QList<LibraryRow> kept,added;
        kept.reserve(libraryRows.size());
        for (const auto &row:libraryRows) {
            const int id=row.book["id"].toInt();
            if (!replacements.contains(id) && !removed.contains(id)) kept.append(row);
        }
        for (auto it=replacements.cbegin();it!=replacements.cend();++it) added.append(it.value());
        if (localView) {
            std::sort(added.begin(),added.end(),[this](const LibraryRow &a,const LibraryRow &b) { return libraryLess(a,b); });
            libraryRows.clear(); libraryRows.reserve(kept.size()+added.size());
            std::merge(kept.begin(),kept.end(),added.begin(),added.end(),std::back_inserter(libraryRows),
                [this](const LibraryRow &a,const LibraryRow &b) { return libraryLess(a,b); });
        } else {
            libraryRows=std::move(kept); libraryRows.append(added);
        }
        indexLibrary();
    }
    if (!libraryFilterDirty && structural.isEmpty()) {
        for (int id:changed) {
            const int at=libraryIndex.value(id,-1),visible=libraryModel.rowFor(id);
            if (at>=0 && visible>=0) libraryModel.update(visible,libraryRows[at].summary);
        }
    } else if (!libraryFilterDirty && structural.size()==1) {
        const int id=*structural.begin(),at=libraryIndex.value(id,-1),visible=libraryModel.rowFor(id);
        if (at<0 || !libraryMatches(libraryRows[at])) { if (visible>=0) libraryModel.remove(visible); }
        else {
            int target=0;
            for (int i=0;i<at;++i) if (libraryMatches(libraryRows[i])) ++target;
            if (visible<0) libraryModel.insert(target,libraryRows[at].summary);
            else { libraryModel.move(visible,target); libraryModel.update(target,libraryRows[at].summary); }
        }
        for (int other:changed-structural) {
            const int row=libraryModel.rowFor(other),source=libraryIndex.value(other,-1);
            if (row>=0 && source>=0) libraryModel.update(row,libraryRows[source].summary);
        }
    } else {
        QVariantList visible;
        for (const auto &row:libraryRows) if (libraryMatches(row)) visible.append(row.summary);
        libraryFilterDirty=false; preservingLibraryAnchor=localView && !reset;
        libraryModel.replace(visible,reset); preservingLibraryAnchor=false;
    }
    if (recentDirty) updateRecent();
}

QJsonArray Client::visibleItems(bool applyQuery) const {
    ensureLibrary(); QJsonArray result;
    if (!applyQuery) for (const auto &row:libraryRows) result.append(row.book);
    else for (const auto &value:libraryModel.snapshot()) {
        const int row=libraryIndex.value(value.toMap().value("bookId").toInt(),-1);
        if (row>=0) result.append(libraryRows[row].book);
    }
    return result;
}

void Client::cancelDownload() {
    if (!downloading() || committingDownload) return;
    cancelled=true;
    if (fileTask) {
        fileTask->store(true); cleanupBooks(); retryKind=0;
        finish(uiMessage("download_cancelled_the_previous_file_was_kept"),false,"download");
    } else if (activeDownload) activeDownload->abort();
    else { retryKind=0; finish(uiMessage("download_cancelled_the_previous_file_was_kept"),false,"download"); }
}

void Client::retry() {
    if (!canRetry()) return;
    if (retryKind == 1) refresh(retryPage, retryQuery);
    else if (retryKind == 2) downloadBook(retryBook, true, true);
    else if (retryKind == 3) refreshDetail();
    else if (retryKind == 4) showCollections();
}

void Client::dismissConflict() {
    ++conflictGeneration;
    if (conflictTask) conflictTask->store(true);
    conflictTask.reset(); conflictPreparing=false; notifyOperation();
    if (conflictId.isEmpty() && conflictMessage.isEmpty() && positionChoices.isEmpty()) return;
    conflictId.clear(); conflictProfile.clear(); conflictLocal.clear(); conflictRemote={}; conflictMessage.clear();
    positionChoices.clear();
    conflictPreview.clear(); conflictStamp={}; conflictDigest.clear();
    emit conflictChanged();
}

void Client::syncAll() {
    if (busy()) return;
    beginFeedback("sync");
    if (!authenticated()) { finish(uiMessage("sign_in_to_sync"),false,"sync"); return; }
    syncQueue.clear();
    for (auto it=downloads.begin(); it!=downloads.end(); ++it)
        if (syncFormat(it.value().toObject()["format"].toString("epub"))) syncQueue.append(it.key());
    if (syncQueue.isEmpty()) { finish(uiMessage("no_downloaded_books_to_sync"),true,"sync"); return; }
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
    if (!authenticated()) { finish(uiMessage("sign_in_to_verify_library_files"),false,"verify"); return; }
    syncQueue=downloads.keys();
    if (syncQueue.isEmpty()) { finish(uiMessage("no_downloaded_files_to_verify"),true,"verify"); return; }
    dismissConflict(); covers.stop(); retryKind=0;
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
        finish(uiMessage("verification_cancelled"),false,"verify");
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
    message =uiMessage("checking_file_1_of_2_3_4_fileid_5")
        .arg(syncCount-syncQueue.size()).arg(syncCount)
        .arg(record["book"].toObject()["title"].toString())
        .arg(record["format"].toString("epub").toUpper()).arg(id);
    notifyOperation(); emit feedbackChanged();
    if (verificationCancelled) { finish(uiMessage("verification_cancelled"),false,"verify"); return; }
    startFileOperation();
    const auto context=fileContext(id,path);
    fileTask=files.submit(this,[path](const FileCancellation &cancel) {
        const auto stamp=fileStamp(path); return std::make_pair(digestFile(path,cancel),stamp);
    },[this,id,path,info,record,context](const auto &result,bool cancelled) {
        if (!acceptFileContext(context,result.second) || cancelled) return;
        if (path.isEmpty() || !info.isFile() || info.isSymLink() || info.canonicalPath()!=info.absolutePath() ||
            info.size()!=record["bytes"].toDouble() || result.first!=record["sha256"].toString().toLatin1()) {
            finish(uiMessage("could_not_verify_the_local_file_is_missing_or_damaged_download_it_again"),false,"verify"); return;
        }
        fileTask.reset();
        verifyRemoteFile(id);
    });
}

void Client::verifyRemoteFile(const QString &id, bool renew) {
    if (verificationCancelled) { finish(uiMessage("verification_cancelled"),false,"verify"); return; }
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
    connect(reply,&QNetworkReply::finished,this,[this,reply,id,renew,drain,hash,size,generation,context] {
        drain(); reply->deleteLater();
        if (generation!=fileGeneration) return;
        if (!acceptFileContext(context)) return;
        activeVerification.clear();
        if (verificationCancelled) { finish(uiMessage("verification_cancelled"),false,"verify"); return; }
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
            finish((code>=400 ? uiMessage("verify_server_http_error").arg(code) : uiMessage("could_not_verify_the_server_file")),false,"verify"); return;
        }
        auto record=downloads.value(id).toObject();
        const auto path=recordFile(id,record),remoteDigest=QString::fromLatin1(hash->result().toHex());
        fileTask=files.submit(this,[path](const FileCancellation &cancel) {
            const auto stamp=fileStamp(path); return std::make_pair(digestFile(path,cancel),stamp);
        },[this,id,record,remoteDigest,context](const auto &result,bool cancelled) mutable {
            if (!acceptFileContext(context,result.second) || cancelled) return;
            if (result.first!=record["sha256"].toString().toLatin1()) {
                finish(uiMessage("could_not_verify_the_local_file_changed_during_verification"),false,"verify"); return;
            }
            const bool different=remoteDigest.toLatin1()!=record["sha256"].toString().toLatin1();
            if (different) record["remoteFileChanged"]=true;
            else record.remove("remoteFileChanged");
            if (!saveRecord(id,record)) {
                if (different) storeRecord(id,record); // Keep the known mismatch blocked in memory even when storage fails.
                finish(uiMessage("could_not_save_the_verification_result_resolve_the_storage_error_bf59aa"),false,"verify"); return;
            }
            finish(different ? uiMessage("the_server_file_differs_progress_sync_is_paused") : uiMessage("the_local_file_matches_bookorbit"),
                   !different,different ? "verify-different" : "verify");
        });
    });
}

void Client::fetchProgressBook(const FileContext &context,const std::function<void(const QJsonObject &,const QJsonArray &)> &callback) {
    const int bookId=libraryBookId(context.record);
    jsonRequest("/api/v1/books/"+QString::number(bookId),{},[this,bookId,context,callback](const QJsonObject &book) {
        if (!acceptFileContext(context)) return;
        const auto variants=bookFiles(book);
        if (book["id"].toInt()!=bookId || variants.isEmpty() ||
            (!book["readStatus"].isNull() && !book["readStatus"].isUndefined() && !book["readStatus"].isObject())) {
            finish(uiMessage("the_server_returned_invalid_book_details"),false,"progress"); return;
        }
        callback(book,variants);
    },true,true);
}

void Client::finishProgress(const QString &id, const QJsonObject &remote, const UiMessage &text) {
    const auto context=fileContext(id,recordFile(id,downloads.value(id).toObject()));
    fetchProgressBook(context,[this,id,remote,text,context](const QJsonObject &book,const QJsonArray &variants) {
        auto primary=variants.first().toObject();
        for (const auto &value:variants) if (value.toObject()["role"]=="primary") { primary=value.toObject(); break; }
        auto publish=[this,bookId=book["id"].toInt(),readStatus=book["readStatus"],text,context](const QJsonObject &progress) {
            if (!acceptFileContext(context)) return;
            const auto percentage=progress["percentage"];
            if (!percentage.isDouble() || percentage.toDouble()<0 || percentage.toDouble()>100) {
                finish(uiMessage("the_server_returned_invalid_data"),false,"progress"); return;
            }
            finishBookProgress(bookId,readStatus,percentage,text);
        };
        // The library percentage belongs to the primary file, even when another format was synced.
        if (primary["id"].toInt()==id.toInt()) publish(remote);
        else jsonRequest("/api/v1/books/files/"+QString::number(primary["id"].toInt())+"/progress",{},publish,true,true);
    });
}

void Client::finishBookProgress(int bookId,const QJsonValue &readStatus,const QJsonValue &percentage,const UiMessage &text) {
    auto update=[&](QJsonObject metadata) {
        if (!percentage.isUndefined()) metadata["readingProgress"]=percentage;
        metadata["readStatus"]=readStatus;
        return metadata;
    };
    bool saved=true;
    for (const auto &fileId:filesByBook.value(bookId)) {
        auto record=downloads.value(fileId).toObject();
        record["book"]=update(record["book"].toObject());
        if (!saveRecord(fileId,record)) saved=false;
    }
    const auto updatePage = [bookId, &update](QJsonArray &books) {
        for (int i=0;i<books.size();++i) if (books[i].toObject()["id"].toInt()==bookId) books[i]=update(books[i].toObject());
    };
    updatePage(items);
    updatePage(catalogBrowsePage.items);
    updatePage(collectionsBrowsePage.items);
    for (auto &saved : browseHistory) {
        updatePage(saved.page.items);
        if (saved.detail.value("id").toInt() == bookId) saved.detail = update(saved.detail);
    }
    if (detailBook.value("id").toInt()==bookId) { detailBook=update(detailBook); detailPending=true; }
    invalidateBook(bookId,false);
    dismissConflict();
    finish(saved ? text : uiMessage("details_received_but_not_saved_for_offline_use"),saved,"progress");
}

// Values passed between sync stages; each field is a snapshot taken at the stage noted.
struct Client::ProgressSync {
    QString id,path,position,profile; int choice=0; bool completed=false; FileContext context;  // local stage start
    QString local; PositionInfo localInfo; Fb2Snapshot fb;                // local check
    QJsonObject remote,progress;                                          // server progress
    QString cfi; int action=0; bool changedChoice=false;                  // decision
};
struct Client::PreparedSync {
    FileStamp stamp; PreparedReaderPosition reader; QVariantList choices; double percentage=0; bool estimate=false;
};

bool Client::readerChanged(const ProgressSync &sync,bool changed) {
    QString current; bool completed=false;
    if (!changed && readerProfile()==sync.profile && readerFileState(sync.path)==ReaderFileState::Closed &&
        readerPosition(sync.path,&current,&completed) && current==sync.position && completed==sync.completed) return false;
    finish(uiMessage("the_reader_state_has_changed_retry_sync"),false,"progress"); return true;
}

void Client::saveConflict(const ProgressSync &sync,QJsonObject state,const QVariantList &choices,const FileStamp &stamp) {
    // Persist both versions; after restart the next GET rechecks the conflict.
    state["conflictLocal"]=sync.local; state["conflictRemote"]=sync.remote;
    if (!persistProgress(sync.id,sync.profile,state)) return;
    if (!syncingAll) prepareConflict(sync.id,sync.local,sync.remote,sync.profile,choices,stamp);
    finish(sync.changedChoice ? uiMessage("the_position_changed_while_you_were_choosing_review_the_choices_again") : uiMessage("choose_a_reading_position"),false,"progress","conflict");
}

void Client::syncBook(const QString &id, int choice) {
    if (busy()) return;
    if (!syncingAll) beginFeedback(detailVisible() ? "book" : "sync", downloads.value(id).toObject()["bookId"].toInt(),id.toInt());
    syncingId=id;
    if (!authenticated()) { finish(uiMessage("sign_in_to_sync"),false,"progress","auth"); return; }
    auto record=downloads.value(id).toObject();
    if (record["remoteFileChanged"].toBool()) {
        finish(uiMessage("the_server_file_differs_progress_sync_is_paused"),false,"progress","file"); return;
    }
    if (!syncFormat(record["format"].toString("epub"))) {
        finish(uiMessage("position_sync_is_available_for_epub_and_supported_fb2_books_you_9f7a76"),false,"progress"); return;
    }
    ProgressSync sync; sync.id=id; sync.choice=choice;
    sync.path=localFile(record["book"].toObject());
    sync.profile=readerProfile();
    startFileOperation(); retryKind=0; covers.stop();
    message =syncingAll ? uiMessage("checking_file_1_of_2_3").arg(syncCount-syncQueue.size()).arg(syncCount)
        .arg(record["book"].toObject()["title"].toString()) : uiMessage("syncing_progress");
    notifyOperation(); emit feedbackChanged();
    if (sync.path.isEmpty()) {
        finish(uiMessage("download_a_valid_book_file_before_syncing"),false,"progress","file"); return;
    }
    const auto reader=readerFileState(sync.path);
    if (reader==ReaderFileState::Open) {
        finish(uiMessage("close_the_book_in_the_built_in_reader_and_retry_sync_the_home_bu_e4edc4"),false,"progress","reader"); return;
    }
    if (reader==ReaderFileState::Unknown || !readerPosition(sync.path,&sync.position,&sync.completed)) {
        finish(uiMessage("could_not_check_the_reader_state_positions_were_kept_unchanged_try_again"),false,"progress","reader_unknown"); return;
    }
    struct LocalCheck { QByteArray digest; FileStamp stamp; QString local; PositionInfo info; Fb2Snapshot fb; bool supported=true,valid=false; };
    sync.context=fileContext(id,sync.path);
    // The scanner seeds a first download with this placeholder before any reader session.
    const bool initialPosition=restoringDownload && sync.position=="pbr:/page?page=0";
    fileTask=files.submit(this,[path=sync.path,position=sync.position,initialPosition,format=record["format"].toString()](const FileCancellation &cancel) {
        LocalCheck result; result.stamp=fileStamp(path);
        if (format=="fb2") { result.fb=fb2Index(path); result.supported=fb2Calibration(cancel).valid && !bookCfi(path,"pbr:/word?page=0",result.fb).isEmpty(); }
        if (result.supported && !fileCancelled(cancel)) {
            result.local=bookCfi(path,position,result.fb);
            result.valid=(position.isEmpty() || initialPosition || !result.local.isEmpty()) &&
                (result.local.isEmpty() || bookPosition(path,result.local,nullptr,nullptr,nullptr,nullptr,&result.info,result.fb));
        }
        result.digest=cachedDigest(path,cancel);
        return result;
    },[this,sync](const LocalCheck &result,bool cancelled) {
        if (!acceptFileContext(sync.context,result.stamp) || cancelled) return;
        if (result.digest!=sync.context.record["sha256"].toString().toLatin1()) {
            libraryFiles.remove(sync.id.toInt()); syncFiles.remove(sync.id.toInt()); invalidateBook(libraryBookId(sync.context.record)); detailPending=detailPending || detailVisible();
            finish(uiMessage("download_a_valid_book_file_before_syncing"),false,"progress","file"); return;
        }
        if (readerChanged(sync)) return;
        const auto comparePositions=[this,sync,result] {
            if (!result.supported) {
                finish(uiMessage("this_fb2_structure_is_not_supported_for_sync_yet_images_tables_p_e1f2b9"),false,"progress","position"); return;
            }
            if (!result.valid) {
                finish(uiMessage("the_local_position_format_is_unsupported_progress_was_kept_unchanged"),false,"progress","position"); return;
            }
            auto next=sync; next.local=result.local; next.localInfo=result.info; next.fb=result.fb;
            compareRemoteProgress(next);
        };
        if (!sync.completed) { comparePositions(); return; }
        fetchProgressBook(sync.context,[this,sync,stamp=result.stamp,comparePositions](const QJsonObject &book,const QJsonArray &variants) {
            if (!acceptFileContext(sync.context,stamp) || readerChanged(sync)) return;
            const auto readStatus=book["readStatus"];
            const bool linked=std::any_of(variants.begin(),variants.end(),[&sync](const auto &file) {
                return file.toObject()["id"].toInt()==sync.id.toInt();
            });
            if (readStatus.toObject()["status"]!="read" || !linked) { comparePositions(); return; }

            // Retire unfinished position transfers for this reading without moving the last confirmed baseline.
            const auto previous=progressState(sync.id,sync.profile);
            if (previous["profile"].isString()) {
                auto state=previous;
                for (const auto *key:{"pending","pendingLocal","outgoing","conflictLocal","conflictRemote"}) state.remove(key);
                if (state!=previous && !persistProgress(sync.id,sync.profile,state)) return;
            }
            finishBookProgress(book["id"].toInt(),readStatus,QJsonValue::Undefined,uiMessage("book_is_marked_as_read_on_both"));
        });
    });
}

void Client::compareRemoteProgress(const ProgressSync &sync) {
    // shortcut: trust the downloaded file identity; explicit library verification detects server replacements.
    jsonRequest("/api/v1/books/files/"+sync.id+"/progress",{},[this,sync](const QJsonObject &remote) {
        if (!acceptFileContext(sync.context) || readerChanged(sync)) return;
        const QString remoteCfi=remote["cfi"].toString();
        const auto percent=remote["percentage"];
        if (!remote.contains("cfi") || (!remote["cfi"].isNull() && !remote["cfi"].isString()) ||
            !percent.isDouble() || percent.toDouble()<0 || percent.toDouble()>100) {
            finish(uiMessage("invalid_progress_response_expected_a_cfi_and_a_percentage_from_0_2d14d6"),false,"progress"); return;
        }
        struct RemoteCheck {
            QByteArray digest; FileStamp stamp; QString point; PositionInfo info; bool valid=true,equal=false,localBaseEqual=false,remoteBaseEqual=false;
        };
        auto next=sync; next.remote=remote; next.progress=downloads.value(sync.id).toObject()["progress"].toObject();
        fileTask=files.submit(this,[path=sync.path,local=sync.local,remoteCfi,progress=next.progress,fb=sync.fb](const FileCancellation &cancel) {
            RemoteCheck result; result.stamp=fileStamp(path); result.point=remoteCfi;
            if (!remoteCfi.isEmpty()) result.valid=bookPosition(path,remoteCfi,nullptr,&result.point,nullptr,nullptr,&result.info,fb);
            if (result.valid && !fileCancelled(cancel)) {
                result.equal=local==result.point || sameBookPosition(path,local,result.point,false,fb);
                if (!result.equal) {
                    const auto localBase=progress["localBase"].toString();
                    const auto remoteBase=progress["remoteBase"].toObject()["cfi"].toString();
                    result.localBaseEqual=local==localBase || sameBookPosition(path,local,localBase,false,fb);
                    result.remoteBaseEqual=remoteCfi==remoteBase || sameBookPosition(path,remoteCfi,remoteBase,true,fb);
                }
            }
            result.digest=cachedDigest(path,cancel);
            return result;
        },[this,sync=next](const RemoteCheck &analysis,bool cancelled) {
            if (!acceptFileContext(sync.context,analysis.stamp) || cancelled ||
                readerChanged(sync,downloads.value(sync.id).toObject()["progress"].toObject()!=sync.progress)) return;
            if (analysis.digest!=sync.context.record["sha256"].toString().toLatin1()) {
                finish(uiMessage("the_local_book_file_changed_during_sync_progress_was_kept_unchanged"),false,"progress","file"); return;
            }
            if (!analysis.valid) {
                finish(uiMessage("could_not_match_the_server_cfi_to_this_book_progress_was_kept_unchanged"),false,"progress","position"); return;
            }
            const auto &id=sync.id,&local=sync.local,&profile=sync.profile; const auto &remote=sync.remote;
            const QString cfi=analysis.point;
            const bool other=hasOtherCoordinates(remote),hasPage=hasPageNumber(remote);
            const bool remoteEmpty=cfi.isEmpty() && remote["percentage"].toDouble()==0 && !hasPage && !other;
            // A first download only restores incoming progress; it must never upload local state.
            if (restoringDownload && remoteEmpty) {
                syncingId.clear();
                finish(uiMessage("book_downloaded_and_available_offline"),true,"download"); return;
            }
            if (cfi.isEmpty() && !remoteEmpty) {
                finish(uiMessage("the_server_has_progress_without_an_exact_cfi_automatic_sync_is_n_df1162"),false,"progress","position"); return;
            }
            const auto state=progressState(id,profile);
            const auto pending=state["pending"].toObject();
            if (analysis.equal) {
                if (persistProgress(id,profile,{{"localBase",local},{"remoteBase",remote}})) finishProgress(id,remote,uiMessage("progress_synced"));
                return;
            }
            const bool known=state.contains("profile") && state["profile"].toString()==profile &&
                state.contains("localBase") && state["remoteBase"].isObject();
            const bool localChanged=known ? !analysis.localBaseEqual : !local.isEmpty();
            const bool remoteChanged=known ? !analysis.remoteBaseEqual : !remoteEmpty;
            auto next=sync; next.cfi=cfi;
            next.changedChoice=sync.choice && (conflictId!=id || conflictProfile!=profile || conflictLocal!=local || conflictRemote!=remote);
            int &action=next.action;
            if (next.changedChoice) action=0; // A stale explicit choice must never become an automatic write.
            else if (sync.choice) action=sync.choice;
            else if (state.contains("profile") && !pending.isEmpty() && pending==remote && state["pendingLocal"].toString()==local && state["profile"].toString()==profile) action=2;
            else {
                switch (decideSync(known,localChanged,remoteChanged)) {
                case SyncDecision::Upload: action=1; break;
                case SyncDecision::ApplyRemote: action=2; break;
                case SyncDecision::Unchanged:
                    if (persistProgress(id,profile,state)) finishProgress(id,remote,uiMessage("progress_unchanged"));
                    return;
                case SyncDecision::Conflict: break;
                }
            }
            if (!action && syncingAll) { saveConflict(next,state); return; }
            if (action==2 && cfi.isEmpty()) { finish(uiMessage("the_server_has_no_exact_position_to_open"),false,"progress"); return; }
            if (action==1 && (local.isEmpty() || other || hasPage)) {
                finish(hasPage ? uiMessage("the_server_position_includes_a_page_number_upload_stopped_to_preserve_it") :
                       other ? uiMessage("the_server_record_contains_other_reading_coordinates_upload_stop_23d8ab") :
                               uiMessage("no_exact_position_on_the_reader_yet"),false,"progress"); return;
            }
#ifdef BOOKORBIT_TEST_HTTP
            if (action==1) ++presentationCounters.uploadPreparations;
            else if (action==2) ++presentationCounters.remotePreparations;
            else ++presentationCounters.conflictPreviews;
#endif
            fileTask=files.submit(this,[path=sync.path,localInfo=sync.localInfo,remoteInfo=analysis.info,cfi,action,stamp=analysis.stamp,fb=sync.fb](const FileCancellation &cancel) {
                PreparedSync result; result.stamp=stamp;
                if (fileCancelled(cancel) || fileStamp(path)!=stamp) return result;
                if (action==1) result.estimate=positionEstimate(path,localInfo,&result.percentage);
                else if (action==2) result.reader=prepareReaderPosition(path,cfi,cancel,fb);
                else for (bool useLocal:{true,false}) {
                    const auto &info=useLocal ? localInfo : remoteInfo;
                    auto preview=info.context; double percentage=0;
                    const bool valid=!fileCancelled(cancel) && positionEstimate(path,info,&percentage);
                    preview["local"]=useLocal; preview["valid"]=valid; preview["percentageValue"]=percentage; preview["empty"]=info.point.isEmpty();
                    result.choices.append(preview);
                }
                return result;
            },[this,sync=next](const PreparedSync &prepared,bool cancelled) {
                if (!acceptFileContext(sync.context,prepared.stamp) || cancelled) return;
                applyProgressAction(sync,prepared);
            });
        });
    },true,true);
}

void Client::applyProgressAction(const ProgressSync &sync,const PreparedSync &prepared) {
    if (readerChanged(sync,downloads.value(sync.id).toObject()["progress"].toObject()!=sync.progress)) return;
    auto state=progressState(sync.id,sync.profile);
    if (!sync.action) { saveConflict(sync,state,prepared.choices,prepared.stamp); return; }
    if (sync.action==2) {
        // Journal before native mutation; failed writes do not advance the agreed baseline.
        state["pending"]=sync.remote; state["pendingLocal"]=sync.local;
        saveProgressIntent(sync.id,sync.profile,state,[this,sync,prepared] {
            if (!acceptFileContext(sync.context,prepared.stamp) || readerChanged(sync)) return;
            UiMessage error;
            if (!saveReaderPosition(sync.path,sync.position,prepared.reader,sync.profile,&error)) {
                finish(error,false,"progress","pending"); return;
            }
            if (persistProgress(sync.id,sync.profile,{{"localBase",sync.cfi},{"remoteBase",sync.remote}}))
                finishProgress(sync.id,sync.remote,uiMessage("position_saved_open_the_book_from_the_built_in_library"));
        });
        return;
    }
    if (!prepared.estimate) {
        finish(uiMessage("the_position_was_recognized_but_the_book_percentage_could_not_be_68fd96"),
               false,"progress","position"); return;
    }
    // Save before POST. An uncertain result is reconciled by the next GET, never blindly retried.
    const QJsonObject outgoing{{"source","text"},{"cfi",sync.local},{"pageNumber",QJsonValue::Null},{"percentage",prepared.percentage}};
    state["outgoing"]=outgoing;
    saveProgressIntent(sync.id,sync.profile,state,[this,sync,outgoing,stamp=prepared.stamp] {
        if (!acceptFileContext(sync.context,stamp) || readerChanged(sync)) return;
        const auto route="/api/v1/books/files/"+sync.id+"/progress";
        const auto sentContext=fileContext(sync.id,sync.path);
        jsonRequest(route,outgoing,[this,id=sync.id,local=sync.local,route,profile=sync.profile,sentContext](const QJsonObject &) {
            if (!acceptFileContext(sentContext)) return;
            jsonRequest(route,{},[this,id,local,profile,sentContext](const QJsonObject &confirmed) {
                if (!acceptFileContext(sentContext)) return;
                if (confirmed["cfi"].toString()!=local) {
                    finish(uiMessage("the_server_position_changed_after_upload_check_again"),false,"progress"); return;
                }
                auto record=downloads.value(id).toObject();
                record["progress"]=QJsonObject{{"localBase",local},{"remoteBase",confirmed},{"profile",profile}};
                if (!saveRecord(id,record)) { finish(uiMessage("uploaded_but_the_result_could_not_be_saved_check_again"),false,"progress"); return; }
                finishProgress(id,confirmed,uiMessage("progress_sent_to_bookorbit"));
            },true,true);
        });
    });
}
