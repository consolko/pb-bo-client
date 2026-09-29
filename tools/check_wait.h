#pragma once

#include "client.h"
#include <QEventLoop>
#include <QTimer>
#include <functional>

inline bool waitForClient(Client &client, const std::function<void()> &action, bool *completed = nullptr) {
    QEventLoop loop;
    bool done = false, ok = false;
    auto connection = QObject::connect(&client, &Client::completed, &loop, [&](const QString &operation, bool success) {
        if (operation == "settings") return;
        done = true; ok = success; loop.quit();
    });
    QTimer::singleShot(0, &loop, action);
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    if (completed) *completed = done;
    return done && ok;
}
