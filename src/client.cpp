#include "client.h"
#include "device.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTimer>
#include <QBuffer>
#include <QImageReader>
#include <QImage>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
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
bool writeObject(const QString &path, const QJsonObject &object) {
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson();
    return bytes.size() <= maxJson && file.open(QIODevice::WriteOnly) &&
           file.write(bytes) == bytes.size() && file.commit();
}
bool validServer(const QUrl &url, bool demo) {
    const bool local = url.scheme() == "http" &&
        (url.host() == "localhost" || url.host() == "127.0.0.1" || url.host() == "host.containers.internal") && url.path().isEmpty();
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
        !url.hasQuery() && !url.hasFragment() && (url.scheme() == "https" || (demo && local));
}
bool positiveId(const QJsonValue &value) {
    return value.isDouble() && value.toDouble() > 0 &&
           value.toDouble() <= 2147483647 && value.toDouble() == value.toInt();
}
QJsonObject smallBook(const QJsonObject &book) {
    return {{"id", book["id"]}, {"title", book["title"]}, {"authors", book["authors"]},
            {"selectedFile", book["selectedFile"]}, {"hasCover", book["hasCover"]},
            {"coverVersion", book["coverVersion"]}};
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
}

Client::Client(QUrl server, QString root, bool demo, QObject *parent, bool restoreAccount)
    : QObject(parent), endpoint(server), rootDir(root), development(demo) {
    network.setTransferTimeout(15000);
    QDir().mkpath(rootDir);
    const auto settings = readObject(rootDir+"/accounts.json");
    for (const auto value : settings["accounts"].toArray()) {
        const auto account = value.toObject();
        if (validServer(QUrl(account["server"].toString()), account["demo"].toBool()) &&
            !account["username"].toString().isEmpty()) savedAccounts.append(account);
    }
    const int selected = settings["selected"].toInt(-1);
    if (restoreAccount && selected >= 0 && selected < savedAccounts.size()) {
        const auto account = savedAccounts[selected].toObject();
        endpoint = QUrl(account["server"].toString());
        user = account["username"].toString();
        development = account["demo"].toBool();
    }
    loadCache();
    localView = !downloads.isEmpty();
    message = items.isEmpty() ? "Войдите, чтобы загрузить каталог" : "Сохранённый каталог · книги доступны без сети";
}

