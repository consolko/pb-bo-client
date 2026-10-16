#pragma once
#include <QString>

// Returns an error without replacing an unreadable or malformed desktop configuration.
QString installLauncher(const QString &storageRoot);
