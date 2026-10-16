#pragma once
#include <QObject>
#include <QElapsedTimer>
#include "i18n.h"
#include "device.h"
#include "progress.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QPointer>
#include <QNetworkReply>
#include <QVariantList>
#include <QHash>
#include <QSet>
#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <functional>

class UpdateManager;
class LibraryModel : public QAbstractListModel {
    Q_OBJECT
    struct Row { int id; QVariantMap value; };
    QList<Row> rows;
    QHash<int,int> positions;
    mutable QVariantList cachedSnapshot;
    mutable bool snapshotDirty=true;
    void indexRows(int first,int last);
public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent={}) const override { return parent.isValid() ? 0 : rows.size(); }
    QVariant data(const QModelIndex &index,int role) const override;
    QHash<int,QByteArray> roleNames() const override;
    QVariantList snapshot() const;
    Q_INVOKABLE int rowFor(int bookId) const;
    Q_INVOKABLE int idAt(int row) const { return row>=0 && row<rows.size() ? rows[row].id : 0; }
    void update(int row,const QVariantMap &value);
    void insert(int row,const QVariantMap &value);
    void remove(int row);
    void move(int from,int to);
    void replace(const QVariantList &values,bool reset);
#ifdef BOOKORBIT_TEST_HTTP
    mutable quint64 lookups=0,indexVisits=0,sequenceVisits=0,snapshots=0;
#endif
};
class SyncModel : public QAbstractListModel {
    Q_OBJECT
    QList<QVariantMap> rows;
    QHash<QString,int> positions;
public:
    using QAbstractListModel::QAbstractListModel;
    enum Role { Key=Qt::UserRole+1,Title,Author,State,Label,Group,Files,SortKey,Data };
    int rowCount(const QModelIndex &parent={}) const override { return parent.isValid() ? 0 : rows.size(); }
    QVariant data(const QModelIndex &index,int role) const override;
    QHash<int,QByteArray> roleNames() const override;
    QVariantMap get(const QString &key) const;
    void put(const QString &key,const QVariantMap &value);
    void replace(const QList<QVariantMap> &values);
};
class SyncProxy : public QSortFilterProxyModel {
    QString group="all";
    bool lessThan(const QModelIndex &a,const QModelIndex &b) const override;
public:
    explicit SyncProxy(QObject *parent=nullptr);
    QString filter() const { return group; }
    void setFilter(const QString &value);
};
class Client : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList languageCodes READ languageCodes CONSTANT)
    Q_PROPERTY(QStringList languageNames READ languageNames CONSTANT)
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QAbstractItemModel* books READ bookModel CONSTANT)
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
    Q_PROPERTY(QAbstractItemModel* syncModel READ syncModel CONSTANT)
    Q_PROPERTY(QString syncFilter READ syncFilter WRITE setSyncFilter NOTIFY syncFilterChanged)
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
    QAbstractItemModel *bookModel() const;
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
    QAbstractItemModel *syncModel() const;
    QString syncFilter() const { return syncView.filter(); }
    void setSyncFilter(const QString &value);
    Q_INVOKABLE QVariantMap syncBookDetails(const QString &key) const;
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
        quint64 recordWrites=0, metadataVisits=0, bookUpdates=0, syncUpdates=0;
        quint64 uploadPreparations=0,remotePreparations=0,conflictPreviews=0;
        quint64 modelLookups=0,modelIndexVisits=0,modelSequenceVisits=0,modelSnapshots=0;
        quint64 indexBuilds=0,indexVisits=0,recentPasses=0,recentVisits=0,syncReconciliations=0,syncSnapshots=0,coverDecodes=0;
        bool operator==(const PresentationWork &) const = default;
    };
    PresentationWork presentationWork() const {
        auto result=presentationCounters; result.coverDecodes=coverDecodeCount->load();
        result.modelLookups=libraryModel.lookups; result.modelIndexVisits=libraryModel.indexVisits;
        result.modelSequenceVisits=libraryModel.sequenceVisits; result.modelSnapshots=libraryModel.snapshots;
        return result;
    }
    void resetPresentationWork() const {
        presentationCounters={}; *coverDecodeCount=0; libraryModel.lookups=libraryModel.indexVisits=libraryModel.sequenceVisits=libraryModel.snapshots=0;
    }
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
    void syncFilterChanged();
    void libraryAboutToReset();
    void libraryReset();
    void syncBatchChanged();
    void connectionChanged();
    void settingsChanged();
    void coversChanged();
    void completed(const QString &operation, bool success);