void Client::loadCache() {
    const auto key = QCryptographicHash::hash((endpoint.toString()+"\n"+user).toUtf8(), QCryptographicHash::Sha256).toHex();
    scopeDir = rootDir + "/" + QString::fromLatin1(key.left(24));
    QDir().mkpath(scopeDir);
    const auto catalog = readObject(scopeDir+"/catalog.json");
    items = catalog["items"].toArray();
    count = catalog["total"].toInt();
    currentPage = catalog["page"].toInt();
    downloads = {};
    bool recordsReadable = true;
    const QString recordsDir = scopeDir+"/records";
    QDir().mkpath(recordsDir);
    for (const auto &name : QDir(recordsDir).entryList({"*.json"}, QDir::Files)) {
        const QString id = name.left(name.size()-5);
        if (!validId(id)) continue;
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
    for (auto it=old.begin(); it!=old.end(); ++it) {
        const QString id = it.key();
        if (!validId(id) || downloads.contains(id)) continue;
        auto record = it.value().toObject();
        if (record["book"].toObject().isEmpty()) {
            for (auto value : items) {
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
        QFile::rename(oldPath, scopeDir+"/downloads-v1.json");
    if (migrated && recordsReadable) {
        cleanupBooks();
        cleanupCovers();
    }
}

void Client::cleanupBooks() {
    static const QRegularExpression owned("^([1-9][0-9]*)(?:-[0-9a-f-]{36})?\\.(?:epub|part)$");
    const QDir dir(scopeDir);
    for (const auto &name : dir.entryList({"*.epub", "*.part"}, QDir::Files)) {
        const auto match = owned.match(name);
        if (!match.hasMatch()) continue;
        const QString id = match.captured(1);
        const auto record = downloads.value(id).toObject();
        if (recordFile(id, record) == dir.filePath(name)) continue;
        // A legacy file is only ours to remove when its migrated record points elsewhere.
        if (name.endsWith(".epub") && record.isEmpty()) continue;
        const QString path = dir.filePath(name);
        if (readerFileState(path) == ReaderFileState::Closed) QFile::remove(path);
    }
}

void Client::removeCover(int bookId) {
    if (QFile::remove(scopeDir+"/cover-"+QString::number(bookId)+".png")) {
        ++coverVersion;
        emit coversChanged();
    }
}

void Client::cleanupCovers() {
    QSet<int> keep;
    for (auto value : items) {
        const auto book = value.toObject();
        if (positiveId(book["id"]) && book["hasCover"] != false) keep.insert(book["id"].toInt());
    }
    for (auto it=downloads.begin(); it!=downloads.end(); ++it) {
        const auto book = it.value().toObject()["book"].toObject();
        if (positiveId(book["id"]) && book["hasCover"] != false) keep.insert(book["id"].toInt());
    }
    static const QRegularExpression owned("^cover-([1-9][0-9]*)\\.png$");
    for (const auto &name : QDir(scopeDir).entryList({"cover-*.png"}, QDir::Files)) {
        const auto match = owned.match(name);
        if (match.hasMatch() && !keep.contains(match.captured(1).toInt()))
            removeCover(match.captured(1).toInt());
    }
}

bool Client::save(const QString &name, const QJsonObject &object) {
    return writeObject(scopeDir+"/"+name, object);
}

bool Client::saveRecord(const QString &id, const QJsonObject &record) {
    if (recordFile(id, record).isEmpty() ||
        !writeObject(scopeDir+"/records/"+id+".json", record)) return false;
    downloads[id] = record;
    return true;
}

QString Client::recordFile(const QString &id, const QJsonObject &record) const {
    static const QRegularExpression versioned("^[1-9][0-9]*-[0-9a-f-]{36}\\.epub$");
    static const QRegularExpression sha256("^[0-9a-f]{64}$");
    const QString name = record["filename"].toString(id+".epub");
    const auto bytes = record["bytes"];
    if (!validId(id) || record["fileId"].toInt() != id.toInt() ||
        !sha256.match(record["sha256"].toString()).hasMatch() ||
        !bytes.isDouble() || bytes.toDouble() < 0 || bytes.toDouble() > maxBook ||
        bytes.toDouble() != qint64(bytes.toDouble()) ||
        (name != id+".epub" && (!versioned.match(name).hasMatch() || !name.startsWith(id+"-"))) ||
        QFileInfo(name).fileName() != name) return {};
    return scopeDir+"/"+name;
}

QString Client::pathFor(const QJsonObject &book) const {
    const auto file = book["selectedFile"].toObject();
    if (!positiveId(file["id"])) return {};
    return scopeDir+"/"+QString::number(file["id"].toInt())+".epub";
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
    return !record["invalid"].toBool() && !path.isEmpty() && record["sha256"].toString().size() == 64 && info.isFile() && !info.isSymLink() &&
           info.size() == record["bytes"].toDouble() ? path : QString{};
}

QVariantList Client::books() const {
    QVariantList result;
    const auto visible = visibleItems();
    for (int i=0; i<visible.size(); ++i) {
        const auto book = visible[i].toObject();
        QStringList authors;
        for (const auto &name : book["authors"].toArray()) authors << name.toString();
        result << QVariantMap{{"bookId", book["id"].toInt()},
                              {"title", book["title"].toString("Без названия")},
                              {"author", authors.join(", ")},
                              {"downloaded", !localFile(book).isEmpty()},
                              {"supported", !pathFor(book).isEmpty()},
                              {"needsRepair", downloads.value(QString::number(book["selectedFile"].toObject()["id"].toInt())).isObject() && localFile(book).isEmpty()}};
    }
    return result;
}

void Client::finish(const QString &text, bool success, const QString &operation) {
    working = false;
    activeDownload.clear();
    if (success) retryKind = 0;
    message = text;
    emit changed();
    emit completed(operation, success);
}

void Client::jsonRequest(const QString &path, const QJsonObject &payload, const Callback &callback) {
    QUrl url(endpoint.toString()+path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer "+token);
    auto reply = network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
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
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300 || bytes->size() > maxJson) {
            if (code == 401) token.clear();
            finish(code == 401 ? "Вход не выполнен или сессия истекла. Войдите снова." :
                   "Сервер недоступен. Скачанные книги можно читать без сети.", false, path);
            return;
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(*bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            finish("Сервер вернул некорректные данные", false, path);
            return;
        }
        callback(doc.object());
    });
}

void Client::login(const QString &username, const QString &password) {
    if (working) return;
    const bool localDemo = development && username == "demo" && password == "demo";
    if (!validServer(endpoint, development) || (endpoint.scheme() == "http" && !localDemo) || username.isEmpty() || password.isEmpty()) {
        finish("Для сервера нужен HTTPS. В локальном демо используйте demo / demo.", false, "login");
        return;
    }
    if (!configure(endpoint.toString(), username, development)) return;
    retryKind = 0;
    working = true;
    message = "Подключение…";
    emit changed();
    jsonRequest("/api/v1/auth/login", {{"username", username}, {"password", password},
                                    {"clientKind", "native"}, {"deviceLabel", "PocketBook prototype"}},
        [this](const QJsonObject &response) {
            const QString value = response["accessToken"].toString();
            if (value.isEmpty() || value.size() > 8192 || value.contains('\r') || value.contains('\n')) {
                finish("В ответе сервера нет действительной сессии", false, "login");
                return;
            }
            token = value.toUtf8();
            working = false;
            refresh();
        });
}

void Client::refresh(int targetPage, const QString &query) {
    if (working) return;
    if (token.isEmpty()) { finish("Сначала войдите на сервер", false, "catalog"); return; }
    if (targetPage < 0 || query.size() > 500) return;
    retryKind = 1; retryPage = targetPage; retryQuery = query;
    working = true;
    message = "Загрузка каталога…";
    emit changed();
    jsonRequest("/api/v1/books/query", {{"sort", QJsonArray{}}, {"pagination", QJsonObject{{"page", targetPage}, {"size", pageSize}}}, {"q", query}},
        [this, targetPage](const QJsonObject &response) {
            const auto array = response["items"].toArray();
            if (!response["items"].isArray() || !response["total"].isDouble() || response["total"].toDouble() < 0 ||
                response["page"].toInt(-1) != targetPage || array.size() > pageSize) {
                finish("Некорректный каталог. Сохранённый список не изменён.", false, "catalog"); return;
            }
            QJsonArray parsed;
            for (auto value : array) {
                auto book = value.toObject();
                if (!positiveId(book["id"]) || !book["files"].isArray()) {
                    finish("Некорректная запись книги", false, "catalog"); return;
                }
                for (auto fileValue : book["files"].toArray()) {
                    const auto file = fileValue.toObject();
                    if (positiveId(file["id"]) && file["format"] == "epub" &&
                        (file["role"] == "primary" || file["role"] == "content")) {
                        book["selectedFile"] = file;
                        if (file["role"] == "primary") break;
                    }
                }
                parsed.append(book);
            }
            // The catalog caches one page; downloads retain their own metadata.
            QJsonObject cache{{"items", parsed}, {"page", targetPage}, {"total", response["total"]}};
            if (!save("catalog.json", cache)) { finish("Не удалось сохранить каталог", false, "catalog"); return; }
            stopCovers();
            for (auto value : parsed) {
                const auto book = value.toObject();
                const int bookId = book["id"].toInt();
                QString oldVersion;
                for (auto prior : items) {
                    if (prior.toObject()["id"].toInt() == bookId) {
                        oldVersion = prior.toObject()["coverVersion"].toString(); break;
                    }
                }
                const QString fileId = QString::number(book["selectedFile"].toObject()["id"].toInt());
                auto record = downloads.value(fileId).toObject();
                if (!record.isEmpty() && record["bookId"].toInt() == bookId) {
                    auto stored = record["book"].toObject();
                    if (oldVersion.isEmpty()) oldVersion = stored["coverVersion"].toString();
                    stored["hasCover"] = book["hasCover"];
                    stored["coverVersion"] = book["coverVersion"];
                    record["book"] = stored;
                    saveRecord(fileId, record);
                }
                const QString newVersion = book["coverVersion"].toString();
                if (book["hasCover"] == false || (!oldVersion.isEmpty() && !newVersion.isEmpty() && oldVersion != newVersion))
                    removeCover(bookId);
            }
            items = parsed; currentPage = targetPage; count = response["total"].toInt();
            cleanupCovers();
            queueCovers();
            finish(items.isEmpty() ? "Книги не найдены" : "Каталог обновлён", true, "catalog");
        });
}

void Client::download(int index) {
    if (working) return;
    const auto visible = visibleItems();
    if (index < 0 || index >= visible.size()) return;
    if (token.isEmpty()) { finish("Для скачивания войдите на сервер", false, "download"); return; }
    downloadBook(visible[index].toObject());
}

void Client::downloadBook(const QJsonObject &book) {
    retryKind = 2; retryBook = book;
    const auto file = book["selectedFile"].toObject();
    const QString path = pathFor(book);
    const QString id = QString::number(file["id"].toInt());
    const double reportedSize = file["sizeBytes"].toDouble(-1);
    if (path.isEmpty() || (!file["sizeBytes"].isNull() &&
        (reportedSize < 0 || reportedSize > maxBook))) { finish("Файл не поддерживается или превышает 100 МБ", false, "download"); return; }
    const auto previous = downloads.value(id).toObject();
    const QString oldFile = previous.isEmpty() ? path : recordFile(id, previous);
    if (oldFile.isEmpty()) { finish("Некорректная запись скачанной книги", false, "download"); return; }
    if ((QFile::exists(oldFile) || !previous.isEmpty()) && readerFileState(oldFile) != ReaderFileState::Closed) {
        finish("Закройте книгу в штатной читалке и повторите загрузку", false, "download"); return;
    }
    const qint64 expected = file["sizeBytes"].isNull() ? 0 : static_cast<qint64>(reportedSize);
    const QString stem = id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString staging = scopeDir+"/"+stem+".part";
    const QString finalPath = scopeDir+"/"+stem+".epub";
    auto output = std::make_shared<QSaveFile>(staging);
    if (!output->open(QIODevice::WriteOnly)) { finish("Не удалось создать файл книги", false, "download"); return; }
    working = true; cancelled = false; message = "Скачивание книги…";
    QNetworkRequest request(QUrl(endpoint.toString()+"/api/v1/books/files/"+QString::number(file["id"].toInt())+"/download"));
    request.setRawHeader("Authorization", "Bearer "+token);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto reply = network.get(request);
    activeDownload = reply;
    emit changed();
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto size = std::make_shared<qint64>(0);
    auto signature = std::make_shared<QByteArray>();
    auto drain = [reply, output, hash, size, signature] {
        const auto chunk = reply->readAll();
        *size += chunk.size();
        if (signature->size() < 4) signature->append(chunk.left(4-signature->size()));
        if (*size > maxBook || output->write(chunk) != chunk.size()) { output->cancelWriting(); reply->abort(); return; }
        hash->addData(chunk);
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    QTimer::singleShot(120000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, output, hash, size, signature, expected, file, drain, book, id, oldFile, staging, finalPath, stem] {
        if (reply->bytesAvailable()) drain();
        reply->deleteLater();
        if (cancelled) {
            output->cancelWriting(); retryKind = 0;
            finish("Загрузка отменена. Прежний файл сохранён.", false, "download"); return;
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401) {
            token.clear(); output->cancelWriting();
            finish("Сессия истекла. Войдите снова для скачивания.", false, "download"); return;
        }
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
            *signature != QByteArray("PK\003\004", 4) || *size == 0 || (expected > 0 && *size != expected)) {
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
            finish("Книга уже скачана", true, "download"); return;
        }
        if (!output->commit() || !QFile::rename(staging, finalPath)) {
            QFile::remove(staging);
            finish("Не удалось сохранить книгу", false, "download"); return;
        }
        if (digestFile(finalPath) != newHash.toLatin1()) {
            QFile::remove(finalPath);
            finish("Сохранённая книга повреждена. Прежняя книга доступна.", false, "download"); return;
        }
        const QJsonObject record{{"bookId", book["id"]}, {"fileId", file["id"]}, {"filename", stem+".epub"},
                                 {"sha256", newHash}, {"bytes", double(*size)}, {"book", smallBook(book)}};
        if (!saveRecord(id, record)) {
            QFile::remove(finalPath);
            finish("Запись загрузки не сохранена. Прежняя книга доступна.", false, "download"); return;
        }
        if (oldFile != finalPath && QFile::exists(oldFile) && readerFileState(oldFile) == ReaderFileState::Closed)
            QFile::remove(oldFile);
        cleanupBooks();
        finish("Книга скачана и доступна без сети", true, "download");
    });
}

void Client::open(int index) {
    if (working) return;
    const auto path = localFile(index);
    if (path.isEmpty()) { finish("Сначала скачайте книгу", false, "open"); return; }
    const QString id = QString::number(visibleItems()[index].toObject()["selectedFile"].toObject()["id"].toInt());
    if (digestFile(path) != downloads.value(id).toObject()["sha256"].toString().toLatin1()) {
        auto record = downloads.value(id).toObject();
        record["invalid"] = true;
        if (!saveRecord(id, record)) downloads[id] = record;
        finish("Файл книги изменён. Скачайте его повторно.", false, "open"); return;
    }
    const bool ok = openReader(path);
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

bool Client::configure(const QString &address, const QString &name, bool demo) {
    if (working) return false;
    QString normalized = address.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    const QUrl url(normalized, QUrl::StrictMode);
    if (!validServer(url, demo) || name.trimmed().isEmpty() || name.size() > 200 ||
        (url.scheme() == "http" && name != "demo")) {
        finish("Укажите HTTPS-адрес и имя пользователя. HTTP доступен только локальному демо.", false, "settings");
        return false;
    }
    QJsonObject account{{"server", url.toString()}, {"username", name}, {"demo", demo}};
    auto next = savedAccounts;
    int selected = -1;
    for (int i=0; i<next.size(); ++i) {
        const auto entry = next[i].toObject();
        if (entry["server"] == account["server"] && entry["username"] == name) { selected = i; break; }
    }
    if (selected < 0) { selected = next.size(); next.append(account); }
    else next[selected] = account;
    if (!writeObject(rootDir+"/accounts.json", {{"accounts", next}, {"selected", selected}})) {
        finish("Не удалось сохранить подключение. Настройки не изменены.", false, "settings"); return false;
    }
    stopCovers();
    token.clear(); retryKind = 0; savedAccounts = next;
    endpoint = url; user = name; development = demo;
    loadCache(); localView = true;
    finish("Подключение сохранено. Скачанные книги доступны без входа.", true, "settings");
    return true;
}

void Client::selectAccount(int index) {
    if (working || index < 0 || index >= savedAccounts.size()) return;
    const auto account = savedAccounts[index].toObject();
    configure(account["server"].toString(), account["username"].toString(), account["demo"].toBool());
}

void Client::logout() {
    if (working) return;
    stopCovers();
    token.clear(); retryKind = 0; localView = true;
    finish("Вход на устройстве завершён. Скачанные книги сохранены.", true, "logout");
}

void Client::showDownloaded(bool value) {
    if (working) return;
    localView = value;
    emit changed();
}

QJsonArray Client::visibleItems() const {
    if (!localView) return items;
    QJsonArray result;
    for (auto it=downloads.begin(); it!=downloads.end(); ++it) {
        const auto record = it.value().toObject();
        auto book = record["book"].toObject();
        if (book.isEmpty()) {
            // Older records may outlive their cached page; keep them readable by file ID.
            book = QJsonObject{{"id", record["bookId"]}, {"title", "Книга · файл "+it.key()},
                              {"selectedFile", QJsonObject{{"id", record["fileId"]}, {"sizeBytes", QJsonValue::Null}}}};
        }
        if (!pathFor(book).isEmpty()) result.append(book);
    }
    return result;
}

void Client::cancelDownload() {
    if (activeDownload) { cancelled = true; activeDownload->abort(); }
}

void Client::retry() {
    if (!canRetry()) return;
    if (retryKind == 1) refresh(retryPage, retryQuery);
    else downloadBook(retryBook);
}

QString Client::coverUrl(int bookId) const {
    const QString path = scopeDir+"/cover-"+QString::number(bookId)+".png";
    return bookId > 0 && QFileInfo::exists(path) ? QUrl::fromLocalFile(path).toString()+"?v="+QString::number(coverVersion) : QString{};
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
        if (book["hasCover"] == false) removeCover(book["id"].toInt());
        else coverQueue.append(book["id"].toInt());
    }
    fetchNextCover();
}

void Client::fetchNextCover() {
    if (coverQueue.isEmpty() || token.isEmpty()) return;
    const int id = coverQueue.takeFirst(), generation = coverGeneration;
    const QString path = scopeDir+"/cover-"+QString::number(id)+".png";
    QNetworkRequest request(QUrl(endpoint.toString()+"/api/v1/books/"+QString::number(id)+"/thumbnail"));
    request.setRawHeader("Authorization", "Bearer "+token);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto reply = network.get(request);
    activeCover = reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > maxJson) reply->abort();
    });
    QTimer::singleShot(15000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, path, generation] {
        reply->deleteLater();
        if (generation != coverGeneration) return;
        activeCover.clear();
        bytes->append(reply->readAll());
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404)
            removeCover(QFileInfo(path).baseName().mid(6).toInt());
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
                    ++coverVersion; emit coversChanged();
                }
            }
        }
        fetchNextCover();
    });
}
