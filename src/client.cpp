#include "client.h"
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
        if (error) *error = "не удалось создать папку " + directory;
        return false;
    }
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (bytes.size() > maxJson) { if (error) *error = "слишком большой файл"; return false; }
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString()+" (код "+QString::number(int(file.error()))+")";
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
bool validContent(const QString &path, const QString &format) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto head = file.peek(1024);
    if (format == "epub" || format == "kepub") return validEpub(path);
    if (format == "docx" || format == "cbz") return head.startsWith(QByteArray("PK\003\004", 4));
    if (format == "pdf") return head.startsWith("%PDF-");
    if (format == "djvu") return head.startsWith("AT&TFORM");
    if (format == "fb2") {
        QXmlStreamReader xml(&file);
        if (!xml.readNextStartElement() || xml.name() != QStringLiteral("FictionBook")) return false;
        while (!xml.atEnd()) xml.readNext();
        return !xml.hasError();
    }
    return !head.isEmpty();
}
bool validId(const QString &id) {
    static const QRegularExpression pattern("^[1-9][0-9]*$");
    bool ok = false;
    const qlonglong number = id.toLongLong(&ok);
    return pattern.match(id).hasMatch() && ok && number <= 2147483647;
}
QByteArray digestFile(const QString &path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result().toHex() : QByteArray{};
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
    network.setTransferTimeout(15000);
    QDir().mkpath(rootDir);
    const auto preferences = readObject(rootDir+"/preferences.json");
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
}

void Client::loadRecords() {
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
            sessionNotice = QFile::remove(sessionFile) ? "Не удалось защитить сохранённый вход. Войдите снова."
                                                       : "Не удалось защитить или удалить данные входа. Проверьте память устройства.";
        } else {
            const auto session = readObject(sessionFile);
            static const QRegularExpression nativeRefresh("^[a-f0-9]{64}$");
            refreshToken = session["refreshToken"].toString();
            if (!nativeRefresh.match(refreshToken).hasMatch()) {
                refreshToken.clear();
                if (!QFile::remove(sessionFile)) sessionNotice = "Не удалось удалить старые данные входа.";
            } else if (session.contains("password")) {
                if (!saveSession()) refreshToken.clear();
            } else sessionStored = true;
        }
    }
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
        const bool confirmed = registered && current["sha256"] == next["sha256"] && current["bytes"] == next["bytes"] &&
            !current["invalid"].toBool() && info.isFile() && !info.isSymLink() &&
            info.canonicalPath() == info.absolutePath() && info.size() == next["bytes"].toDouble() &&
            (newFile == verifiedFile || digestFile(newFile) == next["sha256"].toString().toLatin1()) && validContent(newFile, next["format"].toString());
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

bool Client::saveRecord(const QString &id, const QJsonObject &record) {
    if (recordFile(id, record).isEmpty() ||
        !writeObject(scopeDir+"/records/"+id+".json", record)) return false;
    downloads[id] = record;
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

QString Client::localFile(const QJsonObject &book) const {
    const QString id = QString::number(book["selectedFile"].toObject()["id"].toInt());
    const auto record = downloads.value(id).toObject();
    const QString path = recordFile(id, record);
    const QFileInfo info(path);
    return !record["invalid"].toBool() && !path.isEmpty() && info.canonicalPath() == info.absolutePath() && record["sha256"].toString().size() == 64 && info.isFile() && !info.isSymLink() &&
           info.size() == record["bytes"].toDouble() ? path : QString{};
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

QJsonObject Client::selectBookFile(QJsonObject book) const {
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
            if (!localFile(candidate).isEmpty()) { selected = value.toObject(); break; }
        }
    }
    book["selectedFile"] = selected;
    return book;
}

QVariantMap Client::fileSummary(const QJsonObject &book) const {
    const auto file = book["selectedFile"].toObject();
    const QString id = QString::number(file["id"].toInt()), format = fileFormat(file);
    const auto record = downloads.value(id).toObject(), progress = record["progress"].toObject();
    const bool downloaded = !localFile(book).isEmpty();
    const bool sameProfile=progress.contains("profile") && progress["profile"].toString()==readerProfile();
    return {{"fileId", file["id"].toInt()}, {"format", format.toUpper()},
        {"downloaded", downloaded}, {"readable", readableFormat(format)}, {"canSync", downloaded && format == "epub"},
        {"syncResult", syncResults[id].toString()}, {"pendingProgress", sameProfile && progress["pending"].isObject()},
        {"remoteFileChanged", record["remoteFileChanged"].toBool()}, {"hasConflict", sameProfile && progress["conflictRemote"].isObject()},
        {"supported", !pathFor(book).isEmpty() && file["sizeBytes"].toDouble() <= maxBook},
        {"needsRepair", !record.isEmpty() && !downloaded}};
}

