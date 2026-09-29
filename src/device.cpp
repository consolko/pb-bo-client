#include "device.h"
#include "progress.h"
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#ifdef POCKETBOOK_DEVICE
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <QLibrary>
#include <QFile>
#include <QCryptographicHash>
#include <QDateTime>
#include <exception>
#include <string>
// Keep InkView macros out of Qt translation units.
#include <inkview.h>
#endif

static bool readReaderPosition(const QString &path, QString *position, qint64 *bookId);

QSize setupDevice() {
#ifdef POCKETBOOK_DEVICE
    InitInkview(TASK_MAKEACTIVE | TASK_SINGLEINSTANCE);
    // NewTaskEx can bypass SINGLEINSTANCE; reuse the registered application task.
    const int current = GetCurrentTask();
    const taskinfo *self = GetTaskInfo(current);
    const QByteArray application = self && self->appname ? self->appname : "";
    int tasks[128];
    const int count = qMin(GetTaskList(tasks, 128), 128);
    for (int i = 0; i < count && !application.isEmpty(); ++i) {
        if (tasks[i] == current) continue;
        const taskinfo *info = GetTaskInfo(tasks[i]);
        if (info && info->appname && application == info->appname) {
            SetActiveTask(tasks[i], 0);
            return {}; // main exits this duplicate before opening its cache or network.
        }
    }
    return {ScreenWidth(), ScreenHeight() - PanelHeight()};
#else
    return {600, 800};
#endif
}

QString deviceFont() {
#ifdef POCKETBOOK_DEVICE
    const char *name = iv_get_default_font(FONT_FAMILY);
    return name ? QString::fromUtf8(name) : QStringLiteral("DejaVu Sans");
#else
    return QStringLiteral("DejaVu Sans");
#endif
}

bool openReader(const QString &path) {
#ifdef POCKETBOOK_DEVICE
    // Let the firmware reuse the reader task and perform its normal book hand-off.
    // Direct NewTask creates another reader on every click, even for the same EPUB.
    QByteArray filename = path.toUtf8();
    return OpenBook(filename.constData(), nullptr, 0) > 0;
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
#endif
}

void scanBook(const QString &path) {
#ifdef POCKETBOOK_DEVICE
    const int scanner = FindTaskByAppName("/ebrmain/bin/scanner.app");
    if (scanner <= 0) return;
    // Verified U634 scanner request; scan only the downloaded book's directory.
    QByteArray request = ("-scan:"+QFileInfo(path).absolutePath()).toUtf8();
    SendRequestTo(scanner, REQ_OPENBOOK, request.data(), request.size()+1, 0, 1000);
#else
    Q_UNUSED(path)
#endif
}

bool readerBookIndexed(const QString &path) {
#ifdef POCKETBOOK_DEVICE
    bool indexed = false;
    // U634 schema 39. Short read-only connection also sees committed WAL data.
    const QString connection = "bookorbit-native-index";
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName("/mnt/ext1/system/explorer-3/explorer-3.db");
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare("SELECT 1 FROM files JOIN folders ON folders.id=files.folder_id "
                          "WHERE folders.name=? AND files.filename=? AND files.book_id>0 LIMIT 1");
            const QFileInfo file(path);
            query.addBindValue(file.absolutePath());
            query.addBindValue(file.fileName());
            indexed = query.exec() && query.next();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return indexed;
#else
    Q_UNUSED(path)
    return true;
#endif
}

ReaderFileState readerFileState(const QString &path) {
#ifdef POCKETBOOK_DEVICE
    const QByteArray target = path.toUtf8();
    int foundTask = -1, foundSubtask = -1;
    const int found = FindTaskByBook(target.constData(), &foundTask, &foundSubtask);
    if (found < 0) return ReaderFileState::Unknown;
    int tasks[128];
    const int count = GetTaskList(tasks, 128);
    if (count < 0 || count >= 128) return ReaderFileState::Unknown;
    bool matched = false;
    qint64 targetBook=0;
    for (int i = 0; i < count; ++i) {
        const taskinfo *info = GetTaskInfo(tasks[i]);
        if (!info) return ReaderFileState::Unknown;
        for (int j = 0; j < info->nsubtasks; ++j) {
            const char *book = info->subtasks[j].book;
            if (book && target == book) matched = true;
            else if (book && *book) {
                // The native database can map multiple paths to the same book record.
                QString ignored; qint64 openBookId=0;
                if ((!targetBook && !readReaderPosition(path,&ignored,&targetBook)) ||
                    !readReaderPosition(QString::fromUtf8(book),&ignored,&openBookId)) return ReaderFileState::Unknown;
                if (targetBook>0 && targetBook==openBookId) matched=true;
            }
        }
    }
    if (matched) return ReaderFileState::Open;
    return found == 0 ? ReaderFileState::Closed : ReaderFileState::Unknown;
#else
    Q_UNUSED(path)
    return ReaderFileState::Closed;
#endif
}

// Request the firmware's normal connection flow; do not toggle hardware or hold Wi-Fi awake.
bool connectNetwork() {
#ifdef POCKETBOOK_DEVICE
    const auto *info = NetInfo();
    if (info && info->connected) return true;
    return NetConnect(nullptr) == NET_OK;
#else
    return true;
#endif
}

