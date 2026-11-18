#pragma once
#include "file_executor.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <atomic>
#include <functional>
#include <memory>

// Account-scoped thumbnail cache: cover-<bookId>.png files plus cover-index.json versions.
class CoverCache : public QObject {
    Q_OBJECT
public:
    using Request = std::function<QNetworkRequest(const QString &)>;
    CoverCache(QNetworkAccessManager &network, FileExecutor &files, Request request);
    void setScope(const QString &directory);
    QString url(int bookId) const;
    int revision() const { return version; }
    void remove(int bookId);
    void cleanup(const QSet<int> &visible, const QSet<int> &downloaded);
    void fetch(const QJsonArray &items, bool allowNetworkPrompt = true);
    void stop();
#ifdef BOOKORBIT_TEST_HTTP
    std::shared_ptr<std::atomic<quint64>> decodes=std::make_shared<std::atomic<quint64>>(0);
#endif
signals:
    void changed();
    void drained();
    void networkUnavailable();
private:
    friend struct ClientWorkerCheck;
    void fetchNext();
    void saveIndex();
    QString path(int bookId) const;
    QNetworkAccessManager &network;
    FileExecutor &files;
    Request request;
    QString scope;
    QJsonObject versions, revisions;
    QList<std::pair<int,QString>> queue;
    QPointer<QNetworkReply> active;
    FileCancellation task;
    int version=0, generation=0;
    bool indexDirty=false, allowNetworkPrompt=true;
};