QVariantMap Client::bookSummary(const QJsonObject &book) const {
    QStringList localFormats, formats;
    for (const auto value : book["files"].toArray()) {
        auto variant = book;
        variant["selectedFile"] = value;
        const auto label = fileFormat(value.toObject()).toUpper();
        if (!formats.contains(label)) formats << label;
        if (!localFile(variant).isEmpty() && !localFormats.contains(label)) localFormats << label;
    }
    const auto readingProgress = book["readingProgress"];
    auto result = fileSummary(book);
    result.insert(QVariantMap{{"bookId", book["id"].toInt()}, {"title", book["title"].toString().isEmpty() ? "Без названия" : book["title"].toString()},
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
}

QVariantMap Client::feedback() const {
    return {{"text",feedbackHidden ? QString{} : message},{"context",feedbackContext},
        {"result",feedbackResult},{"bookId",feedbackBookId},{"fileId",feedbackFileId}};
}

void Client::setUiContext(const QString &context) {
    if (uiContext==context) return;
    if (!uiContext.isEmpty() && feedbackResult=="success") feedbackHidden=true;
    uiContext=context; emit changed();
}

QVariantMap Client::syncBatch() const {
    return {{"running",syncingAll},{"completed",syncCompleted},{"total",syncCount},{"stopRequested",syncStopRequested}};
}

void Client::stopSyncAfterCurrent() {
    if (!syncingAll || syncStopRequested) return;
    syncStopRequested=true; emit changed();
}

void Client::searchDownloaded(const QString &query) {
    if (working || query.size()>500) return;
    libraryQuery=query; emit changed();
}

void Client::setLocalSort(const QString &sort) {
    if (working || !QStringList{"recent","title","author"}.contains(sort)) return;
    auto preferences=readObject(rootDir+"/preferences.json"); preferences["localSort"]=sort;
    beginFeedback("catalog");
    if (!writeObject(rootDir+"/preferences.json",preferences)) {
        finish("Не удалось сохранить порядок книг.",false,"settings"); return;
    }
    librarySort=sort; feedbackResult="success"; emit changed();
}

void Client::refreshRecents() {
    const auto profile=readerProfile();
    if (nativeRecents.profile!=profile) { nativeRecents={}; nativeRecents.profile=profile; emit changed(); }
    recentsRequested=true;
    if (working || recentsScheduled) return;
    recentsScheduled=true;
    QTimer::singleShot(0,this,[this] {
        recentsScheduled=false;
        if (working || !recentsRequested) return;
        recentsRequested=false;
        QStringList paths;
        for (const auto value:downloads) {
            const auto book=value.toObject()["book"].toObject();
            const QString path=localFile(book);
            if (!path.isEmpty() && readableFormat(fileFormat(book["selectedFile"].toObject()))) paths << path;
        }
        paths.removeDuplicates();
        nativeRecents=readerRecents(paths); emit changed();
    });
}

QJsonObject Client::recentFile(QJsonObject book) const {
    QJsonObject best; qint64 latest=0;
    const int preferred=fileChoices[QString::number(book["id"].toInt())].toInt();
    for (const auto value:book["files"].toArray()) {
        const auto file=value.toObject(); book["selectedFile"]=file;
        if (!readableFormat(fileFormat(file))) continue;
        const QString path=localFile(book);
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
    QJsonObject best;
    for (const auto value:visibleItems(false)) {
        const auto book=recentFile(value.toObject());
        if (book["nativeOpenTime"].toDouble()<=0) continue;
        if (best.isEmpty() || book["nativeOpenTime"].toDouble()>best["nativeOpenTime"].toDouble() ||
            (book["nativeOpenTime"]==best["nativeOpenTime"] &&
                (QString::localeAwareCompare(book["title"].toString(),best["title"].toString())<0 ||
                 (book["title"]==best["title"] && book["id"].toInt()<best["id"].toInt())))) best=book;
    }
    return best.isEmpty() ? QVariantMap{} : bookSummary(best);
}

void Client::openFile(int fileId,bool applyIncoming) {
    if (working) return;
    const auto record=downloads.value(QString::number(fileId)).toObject();
    if (!record.isEmpty()) openBook(record["book"].toObject(),applyIncoming);
}

QVariantList Client::books() const {
    QVariantList result;
    for (const auto value : visibleItems()) result.append(bookSummary(value.toObject()));
    return result;
}

QVariantMap Client::syncFileStatus(const QString &id, const QString &profile) const {
    const auto record=downloads.value(id).toObject(), progress=record["progress"].toObject();
    const auto saved=record["syncStatus"].toObject();
    const bool sameProfile=saved["profile"].toString()==profile;
    const bool progressProfile=progress.contains("profile") && progress["profile"].toString()==profile;
    QString state=sameProfile ? saved["state"].toString("unknown") : "unknown";
    QString reason=sameProfile ? saved["text"].toString() : QString{};
    const auto book=record["book"].toObject();
    const bool available=!localFile(book).isEmpty();
    if (record["remoteFileChanged"].toBool()) {
        state="file"; reason="Файл на сервере отличается. Скачайте новую версию перед обменом позицией.";
    } else if (!available) {
        state="file"; reason="Локальный EPUB отсутствует или повреждён. Скачайте его заново.";
    } else if (progressProfile && progress["conflictRemote"].isObject()) state="conflict";
    else if (progressProfile && progress["outgoing"].isObject()) state="uncertain";
    else if (progressProfile && progress["pending"].isObject() && (state=="synced" || state=="unknown")) state="pending";
    static const QMap<QString,QString> labels{
        {"synced","Синхронизировано"},{"conflict","Выберите позицию"},{"reader","Закройте читалку"},
        {"reader_unknown","Не удалось проверить читалку"},{"network","Ожидает сети"},
        {"pending","Позиция ещё не применена"},{"uncertain","Нужно проверить результат"},
        {"unknown","Ещё не проверено"},{"auth","Войдите в BookOrbit"},{"file","Проверьте файл EPUB"},
        {"position","Не удалось сопоставить позицию"},{"error","Не удалось синхронизировать"}};
    if (!labels.contains(state)) state="unknown";
    if (state=="network") reason="Подключите сеть и повторите сверку";
    if (state=="unknown") reason="Выполните сверку, чтобы узнать состояние позиции.";
    const auto at=QDateTime::fromString(saved["at"].toString(),Qt::ISODate).toLocalTime();
    return {{"fileId",id.toInt()},{"filename",book["selectedFile"].toObject()["filename"].toString(record["filename"].toString())},
        {"state",state},{"label",labels[state]},{"reason",reason},{"available",available},
        {"checkedAt",sameProfile && at.isValid() ? at.toString("dd.MM.yyyy HH:mm") : QString{}}};
}

QVariantList Client::syncBooks() const {
    QMap<QString,QVariantMap> grouped;
    const QString profile=readerProfile();
    const QStringList priority{"conflict","file","position","reader","reader_unknown","error","auth","pending","uncertain","network","unknown","synced"};
    for (auto it=downloads.begin(); it!=downloads.end(); ++it) {
        const auto record=it.value().toObject();
        if (record["format"].toString("epub")!="epub") continue;
        const auto book=record["book"].toObject();
        const QString key=positiveId(book["id"]) ? "book:"+QString::number(book["id"].toInt()) : "file:"+it.key();
        const auto file=syncFileStatus(it.key(),profile);
        auto &entry=grouped[key];
        if (entry.isEmpty()) entry={{"key",key},{"title",book["title"].toString("Без названия")},
            {"author",authorNames(book["authors"].toArray())},{"state","synced"},{"label","Синхронизировано"}};
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
    std::stable_sort(result.begin(),result.end(),[&priority](const QVariant &a,const QVariant &b) {
        const auto left=a.toMap(),right=b.toMap();
        const int l=priority.indexOf(left["state"].toString()),r=priority.indexOf(right["state"].toString());
        return l!=r ? l<r : QString::localeAwareCompare(left["title"].toString(),right["title"].toString())<0;
    });
    return result;
}

QVariantMap Client::syncSummary() const {
    const auto books=syncBooks();
    int synced=0,waiting=0,attention=0;
    for (const auto &book:books) {
        const auto group=book.toMap()["group"];
        if (group=="synced") ++synced; else if (group=="waiting") ++waiting; else ++attention;
    }
    QSet<int> unsupported,epubs;
    for (const auto value : downloads) {
        const auto record=value.toObject(); const int id=record["book"].toObject()["id"].toInt();
        if (record["format"].toString("epub")=="epub") epubs.insert(id); else unsupported.insert(id);
    }
    unsupported.subtract(epubs);
    return {{"books",books},{"total",books.size()},{"synced",synced},{"waiting",waiting},{"attention",attention},{"unsupported",unsupported.size()}};
}

void Client::syncFile(int fileId) {
    if (working || !downloads.contains(QString::number(fileId))) return;
    beginFeedback("sync",0,fileId);
    dismissConflict(); syncBook(QString::number(fileId));
}

void Client::showSyncFile(int fileId) {
    if (working || !downloads.contains(QString::number(fileId))) return;
    ++detailGeneration;
    detailBook=downloads.value(QString::number(fileId)).toObject()["book"].toObject();
    detailMessage="Сведения сохранены на устройстве";
    emit changed();
}

void Client::connectForSync() {
    if (working) return;
    beginFeedback("sync");
    if (ensureNetwork()) { feedbackResult="success"; message="Сеть подключена. Теперь повторите сверку."; emit changed(); }
}

void Client::prepareConflict(const QString &id,const QString &local,const QJsonObject &remote,const QString &profile) {
    const bool different=conflictId!=id || conflictLocal!=local || conflictRemote!=remote || conflictProfile!=profile;
    conflictId=id; conflictLocal=local; conflictRemote=remote; conflictProfile=profile;
    conflictMessage=downloads.value(id).toObject()["book"].toObject()["title"].toString();
    const auto record=downloads.value(id).toObject();
    const QString path=localFile(record["book"].toObject());
    const bool sameFile=!path.isEmpty() && digestFile(path)==record["sha256"].toString().toLatin1();
    bool other=false;
    for (const auto key : {"pageNumber","positionSeconds","mediaOverlayFragment","mediaOverlaySectionIndex","koboLocationSource",
            "koboLocationType","koboLocationValue","koboContentSourceProgressPercent","koreaderProgress","narrationPercentage","narrationUpdatedAt"})
        if (remote.contains(key) && !remote[key].isNull()) other=true;
    positionChoices.clear();
    for (bool useLocal : {true,false}) {
        QVariantMap context; double percentage=0;
        const auto cfi=useLocal ? local : remote["cfi"].toString();
        const bool valid=sameFile && epubPosition(path,cfi,&percentage,nullptr,&context);
        context["source"]=useLocal ? "На этом PocketBook" : "В BookOrbit";
        context["local"]=useLocal;
        context["percentage"]=valid ? QString("Около %1% книги").arg(QString::number(percentage,'f',1).replace('.',',')) : cfi.isEmpty() ? "Позиция ещё не сохранена" : "Координата не подходит к этому EPUB";
        context["canUse"]=valid && !(useLocal && other) && !record["remoteFileChanged"].toBool();
        context["notice"]=useLocal && other ? "Отправка остановлена, чтобы сохранить другие серверные координаты." : QString{};
        positionChoices.append(context);
    }
    if (different) ++positionRevision;
}

void Client::inspectConflict(int fileId) {
    if (working) return;
    const QString id=QString::number(fileId);
    const auto progress=downloads.value(id).toObject()["progress"].toObject();
    if (!progress["conflictRemote"].isObject() || progress["profile"].toString()!=readerProfile()) return;
    prepareConflict(id,progress["conflictLocal"].toString(),progress["conflictRemote"].toObject(),progress["profile"].toString());
    emit changed();
}

QVariantMap Client::detail() const {
    if (detailBook.isEmpty()) return {};
    auto result = detailBook.toVariantMap();
    const auto summary = bookSummary(detailBook);
    for (auto it = summary.begin(); it != summary.end(); ++it) result[it.key()] = it.value();
    result["notice"] = detailMessage;
    QVariantList files;
    for (const auto value : detailBook["files"].toArray()) {
        auto book = detailBook;
        book["selectedFile"] = value;
        auto file = value.toObject().toVariantMap();
        const auto info = fileSummary(book);
        for (const auto key : {"downloaded", "readable", "supported", "needsRepair", "remoteFileChanged", "syncResult"}) file[key] = info[key];
        file["format"] = fileFormat(value.toObject()).toUpper();
        const double bytes = value.toObject()["sizeBytes"].toDouble(-1);
        file["sizeLabel"] = bytes < 0 ? QString{} : bytes < 1024 ? QString::number(bytes, 'f', 0)+" Б" : bytes < 1024*1024 ? QString::number(bytes/1024, 'f', 0)+" КБ" : QString::number(bytes/(1024*1024), 'f', 1)+" МБ";
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
    if (writeObject(scopeDir+"/file-choices.json", choices)) fileChoices = choices;
}

void Client::showDetail(int bookId) {
    if (working || bookId <= 0) return;
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
        detailMessage = detailBook["detailed"].toBool() ? "Сведения сохранены на устройстве" : "";
        ++detailGeneration;
        emit changed();
        if (authenticated() && !localView && !(retryKind==2 && retryBook["id"].toInt()==bookId)) refreshDetail();
        else if (!detailBook["detailed"].toBool()) {
            detailMessage = "Подробные сведения ещё не загружены";
            emit changed();
        }
        return;
    }
}

void Client::refreshDetail() {
    if (working || detailBook.isEmpty()) return;
    if (!authenticated()) { detailMessage = "Войдите для загрузки подробных сведений"; emit changed(); return; }
    beginFeedback("book",detailBook["id"].toInt(),detailBook["selectedFile"].toObject()["id"].toInt());
    const int id = detailBook["id"].toInt(), generation = detailGeneration;
    const auto selectedFile = detailBook["selectedFile"];
    retryKind = 3; working = true; message.clear(); detailMessage = "Загружаются сведения…"; emit changed();
    jsonRequest("/api/v1/books/"+QString::number(id), {}, [this, id, generation, selectedFile](const QJsonObject &response) {
        if (generation != detailGeneration) { finish("", true, "details"); return; }
        if (response["id"].toInt() != id || !response["files"].isArray() || !response["authors"].isArray()) {
            finish("Сервер вернул некорректные сведения о книге", false, "details"); return;
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
        detailBook = book;
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
        detailMessage = saved ? "" : "Сведения получены, но не сохранены для чтения без сети";
        finish("", true, "details");
    }, true, true);
}

void Client::closeDetail() {
    ++detailGeneration; detailBook = {}; detailMessage.clear();
    if (retryKind == 3) retryKind = 0;
    emit changed();
}

void Client::selectFile(int fileId) {
    if (working || detailBook.isEmpty()) return;
    for (const auto value : detailBook["files"].toArray()) {
        if (value.toObject()["id"].toInt() != fileId) continue;
        if (feedbackContext=="book" && feedbackFileId!=fileId && feedbackResult=="success") feedbackHidden=true;
        detailBook["selectedFile"] = value;
        rememberFile(detailBook);
        emit changed();
        return;
    }
}

void Client::downloadSelected() {
    if (working || detailBook.isEmpty()) return;
    if (!authenticated()) { finish("Для скачивания войдите на сервер", false, "download"); return; }
    downloadBook(detailBook);
}

void Client::openSelected(bool applyIncoming) {
    if (!working && !detailBook.isEmpty()) openBook(detailBook, applyIncoming);
}

void Client::syncSelected() {
    if (working || detailBook.isEmpty()) return;
    dismissConflict();
    syncBook(QString::number(detailBook["selectedFile"].toObject()["id"].toInt()));
}

void Client::showCollections() {
    if (working) return;
    beginFeedback("catalog");
    closeDetail(); stopCovers(); browseHistory.clear();
    collectionList = true; localView = false; activeCollection = 0; activeCollectionName.clear();
    collectionItems = {}; items = {}; count = currentPage = 0;
    retryKind = 4;
    if (!authenticated()) { finish("Войдите, чтобы загрузить коллекции", false, "collections"); return; }
    working = true; message = "Загрузка коллекций…"; emit changed();
    jsonRequest("/api/v1/collections", {}, [this](const QJsonObject &response) {
        QJsonArray parsed;
        QSet<int> seen;
        for (const auto value : response["items"].toArray()) {
            const auto collection = value.toObject();
            if (!positiveId(collection["id"]) || !collection["name"].isString() || !collection["bookCount"].isDouble() || collection["bookCount"].toDouble() < 0 || seen.contains(collection["id"].toInt())) {
                finish("Сервер вернул некорректный список коллекций", false, "collections"); return;
            }
            seen.insert(collection["id"].toInt());
            if (collection["mediaType"] == "books") parsed.append(collection);
        }
        collectionItems = parsed;
        finish("", true, "collections");
    }, true, true, true);
}

void Client::openCollection(int id, const QString &name, double catalogOffset, double detailOffset) {
    if (working || id <= 0) return;
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
    refresh();
}

QVariantMap Client::backFromCollection() {
    if (working || browseHistory.isEmpty()) return {};
    const auto saved = browseHistory.takeLast();
    stopCovers(); ++detailGeneration;
    detailBook = saved["detail"].toObject(); items = saved["items"].toArray();
    currentPage = saved["page"].toInt(); count = saved["total"].toInt(); localView = saved["local"].toBool();
    activeCollection = saved["collectionId"].toInt(); activeCollectionName = saved["collectionName"].toString();
    committedQuery = saved["query"].toString(); retryQuery=committedQuery; detailMessage = saved["notice"].toString();
    collectionList = false; retryKind = 0; message.clear();
    emit changed();
    return {{"query", committedQuery}, {"catalogY", saved["catalogY"].toDouble()}, {"detailY", saved["detailY"].toDouble()}};
}

void Client::finish(QString text, bool success, const QString &operation, const QString &syncState) {
    working = syncingAll || checkingLibrary;
    activeDownload.clear();
    if (!success && retryKind == 3 && detailVisible()) detailMessage = text;
    if (success) retryKind = 0;
    const bool hasFile = !syncingId.isEmpty();
    if (hasFile && !checkingLibrary) {
        auto record=downloads.value(syncingId).toObject();
        record["syncStatus"]=QJsonObject{{"state",syncState.isEmpty() ? (success ? "synced" : "error") : syncState},
            {"text",text},{"at",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},{"profile",readerProfile()}};
        if (!saveRecord(syncingId,record)) {
            success=false;
            text="Не удалось сохранить результат сверки. Повторите после устранения ошибки записи.";
            record["syncStatus"]=QJsonObject{{"state","error"},{"text",text},{"profile",readerProfile()}};
            downloads[syncingId]=record;
        }
    }
    if (!success && feedbackFileId>0 && !hasFile) syncResults[QString::number(feedbackFileId)]=text;
    message = text; feedbackResult=success ? "success" : "error"; feedbackHidden=false;
    logEvent(operation, success ? 0 : 1);
    if (hasFile) {
        if (!checkingLibrary || !verificationCancelled) syncResults[syncingId]=text;
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
        dismissConflict(); // Each unresolved conflict remains in its book record for individual retry.
        if (!syncQueue.isEmpty()) {
            working=true;
            emit changed();
            QTimer::singleShot(0,this,[this] {
                working=false;
                if (checkingLibrary && verificationCancelled) {
                    finish("Проверка отменена",false,"verify"); return;
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
            message=QString("%1. Файлов: %2; совпало: %3; отличается: %4; ошибок: %5; не проверено: %6. Результаты — на карточках книг.")
                .arg(verificationCancelled ? "Проверка отменена" : !authenticated() ? "Проверка остановлена: войдите снова" : "Проверка завершена")
                .arg(syncCount).arg(syncSucceeded).arg(verificationDifferent).arg(verificationErrors).arg(unchecked);
            if (recentsRequested) refreshRecents();
            emit changed();
            emit completed("verify",!verificationCancelled && syncSucceeded==syncCount);
            return;
        }
        syncingAll=false;
        working=false;
        const auto summary=syncSummary();
        const bool allOk=!syncStopRequested && authenticated() && syncSucceeded==syncCount;
        message=QString("%1. Завершено файлов: %2 из %3. Синхронизировано книг: %4 из %5; нужно действие: %6; ожидают сверки: %7.")
            .arg(!authenticated() ? "Сверка остановлена: войдите снова" : syncStopRequested ? "Сверка остановлена пользователем" : allOk ? "Сверка завершена" : "Сверка завершена с проблемами")
            .arg(syncCompleted).arg(syncCount).arg(summary["synced"].toInt()).arg(summary["total"].toInt())
            .arg(summary["attention"].toInt()).arg(summary["waiting"].toInt());
        feedbackResult=allOk ? "success" : "error";
        refreshRecents();
        emit changed();
        emit completed("sync",allOk);
        return;
    }
    if (operation=="progress" || operation=="download" || operation=="open" || recentsRequested) refreshRecents();
    emit changed();
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
        finish("В ответе сервера нет действительной сессии", false, "login");
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
        sessionNotice = removed ? "Не удалось защитить сохранённый вход. После закрытия приложения потребуется войти снова."
                                : "Не удалось защитить или удалить данные входа. Проверьте память устройства.";
    } else sessionNotice.clear();
    logEvent(sessionStored ? "session saved" : "session save failed");
    emit changed();
    return sessionStored;
}

void Client::invalidateSession() {
    token.clear(); refreshToken.clear(); sessionStored = false; sessionNotice.clear();
    stopCovers();
    const QString path = scopeDir+"/session.json";
    if ((QFile::exists(path) || QFileInfo(path).isSymLink()) && !QFile::remove(path))
        sessionNotice = "Не удалось удалить недействительную сессию с устройства.";
}

void Client::restoreSession() {
    if (working || authenticated() || refreshToken.isEmpty()) return;
    beginFeedback("connection");
    working = true;
    message = "Восстановление входа…";
    emit changed();
    jsonRequest("/api/v1/auth/refresh", {{"refreshToken", refreshToken}}, [this](const QJsonObject &response) {
        if (!setCredentials(response)) return;
        working = false;
        refresh();
    }, false);
}

void Client::renewSession(const std::function<void()> &resume) {
    stopCovers();
    message = "Обновление сессии…";
    emit changed();
    jsonRequest("/api/v1/auth/refresh", {{"refreshToken", refreshToken}}, [this, resume](const QJsonObject &response) {
        if (setCredentials(response)) { queueCovers(); resume(); }
    }, false);
}

bool Client::ensureNetwork() {
    if (connectNetwork()) return true;
    finish("Wi-Fi не подключён. Подключитесь к сети и повторите действие.", false, "network", "network");
    return false;
}

void Client::jsonRequest(const QString &path, const QJsonObject &payload, const Callback &callback, bool renew, bool get, bool arrayResponse) {
    if (checkingLibrary && verificationCancelled) { finish("Проверка отменена",false,"verify"); return; }
    if (!ensureNetwork()) return;
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, callback, path, payload, renew, get, arrayResponse] {
        reply->deleteLater();
        if (activeVerification==reply) activeVerification.clear();
        if (checkingLibrary && verificationCancelled) { finish("Проверка отменена",false,"verify"); return; }
        bytes->append(reply->readAll());
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        logEvent(path.section('?', 0, 0), int(reply->error()), code);
        if (code == 401 && renew && (path.startsWith("/api/v1/books/") || path.startsWith("/api/v1/collections")) && !refreshToken.isEmpty()) {
            if (!get && path.endsWith("/progress")) {
                // A refreshed token is not permission to replay an old sync
                // decision. Keep the outgoing journal; the next sync starts with
                // fresh reader/profile/server snapshots, including explicit choices.
                renewSession([this] {
                    finish("Вход восстановлен. Повторите сверку: прежняя позиция не отправлена повторно.",
                           false,"progress","uncertain");
                });
            } else {
                renewSession([this, path, payload, callback, get, arrayResponse] { jsonRequest(path, payload, callback, false, get, arrayResponse); });
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300 || bytes->size() > maxJson) {
            if (code == 401) invalidateSession();
            finish(path == "/api/v1/auth/logout" ? "Вы вышли на устройстве. Отзыв сессии на сервере не подтверждён." :
                   reply->error() == QNetworkReply::SslHandshakeFailedError ? "Не удалось проверить TLS-сертификат сервера. Проверьте доверенный CA и дату устройства." :
                   reply->error() == QNetworkReply::HostNotFoundError ? "Не найден сервер "+endpoint.host()+". Проверьте Wi-Fi и DNS." :
                   reply->error() == QNetworkReply::ConnectionRefusedError ? "Сервер отклонил соединение. Проверьте адрес и сеть." :
                   reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::OperationCanceledError ? "Время подключения истекло. Проверьте сеть и повторите." :
                   code == 401 ? "Вход не выполнен или сессия истекла. Войдите снова." :
                   code >= 400 ? "Сервер ответил HTTP "+QString::number(code)+". Проверьте адрес и повторите." :
                   "Сервер недоступен. Скачанные книги можно читать без сети.", false, path, code==401 ? "auth" : "error");
            return;
        }
        if (!get && path.endsWith("/progress") && bytes->trimmed().isEmpty()) { callback({}); return; }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(*bytes, &error);
        if (error.error != QJsonParseError::NoError || (arrayResponse ? !doc.isArray() : !doc.isObject())) {
            finish("Сервер вернул некорректные данные", false, path);
            return;
        }
        callback(arrayResponse ? QJsonObject{{"items", doc.array()}} : doc.object());
    });
}

void Client::login(const QString &address, const QString &username, const QString &password) {
    if (working) return;
    beginFeedback("connection");
    QString normalized = address.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    const QUrl url(normalized, QUrl::StrictMode);
    if (validServer(url) && url == endpoint && password.isEmpty() && username == user && !refreshToken.isEmpty()) {
        restoreSession(); return;
    }
    const QString secret = password;
    if (!validServer(url) || username.trimmed().isEmpty() || secret.isEmpty()) {
        finish("Укажите HTTPS-адрес, имя пользователя и пароль.", false, "login");
        return;
    }
    if (!configure(normalized, username)) return;
    retryKind = 0;
    feedbackResult="running";
    working = true;
    message = "Подключение…";
    emit changed();
    jsonRequest("/api/v1/auth/login", {{"username", username}, {"password", secret},
                                    {"clientKind", "native"}, {"deviceLabel", "PocketBook prototype"}},
        [this](const QJsonObject &response) {
            if (!setCredentials(response)) return;
            working = false;
            refresh();
        });
}

void Client::refresh(int targetPage, const QString &query) {
    if (working) return;
    if (targetPage < 0 || query.size() > 500) return;
    collectionList = false;
    stopCovers();
    localView = false;
    beginFeedback("catalog");
    if (token.isEmpty()) { finish("Сначала войдите на сервер", false, "catalog"); return; }
    retryKind = 1; retryPage = targetPage; retryQuery = query;
    working = true;
    message = "Загрузка каталога…";
    emit changed();
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
                finish("Сервер вернул некорректный каталог.", false, "catalog"); return;
            }
            QJsonArray parsed;
            for (auto value : array) {
                auto book = value.toObject();
                if (!positiveId(book["id"]) || !book["files"].isArray()) {
                    finish("Некорректная запись книги", false, "catalog"); return;
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
            cleanupCovers();
            queueCovers();
            finish(items.isEmpty() ? "Книги не найдены" : "", true, "catalog");
        }, true, activeCollection > 0);
}

void Client::download(int index) {
    if (working) return;
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return;
    if (token.isEmpty()) { finish("Для скачивания войдите на сервер", false, "download"); return; }
    downloadBook(visible[index].toObject());
}

void Client::downloadBook(const QJsonObject &book, bool renew) {
    const int bookId=book["id"].toInt(), fileId=book["selectedFile"].toObject()["id"].toInt();
    const auto previous=downloads.value(QString::number(fileId)).toObject();
    if (previous.isEmpty()) { transferBook(book,renew); return; }
    beginFeedback("book",bookId,fileId);
    retryKind=2; retryBook=book;
    if (bookId<=0 || fileId<=0 || previous["bookId"].toInt()!=bookId) {
        finish("Этот файл уже связан с другой книгой. Обновите сведения.",false,"download"); return;
    }
    // Repairs and retries resolve file metadata again. A known replacement may
    // have a different byte count under the same fileId; never use cached size.
    working=true; message="Проверяем актуальную версию файла…"; emit changed();
    jsonRequest("/api/v1/books/"+QString::number(bookId),{},
        [this,book,bookId,fileId,renew](const QJsonObject &response) {
            if (response["id"].toInt()!=bookId || !response["files"].isArray()) {
                finish("Не удалось получить актуальные сведения о файле.",false,"download"); return;
            }
            QJsonObject selected;
            for (const auto value:bookFiles(response))
                if (value.toObject()["id"].toInt()==fileId) selected=value.toObject();
            if (selected.isEmpty()) {
                finish("Выбранного файла больше нет на сервере. Обновите карточку книги.",false,"download"); return;
            }
            if (fileFormat(selected)!=fileFormat(book["selectedFile"].toObject())) {
                finish("Формат серверного файла изменился. Обновите карточку книги.",false,"download"); return;
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
        (!file["sizeBytes"].isDouble() || reportedSize < 0 || reportedSize > maxBook || reportedSize != qint64(reportedSize)))) { finish("Файл не поддерживается или превышает 100 МБ", false, "download"); return; }
    const auto previous = downloads.value(id).toObject();
    if (!previous.isEmpty() && previous["bookId"] != book["id"]) {
        finish("Этот файл уже связан с другой книгой. Обновите сведения.", false, "download"); return;
    }
    const QString oldFile = previous.isEmpty() ? path : recordFile(id, previous);
    if (oldFile.isEmpty()) { finish("Некорректная запись скачанной книги", false, "download"); return; }
    if ((QFile::exists(oldFile) || !previous.isEmpty()) && readerFileState(oldFile) != ReaderFileState::Closed) {
        finish("Закройте книгу в штатной читалке и повторите загрузку", false, "download"); return;
    }
    const qint64 expected = reportedSize < 0 ? 0 : static_cast<qint64>(reportedSize);
    const QString directory = downloadDirectory();
    if (!accessibleDirectory(directory) || !sameStorage(scopeDir,directory)) {
        finish("Папка загрузки недоступна. Выберите другую папку в настройках.", false, "download"); return;
    }
    const QString stem = id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString staging = scopeDir+"/"+stem+".part";
    const QString finalPath = directory+"/"+stem+"."+format;
    auto output = std::make_shared<QSaveFile>(staging);
    if (!output->open(QIODevice::WriteOnly)) { finish("Не удалось создать файл книги", false, "download"); return; }
    working = true; cancelled = false; message = "Скачивание книги…";
    auto request = this->request("/api/v1/books/files/"+QString::number(file["id"].toInt())+"/download");
    auto reply = network.get(request);
    activeDownload = reply;
    emit changed();
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, output, hash, size, expected, file, drain, book, id, oldFile, previous, staging, finalPath, stem, directory, renew, format] {
        if (reply->bytesAvailable()) drain();
        reply->deleteLater();
        logEvent("download", int(reply->error()), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        if (cancelled) {
            output->cancelWriting(); retryKind = 0;
            finish("Загрузка отменена. Прежний файл сохранён.", false, "download"); return;
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401) {
            output->cancelWriting(); activeDownload.clear();
            if (renew && !refreshToken.isEmpty()) {
                renewSession([this, book] { downloadBook(book, false); });
                return;
            }
            invalidateSession();
            finish("Сессия истекла. Войдите снова для скачивания.", false, "download"); return;
        }
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
            reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("application/json") ||
            (reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/html") && format != "html" && format != "htm") ||
            *size == 0 || *size > maxBook || (expected > 0 && *size != expected)) {
            output->cancelWriting();
            finish("Загрузка не завершена. Повторите попытку; прежний файл сохранён.", false, "download"); return;
        }
        if ((QFile::exists(oldFile) || downloads.contains(id)) && readerFileState(oldFile) != ReaderFileState::Closed) {
            output->cancelWriting();
            finish("Книга открыта в штатной читалке. Закройте её и повторите загрузку.", false, "download"); return;
        }
        const QString newHash = QString::fromLatin1(hash->result().toHex());
        if (downloads.value(id).toObject()["sha256"] == newHash &&
            !localFile(book).isEmpty() && digestFile(oldFile) == newHash.toLatin1()) {
            output->cancelWriting();
            auto record=downloads.value(id).toObject();
            if (record.contains("remoteFileChanged")) {
                record.remove("remoteFileChanged");
                record.remove("syncStatus");
                if (!saveRecord(id,record)) {
                    finish("Не удалось сохранить результат загрузки. Прежняя книга доступна.",false,"download"); return;
                }
            }
            syncResults.remove(id);
            rememberFile(book);
            finish("Книга уже скачана", true, "download"); return;
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
            finish("Не удалось сохранить журнал загрузки. Прежняя книга доступна.", false, "download"); return;
        }
        if (!accessibleDirectory(directory) || !sameStorage(scopeDir,directory) || !output->commit() ||
            !validContent(staging, format) || !QFile::rename(staging, finalPath)) {
            cleanupBooks();
            finish("Не удалось сохранить книгу", false, "download"); return;
        }
        if (digestFile(finalPath) != newHash.toLatin1()) {
            cleanupBooks();
            finish("Сохранённая книга повреждена. Прежняя книга доступна.", false, "download"); return;
        }
        if (!saveRecord(id, record)) {
            cleanupBooks();
            finish("Запись загрузки не сохранена. Прежняя книга доступна.", false, "download"); return;
        }
        syncResults.remove(id);
        rememberFile(book);
        cleanupBooks(finalPath); // Its digest was verified immediately before saveRecord().
        if (readableFormat(format)) scanBook(finalPath);
        finish("Книга скачана и доступна без сети", true, "download");
    });
}

void Client::open(int index, bool applyIncoming) {
    if (working) return;
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return;
    openBook(visible[index].toObject(), applyIncoming);
}

void Client::openBook(const QJsonObject &book, bool applyIncoming) {
    beginFeedback("book",book["id"].toInt(),book["selectedFile"].toObject()["id"].toInt());
    const auto path = localFile(book);
    if (path.isEmpty()) { finish("Сначала скачайте книгу", false, "open"); return; }
    if (!readableFormat(fileFormat(book["selectedFile"].toObject()))) {
        finish("Файл скачан. Открытие этого формата из клиента пока не поддерживается.", false, "open"); return;
    }
    const QString id = QString::number(book["selectedFile"].toObject()["id"].toInt());
    if (digestFile(path) != downloads.value(id).toObject()["sha256"].toString().toLatin1()) {
        auto record = downloads.value(id).toObject();
        record["invalid"] = true;
        if (!saveRecord(id, record)) downloads[id] = record;
        finish("Файл книги изменён. Скачайте его повторно.", false, "open"); return;
    }
    if (!readerBookIndexed(path)) {
        scanBook(path);
        finish("Книга ещё не зарегистрирована в библиотеке. Подождите и нажмите «Читать» снова.", false, "open");
        return;
    }
    auto record = downloads.value(id).toObject();
    auto sync = record["progress"].toObject();
    const auto pending = sync["pending"].toObject();
    if (applyIncoming && !pending.isEmpty()) {
        finish("Полученная позиция ещё не применена. Синхронизируйте позицию или выберите «Читать локально».",false,"open");
        return;
    }

    const bool ok = openReader(path);
    if (ok) rememberFile(book);
    finish(ok ? "Открытие в штатной читалке…" : "Не удалось запустить читалку", ok, "open");
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
    if (working) return false;
    beginFeedback("settings");
    if (!validDownloadDirectory(path)) {
        finish("Выберите доступную папку во внутренней памяти, вне служебных каталогов.", false, "settings"); return false;
    }
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["downloadDirectory"] = path;
    QString error;
    if (!writeObject(rootDir+"/preferences.json", preferences, &error)) {
        logEvent("folder save failed: "+error);
        finish("Не удалось сохранить папку: "+error, false, "settings"); return false;
    }
    bookDirectory = path;
    logEvent("folder saved");
    finish("Папка сохранена. Уже скачанные книги остались на своих местах.", true, "settings");
    return true;
}

bool Client::setDiagnosticLogging(bool enabled) {
    beginFeedback("settings");
    diagnostics = enabled;
    logEvent(enabled ? "diagnostics enabled" : "diagnostics disabled");
    auto preferences = readObject(rootDir+"/preferences.json");
    preferences["diagnosticLogging"] = enabled;
    QString error;
    if (!writeObject(rootDir+"/preferences.json", preferences, &error)) {
        finish((enabled ? "Журнал включён до закрытия приложения. " : "Журнал выключен. ")+QString("Не удалось сохранить настройку: ")+error,
               false, "settings"); return false;
    }
    finish(enabled ? "Диагностический журнал включён" : "Диагностический журнал выключен", true, "settings");
    return true;
}

void Client::logEvent(const QString &event, int code, int http) {
    if (!diagnostics) return;
    const QString path = diagnosticLogPath();
    if (QFileInfo(path).size() > 128 * 1024) {
        QFile::remove(path+".1");
        QFile::rename(path, path+".1");
    }
    QFile log(path);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const auto line = (QDateTime::currentDateTimeUtc().toString(Qt::ISODate)+" "+event+
                       " error="+QString::number(code)+" http="+QString::number(http)+"\n").toUtf8();
    log.write(line);
    log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
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
    if (working) return false;
    beginFeedback("connection");
    QString normalized = address.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    const QUrl url(normalized, QUrl::StrictMode);
    if (!validServer(url) || name.trimmed().isEmpty() || name.size() > 200) {
        finish("Укажите HTTPS-адрес и имя пользователя.", false, "settings");
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
        finish("Не удалось сохранить подключение: "+error, false, "settings"); return false;
    }
    dismissConflict();
    stopCovers();
    token.clear(); refreshToken.clear(); retryKind = 0; savedAccounts = next;
    endpoint = url; user = name;
    loadRecords(); localView = true;
    finish("Подключение сохранено. Скачанные книги доступны без входа.", true, "settings");
    return true;
}

void Client::selectAccount(int index) {
    if (working || index < 0 || index >= savedAccounts.size()) return;
    const auto account = savedAccounts[index].toObject();
    configure(account["server"].toString(), account["username"].toString());
}

void Client::logout() {
    if (working) return;
    beginFeedback("connection");
    stopCovers();
    const QString credential = refreshToken;
    const QString sessionFile = scopeDir+"/session.json";
    if (QFile::exists(sessionFile) && !QFile::remove(sessionFile)) {
        sessionNotice="Не удалось удалить сохранённый вход. Проверьте память устройства.";
        finish(sessionNotice, false, "logout"); return;
    }
    sessionStored = false; sessionNotice.clear();
    token.clear(); refreshToken.clear(); retryKind = 0; localView = true;
    items = {}; count = currentPage = 0; collectionItems = {}; detailBook = {};
    ++detailGeneration; browseHistory.clear(); collectionList = false; activeCollection = 0; activeCollectionName.clear();
    cleanupCovers();
    logEvent("local logout");
    if (!credential.isEmpty()) {
        working = true; message = "Выход из аккаунта…";
        emit changed();
        jsonRequest("/api/v1/auth/logout", {{"refreshToken", credential}}, [this](const QJsonObject &) {
            finish("Сессия отозвана. Скачанные книги сохранены.", true, "logout");
        }, false);
        return;
    }
    finish("Вход на устройстве завершён. Скачанные книги сохранены.", true, "logout");
}

void Client::showDownloaded(bool value) {
    if (working) return;
    detailBook = {}; ++detailGeneration; browseHistory.clear(); collectionList = false; activeCollection = 0; activeCollectionName.clear();
    if (!value) { refresh(); return; }
    localView = true;
    refreshRecents();
    stopCovers();
    items = {}; count = currentPage = 0;
    cleanupCovers();
    emit changed();
}

QJsonArray Client::visibleItems(bool applyQuery) const {
    QJsonArray result;
    if (!localView) {
        for (const auto value : items) result.append(selectBookFile(value.toObject()));
        return result;
    }
    QMap<int, QJsonObject> grouped;
    for (auto it = downloads.begin(); it != downloads.end(); ++it) {
        const auto record = it.value().toObject();
        auto book = record["book"].toObject();
        if (book.isEmpty()) book = {{"id", record["bookId"]}, {"title", "Книга · файл "+it.key()}};
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
    QList<QJsonObject> sorted;
    const QString query=libraryQuery.normalized(QString::NormalizationForm_KC).toCaseFolded();
    for (const auto &book:grouped) {
        const QString text=(book["title"].toString()+"\n"+authorNames(book["authors"].toArray())+"\n"+book["seriesName"].toString()).normalized(QString::NormalizationForm_KC).toCaseFolded();
        if (!applyQuery || query.isEmpty() || text.contains(query)) {
            auto entry=selectBookFile(book);
            entry["nativeOpenTime"]=recentFile(entry)["nativeOpenTime"]; sorted.append(entry);
        }
    }
    std::sort(sorted.begin(),sorted.end(),[this](const QJsonObject &a,const QJsonObject &b) {
        if (librarySort=="recent" && nativeRecents.available) {
            const double l=a["nativeOpenTime"].toDouble(),r=b["nativeOpenTime"].toDouble();
            if (l!=r) return l>r;
        }
        if (librarySort=="author") {
            const int cmp=QString::localeAwareCompare(authorNames(a["authors"].toArray()),authorNames(b["authors"].toArray()));
            if (cmp) return cmp<0;
        }
        const int cmp=QString::localeAwareCompare(a["title"].toString(),b["title"].toString());
        return cmp ? cmp<0 : a["id"].toInt()<b["id"].toInt();
    });
    for (const auto &book:sorted) result.append(book);
    return result;
}

void Client::cancelDownload() {
    if (activeDownload) { cancelled = true; activeDownload->abort(); }
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
            QBuffer buffer(bytes.get()); buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const auto format = reader.format();
            const auto size = reader.size();
            if ((format == "png" || format == "jpeg" || format == "webp") && size.width() > 0 &&
                size.height() > 0 && size.width() <= 2048 && size.height() <= 2048) {
                reader.setScaledSize(size.scaled(240, 320, Qt::KeepAspectRatio));
                const auto cover = reader.read();
                QSaveFile file(path);
                if (!cover.isNull() && file.open(QIODevice::WriteOnly) && cover.save(&file, "PNG") && file.commit()) {
                    if (version.isEmpty()) coverVersions.remove(QString::number(id));
                    else coverVersions[QString::number(id)]=version;
                    writeObject(scopeDir+"/cover-index.json",coverVersions);
                    coverRevisions[path]=++coverVersion; emit coversChanged();
                    cleanupCovers();
                }
            }
        }
        fetchNextCover();
    });
}

void Client::dismissConflict() {
    conflictId.clear(); conflictProfile.clear(); conflictLocal.clear(); conflictRemote={}; conflictMessage.clear();
    positionChoices.clear();
    emit changed();
}

void Client::syncAll() {
    if (working) return;
    beginFeedback("sync");
    if (!authenticated()) { finish("Для синхронизации войдите на сервер",false,"sync"); return; }
    syncQueue.clear();
    for (auto it=downloads.begin(); it!=downloads.end(); ++it)
        if (it.value().toObject()["format"].toString("epub") == "epub") syncQueue.append(it.key());
    if (syncQueue.isEmpty()) { finish("Нет скачанных книг для синхронизации",true,"sync"); return; }
    dismissConflict();
    syncStopRequested=false; syncCompleted=0;
    syncingAll=true; syncCount=syncQueue.size(); syncSucceeded=0;
    syncBook(syncQueue.takeFirst());
}

void Client::resolveProgress(bool useLocal) {
    if (!working && !conflictId.isEmpty()) syncBook(conflictId,useLocal?1:2);
}

void Client::verifyLibrary() {
    if (working) return;
    beginFeedback("settings");
    if (!authenticated()) { finish("Для проверки библиотеки войдите на сервер",false,"verify"); return; }
    syncQueue=downloads.keys();
    if (syncQueue.isEmpty()) { finish("Нет скачанных файлов для проверки",true,"verify"); return; }
    dismissConflict(); stopCovers(); retryKind=0;
    checkingLibrary=true; verificationCancelled=false;
    syncCount=syncQueue.size(); syncSucceeded=verificationDifferent=verificationErrors=0;
    verifyBook(syncQueue.takeFirst());
}

void Client::cancelLibraryVerification() {
    if (!checkingLibrary) return;
    verificationCancelled=true;
    if (activeVerification) activeVerification->abort();
    // Between files the queued continuation completes cancellation before starting another file.
}

void Client::verifyBook(const QString &id) {
    syncingId=id;
    const auto record=downloads.value(id).toObject();
    const auto path=recordFile(id,record);
    const QFileInfo info(path);
    working=true;
    message=QString("Проверка файла %1 из %2: %3 (%4, fileId %5)…")
        .arg(syncCount-syncQueue.size()).arg(syncCount)
        .arg(record["book"].toObject()["title"].toString())
        .arg(record["format"].toString("epub").toUpper(),id);
    emit changed();
    if (verificationCancelled) { finish("Проверка отменена",false,"verify"); return; }
    if (path.isEmpty() || !info.isFile() || info.isSymLink() || info.canonicalPath()!=info.absolutePath() ||
        info.size()!=record["bytes"].toDouble() || digestFile(path)!=record["sha256"].toString().toLatin1()) {
        finish("Не удалось проверить: локальный файл отсутствует или повреждён. Скачайте заново.",false,"verify"); return;
    }
    verifyRemoteFile(id);
}

void Client::verifyRemoteFile(const QString &id, bool renew) {
    if (verificationCancelled) { finish("Проверка отменена",false,"verify"); return; }
    if (!ensureNetwork()) return;
    auto reply=network.get(request("/api/v1/books/files/"+id+"/download"));
    activeVerification=reply;
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
    connect(reply,&QNetworkReply::finished,this,[this,reply,id,renew,drain,hash,size] {
        drain(); reply->deleteLater();
        activeVerification.clear();
        if (verificationCancelled) { finish("Проверка отменена",false,"verify"); return; }
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
            finish("Не удалось проверить файл на сервере"+(code>=400 ? QString(" (HTTP %1)").arg(code) : QString{})+".",false,"verify"); return;
        }
        auto record=downloads.value(id).toObject();
        const auto path=recordFile(id,record);
        if (digestFile(path)!=record["sha256"].toString().toLatin1()) {
            finish("Не удалось проверить: локальный файл изменился во время проверки.",false,"verify"); return;
        }
        const bool different=hash->result().toHex()!=record["sha256"].toString().toLatin1();
        if (different) record["remoteFileChanged"]=true;
        else record.remove("remoteFileChanged");
        if (record!=downloads.value(id).toObject() && !saveRecord(id,record)) {
            if (different) downloads[id]=record; // Keep the known mismatch blocked in memory even when storage fails.
            finish("Не удалось сохранить результат проверки. Повторите проверку после устранения ошибки записи.",false,"verify"); return;
        }
        finish(different ? "Файл на сервере отличается. Обмен прогрессом приостановлен" : "Локальный файл совпадает с BookOrbit",
               !different,different ? "verify-different" : "verify");
    });
}

void Client::syncBook(const QString &id, int choice) {
    if (working) return;
    if (!syncingAll) beginFeedback(detailVisible() ? "book" : "sync", downloads.value(id).toObject()["bookId"].toInt(),id.toInt());
    syncingId=id;
    if (!authenticated()) { finish("Для синхронизации войдите на сервер",false,"progress","auth"); return; }
    auto record=downloads.value(id).toObject();
    if (record["remoteFileChanged"].toBool()) {
        finish("Файл на сервере отличается. Обмен прогрессом приостановлен",false,"progress","file"); return;
    }
    if (record["format"].toString("epub") != "epub") {
        finish("Обмен позицией доступен только для EPUB. Файл можно читать локально.",false,"progress"); return;
    }
    const QString path=localFile(record["book"].toObject());
    QString position;
    const QString profile=readerProfile();
    if (path.isEmpty() || digestFile(path)!=record["sha256"].toString().toLatin1()) {
        finish("Скачайте действительный EPUB перед синхронизацией",false,"progress","file"); return;
    }
    const auto reader=readerFileState(path);
    if (reader==ReaderFileState::Open) {
        finish("Закройте книгу в штатной читалке и повторите синхронизацию. Кнопка Home оставляет книгу открытой.",false,"progress","reader"); return;
    }
    if (reader==ReaderFileState::Unknown || !readerPosition(path,&position)) {
        finish("Не удалось проверить состояние читалки. Позиции не изменены; повторите проверку.",false,"progress","reader_unknown"); return;
    }
    const QString local=nativeCfi(position);
    if ((!position.isEmpty() && local.isEmpty()) || (!local.isEmpty() && !epubPosition(path,local,nullptr))) {
        finish("Формат локальной позиции не поддерживается. Прогресс не изменён.",false,"progress","position"); return;
    }
    working=true; retryKind=0; stopCovers();
    message=syncingAll ? QString("Проверяем файл %1 из %2: %3…").arg(syncCount-syncQueue.size()).arg(syncCount)
        .arg(record["book"].toObject()["title"].toString()) : "Синхронизация прогресса…";
    emit changed();
    const QString route="/api/v1/books/files/"+id+"/progress";
    // ponytail: trust the downloaded file identity; explicit library verification detects server replacements.
        jsonRequest(route,{},[this,id,path,position,local,choice,route,profile](const QJsonObject &remote) {
            QString current;
            if (readerProfile()!=profile || readerFileState(path)!=ReaderFileState::Closed || !readerPosition(path,&current) || current!=position) {
                finish("Состояние читалки изменилось. Повторите синхронизацию.",false,"progress"); return;
            }
            if (digestFile(path)!=downloads.value(id).toObject()["sha256"].toString().toLatin1()) {
                finish("Локальный EPUB изменился во время сверки. Прогресс не изменён.",false,"progress","file"); return;
            }
            const QString remoteCfi=remote["cfi"].toString();
            QString cfi=remoteCfi;
            const auto percent=remote["percentage"];
            if (!remote.contains("cfi") || (!remote["cfi"].isNull() && !remote["cfi"].isString()) ||
                !percent.isDouble() || percent.toDouble()<0 || percent.toDouble()>100) {
                finish("Некорректный ответ сервера о прогрессе: ожидаются CFI и процент от 0 до 100. Прогресс не изменён.",false,"progress"); return;
            }
            if (!cfi.isEmpty() && !epubPosition(path,remoteCfi,nullptr,&cfi)) {
                finish("Не удалось сопоставить серверную координату CFI с этим EPUB. Прогресс не изменён.",false,"progress","position"); return;
            }
            bool other=false;
            for (const auto key : {"positionSeconds","mediaOverlayFragment","mediaOverlaySectionIndex","koboLocationSource",
                                  "koboLocationType","koboLocationValue","koboContentSourceProgressPercent","koreaderProgress","narrationPercentage","narrationUpdatedAt"})
                if (remote.contains(key) && !remote[key].isNull()) other=true;
            const bool hasPage=remote.contains("pageNumber") && !remote["pageNumber"].isNull();
            const bool remoteEmpty=cfi.isEmpty() && percent.toDouble()==0 && !hasPage && !other;
            if (cfi.isEmpty() && !remoteEmpty) {
                finish("На сервере есть прогресс без точного CFI. Автоматический обмен невозможен.",false,"progress","position"); return;
            }
            auto record=downloads.value(id).toObject(); auto state=record["progress"].toObject();
            if (state.contains("profile") && state["profile"].toString()!=profile) state={};
            const auto pending=state["pending"].toObject();
            auto persist=[this,id,profile,&record,&state] {
                state["profile"]=profile;
                record["progress"]=state;
                if (saveRecord(id,record)) return true;
                finish("Не удалось сохранить прогресс. Повторите синхронизацию.",false,"progress"); return false;
            };
            auto acknowledge=[&] {
                state={{"localBase",local},{"remoteBase",remote}};
                if (persist()) { dismissConflict(); finish("Прогресс синхронизирован",true,"progress"); }
            };
            const auto equivalent=[&path](const QString &a,const QString &b,bool range=false) {
                return a==b || sameEpubPosition(path,a,b,range);
            };
            if (equivalent(local,cfi)) { acknowledge(); return; }
            const bool known=state.contains("profile") && state["profile"].toString()==profile &&
                state.contains("localBase") && state["remoteBase"].isObject();
            const bool localChanged=known ? !equivalent(local,state["localBase"].toString()) : !local.isEmpty();
            const bool remoteChanged=known ? !equivalent(remoteCfi,state["remoteBase"].toObject()["cfi"].toString(),true) : !remoteEmpty;
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
                    if (persist()) finish("Прогресс не изменился",true,"progress");
                    return;
                case SyncDecision::Conflict: break;
                }
            }
            if (!action) {
                // Persist both versions; after restart the next GET rechecks the conflict.
                state["conflictLocal"]=local; state["conflictRemote"]=remote;
                if (!persist()) return;
                if (!syncingAll) prepareConflict(id,local,remote,profile);
                finish(changedChoice ? "Позиция изменилась во время выбора. Проверьте варианты ещё раз." : "Нужно выбрать позицию чтения",false,"progress","conflict"); return;
            }
            if (action==2) {
                if (cfi.isEmpty()) { finish("На сервере нет точной позиции для открытия.",false,"progress"); return; }
                // Journal before native mutation; failed writes do not advance the agreed baseline.
                state["pending"]=remote; state["pendingLocal"]=local;
                if (!persist()) return;
                QString error;
                if (!saveReaderPosition(path,position,cfi,profile,&error)) {
                    finish(error,false,"progress","pending"); return;
                }
                state={{"localBase",cfi},{"remoteBase",remote}};
                if (persist()) { dismissConflict(); finish("Позиция сохранена. Откройте книгу из штатной библиотеки.",true,"progress"); }
                return;
            }
            if (local.isEmpty() || other || hasPage) {
                finish(hasPage ? "Серверная позиция содержит номер страницы. Отправка остановлена, чтобы не стереть его." :
                       other ? "Серверная запись содержит другие координаты чтения. Отправка остановлена, чтобы сохранить их." :
                               "На ридере ещё нет точной позиции.",false,"progress"); return;
            }
            double percentage=0;
            if (!epubPosition(path,local,&percentage)) {
                finish("Координата распознана, но не удалось оценить процент книги для отправки. Позиции не изменены.",
                       false,"progress","position"); return;
            }
            // Save before POST. An uncertain result is reconciled by the next GET, never blindly retried.
            const QJsonObject outgoing{{"source","text"},{"cfi",local},{"pageNumber",QJsonValue::Null},{"percentage",percentage}};
            state["outgoing"]=outgoing;
            if (!persist()) return;
            jsonRequest(route,outgoing,[this,id,local,route,profile](const QJsonObject &) {
                jsonRequest(route,{},[this,id,local,profile](const QJsonObject &confirmed) {
                    if (confirmed["cfi"].toString()!=local) {
                        finish("После отправки серверная позиция изменилась. Повторите сверку.",false,"progress"); return;
                    }
                    auto record=downloads.value(id).toObject();
                    record["progress"]=QJsonObject{{"localBase",local},{"remoteBase",confirmed},{"profile",profile}};
                    if (!saveRecord(id,record)) { finish("Отправлено, но не удалось сохранить результат. Повторите сверку.",false,"progress"); return; }
                    dismissConflict(); finish("Прогресс отправлен в BookOrbit",true,"progress");
                },true,true);
            });
        },true,true);
}
