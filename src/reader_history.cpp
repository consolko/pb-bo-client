#include "device.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include <QUuid>

ReaderRecents readReaderRecents(const QString &database,const QStringList &paths,const QString &profile,
                              const FileCancellation &cancel,int *batches) {
    ReaderRecents result; result.profile=profile;
    if (batches) *batches=0;
    const QString connection="bookorbit-recents-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",connection);
        db.setDatabaseName(database);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        if (db.open() && db.transaction()) {
            bool ok=!fileCancelled(cancel);
            const QMap<QString,QStringList> columns{
                {"files",{"book_id","folder_id","filename"}}, {"folders",{"id","name"}},
                {"profiles",{"id","name"}}, {"books_settings",{"bookid","profileid","opentime"}}};
            for (auto it=columns.begin();it!=columns.end() && ok;++it) {
                QSqlQuery schema(db); QStringList found;
                ok=schema.exec("PRAGMA table_info("+it.key()+")");
                while (schema.next()) found << schema.value(1).toString();
                for (const auto &column:it.value()) ok=ok && found.contains(column);
            }
            qint64 profileId=0;
            {
                QSqlQuery query(db);
                ok=ok && query.prepare("SELECT id FROM profiles WHERE name=? OR (?='' AND name='default' AND (SELECT COUNT(*) FROM profiles)=1)");
                query.addBindValue(profile); query.addBindValue(profile);
                ok=ok && query.exec() && query.next();
                if (ok) { profileId=query.value(0).toLongLong(); ok=!query.next(); }
            }
            QStringList unique=paths; unique.removeDuplicates();
            for (int offset=0;offset<unique.size() && ok;offset+=200) {
                if (fileCancelled(cancel)) { ok=false; break; }
                const auto batch=unique.mid(offset,200);
                QStringList values; for (int i=0;i<batch.size();++i) values << "(?,?,?)";
                QSqlQuery query(db);
                ok=query.prepare("WITH requested(path,folder,filename) AS (VALUES "+values.join(',')+") "
                    "SELECT r.path,f.book_id,s.opentime FROM requested r JOIN folders d ON d.name=r.folder "
                    "JOIN files f ON f.folder_id=d.id AND f.filename=r.filename "
                    "LEFT JOIN books_settings s ON s.bookid=f.book_id AND s.profileid=? WHERE f.book_id>0");
                for (const auto &path:batch) {
                    const QFileInfo info(path);
                    query.addBindValue(path); query.addBindValue(info.absolutePath()); query.addBindValue(info.fileName());
                }
                query.addBindValue(profileId);
                if (batches) ++*batches;
                ok=ok && query.exec();
                while (ok && query.next()) {
                    const QString path=query.value(0).toString();
                    if (result.files.contains(path) || fileCancelled(cancel)) { ok=false; break; }
                    result.files.insert(path,{query.value(1).toLongLong(),qMax(qint64(0),query.value(2).toLongLong())});
                }
                ok=ok && !query.lastError().isValid();
            }
            result.available=ok && !fileCancelled(cancel);
            db.rollback(); // Release the WAL snapshot without modifying the native database.
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (!result.available) result.files.clear();
    return result;
}
