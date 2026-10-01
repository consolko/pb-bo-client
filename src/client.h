#pragma once
#include <QObject>
#include "device.h"
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
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY changed)
    Q_PROPERTY(bool detailVisible READ detailVisible NOTIFY changed)
    Q_PROPERTY(QVariantList collections READ collections NOTIFY changed)
    Q_PROPERTY(bool collectionsView READ collectionsView NOTIFY changed)
    Q_PROPERTY(int collectionId READ collectionId NOTIFY changed)
    Q_PROPERTY(QString collectionName READ collectionName NOTIFY changed)
    Q_PROPERTY(bool progressConflict READ progressConflict NOTIFY changed)
    Q_PROPERTY(QString conflictDescription READ conflictDescription NOTIFY changed)
    Q_PROPERTY(QVariantList conflictPositions READ conflictPositions NOTIFY changed)
    Q_PROPERTY(int conflictRevision READ conflictRevision NOTIFY changed)
    Q_PROPERTY(QVariantMap syncSummary READ syncSummary NOTIFY changed)
    Q_PROPERTY(QVariantMap feedback READ feedback NOTIFY changed)
    Q_PROPERTY(QVariantMap syncBatch READ syncBatch NOTIFY changed)
    Q_PROPERTY(QString localQuery READ localQuery NOTIFY changed)
    Q_PROPERTY(QString localSort READ localSort NOTIFY changed)
    Q_PROPERTY(QString catalogQuery READ catalogQuery NOTIFY changed)
    Q_PROPERTY(QVariantMap recentBook READ recentBook NOTIFY changed)
    Q_PROPERTY(bool historyAvailable READ historyAvailable NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int coverRevision READ coverRevision NOTIFY coversChanged)
    Q_PROPERTY(QString server READ server NOTIFY changed)
    Q_PROPERTY(QString username READ username NOTIFY changed)
    Q_PROPERTY(QStringList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(bool offlineOnly READ offlineOnly NOTIFY changed)
    Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
    Q_PROPERTY(bool verifyingLibrary READ verifyingLibrary NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory NOTIFY changed)
    Q_PROPERTY(bool diagnosticLogging READ diagnosticLogging NOTIFY changed)
    Q_PROPERTY(QString diagnosticLogPath READ diagnosticLogPath CONSTANT)
    Q_PROPERTY(bool hasSavedSession READ hasSavedSession NOTIFY changed)
    Q_PROPERTY(QString sessionWarning READ sessionWarning NOTIFY changed)
public:
    Client(QUrl server, QString root, QObject *parent = nullptr, bool restoreAccount = true);
    QVariantList books() const;
    QVariantMap detail() const;
    bool detailVisible() const { return !detailBook.isEmpty(); }
    QVariantList collections() const { return collectionItems.toVariantList(); }
    bool collectionsView() const { return collectionList; }
    int collectionId() const { return activeCollection; }
    QString collectionName() const { return activeCollectionName; }
    Q_INVOKABLE void showCollections();
    Q_INVOKABLE void openCollection(int id, const QString &name, double catalogOffset = 0, double detailOffset = 0);
    Q_INVOKABLE QVariantMap backFromCollection();
    Q_INVOKABLE void showDetail(int bookId);
    Q_INVOKABLE void refreshDetail();
    Q_INVOKABLE void closeDetail();
    Q_INVOKABLE void selectFile(int fileId);
    Q_INVOKABLE void downloadSelected();
    Q_INVOKABLE void openSelected(bool applyIncoming = true);
    Q_INVOKABLE void syncSelected();
    bool progressConflict() const { return !syncingAll && !conflictId.isEmpty(); }
    QString conflictDescription() const { return conflictMessage; }
    QVariantList conflictPositions() const { return positionChoices; }
    int conflictRevision() const { return positionRevision; }
    QVariantList syncBooks() const;
    QVariantMap syncSummary() const;
    Q_INVOKABLE void syncFile(int fileId);
    Q_INVOKABLE void inspectConflict(int fileId);
    Q_INVOKABLE void showSyncFile(int fileId);
    Q_INVOKABLE void connectForSync();
    Q_INVOKABLE void syncAll();
    Q_INVOKABLE void verifyLibrary();
    Q_INVOKABLE void cancelLibraryVerification();
    bool verifyingLibrary() const { return checkingLibrary; }
    Q_INVOKABLE void resolveProgress(bool useLocal);
    Q_INVOKABLE void dismissConflict();
    QVariantMap feedback() const;
    Q_INVOKABLE void setUiContext(const QString &context);
    QVariantMap syncBatch() const;
    Q_INVOKABLE void stopSyncAfterCurrent();
    QString localQuery() const { return libraryQuery; }
    QString localSort() const { return librarySort; }
    QString catalogQuery() const { return committedQuery; }
    Q_INVOKABLE void searchDownloaded(const QString &query);
    Q_INVOKABLE void setLocalSort(const QString &sort);
    Q_INVOKABLE void refreshRecents();
    QVariantMap recentBook() const;
    bool historyAvailable() const { return nativeRecents.available; }
    Q_INVOKABLE void openFile(int fileId, bool applyIncoming=false);
    QString status() const { return message; }
    bool busy() const { return working; }
    bool authenticated() const { return !token.isEmpty(); }
    int page() const { return currentPage; }
    int total() const { return count; }
    int coverRevision() const { return coverVersion; }
    Q_INVOKABLE QString coverUrl(int bookId) const;
    QString server() const { return endpoint.toString(); }
    QString username() const { return user; }
    QStringList accounts() const;
    bool offlineOnly() const { return localView; }
    bool downloading() const { return !activeDownload.isNull(); }
    bool canRetry() const { return !working && authenticated() && retryKind != 0; }
    QString downloadDirectory() const;
    bool diagnosticLogging() const { return diagnostics; }
    QString diagnosticLogPath() const;
    bool hasSavedSession() const { return sessionStored; }
    QString sessionWarning() const { return sessionNotice; }
    Q_INVOKABLE QVariantList directories(const QString &path) const;
    Q_INVOKABLE bool setDownloadDirectory(const QString &path);
    Q_INVOKABLE bool setDiagnosticLogging(bool enabled);
    Q_INVOKABLE bool configure(const QString &server, const QString &username);
    Q_INVOKABLE void selectAccount(int index);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void showDownloaded(bool value);
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void login(const QString &server, const QString &username, const QString &password);
    Q_INVOKABLE void restoreSession();
    Q_INVOKABLE void refresh(int page = 0, const QString &query = {});
    Q_INVOKABLE void download(int index);
    Q_INVOKABLE void open(int index, bool applyIncoming = true);
    QString localFile(int index) const;
signals:
    void changed();
    void coversChanged();
    void completed(const QString &operation, bool success);
private:
    QVariantMap syncFileStatus(const QString &id, const QString &profile) const;
    void prepareConflict(const QString &id, const QString &local, const QJsonObject &remote, const QString &profile);
    QVariantList positionChoices;
    int positionRevision = 0;
    void syncBook(const QString &id, int choice = 0);
    void verifyRemoteFile(const QString &id, bool renew = true);
    void verifyBook(const QString &id);
    QString conflictId, conflictLocal, conflictMessage, conflictProfile;
    QString syncingId;
    QStringList syncQueue;
    QJsonObject syncResults;
    void beginFeedback(const QString &context, int bookId=0, int fileId=0);
    QString feedbackContext="catalog", feedbackResult="success", uiContext;
    int feedbackBookId=0, feedbackFileId=0;
    bool feedbackHidden=false;
    QString libraryQuery, librarySort="recent", committedQuery;
    ReaderRecents nativeRecents;
    bool recentsRequested=false, recentsScheduled=false;
    QJsonObject recentFile(QJsonObject book) const;
    bool syncingAll=false, syncStopRequested=false;
    int syncCompleted=0;
    bool checkingLibrary=false, verificationCancelled=false;
    int syncCount=0, syncSucceeded=0;
    int verificationDifferent=0, verificationErrors=0;
    QJsonObject conflictRemote;
    bool ensureNetwork();
    using Callback = std::function<void(const QJsonObject &)>;
    QNetworkRequest request(const QString &path) const;
    void jsonRequest(const QString &path, const QJsonObject &payload, const Callback &callback, bool renew = true, bool get = false, bool arrayResponse = false);
    bool setCredentials(const QJsonObject &response);
    void renewSession(const std::function<void()> &resume);
    void finish(QString text, bool success, const QString &operation, const QString &syncState = {});
    bool saveRecord(const QString &id, const QJsonObject &record);
    QString recordFile(const QString &id, const QJsonObject &record) const;
    void loadRecords();
    void cleanupBooks(const QString &verifiedFile = {});
    void cleanupCovers();
    void removeCover(int bookId);
    void queueCovers();
    void fetchNextCover();
    void stopCovers();
    QJsonArray visibleItems(bool applyQuery=true) const;
    QString localFile(const QJsonObject &book) const;
    void downloadBook(const QJsonObject &book, bool renew = true);
    void transferBook(const QJsonObject &book, bool renew);
    void openBook(const QJsonObject &book, bool applyIncoming);
    QJsonObject selectBookFile(QJsonObject book) const;
    QVariantMap bookSummary(const QJsonObject &book) const;
    QVariantMap fileSummary(const QJsonObject &book) const;
    void rememberFile(const QJsonObject &book);
    QString pathFor(const QJsonObject &book) const;
    bool validDownloadDirectory(const QString &path) const;
    bool saveSession();
    void invalidateSession();
    void logEvent(const QString &event, int code = 0, int http = 0);
    QNetworkAccessManager network;
    QUrl endpoint;
    QString rootDir, scopeDir, bookDirectory, user, message;
    QByteArray token;
    QString refreshToken;
    QString sessionNotice;
    QJsonArray items;
    QJsonArray collectionItems;
    QList<QJsonObject> browseHistory;
    QJsonObject detailBook, fileChoices, coverVersions, coverRevisions;
    QString detailMessage, activeCollectionName;
    int activeCollection = 0, detailGeneration = 0;
    bool collectionList = false;
    QJsonObject downloads;
    QJsonArray savedAccounts;
    QPointer<QNetworkReply> activeDownload, activeCover;
    QPointer<QNetworkReply> activeVerification;
    QList<int> coverQueue;
    int coverVersion = 0, coverGeneration = 0;
    QJsonObject retryBook;
    QString retryQuery;
    int retryPage = 0, retryKind = 0;
    bool localView = false, cancelled = false, canCleanupBooks = false;
    bool working = false, diagnostics = false, sessionStored = false;
    int currentPage = 0, count = 0;
};
