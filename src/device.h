#pragma once
#include <QString>
#include <QJsonObject>
#include <QSize>
#include <QMap>
#include <QStringList>
#include "file_work.h"
#include <functional>

struct ReaderRecent { qint64 bookId=0, openTime=0; };
struct ReaderRecents { QString profile; bool available=false; QMap<QString,ReaderRecent> files; };
ReaderRecents readerRecents(const QStringList &paths);
std::function<ReaderRecents(const FileCancellation &)> readerRecentsTask(const QStringList &paths,const QString &profile);
ReaderRecents readReaderRecents(const QString &database,const QStringList &paths,const QString &profile,
                              const FileCancellation &cancel={},int *batches=nullptr);

QSize setupDevice();
QString deviceFont();
QString deviceLanguage();
bool openReader(const QString &path);
void scanBook(const QString &path);
bool readerBookIndexed(const QString &path);
enum class ReaderFileState { Closed, Open, Unknown };
ReaderFileState readerFileState(const QString &path);

bool connectNetwork();
bool networkConnected();
QString updateDeviceError(QJsonObject *details=nullptr);
bool readerPosition(const QString &path, QString *position);
QString readerProfile();
bool saveReaderPosition(const QString &path, const QString &expectedPosition,
                        const PreparedReaderPosition &prepared, const QString &profile, QString *error);
