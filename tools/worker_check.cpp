#include "file_executor.h"
#include "file_work.h"
#include "device.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QElapsedTimer>
#include <cstdio>
#include <cstdlib>

static void check(bool ok,const char *label) {
    std::printf("%s: %s\n",ok ? "PASS" : "FAIL",label); std::fflush(stdout);
    if(!ok) std::exit(1);
}
static bool until(const std::function<bool()> &predicate) {
    QEventLoop loop; QTimer poll;
    QObject::connect(&poll,&QTimer::timeout,&loop,[&] { if(predicate()) loop.quit(); });
    poll.start(5); QTimer::singleShot(5000,&loop,&QEventLoop::quit);
    if(!predicate()) loop.exec();
    return predicate();
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); QTemporaryDir temp; check(temp.isValid(),"isolated temporary storage");
    FileExecutor executor;
    auto entered=std::make_shared<std::atomic_bool>(false),release=std::make_shared<std::atomic_bool>(false);
    int ticks=0,completed=0; bool guiCallback=false,workerThread=false;
    QTimer timer; QObject::connect(&timer,&QTimer::timeout,[&] { ++ticks; }); timer.start(5);
    executor.submit(&app,[=,&workerThread,guiThread=app.thread()](const FileCancellation &cancel) {
        workerThread=QThread::currentThread()!=guiThread; entered->store(true);
        while(!release->load() && !fileCancelled(cancel)) QThread::msleep(1);
        return 1;
    },[&](int value,bool cancelled) { guiCallback=QThread::currentThread()==app.thread(); completed=value; check(!cancelled,"active task completes"); });
    check(until([&] { return entered->load() && ticks>=5; }) && !completed,"GUI timers run while the worker is occupied");
    bool queuedRan=false,queuedCancelled=false;
    auto queued=executor.submit(&app,[&](const FileCancellation &cancel) { queuedRan=true; return fileCancelled(cancel); },
        [&](bool cancelled,bool flag) { queuedCancelled=cancelled && flag; completed=2; });
    queued->store(true); release->store(true);
    check(until([&] { return completed==2; }) && workerThread && guiCallback && queuedRan && queuedCancelled,
          "sequential executor delivers on GUI and executes cancelled cleanup safely");
    bool orphanCallback=false,orphanWork=false;
    auto receiver=new QObject;
    executor.submit(receiver,[&](const FileCancellation &) { orphanWork=true; return true; },[&](bool,bool) { orphanCallback=true; });
    delete receiver;
    bool drained=false;
    executor.submit(&app,[](const FileCancellation &) { return true; },[&](bool,bool) { drained=true; });
    check(until([&] { return drained; }) && orphanWork && !orphanCallback,"destroyed receiver never receives a result");

    const QString file=temp.path()+"/content";
    QFile content(file); check(content.open(QIODevice::WriteOnly),"open hash fixture");
    const QByteArray payload(16*1024*1024,'x'); check(content.write(payload)==payload.size(),"write hash fixture"); content.close();
    const auto stamp=fileStamp(file);
    const auto expected=QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex();
    check(digestFile(file)==expected,"bounded full-file hash preserves SHA-256");
    auto cancelled=std::make_shared<std::atomic_bool>(true);
    check(digestFile(file,cancelled).isEmpty(),"cancelled hashing yields no usable digest");
    check(content.open(QIODevice::WriteOnly) && content.write(QByteArray(payload.size(),'y'))==payload.size(),"change content without changing its size"); content.close();
    check(fileStamp(file)!=stamp && digestFile(file)!=expected,"identity stamp detects a same-size rewrite");
    check(QFile::link(file,temp.path()+"/alias") && !fileStamp(temp.path()+"/alias").valid,"symlink cannot provide a trusted file identity");

    const QString database=temp.path()+"/history.db";
    auto db=QSqlDatabase::addDatabase("QSQLITE","fixture-writer"); db.setDatabaseName(database);
    check(db.open(),"open SQLite history fixture"); QSqlQuery query(db);
    check(query.exec("PRAGMA journal_mode=WAL") && query.next() && query.value(0).toString()=="wal","use a real WAL database");
    for(const auto *sql:{"CREATE TABLE folders(id INTEGER,name TEXT)","CREATE TABLE files(book_id INTEGER,folder_id INTEGER,filename TEXT)",
        "CREATE TABLE profiles(id INTEGER,name TEXT)","CREATE TABLE books_settings(bookid INTEGER,profileid INTEGER,opentime INTEGER)",
        "INSERT INTO folders VALUES(1,'/books')","INSERT INTO profiles VALUES(1,'default'),(2,'other')"}) check(query.exec(sql),"create native-schema fixture");
    check(db.transaction(),"start fixture transaction"); QStringList paths;
    for(int i=1;i<=405;++i) {
        const QString name=QString::number(i)+".epub"; paths << "/books/"+name;
        query.prepare("INSERT INTO files VALUES(?,1,?)"); query.addBindValue(i); query.addBindValue(name); check(query.exec(),"insert file");
        query.prepare("INSERT INTO books_settings VALUES(?,1,?)"); query.addBindValue(i); query.addBindValue(i*10); check(query.exec(),"insert native history");
    }
    check(db.commit() && QFileInfo(database+"-wal").size()>0,"committed history is still present in WAL");
    paths << paths.first() << "/books/missing.epub";
    ReaderRecents result; int batches=0; drained=false;
    executor.submit(&app,[=,&batches](const FileCancellation &cancel) { return readReaderRecents(database,paths,"default",cancel,&batches); },
        [&](const ReaderRecents &value,bool cancelled) { check(!cancelled,"history task completes"); result=value; drained=true; });
    check(until([&] { return drained; }) && result.available && result.files.size()==405 && result.files["/books/405.epub"].openTime==4050 && batches==3,
          "406 unique paths read in three batches, including the current WAL snapshot");
    const auto other=readReaderRecents(database,paths,"other");
    check(other.available && other.files.size()==405 && other.files.first().openTime==0,"profile history never leaks another profile's time");
    check(!readReaderRecents(database,paths,"",{}).available,"ambiguous unnamed profile is rejected");
    check(!readReaderRecents(database,paths,"default",cancelled).available,"cancelled history cannot publish a snapshot");
    check(query.exec("INSERT INTO files VALUES(999,1,'1.epub')") && !readReaderRecents(database,paths,"default").available,"ambiguous native path rejects the whole history snapshot");
    check(QSqlDatabase::connectionNames()==QStringList{"fixture-writer"},"worker read-only connections are removed in their owning thread");
    query=QSqlQuery(); db.close(); db=QSqlDatabase(); QSqlDatabase::removeDatabase("fixture-writer");

    entered->store(false);
    QElapsedTimer shutdown; shutdown.start();
    bool guiDestroyed=false;
    {
        FileExecutor closing;
        auto resource=std::make_shared<QObject>();
        QObject::connect(resource.get(),&QObject::destroyed,[&] { guiDestroyed=QThread::currentThread()==app.thread(); });
        closing.submit(&app,[=](const FileCancellation &cancel) { entered->store(true); while(!fileCancelled(cancel)) QThread::msleep(1); return true; },[resource](bool,bool) {});
        resource.reset();
        check(until([&] { return entered->load(); }),"shutdown fixture entered worker");
    }
    check(shutdown.elapsed()<1000 && guiDestroyed,"shutdown joins the worker and destroys callback-owned GUI objects on the GUI thread");
    return 0;
}
