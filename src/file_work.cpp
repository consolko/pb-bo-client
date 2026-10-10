#include "file_work.h"
#include "progress.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <sys/stat.h>

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
PreparedReaderPosition prepareReaderPosition(const QString &path,const QString &cfi,const FileCancellation &cancel) {
    PreparedReaderPosition result; result.cfi=cfi; result.file=fileStamp(path);
    if (fileCancelled(cancel)) return result;
    result.coordinate=bookPosition(path,cfi,nullptr,nullptr,nullptr,&result.native);
    if (result.coordinate && !fileCancelled(cancel)) result.estimate=bookPosition(path,cfi,&result.percentage);
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
