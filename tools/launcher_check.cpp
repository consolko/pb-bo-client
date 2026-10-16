#include "launcher.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    std::printf("PASS: %s\n", message);
}
static QByteArray read(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read fixture"); return file.readAll();
}
static void write(const QString &path, const QByteArray &bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    require(temp.isValid(), "isolated storage");
    const QString root = temp.path(), config = root + "/system/config/desktop/view.json";
    require(!installLauncher(root).isEmpty(), "missing firmware configuration is not invented");
    require(QDir().mkpath(QFileInfo(config).absolutePath()), "create fixture directory");
    const QByteArray original = R"({"version":"7","applications":{"Other":{"title":"Other"},"U_bookorbit":{"path":"applications/bookorbit.app","visible":"0","param":["keep"]}},"view":{"groups":[{"title":"Custom","apps":["Other"]}]}})";
    write(config, original);
    require(installLauncher(root).isEmpty(), "register launcher");
    const auto updated = read(config);
    const auto doc = QJsonDocument::fromJson(updated).object();
    const auto old = QJsonDocument::fromJson(original).object();
    const auto apps = doc["applications"].toObject();
    const auto entry = apps["U_bookorbit"].toObject();
    const auto group = doc["view"].toObject()["groups"].toArray().first().toObject();
    require(group["title"] == "Custom" && group["apps"].toArray() == QJsonArray({"Other", "U_bookorbit"})
            && apps["Other"] == old["applications"].toObject()["Other"], "append to existing group and preserve other apps");
    write(root + "/applications/bookorbit.app", "executable fixture");
    const QString executable = entry["path"].toString();
    require(entry["title"] == "BookOrbit" && QDir::isAbsolutePath(executable) && QFileInfo::exists(executable)
            && read(executable) == "executable fixture", "launcher resolves executable independently of working directory");
    require(entry["visible"] == "0" && entry["param"] == old["applications"].toObject()["U_bookorbit"].toObject()["param"], "preserve custom entry fields");
    require(read(config + ".bookorbit-backup") == original, "exact original backup");
    const QString normalPath = root + '/' + entry["icon"].toObject()["path"].toString();
    const QString focusedPath = root + '/' + entry["focused_icon"].toObject()["path"].toString();
    const QImage normal(normalPath), focused(focusedPath);
    require(normal.size() == QSize(120, 120) && focused.size() == normal.size(), "native icon dimensions");
    require(read(normalPath).mid(28, 2) == QByteArray::fromHex("0800"), "8-bit BMP for PocketBook");
    require(normal.pixelColor(0, 0).lightness() > 240 && focused.pixelColor(0, 0).lightness() < 15, "normal and focused states");
    require(installLauncher(root).isEmpty() && read(config) == updated && read(config + ".bookorbit-backup") == original, "repeat preserves config and backup");
    require(QFile::remove(normalPath) && installLauncher(root).isEmpty() && QFile::exists(normalPath), "restore missing icon");
    for (const auto bytes : {QByteArray("broken JSON"), QByteArray("[]"), QByteArray("{\"applications\":[]}")}) {
        write(config, bytes);
        require(!installLauncher(root).isEmpty() && read(config) == bytes, "refuse malformed config without changes");
    }
    write(config, original);
    require(QFile::remove(focusedPath) && QDir().mkdir(focusedPath), "block icon destination");
    require(!installLauncher(root).isEmpty() && read(config) == original, "icon failure preserves config");
    return 0;
}
