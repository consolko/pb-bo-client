#pragma once
#include <QJsonObject>
#include <QString>
#include <QByteArray>

namespace Ota {
constexpr qint64 maxArchive = 32 * 1024 * 1024;
constexpr qint64 maxClient = 32 * 1024 * 1024;
QString version();
const char *buildInfo();
bool validVersion(const QString &value);
bool newer(const QString &candidate, const QString &current);
QByteArray publicKey();
QByteArray read(const QString &path, qint64 limit);
QString hashFile(const QString &path);
bool safeDirectory(const QString &path);
bool save(const QString &path, const QByteArray &data, QString *error);
bool saveJson(const QString &path, const QJsonObject &data, QString *error);
bool verifyManifest(const QByteArray &data, const QByteArray &signature,
                    QJsonObject *manifest, QString *error);
bool stageArchive(const QString &archive, const QString &destination,
                  const QString &expectedVersion, QJsonObject *manifest, QString *error);
bool install(const QString &staged, const QString &destination,
             const QJsonObject &manifest, QString *error);
}
