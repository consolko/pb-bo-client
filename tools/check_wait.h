#pragma once

#include "client.h"
#include <QEventLoop>
#include <QTimer>
#include <functional>

struct ClientWorkerCheck {
    static FileExecutor &executor(Client &client) { return client.files; }
    static bool recentsIdle(const Client &client) { return !client.recentsScheduled && !client.recentsRequested; }
    static void changeAccount(Client &client) { ++client.accountGeneration; }
    static FileCancellation task(const Client &client) { return client.fileTask; }
    static bool cleanupRunning(const Client &client) { return client.cleanupState==Client::CleanupState::Running; }
    static bool cleanupCancelled(const Client &client) { return client.cleanupState==Client::CleanupState::Cancelled; }
    static quint64 cleanupId(const Client &client) { return client.cleanupGeneration; }
    static QString scope(const Client &client) { return client.scopeDir; }
    static void cleanup(Client &client) { client.cleanupBooks(); }
    static void invalidateSession(Client &client) { client.invalidateSession(); }
    static void reload(Client &client) { client.loadRecords(); }
    static bool maintenanceIdle(const Client &client) { return client.cleanupState!=Client::CleanupState::Running && !client.cleanupPending; }
};

inline bool waitUntil(const std::function<bool()> &predicate,int timeout=10000) {
    QEventLoop loop; QTimer poll;
    QObject::connect(&poll,&QTimer::timeout,&loop,[&] { if (predicate()) loop.quit(); });
    poll.start(5); QTimer::singleShot(timeout,&loop,&QEventLoop::quit);
    if (!predicate()) loop.exec();
    return predicate();
}
inline bool waitForRecents(Client &client) {
    return waitUntil([&] { return ClientWorkerCheck::recentsIdle(client); });
}
inline bool waitForMaintenance(Client &client) {
    return waitUntil([&] { return ClientWorkerCheck::maintenanceIdle(client); });
}

inline bool waitForClient(Client &client, const std::function<void()> &action, bool *completed = nullptr) {
    QEventLoop loop;
    bool done = false, ok = false;
    auto connection = QObject::connect(&client, &Client::completed, &loop, [&](const QString &operation, bool success) {
        if (operation == "settings") return;
        done = true; ok = success; if (!client.busy()) loop.quit();
    });
    auto operation=QObject::connect(&client,&Client::operationChanged,&loop,[&] { if (done && !client.busy()) loop.quit(); });
    QTimer::singleShot(0, &loop, action);
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection); QObject::disconnect(operation);
    if (completed) *completed = done;
    return done && ok;
}
