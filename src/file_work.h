#pragma once
#include "file_executor.h"
#include <QString>
#include <QByteArray>
class QSaveFile;

enum class FileCommitState { FailedBeforeReplace, ReplacedUnconfirmed, Complete };
struct FileCommitResult { FileCommitState state; QString detail; };
FileCommitResult commitFile(QSaveFile &file);
FileCommitResult saveDurable(const QString &path,const QByteArray &data);
FileCommitResult moveDurable(const QString &source,const QString &destination);
bool syncDirectory(const QString &path,QString *error=nullptr);
bool confirmFile(const QString &path,QString *error=nullptr);
bool makeDurableDirectory(const QString &path,QString *error=nullptr);

struct FileStamp {
    quint64 device=0,inode=0;
    qint64 bytes=0,modified=0,changed=0;
    bool valid=false;
    bool operator==(const FileStamp &) const = default;
};
FileStamp fileStamp(const QString &path);
QByteArray digestFile(const QString &path,const FileCancellation &cancel={});
bool validContent(const QString &path,const QString &format,const FileCancellation &cancel={});
FileStamp fb2Calibration(const FileCancellation &cancel={});
struct PreparedReaderPosition {
    QString cfi,native;
    double percentage=-1;
    FileStamp file,framework,fb2;
    bool coordinate=false,estimate=false;
};
PreparedReaderPosition prepareReaderPosition(const QString &path,const QString &cfi,const FileCancellation &cancel={});