private:
    friend class UpdateManager;
    friend struct ClientWorkerCheck;
    FileExecutor files;
    FileCancellation fileTask,recentsTask,conflictTask,cleanupTask,coverTask;
    quint64 fileGeneration=0,accountGeneration=0,recentsGeneration=0,conflictGeneration=0,cleanupGeneration=0;
    bool fileDownloading=false,conflictPreparing=false,cleanupPending=false;
    bool restoringDownload=false;
    void restoreDownloadedProgress(const QString &id, int attempts=20);
    enum class CleanupState { Idle, Running, Completed, Cancelled };
    CleanupState cleanupState=CleanupState::Idle;
    void cancelCleanup();
    struct FileContext { quint64 generation,account; QString id,path,profile; QJsonObject record; };
    struct CleanupProof { FileStamp stamp; bool valid=false; };
    QMap<QString,CleanupProof> cleanupProofs;
    QSet<QString> blockedTransfers;
    QMap<QString,std::pair<FileStamp,FileStamp>> durableRecordStamps;
    FileContext fileContext(const QString &id,const QString &path) const;
    bool acceptFileContext(const FileContext &context,const FileStamp &stamp={});
    void startFileOperation();
    using LocalFiles = QHash<int, QString>;
    struct LibraryRow { QJsonObject book; QVariantMap summary,recent; QString search; };
    mutable QList<LibraryRow> libraryRows;
    mutable LibraryModel libraryModel;
    mutable bool modelObserved=false, syncObserved=false, preservingLibraryAnchor=false;
    mutable QHash<int,int> libraryIndex;
    mutable QSet<int> changedBooks;
    mutable LocalFiles libraryFiles,syncFiles;
    mutable QVariantMap libraryRecent, cachedSyncSummary;
    mutable SyncModel syncRows;
    mutable SyncProxy syncView;
    mutable QSet<QString> changedSyncGroups;
    mutable QSet<int> changedUnsupported,unsupportedBooks;
    mutable bool libraryDirty=true, libraryOrderDirty=true, libraryFilterDirty=true, syncDirty=true;
    bool libraryPending=false, syncPending=false, detailPending=false;
    QVariantList lastOperation;
    void invalidateLibrary(bool data=true);
    void invalidateSync();
    void invalidateSyncBook(int bookId);
    QVariantMap syncGroup(const QString &key) const;
    void publishDataChanges();
    void notifyOperation();
    void ensureLibrary() const;
    void invalidateBook(int bookId,bool sync=true);
    QJsonObject localBook(int bookId) const;
    LibraryRow libraryRow(QJsonObject book) const;
    bool libraryLess(const LibraryRow &left,const LibraryRow &right) const;
    bool libraryMatches(const LibraryRow &row) const;
    void indexLibrary() const;
    void updateRecent() const;
    void storeRecord(const QString &id, const QJsonObject &record);
    void setSyncResult(const QString &id, const UiMessage &text);
    void clearSyncResult(const QString &id);
#ifdef BOOKORBIT_TEST_HTTP
    mutable PresentationWork presentationCounters;
    std::shared_ptr<std::atomic<quint64>> coverDecodeCount=std::make_shared<std::atomic<quint64>>(0);
#endif
    bool updateLocked=false;
    UiMessage logError;
    QElapsedTimer diagnosticClock;
    qint64 diagnosticSequence=0;
    QVariantMap syncFileStatus(const QString &id, const QString &profile,LocalFiles *files=nullptr) const;
    void prepareConflict(const QString &id, const QString &local, const QJsonObject &remote, const QString &profile,const QVariantList &prepared={},const FileStamp &proof={});
    QVariantList conflictPreview;
    FileStamp conflictStamp;
    QString conflictDigest;
    QVariantList positionChoices;
    int positionRevision = 0;
    void syncBook(const QString &id, int choice = 0);
    void finishProgress(const QString &id, const QJsonObject &remote, const UiMessage &text);
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
    bool recentsFull=false;
    QSet<int> recentsBooks;
    void refreshBookRecents(int bookId);
    void runRecents();
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
    QHash<QString,FileStamp> savedRecordStamps;
    QMap<int,QStringList> filesByBook;
    void indexRecord(const QString &id, const QJsonObject &before, const QJsonObject &after);
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
