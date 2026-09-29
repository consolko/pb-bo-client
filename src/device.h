#pragma once
#include <QString>
#include <QSize>

QSize setupDevice();
QString deviceFont();
bool openReader(const QString &path);
void scanBook(const QString &path);
bool readerBookIndexed(const QString &path);
enum class ReaderFileState { Closed, Open, Unknown };
ReaderFileState readerFileState(const QString &path);

bool connectNetwork();
bool readerPosition(const QString &path, QString *position);
QString readerProfile();
bool saveReaderPosition(const QString &path, const QString &expectedPosition,
                        const QString &cfi, const QString &profile, QString *error);
