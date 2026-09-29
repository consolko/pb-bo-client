#pragma once
#include <QString>

// Percentage is a text-length estimate; only CFI is used to restore a position.
// Validated ranges use their start when a reader needs a single location.
bool epubPosition(const QString &path, const QString &cfi, double *percentage, QString *point = nullptr);
QString nativeCfi(const QString &position);
