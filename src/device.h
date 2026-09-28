#pragma once
#include <QString>
#include <QSize>

QSize setupDevice();
QString deviceFont();
bool openReader(const QString &path);
enum class ReaderFileState { Closed, Open, Unknown };
ReaderFileState readerFileState(const QString &path);
