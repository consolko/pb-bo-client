// Executes the real Client against the local fixture; never launches the reader.
#include "client.h"
#include "device.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <cstdio>
#include <cstdlib>

bool openReader(const QString &) { return true; }
ReaderFileState fakeReader = ReaderFileState::Closed;
ReaderFileState readerFileState(const QString &) { return fakeReader; }
void require(bool value, const char *label) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
    std::printf("PASS: %s\n", label); std::fflush(stdout);
}
bool wait(Client &client, const std::function<void()> &action) {
    QEventLoop loop;
    bool ok = false, done = false;
    auto connection = QObject::connect(&client, &Client::completed, &loop, [&](const QString &op, bool success) {
        if (op == "settings") return;
        ok = success; done = true; loop.quit();
    });
    QTimer::singleShot(0, &loop, action);
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    loop.exec(); QObject::disconnect(connection);
    require(done, "operation completed within timeout");
    return ok;
}
QByteArray contents(const QString &path) { QFile f(path); require(f.open(QIODevice::ReadOnly), "read test file"); return f.readAll(); }
void write(const QString &path, const QByteArray &data) { QFile f(path); require(f.open(QIODevice::WriteOnly) && f.write(data) == data.size(), "write test data"); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    require(argc == 3, "data directory and fixture fault file supplied");
    const QString root = QString::fromLocal8Bit(argv[1]), fault = QString::fromLocal8Bit(argv[2]);
    const QUrl endpoint("http://host.containers.internal:8766");
    Client c(endpoint, root, true);
    write(fault, "");
    require(!c.configure("http://example.com", "demo", true), "reject non-local HTTP");
    require(!c.configure("https://user:secret@example.com", "a", false), "reject URL credentials");
    require(!c.configure("https://example.com?token=secret", "a", false), "reject URL query");
    require(wait(c, [&] { c.login("demo", "demo"); }), "login and catalog");
    c.showDownloaded(false);
    require(c.books().size() == 3, "catalog contains three books");
    require(!c.books()[1].toMap()["needsRepair"].toBool(), "new book offers first download, not repair");
    require(wait(c, [&] { c.download(0); }), "download first book");
    const auto file = c.localFile(0), original = QString::fromLatin1(QCryptographicHash::hash(contents(file), QCryptographicHash::Sha256).toHex());
    require(wait(c, [&] { c.download(1); }), "download second book");
    const auto scope = QFileInfo(file).absolutePath();
    require(QFile::exists(scope+"/records/101.json") && !QFile::exists(scope+"/downloads.json"), "per-book records replace shared registry");
    fakeReader = ReaderFileState::Open;
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == file, "open reader blocks replacement");
    fakeReader = ReaderFileState::Unknown;
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == file, "unknown reader state blocks replacement");
    fakeReader = ReaderFileState::Closed;
    QEventLoop covers;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &covers, [&] { if (!c.coverUrl(1).isEmpty() && !c.coverUrl(2).isEmpty()) covers.quit(); });
    poll.start(20); QTimer::singleShot(10000, &covers, &QEventLoop::quit); covers.exec();
    require(!c.coverUrl(1).isEmpty() && !c.coverUrl(2).isEmpty() && c.coverUrl(3).isEmpty(), "covers cached; absent cover stays empty");
    write(fault, "cover_removed");
    require(wait(c, [&] { c.refresh(0); }) && c.coverUrl(1).isEmpty() && !QFile::exists(scope+"/cover-1.png"), "removed cover deletes cached image");
    write(fault, "cover_changed");
    require(wait(c, [&] { c.refresh(0); }), "changed cover catalog");
    QEventLoop changedCover;
    QTimer changedPoll;
    QObject::connect(&changedPoll, &QTimer::timeout, &changedCover, [&] { if (!c.coverUrl(1).isEmpty()) changedCover.quit(); });
    changedPoll.start(20); QTimer::singleShot(10000, &changedCover, &QEventLoop::quit); changedCover.exec();
    require(!c.coverUrl(1).isEmpty(), "changed cover fetched after invalidation");
    write(fault, "");
    require(wait(c, [&] { c.refresh(0, "no such title"); }), "empty search");
    require(c.books().isEmpty(), "empty catalog shown");
    c.showDownloaded(true);
    require(c.books().size() == 2 && !c.localFile(0).isEmpty(), "downloads independent of search");
    write(scope+"/cover-999.png", "stale");
    write(scope+"/cover-other.png", "keep");
    require(wait(c, [&] { c.refresh(0, "no such title"); }) &&
            !QFile::exists(scope+"/cover-999.png") && QFile::exists(scope+"/cover-other.png"), "cover cleanup touches only owned unreferenced names");
    write(fault, "slow");
    require(!wait(c, [&] { c.download(0); QTimer::singleShot(200, &c, [&] { c.cancelDownload(); }); }), "cancel active download");
    require(QString::fromLatin1(QCryptographicHash::hash(contents(file), QCryptographicHash::Sha256).toHex()) == original, "cancel preserves previous EPUB");
    fakeReader = ReaderFileState::Closed;
    require(!wait(c, [&] { c.download(0); QTimer::singleShot(200, &c, [] { fakeReader = ReaderFileState::Open; }); }) &&
            c.localFile(0) == file, "reader opening during transfer blocks promotion");
    fakeReader = ReaderFileState::Closed;
    write(fault, "truncated");
    require(!wait(c, [&] { c.download(0); }) && c.canRetry(), "failed download offers retry");
    write(fault, "");
    require(wait(c, [&] { c.retry(); }), "retry succeeds");
    require(c.localFile(0) == file, "identical redownload keeps file identity");
    write(file, QByteArray(contents(file).size(), 'x'));
    c.open(0);
    require(c.localFile(0).isEmpty() && c.books().size() == 2 && c.books()[0].toMap()["needsRepair"].toBool(), "corruption preserves library entry with repair action");
    require(wait(c, [&] { c.download(0); }), "redownload repairs corruption");
    const auto repaired = c.localFile(0);
    require(!repaired.isEmpty() && repaired != file, "repair switches to a new EPUB path");
    write(fault, "changed");
    require(wait(c, [&] { c.download(0); }) && c.localFile(0) != repaired, "changed EPUB under same ID gets a new path");
    const auto changedFile = c.localFile(0);
    require(QFile::exists(changedFile) && !QFile::exists(repaired), "old closed version cleaned after switch");
    const auto recordPath = scope+"/records/101.json";
    require(QFile::rename(recordPath, recordPath+".saved") && QDir().mkdir(recordPath), "inject metadata write failure");
    write(fault, "");
    require(!wait(c, [&] { c.download(0); }) && c.localFile(0) == changedFile, "failed metadata write preserves current EPUB");
    require(QDir().rmdir(recordPath) && QFile::rename(recordPath+".saved", recordPath), "restore metadata record");
    Client afterFailure(endpoint, root, true, nullptr, false);
    afterFailure.showDownloaded(true);
    require(!afterFailure.localFile(0).isEmpty(), "restart after metadata failure keeps previous book");
    const QString orphan = scope+"/101-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".epub";
    write(orphan, "orphan");
    Client afterCrash(endpoint, root, true, nullptr, false);
    require(!QFile::exists(orphan), "restart removes unreferenced version");
    require(QFile::rename(recordPath, recordPath+".saved"), "save record before corruption test");
    write(recordPath, "{");
    Client badRecord(endpoint, root, true, nullptr, false);
    require(QFile::exists(changedFile), "invalid record never deletes an EPUB");
    auto incomplete = QJsonDocument::fromJson(contents(recordPath+".saved")).object();
    incomplete.remove("sha256");
    write(recordPath, QJsonDocument(incomplete).toJson());
    Client incompleteRecord(endpoint, root, true, nullptr, false);
    require(QFile::exists(changedFile), "incomplete record never deletes an EPUB");
    require(QFile::remove(recordPath) && QFile::rename(recordPath+".saved", recordPath), "restore record after corruption test");
    write(fault, "expired");
    require(!wait(c, [&] { c.download(0); }) && !c.authenticated(), "download 401 expires session");
    write(fault, "");
    require(wait(c, [&] { c.login("demo", "demo"); }), "login after expiry");
    write(fault, "error");
    require(!wait(c, [&] { c.refresh(0, ""); }) && c.canRetry(), "catalog outage offers retry");
    write(fault, "");
    require(wait(c, [&] { c.retry(); }), "catalog retry succeeds");
    c.logout();
    require(!c.authenticated() && c.books().size() == 2, "logout keeps downloads");
    require(c.configure("https://books.example.test/", "alice", false), "save non-demo account without network");
    require(c.books().isEmpty() && c.coverUrl(1).isEmpty(), "account isolation includes covers");
    Client restarted(endpoint, root, true);
    require(restarted.username() == "alice" && restarted.server() == "https://books.example.test" && !restarted.authenticated(), "restart restores selected account without token");
    restarted.selectAccount(0);
    require(restarted.books().size() == 2 && !restarted.localFile(0).isEmpty() && !restarted.coverUrl(1).isEmpty(), "offline account selection restores books and covers");
    const auto settings = contents(root+"/accounts.json");
    require(!settings.contains("password") && !settings.contains("Token") && !settings.contains("local-development-only"), "settings contain no credentials");
    // A partial migration keeps the old registry and resumes on the next launch.
    const QString legacyRoot = root+"/legacy-check";
    const QString legacyScope = legacyRoot+"/"+QFileInfo(scope).fileName();
    require(QDir().mkpath(legacyScope+"/records"), "create isolated legacy account");
    QJsonObject legacyRecords;
    for (int id : {101, 102}) {
        const auto saved = QJsonDocument::fromJson(contents(scope+"/records/"+QString::number(id)+".json")).object();
        auto old = saved;
        old.remove("filename");
        if (id == 102) old.remove("book");
        legacyRecords[QString::number(id)] = old;
        require(QFile::copy(scope+"/"+saved["filename"].toString(), legacyScope+"/"+QString::number(id)+".epub"), "copy legacy EPUB");
    }
    write(legacyScope+"/downloads.json", QJsonDocument(legacyRecords).toJson());
    require(QDir().mkdir(legacyScope+"/records/102.json"), "inject interrupted migration");
    Client partial(endpoint, legacyRoot, true, nullptr, false);
    partial.showDownloaded(true);
    require(partial.books().size() == 2 && !partial.localFile(0).isEmpty() && QFile::exists(legacyScope+"/downloads.json"), "partial migration keeps legacy data");
    require(QDir().rmdir(legacyScope+"/records/102.json"), "clear migration fault");
    require(QDir().mkdir(legacyScope+"/downloads-v1.json"), "inject backup rename failure");
    Client beforeBackup(endpoint, legacyRoot, true, nullptr, false);
    beforeBackup.showDownloaded(true);
    require(beforeBackup.books().size() == 2 && QFile::exists(legacyScope+"/records/101.json") &&
            QFile::exists(legacyScope+"/records/102.json") && QFile::exists(legacyScope+"/downloads.json"),
            "restart after all records written keeps old registry until backup succeeds");
    require(QDir().rmdir(legacyScope+"/downloads-v1.json"), "clear backup rename fault");
    Client legacy(endpoint, legacyRoot, true, nullptr, false);
    legacy.showDownloaded(true);
    require(legacy.books().size() == 2 && !legacy.localFile(0).isEmpty() &&
            QFile::exists(legacyScope+"/downloads-v1.json") && !QFile::exists(legacyScope+"/downloads.json"), "migration resumes and keeps backup");
    const QString manyRoot = root+"/many-check";
    const QString manyScope = manyRoot+"/"+QFileInfo(scope).fileName();
    require(QDir().mkpath(manyScope+"/records"), "create large isolated library");
    qint64 aggregate = 0;
    bool allWrote = true;
    for (int id=1000; id<2200; ++id) {
        const QJsonObject book{{"id", id}, {"title", QString(2000, 'T')},
                               {"selectedFile", QJsonObject{{"id", id}, {"sizeBytes", QJsonValue::Null}}}};
        const QJsonObject record{{"bookId", id}, {"fileId", id}, {"filename", QString::number(id)+".epub"},
                                 {"sha256", QString(64, 'a')}, {"bytes", 1}, {"book", book}};
        const QString name = manyScope+"/records/"+QString::number(id)+".json";
        const auto data = QJsonDocument(record).toJson();
        QFile output(name);
        if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size()) { allWrote = false; break; }
        output.close();
        aggregate += QFileInfo(name).size();
    }
    require(allWrote, "write large library records");
    require(aggregate > 2*1024*1024, "per-book records exceed old aggregate limit");
    Client many(endpoint, manyRoot, true, nullptr, false);
    many.showDownloaded(true);
    require(many.books().size() == 1200, "large downloaded list remains available");
    std::puts("PASS: stage 3 client checks");
}