QString readerProfile() {
#ifdef POCKETBOOK_DEVICE
    const char *profile = GetCurrentProfile();
    return profile ? QString::fromUtf8(profile) : QStringLiteral("");
#else
    return {};
#endif
}

static bool readReaderPosition(const QString &path, QString *position, qint64 *bookId) {
#ifdef POCKETBOOK_DEVICE
    const QString profileName = readerProfile();
    bool ok = false;
    const QString connection = "bookorbit-native-progress";
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName("/mnt/ext1/system/explorer-3/explorer-3.db");
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=100");
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare("SELECT s.position, f.book_id FROM files f JOIN folders d ON d.id=f.folder_id "
                      "JOIN profiles p ON (p.name=? OR (?='' AND p.name='default' AND (SELECT COUNT(*) FROM profiles)=1)) "
                      "LEFT JOIN books_settings s ON s.bookid=f.book_id AND s.profileid=p.id "
                      "WHERE d.name=? AND f.filename=? AND f.book_id>0");
            // U634 returns NULL when profiles are disabled. Only the unambiguous default is safe.
            q.addBindValue(profileName);
            q.addBindValue(profileName);
            q.addBindValue(QFileInfo(path).absolutePath());
            q.addBindValue(QFileInfo(path).fileName());
            if (q.exec() && q.next()) {
                *position=q.value(0).toString();
                if (bookId) *bookId=q.value(1).toLongLong();
                ok=!q.next();
            }
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
#else
    Q_UNUSED(path) Q_UNUSED(position) Q_UNUSED(bookId)
    return false;
#endif
}

bool readerPosition(const QString &path, QString *position) {
    return readReaderPosition(path, position, nullptr);
}

bool saveReaderPosition(const QString &path, const QString &expectedPosition,
                        const QString &cfi, const QString &profile, QString *error) {
    *error = "Штатный механизм сохранения позиции недоступен для этой прошивки.";
#ifdef POCKETBOOK_DEVICE
    // Private native Cloud ABI, verified only in this exact U634 library. No SQL writes here.
    if (QByteArray(GetSoftwareVersion()) != "U634.6.10.3425") return false;
    QFile library("/ebrmain/lib/libframework2.so");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!library.open(QIODevice::ReadOnly) || !hash.addData(&library) || hash.result().toHex() !=
        "69de9e57ddb9d96f6d38e54150a120efd061dbe524f8d430f90ff5b39a10c5f9") return false;
    static QLibrary native("/ebrmain/lib/libframework2.so");
    native.setLoadHints(QLibrary::PreventUnloadHint);
    const auto instance = reinterpret_cast<void *(*)()>(native.resolve("_ZN10pocketbook2db9DbManager8InstanceEv"));
    const auto initProfile = reinterpret_cast<void (*)(void *)>(native.resolve("_ZN10pocketbook2db9DbManager11InitProfileEv"));
    const auto setPosition = reinterpret_cast<int (*)(void *, long long, const std::string &, int, long)>(native.resolve(
        "_ZN10pocketbook2db9DbManager11SetPositionExRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEil"));
    if (!instance || !initProfile || !setPosition) return false;
    double percentage=0;
    QString point;
    *error = "Координата не подходит к этому EPUB.";
    if (!epubPosition(path,cfi,&percentage,&point)) return false;
    *error = "Закройте все книги в штатной читалке и повторите синхронизацию.";
    if (readerFileState(path) != ReaderFileState::Closed) return false;
    // Different paths can share one native book_id. Block all open books, including aliases.
    const auto allBooksClosed=[] {
        int tasks[128]; const int count=GetTaskList(tasks,128);
        if (count<0 || count>=128) return false;
        for (int i=0;i<count;++i) {
            const auto *info=GetTaskInfo(tasks[i]);
            if (!info) return false;
            for (int j=0;j<info->nsubtasks;++j)
                if (info->subtasks[j].book && *info->subtasks[j].book) return false;
        }
        return true;
    };
    if (!allBooksClosed()) return false;
    QString current; qint64 bookId=0;
    *error = "Позиция или профиль ридера изменились. Повторите синхронизацию.";
    if (readerProfile()!=profile || !readReaderPosition(path,&current,&bookId) || bookId<=0 || current!=expectedPosition) return false;
    const QString incoming="pbr:/webkit?##"+point;
    *error = "Не удалось подтвердить сохранение позиции. Повторите синхронизацию.";
    try {
        void *db=instance();
        if (!db) return false;
        initProfile(db);
        if (readerProfile()!=profile || !allBooksClosed() ||
            !readerPosition(path,&current) || current!=expectedPosition) return false;
        // The native Cloud setter owns timestamps and temporary percentage/100 counters.
        // Those counters never determine the CFI or the winner of a conflict.
        if (setPosition(db,bookId,incoming.toStdString(),qRound(percentage),long(QDateTime::currentSecsSinceEpoch()))!=0) return false;
    } catch (const std::exception &) { return false; }
    return readerProfile()==profile && allBooksClosed() && readerPosition(path,&current) && current==incoming;
#else
    Q_UNUSED(path) Q_UNUSED(expectedPosition) Q_UNUSED(cfi) Q_UNUSED(profile)
    return false;
#endif
}
