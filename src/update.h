#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QJsonObject>
#include <QTimer>
#include <functional>
#include "file_executor.h"
class Client;

class UpdateManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(qint64 archiveBytes READ archiveBytes NOTIFY changed)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic WRITE setAutomatic NOTIFY changed)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY changed)
public:
    UpdateManager(Client *client, QString root, QString executable, QObject *parent=nullptr);
    ~UpdateManager() override;
    QString version() const;
    QString availableVersion() const { return candidateVersion; }
    QString notes() const { return releaseNotes; }
    QString state() const { return phase; }
    QString message() const { return notice; }
    double progress() const { return fraction; }
    qint64 archiveBytes() const { return archiveSize; }
    QString lastChecked() const;
    bool automatic() const { return settings["automatic"].toBool(true); }
    void setAutomatic(bool value);
    bool canInstall() const;
    void windowReady();
    void automaticCheck();
    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void install();
signals:
    void changed();
    void startupCommitted();
private:
    friend struct UpdateManagerCheck;
    void checkRelease(bool manual);
    void fail(const QString &error);
    bool allowed(const QUrl &url) const;
    void fetch(QUrl url,qint64 limit,std::function<void(QByteArray,int,QString)> done,int redirects=5,bool conditional=true);
    void downloadZip(QUrl url,int redirects=5);
    void verifyArchive();
    void discardAttempt();
    void recoverAttempts();
    void installPrepared(bool confirmOnly);
    bool persist();
    bool persistCache();
    QJsonObject normalizeRelease(const QJsonObject &release) const;
    QJsonObject normalizeCache(const QJsonObject &value) const;
    void receiveRelease(QByteArray data,int code,const QString &etag);
    void trace(const QString &event, QJsonObject fields={});
    void traceContext();
    Client *client;
    FileCancellation fileTask;
    QString attemptDir;
    QString root,executable,phase="idle",notice,candidateVersion,releaseNotes,zipDigest;
    QUrl apiBase,zipUrl,checksumsUrl;
    QJsonObject settings,cache,prepared;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> active;
    QTimer totalDeadline;
    qint64 archiveSize=0;
    double fraction=0;
    int generation=0;
    bool startupDone=false,automaticAttempted=false;
};
