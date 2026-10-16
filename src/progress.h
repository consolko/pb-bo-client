#pragma once
#include <QString>
#include <QVariantMap>
#include <memory>
struct Fb2Index;
using Fb2Snapshot = std::shared_ptr<const Fb2Index>;
Fb2Snapshot fb2Index(const QString &path);
#include "file_work.h"

// Percentage is a text-length estimate; only CFI is used to restore a position.
// Validated ranges use their start when a reader needs a single location.
struct PositionInfo { QString point; QVariantMap context; int chapter=-1; qint64 offset=-1,total=0; FileStamp file; };
bool epubPosition(const QString &path, const QString &cfi, double *percentage, QString *point = nullptr, QVariantMap *context = nullptr,PositionInfo *resolved=nullptr);
bool positionEstimate(const QString &path,const PositionInfo &resolved,double *percentage);
QString nativeCfi(const QString &position);

// Compare validated logical locations while retaining original CFI strings in
// stored progress. compareRange also includes the end of a server range.
bool sameEpubPosition(const QString &path, const QString &first, const QString &second, bool compareRange = false);

// Bounded container/package/resource validation before committing a download.
// Does not claim full EPUB conformance or CFI support for every resource.
bool validEpub(const QString &path);
void clearEpubCache(); // Call in the owning file thread when its account scope changes.
#ifdef BOOKORBIT_TEST_HTTP
struct ProgressWork {
    quint64 packages=0,chapters=0,lengthPasses=0,lengthReads=0,hits=0,evictions=0,fb2Parses=0;
    qint64 cachedBytes=0;
    int cachedBooks=0,cachedNodes=0;
};
ProgressWork progressWork();
void resetProgressWork();
void expireEpubCache();
#endif

// Shared sync boundary. FB2 uses a calibrated PB634 prose model; unsupported
// structure/coordinates fail before a server or native position is changed.
bool syncFormat(const QString &format);
QString bookCfi(const QString &path, const QString &position,const Fb2Snapshot &fb={});
bool bookPosition(const QString &path, const QString &cfi, double *percentage,
                  QString *point = nullptr, QVariantMap *context = nullptr, QString *native = nullptr,PositionInfo *resolved=nullptr,const Fb2Snapshot &fb={});
bool sameBookPosition(const QString &path, const QString &first, const QString &second, bool compareRange = false,const Fb2Snapshot &fb={});
