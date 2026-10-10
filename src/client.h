#pragma once
#include <QObject>
#include <QElapsedTimer>
#include "i18n.h"
#include "device.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QPointer>
#include <QNetworkReply>
#include <QVariantList>
#include <QHash>
#include <functional>

class UpdateManager;
class Client : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList languageCodes READ languageCodes CONSTANT)
    Q_PROPERTY(QStringList languageNames READ languageNames CONSTANT)
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QVariantList books READ books NOTIFY libraryChanged)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)
    Q_PROPERTY(bool detailVisible READ detailVisible NOTIFY detailChanged)
    Q_PROPERTY(QVariantList collections READ collections NOTIFY catalogChanged)
    Q_PROPERTY(bool collectionsView READ collectionsView NOTIFY catalogChanged)
    Q_PROPERTY(int collectionId READ collectionId NOTIFY catalogChanged)
    Q_PROPERTY(QString collectionName READ collectionName NOTIFY catalogChanged)
    Q_PROPERTY(bool progressConflict READ progressConflict NOTIFY conflictChanged)
    Q_PROPERTY(QString conflictDescription READ conflictDescription NOTIFY conflictChanged)
    Q_PROPERTY(QVariantList conflictPositions READ conflictPositions NOTIFY conflictChanged)
    Q_PROPERTY(int conflictRevision READ conflictRevision NOTIFY conflictChanged)
    Q_PROPERTY(QVariantMap syncSummary READ syncSummary NOTIFY syncSummaryChanged)
    Q_PROPERTY(QVariantMap feedback READ feedback NOTIFY feedbackChanged)
    Q_PROPERTY(QVariantMap syncBatch READ syncBatch NOTIFY syncBatchChanged)
    Q_PROPERTY(QString localQuery READ localQuery NOTIFY libraryChanged)
    Q_PROPERTY(QString localSort READ localSort NOTIFY libraryChanged)
    Q_PROPERTY(QString catalogQuery READ catalogQuery NOTIFY catalogChanged)
    Q_PROPERTY(QVariantMap recentBook READ recentBook NOTIFY libraryChanged)
    Q_PROPERTY(bool historyAvailable READ historyAvailable NOTIFY libraryChanged)
    Q_PROPERTY(QString status READ status NOTIFY feedbackChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY operationChanged)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY connectionChanged)
    Q_PROPERTY(int page READ page NOTIFY catalogChanged)
    Q_PROPERTY(int total READ total NOTIFY catalogChanged)
    Q_PROPERTY(int coverRevision READ coverRevision NOTIFY coversChanged)
    Q_PROPERTY(QString server READ server NOTIFY connectionChanged)
    Q_PROPERTY(QString username READ username NOTIFY connectionChanged)
    Q_PROPERTY(QStringList accounts READ accounts NOTIFY connectionChanged)
    Q_PROPERTY(bool offlineOnly READ offlineOnly NOTIFY catalogChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY operationChanged)
    Q_PROPERTY(bool verifyingLibrary READ verifyingLibrary NOTIFY operationChanged)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY operationChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory NOTIFY settingsChanged)
    Q_PROPERTY(QString diagnosticError READ diagnosticError NOTIFY settingsChanged)
    Q_PROPERTY(bool diagnosticLogging READ diagnosticLogging NOTIFY settingsChanged)
    Q_PROPERTY(QString diagnosticLogPath READ diagnosticLogPath CONSTANT)
    Q_PROPERTY(bool hasSavedSession READ hasSavedSession NOTIFY connectionChanged)
    Q_PROPERTY(QString sessionWarning READ sessionWarning NOTIFY connectionChanged)
public:
    Client(QUrl server, QString root, QObject *parent = nullptr, bool restoreAccount = true);
    bool prepareUpdate();
    void cancelUpdate();
    QStringList languageCodes() const { return interfaceLanguages().keys(); }
    QStringList languageNames() const { return interfaceLanguages().values(); }
    QString language() const { return languagePreference; }
    void retranslate();
    Q_INVOKABLE bool setLanguage(const QString &language);
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
    QString status() const { return message.text(); }
    bool busy() const { return working || conflictPreparing; }
    bool authenticated() const { return !token.isEmpty(); }
    int page() const { return currentPage; }
    int total() const { return count; }
    int coverRevision() const { return coverVersion; }
    Q_INVOKABLE QString coverUrl(int bookId) const;
    QString server() const { return endpoint.toString(); }
    QString username() const { return user; }
    QStringList accounts() const;
    bool offlineOnly() const { return localView; }
    bool downloading() const { return fileDownloading || !activeDownload.isNull(); }
    bool canRetry() const { return !busy() && authenticated() && retryKind != 0; }
    QString downloadDirectory() const;
    bool diagnosticLogging() const { return diagnostics; }
    QString diagnosticLogPath() const;
    QString diagnosticError() const { return logError.text(); }
    void logDiagnostic(const QString &event, QJsonObject fields = {});
    bool hasSavedSession() const { return sessionStored; }
    QString sessionWarning() const { return sessionNotice.text(); }
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
#ifdef BOOKORBIT_TEST_HTTP
    struct PresentationWork {
        quint64 libraryBuilds=0, librarySorts=0, localFileChecks=0, syncBuilds=0, syncSorts=0;
        bool operator==(const PresentationWork &) const = default;
    };
    PresentationWork presentationWork() const { return presentationCounters; }
    void resetPresentationWork() const { presentationCounters={}; }
