#include "file_work.h"
#include "progress.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <sys/stat.h>
#include <QDir>
#include <QSaveFile>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

static bool ioFailure(QString *error,const QString &operation) {
    if (error) *error=operation+": "+QString::fromLocal8Bit(std::strerror(errno));
    return false;
}
bool syncDirectory(const QString &path,QString *error) {
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink() || info.canonicalFilePath()!=info.absoluteFilePath()) {
        if (error) *error="open_directory: unsafe directory";
        return false;
    }
    const int fd=::open(QFile::encodeName(path),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return ioFailure(error,"open_directory");
    const bool ok=::fsync(fd)==0;
    if (!ok) ioFailure(error,"sync_directory");
    ::close(fd); return ok;
}
FileCommitResult commitFile(QSaveFile &file) {
    auto failed=[](const QString &detail) { return FileCommitResult{FileCommitState::FailedBeforeReplace,detail}; };
    if (!file.flush()) return failed("flush: "+file.errorString());
    if (::fsync(file.handle())!=0) return failed("sync_file: "+QString::fromLocal8Bit(std::strerror(errno)));
    const auto parent=QFileInfo(file.fileName()).absolutePath();
    const int dir=::open(QFile::encodeName(parent),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (dir<0) return failed("open_directory: "+QString::fromLocal8Bit(std::strerror(errno)));
    if (!file.commit()) { ::close(dir); return failed("commit: "+file.errorString()); }
    const bool ok=::fsync(dir)==0;
    const QString detail=ok ? QString{} : "sync_directory: "+QString::fromLocal8Bit(std::strerror(errno));
    ::close(dir);
    return {ok ? FileCommitState::Complete : FileCommitState::ReplacedUnconfirmed,detail};
}
FileCommitResult saveDurable(const QString &path,const QByteArray &data) {
    const QFileInfo info(path),parent(info.absolutePath());
    if (info.isSymLink() || !parent.isDir() || parent.canonicalFilePath()!=parent.absoluteFilePath())
        return {FileCommitState::FailedBeforeReplace,"save: unsafe path"};
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size())
        return {FileCommitState::FailedBeforeReplace,"write: "+file.errorString()};
    return commitFile(file);
}
bool confirmFile(const QString &path,QString *error) {
    const auto before=fileStamp(path);
    if (!before.valid) { if (error) *error="sync_file: unsafe file"; return false; }
    const int fd=::open(QFile::encodeName(path),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return ioFailure(error,"open_file");
    const bool ok=::fsync(fd)==0;
    if (!ok) ioFailure(error,"sync_file");
    ::close(fd);
    return ok && fileStamp(path)==before && syncDirectory(QFileInfo(path).absolutePath(),error);
}
FileCommitResult moveDurable(const QString &source,const QString &destination) {
    if (!fileStamp(source).valid || QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
        return {FileCommitState::FailedBeforeReplace,"rename: unsafe source or existing destination"};
    if (::rename(QFile::encodeName(source),QFile::encodeName(destination))!=0)
        return {FileCommitState::FailedBeforeReplace,"rename: "+QString::fromLocal8Bit(std::strerror(errno))};
    QString error;
    const bool ok=syncDirectory(QFileInfo(destination).absolutePath(),&error) && syncDirectory(QFileInfo(source).absolutePath(),&error);
    return {ok ? FileCommitState::Complete : FileCommitState::ReplacedUnconfirmed,error};
}
bool makeDurableDirectory(const QString &path,QString *error) {
    if (!QDir().mkpath(path)) { if (error) *error="create_directory: failed"; return false; }
    // Account setup may have created ancestors before this transaction started.
    struct stat device{}; if (::stat(QFile::encodeName(path),&device)!=0) return ioFailure(error,"stat_directory");
    QString directory=path;
    while (true) {
        if (!syncDirectory(directory,error)) return false;
        const auto parent=QFileInfo(directory).absolutePath(); struct stat next{};
        if (parent==directory) break;
        if (::stat(QFile::encodeName(parent),&next)!=0) return ioFailure(error,"stat_directory");
        if (next.st_dev!=device.st_dev) break;
        directory=parent;
    }
    return true;
}

FileStamp fileStamp(const QString &path) {
    const QFileInfo info(path); struct stat state{};
    if (!info.isFile() || info.isSymLink() || info.canonicalFilePath()!=info.absoluteFilePath() ||
        ::stat(QFile::encodeName(path).constData(),&state)!=0) return {};
#ifdef Q_OS_MACOS
    const auto modified=state.st_mtimespec,changed=state.st_ctimespec;
#else
    const auto modified=state.st_mtim,changed=state.st_ctim;
#endif
    return {quint64(state.st_dev),quint64(state.st_ino),qint64(state.st_size),
        qint64(modified.tv_sec)*1000000000+modified.tv_nsec,
        qint64(changed.tv_sec)*1000000000+changed.tv_nsec,true};
}
QByteArray digestFile(const QString &path,const FileCancellation &cancel) {
    const auto before=fileStamp(path);
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!before.valid || !file.open(QIODevice::ReadOnly)) return {};
    while (!file.atEnd()) {
        if (fileCancelled(cancel)) return {};
        const auto bytes=file.read(65536);
        if (bytes.isEmpty()) return {};
        hash.addData(bytes);
    }
    return !fileCancelled(cancel) && fileStamp(path)==before ? hash.result().toHex() : QByteArray{};
}
bool validContent(const QString &path,const QString &format,const FileCancellation &cancel) {
    QFile file(path);
    if (fileCancelled(cancel) || !file.open(QIODevice::ReadOnly)) return false;
    const auto head=file.peek(1024);
    if (format=="epub" || format=="kepub") return validEpub(path);
    if (format=="docx" || format=="cbz") return head.startsWith(QByteArray("PK\003\004",4));
    if (format=="pdf") return head.startsWith("%PDF-");
    if (format=="djvu") return head.startsWith("AT&TFORM");
    if (format=="fb2") {
        QXmlStreamReader xml(&file);
        if (!xml.readNextStartElement() || xml.name()!=QStringLiteral("FictionBook")) return false;
        while (!xml.atEnd() && !fileCancelled(cancel)) xml.readNext();
        return !fileCancelled(cancel) && !xml.hasError();
    }
    return !head.isEmpty();
}
FileStamp fb2Calibration(const FileCancellation &cancel) {
#ifdef POCKETBOOK_DEVICE
    const QString path=QFileInfo("/ebrmain/lib/libpbrdwrapper.so").canonicalFilePath();
    const auto before=fileStamp(path);
    return digestFile(path,cancel)=="ab547f547a9e80338434c3c072dd7b560254a3d7f5a74a9adc58c89f0900b965" &&
        fileStamp(path)==before ? before : FileStamp{};
#else
    Q_UNUSED(cancel)
    FileStamp result; result.valid=true; return result;
#endif
}
PreparedReaderPosition prepareReaderPosition(const QString &path,const QString &cfi,const FileCancellation &cancel,const std::shared_ptr<const Fb2Index> &fb) {
    PreparedReaderPosition result; result.cfi=cfi; result.file=fileStamp(path);
    if (fileCancelled(cancel)) return result;
    PositionInfo resolved;
    result.coordinate=bookPosition(path,cfi,nullptr,nullptr,nullptr,&result.native,&resolved,fb);
    if (result.coordinate && !fileCancelled(cancel)) result.estimate=positionEstimate(path,resolved,&result.percentage);
#ifdef POCKETBOOK_DEVICE
    const QString framework=QFileInfo("/ebrmain/lib/libframework2.so").canonicalFilePath();
    const auto before=fileStamp(framework);
    if (digestFile(framework,cancel)=="69de9e57ddb9d96f6d38e54150a120efd061dbe524f8d430f90ff5b39a10c5f9" &&
        fileStamp(framework)==before) result.framework=before;
#else
    result.framework.valid=true;
#endif
    if (path.endsWith(".fb2",Qt::CaseInsensitive)) result.fb2=fb2Calibration(cancel);
    if (fileCancelled(cancel) || fileStamp(path)!=result.file) result.coordinate=result.estimate=false;
    return result;
}
