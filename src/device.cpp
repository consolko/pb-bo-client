#include "device.h"
#include <QDesktopServices>
#include <QUrl>
#ifdef POCKETBOOK_DEVICE
// Keep InkView macros out of Qt translation units.
#include <inkview.h>
#endif

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
    for (int i = 0; i < count; ++i) {
        const taskinfo *info = GetTaskInfo(tasks[i]);
        if (!info) return ReaderFileState::Unknown;
        for (int j = 0; j < info->nsubtasks; ++j) {
            const char *book = info->subtasks[j].book;
            if (book && target == book) matched = true;
        }
    }
    if (matched) return ReaderFileState::Open;
    return found == 0 ? ReaderFileState::Closed : ReaderFileState::Unknown;
#else
    Q_UNUSED(path)
    return ReaderFileState::Closed;
#endif
}
