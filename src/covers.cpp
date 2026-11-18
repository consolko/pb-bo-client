#include "covers.h"
#include "device.h"
#include "file_work.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImage>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <algorithm>

CoverCache::CoverCache(QNetworkAccessManager &network, FileExecutor &files, Request request)
    : network(network), files(files), request(std::move(request)) {}

void CoverCache::setScope(const QString &directory) {
    stop();
    scope=directory; versions=readObject(scope+"/cover-index.json"); indexDirty=false;
}

QString CoverCache::path(int bookId) const { return scope+"/cover-"+QString::number(bookId)+".png"; }

QString CoverCache::url(int bookId) const {
    const QString file=path(bookId);
    return bookId > 0 && QFileInfo::exists(file) ? QUrl::fromLocalFile(file).toString()+"?v="+QString::number(revisions.value(file).toInt()) : QString{};
}

void CoverCache::saveIndex() {
    if (indexDirty && writeObject(scope+"/cover-index.json", versions)) indexDirty=false;
}

void CoverCache::remove(int bookId) {
    const QString key=QString::number(bookId);
    if (versions.contains(key)) { versions.remove(key); indexDirty=true; }
    if (QFile::remove(path(bookId))) {
        revisions[path(bookId)]=++version;
        emit changed();
    }
}

void CoverCache::cleanup(const QSet<int> &visible, const QSet<int> &downloaded) {
    static const QRegularExpression owned("^cover-([1-9][0-9]*)\\.png$");
    QList<QFileInfo> transient;
    for (const auto &name : QDir(scope).entryList({"cover-*.png"}, QDir::Files)) {
        const auto match = owned.match(name);
        if (!match.hasMatch()) continue;
        const int id = match.captured(1).toInt();
        if (downloaded.contains(id)) continue;
        if (!versions.contains(QString::number(id)) && !visible.contains(id)) remove(id);
        else transient.append(QFileInfo(scope+"/"+name));
    }
    std::sort(transient.begin(), transient.end(), [&visible](const QFileInfo &a, const QFileInfo &b) {
        const int left=a.baseName().mid(6).toInt(), right=b.baseName().mid(6).toInt();
        if (visible.contains(left) != visible.contains(right)) return !visible.contains(left);
        return a.lastModified() < b.lastModified();
    });
    for (int i=0; i<transient.size()-64; ++i) remove(transient[i].baseName().mid(6).toInt());
    saveIndex();
}

void CoverCache::stop() {
    if (task) task->store(true);
    task.reset();
    ++generation;
    queue.clear();
    if (active) active->abort();
    active.clear();
    saveIndex();
}

void CoverCache::fetch(const QJsonArray &items, bool allowPrompt) {
    stop();
    allowNetworkPrompt=allowPrompt;
    for (const auto value : items) {
        const auto book = value.toObject();
        if (!positiveId(book["id"])) continue;
        const int id=book["id"].toInt();
        if (book["hasCover"] == false) { remove(id); continue; }
        const QString current=book["coverVersion"].toString(), key=QString::number(id);
        if (!current.isEmpty() && versions[key].toString()==current && QFile::exists(path(id))) continue;
        if (!current.isEmpty() && versions.contains(key) && versions[key].toString()!=current) remove(id);
        queue.append({id,current});
    }
    fetchNext();
}

void CoverCache::fetchNext() {
    if (queue.isEmpty()) { saveIndex(); emit drained(); return; }
    if (!(allowNetworkPrompt ? connectNetwork() : networkConnected())) { stop(); emit networkUnavailable(); return; }
    const auto [id,coverVersion] = queue.takeFirst();
    const int run = generation;
    const QString file = path(id);
    auto reply = network.get(request("/api/v1/books/"+QString::number(id)+"/thumbnail"));
    active = reply;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->readAll());
        if (bytes->size() > maxJson) reply->abort();
    });
    QTimer::singleShot(15000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, file, id, coverVersion, run] {
        reply->deleteLater();
        if (run != generation) return;
        active.clear();
        bytes->append(reply->readAll());
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) remove(id);
        if (reply->error() == QNetworkReply::NoError && bytes->size() <= maxJson &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            task=files.submit(this,[data=*bytes
#ifdef BOOKORBIT_TEST_HTTP
                ,counter=decodes
#endif
            ](const FileCancellation &cancel) {
                if (fileCancelled(cancel)) return QByteArray{};
                QByteArray source=data,encoded;
                QBuffer buffer(&source); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
                const auto format=reader.format(); const auto size=reader.size();
                if (!fileCancelled(cancel) && (format=="png" || format=="jpeg" || format=="webp") &&
                    size.width()>0 && size.height()>0 && size.width()<=2048 && size.height()<=2048) {
                    reader.setScaledSize(size.scaled(240,320,Qt::KeepAspectRatio));
                    if (fileCancelled(cancel)) return QByteArray{};
#ifdef BOOKORBIT_TEST_HTTP
                    ++*counter;
#endif
                    const auto cover=reader.read();
                    if (fileCancelled(cancel) || cover.isNull()) return QByteArray{};
                    QBuffer output(&encoded); output.open(QIODevice::WriteOnly);
                    if (!cover.save(&output,"PNG")) encoded.clear();
                }
                return encoded;
            },[this,file,id,coverVersion,run](const QByteArray &encoded,bool cancelled) {
                // Account changes and sign-out always call stop(), so the generation also guards the account.
                if (run!=generation) return;
                task.reset();
                QSaveFile output(file);
                if (!cancelled && !encoded.isEmpty() && output.open(QIODevice::WriteOnly) && output.write(encoded)==encoded.size() && output.commit()) {
                    if (coverVersion.isEmpty()) versions.remove(QString::number(id)); else versions[QString::number(id)]=coverVersion;
                    indexDirty=true;
                    revisions[file]=++version; emit changed();
                }
                fetchNext();
            });
            return;
        }
        fetchNext();
    });
}