#endif
signals:
    void diagnosticsEnabled();
    void languageChanged();
    void operationChanged();
    void feedbackChanged();
    void libraryChanged();
    void catalogChanged();
    void detailChanged();
    void conflictChanged();
    void syncSummaryChanged();
    void syncBatchChanged();
    void connectionChanged();
    void settingsChanged();
    void coversChanged();
    void completed(const QString &operation, bool success);
private:
    friend class UpdateManager;
    friend struct ClientWorkerCheck;
    FileExecutor files;
    FileCancellation fileTask,recentsTask,conflictTask,cleanupTask;
    quint64 fileGeneration=0,accountGeneration=0,recentsGeneration=0,conflictGeneration=0,cleanupGeneration=0;
    bool fileDownloading=false,conflictPreparing=false,cleanupPending=false;
    enum class CleanupState { Idle, Running, Completed, Cancelled };
    CleanupState cleanupState=CleanupState::Idle;
    void cancelCleanup();
    struct FileContext { quint64 generation,account; QString id,path,profile; QJsonObject record; };
    struct CleanupProof { FileStamp stamp; bool valid=false; };
    QMap<QString,CleanupProof> cleanupProofs;
    FileContext fileContext(const QString &id,const QString &path) const;
    bool acceptFileContext(const FileContext &context,const FileStamp &stamp={});
    void startFileOperation();
    using LocalFiles = QHash<int, QString>;
    struct LibraryRow { QJsonObject book; QVariantMap summary; QString search; };
    mutable QList<LibraryRow> libraryRows;
    mutable QJsonArray libraryAll, libraryVisible;
    mutable QVariantList libraryBooks;
    mutable QVariantMap libraryRecent, cachedSyncSummary;
    mutable bool libraryDirty=true, libraryOrderDirty=true, libraryFilterDirty=true, syncDirty=true;
    bool libraryPending=false, syncPending=false, detailPending=false;
    QVariantList lastOperation;
    void invalidateLibrary(bool data=true);
    void invalidateSync();
    void publishDataChanges();
    void notifyOperation();
    void ensureLibrary() const;
    QVariantList buildSyncBooks() const;
    void storeRecord(const QString &id, const QJsonObject &record);
    void setSyncResult(const QString &id, const UiMessage &text);
    void clearSyncResult(const QString &id);
#ifdef BOOKORBIT_TEST_HTTP
    mutable PresentationWork presentationCounters;
#endif
    bool updateLocked=false;
    UiMessage logError;
    QElapsedTimer diagnosticClock;
    qint64 diagnosticSequence=0;
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
    QJsonObject recentFile(QJsonObject book, LocalFiles *files=nullptr) const;
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
    void finish(UiMessage text, bool success, const QString &operation, const QString &syncState = {});
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
    QString localFile(const QJsonObject &book, LocalFiles *files=nullptr) const;
    void downloadBook(const QJsonObject &book, bool renew = true);
    void transferBook(const QJsonObject &book, bool renew);
    void openBook(const QJsonObject &book, bool applyIncoming);
    QJsonObject selectBookFile(QJsonObject book, LocalFiles *files=nullptr) const;
    QVariantMap bookSummary(const QJsonObject &book, LocalFiles *files=nullptr) const;
    QVariantMap fileSummary(const QJsonObject &book, LocalFiles *files=nullptr) const;
    void rememberFile(const QJsonObject &book);
    QString pathFor(const QJsonObject &book) const;
    bool validDownloadDirectory(const QString &path) const;
    bool saveSession();
    void invalidateSession();
    void logEvent(const QString &event, int code = 0, int http = 0);
    QNetworkAccessManager network;
    QUrl endpoint;
    QString languagePreference = "system";
    QString rootDir, scopeDir, bookDirectory, user;
    UiMessage message;
    QByteArray token;
    QString refreshToken;
    UiMessage sessionNotice;
    QJsonArray items;
    QJsonArray collectionItems;
    QList<QJsonObject> browseHistory;
    QJsonObject detailBook, fileChoices, coverVersions, coverRevisions;
    UiMessage detailMessage;
    QString activeCollectionName;
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
