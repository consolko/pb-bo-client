#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QPointer>
#include <QNetworkReply>
#include <QVariantList>
#include <functional>

class Client : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList books READ books NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int coverRevision READ coverRevision NOTIFY coversChanged)
    Q_PROPERTY(QString server READ server NOTIFY changed)
    Q_PROPERTY(QString username READ username NOTIFY changed)
    Q_PROPERTY(bool demo READ demo NOTIFY changed)
    Q_PROPERTY(QStringList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(bool offlineOnly READ offlineOnly NOTIFY changed)
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
public:
    Client(QUrl server, QString root, bool demo, QObject *parent = nullptr, bool restoreAccount = true);
    QVariantList books() const;
    QString status() const { return message; }
    bool busy() const { return working; }
    bool authenticated() const { return !token.isEmpty(); }
    int page() const { return currentPage; }
    int total() const { return count; }
    int coverRevision() const { return coverVersion; }
    Q_INVOKABLE QString coverUrl(int bookId) const;
    QString server() const { return endpoint.toString(); }
    QString username() const { return user; }
    bool demo() const { return development; }
    QStringList accounts() const;
    bool offlineOnly() const { return localView; }
    bool downloading() const { return !activeDownload.isNull(); }
    bool canRetry() const { return !working && authenticated() && retryKind != 0; }
    Q_INVOKABLE bool configure(const QString &server, const QString &username, bool demo);
    Q_INVOKABLE void selectAccount(int index);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void showDownloaded(bool value);
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void login(const QString &username, const QString &password);
    Q_INVOKABLE void refresh(int page = 0, const QString &query = {});
    Q_INVOKABLE void download(int index);
    Q_INVOKABLE void open(int index);
    QString localFile(int index) const;
signals:
    void changed();
    void coversChanged();
    void completed(const QString &operation, bool success);
private:
    using Callback = std::function<void(const QJsonObject &)>;
    void jsonRequest(const QString &path, const QJsonObject &payload, const Callback &callback);
    void finish(const QString &text, bool success, const QString &operation);
    bool save(const QString &name, const QJsonObject &object);
    bool saveRecord(const QString &id, const QJsonObject &record);
    QString recordFile(const QString &id, const QJsonObject &record) const;
    void loadCache();
    void cleanupBooks();
    void cleanupCovers();
    void removeCover(int bookId);
    void queueCovers();
    void fetchNextCover();
    void stopCovers();
    QJsonArray visibleItems() const;
    QString localFile(const QJsonObject &book) const;
    void downloadBook(const QJsonObject &book);
    QString pathFor(const QJsonObject &book) const;
    QNetworkAccessManager network;
    QUrl endpoint;
    QString rootDir, scopeDir, user = "demo", message;
    QByteArray token;
    QJsonArray items;
    QJsonObject downloads;
    QJsonArray savedAccounts;
    QPointer<QNetworkReply> activeDownload, activeCover;
    QList<int> coverQueue;
    int coverVersion = 0, coverGeneration = 0;
    QJsonObject retryBook;
    QString retryQuery;
    int retryPage = 0, retryKind = 0;
    bool localView = false, cancelled = false;
    bool working = false, development = false;
    int currentPage = 0, count = 0;
};
