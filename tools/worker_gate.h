#pragma once
#include "file_executor.h"
#include <QObject>
#include <atomic>
#include <memory>

// Hold the sequential worker so race checks never depend on EPUB size or CPU speed.
struct WorkerGate {
    std::shared_ptr<std::atomic_bool> entered=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> release=std::make_shared<std::atomic_bool>(false);
    WorkerGate(FileExecutor &executor,QObject *receiver) {
        executor.submit(receiver,[entered=entered,release=release](const FileCancellation &cancel) {
            entered->store(true);
            while(!release->load() && !fileCancelled(cancel)) QThread::msleep(1);
            return true;
        },[](bool,bool) {});
    }
    ~WorkerGate() { release->store(true); }
    void open() { release->store(true); }
};
