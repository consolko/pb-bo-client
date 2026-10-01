#pragma once
#include <QString>
#include <QVariantMap>

// Percentage is a text-length estimate; only CFI is used to restore a position.
// Validated ranges use their start when a reader needs a single location.
bool epubPosition(const QString &path, const QString &cfi, double *percentage, QString *point = nullptr, QVariantMap *context = nullptr);
QString nativeCfi(const QString &position);

// Compare validated logical locations while retaining original CFI strings in
// stored progress. compareRange also includes the end of a server range.
bool sameEpubPosition(const QString &path, const QString &first, const QString &second, bool compareRange = false);

// Bounded container/package/resource validation before committing a download.
// Does not claim full EPUB conformance or CFI support for every resource.
bool validEpub(const QString &path);
