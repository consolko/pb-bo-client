#pragma once
#include <QString>

// Percentage is a text-length estimate; only CFI is used to restore a position.
bool epubPosition(const QString &path, const QString &cfi, double *percentage);
QString nativeCfi(const QString &position);
