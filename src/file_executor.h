#pragma once
#include <QObject>
#include <QPointer>
#include <QThread>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>

using FileCancellation = std::shared_ptr<std::atomic_bool>;
inline bool fileCancelled(const FileCancellation &cancel) { return cancel && cancel->load(); }

inline thread_local FileCancellation activeFileCancellation;
inline bool fileTaskCancelled() { return fileCancelled(activeFileCancellation); }
struct FileCancellationScope {
    FileCancellation previous;
    explicit FileCancellationScope(const FileCancellation &cancel):previous(activeFileCancellation) { activeFileCancellation=cancel; }
    ~FileCancellationScope() { activeFileCancellation=previous; }
};

class FileExecutor : public QObject {
    QThread thread;
    QObject *worker = new QObject;
    std::vector<std::weak_ptr<std::atomic_bool>> tasks;
public:
    FileExecutor() {
        worker->moveToThread(&thread);
        connect(&thread,&QThread::finished,worker,&QObject::deleteLater);
        thread.start();
    }
    ~FileExecutor() override {
        for (const auto &task:tasks) if (auto cancel=task.lock()) cancel->store(true);
        thread.quit(); thread.wait();
    }
    template<class Work,class Done>
    FileCancellation submit(QObject *context,Work work,Done done) {
        auto cancel=std::make_shared<std::atomic_bool>(false);
        std::erase_if(tasks,[](const auto &task) { return task.expired(); });
        tasks.push_back(cancel);
        // Callbacks may own GUI objects; keep their destruction on the GUI thread too.
        struct Delivery : QObject {
            QPointer<QObject> receiver;
            Done done;
            Delivery(QObject *parent,QObject *context,Done callback):QObject(parent),receiver(context),done(std::move(callback)) {}
        };
        QPointer<Delivery> delivery(new Delivery(this,context,std::move(done)));
        QMetaObject::invokeMethod(worker,[this,delivery,cancel,work=std::move(work)]() mutable {
            FileCancellationScope scope(cancel);
            auto result=work(cancel);
            // The executor outlives its worker, including during shutdown.
            QMetaObject::invokeMethod(this,[delivery,cancel,result=std::move(result)]() mutable {
                if (!delivery) return;
                if (delivery->receiver) delivery->done(std::move(result),fileCancelled(cancel));
                delete delivery.data();
            },Qt::QueuedConnection);
        },Qt::QueuedConnection);
        return cancel;
    }
};
