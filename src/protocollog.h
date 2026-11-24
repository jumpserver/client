#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

inline void protocolLog(const char *message) {
    const QString root = qEnvironmentVariable("APPDATA", QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
    const QString directory = QDir(root).filePath(QStringLiteral("osm-client"));
    if (!QDir().mkpath(directory)) return;
    QFile file(QDir(directory).filePath(QStringLiteral("protocol.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    file.write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + " " + message + "\n");
}
