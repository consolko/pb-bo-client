#include "launcher.h"
#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>

QString installLauncher(const QString &storageRoot) {
    const QString configPath = storageRoot + "/system/config/desktop/view.json";
    QFile config(configPath);
    if (!config.open(QIODevice::ReadOnly) || config.size() > 1024 * 1024)
        return "Cannot read desktop configuration";
    const QByteArray original = config.readAll();
    if (config.error() != QFile::NoError) return config.errorString();
    config.close();
    const auto document = QJsonDocument::fromJson(original);
    if (!document.isObject() || !document.object().value("applications").isObject())
        return "Invalid desktop configuration";
    auto root = document.object();
    auto applications = root.value("applications").toObject();
    const auto previous = applications.value("U_bookorbit");
    if (!previous.isUndefined() && !previous.isObject()) return "Invalid BookOrbit desktop entry";
    auto entry = previous.toObject();
    // NewTask uses this path literally, unlike the desktop icon loader.
    entry["path"] = storageRoot + "/applications/bookorbit.app";
    entry["title"] = "BookOrbit";
    entry["icon"] = QJsonObject{{"path", "applications/bookorbit/icons/bookorbit.bmp"}};
    entry["focused_icon"] = QJsonObject{{"path", "applications/bookorbit/icons/bookorbit_f.bmp"}};
    auto view = root.value("view").toObject();
    auto groups = view.value("groups").toArray();
    int destination = -1;
    bool listed = false;
    for (int i = 0; i < groups.size(); ++i) {
        const auto apps = groups[i].toObject().value("apps");
        if (!apps.isArray()) continue;
        if (destination < 0) destination = i;
        listed |= apps.toArray().contains("U_bookorbit");
    }
    if (!listed) {
        if (destination < 0) return "No desktop application group";
        auto group = groups[destination].toObject();
        auto apps = group.value("apps").toArray();
        apps.append("U_bookorbit");
        group["apps"] = apps;
        groups[destination] = group;
        view["groups"] = groups;
        root["view"] = view;
    }

    const QString iconDir = storageRoot + "/applications/bookorbit/icons";
    QImage icon(":/icons/bookorbit.png");
    if (icon.isNull()) return "Cannot read launcher icon resource";
    icon = icon.scaled(120, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation)
               .convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_Indexed8);
    if (!QDir().mkpath(iconDir)) return "Cannot create launcher icon directory";
    for (const auto *name : {"bookorbit.bmp", "bookorbit_f.bmp"}) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!icon.save(&buffer, "BMP")) return "Cannot encode launcher icon";
        icon.invertPixels();
        QFile current(iconDir + '/' + name);
        if (current.open(QIODevice::ReadOnly) && current.size() == bytes.size() && current.readAll() == bytes)
            continue;
        QSaveFile output(current.fileName());
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
            return output.errorString();
    }
    if (previous == entry && listed) return {};
    // Keep the first configuration for manual rollback, including other apps and groups.
    const QString backup = configPath + ".bookorbit-backup";
    if (!QFile::exists(backup) && !QFile::copy(configPath, backup))
        return "Cannot back up desktop configuration";
    applications["U_bookorbit"] = entry;
    root["applications"] = applications;
    const QByteArray updated = QJsonDocument(root).toJson();
    QSaveFile output(configPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(updated) != updated.size() || !output.commit())
        return output.errorString();
    return {};
}
