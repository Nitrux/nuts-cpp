// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include "NutsExport.h"
#include "Types.h"
#include <QString>
#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QMutex>

namespace Nuts {

class NUTS_EXPORT Logger : public QObject {
    Q_OBJECT

public:
    static Logger& instance();

    void setLogFile(const QString& path);
    void log(LogLevel level, const QString& message);

    void info(const QString& message) { log(LogLevel::Info, message); }
    void success(const QString& message) { log(LogLevel::Success, message); }
    void warning(const QString& message) { log(LogLevel::Warning, message); }
    void error(const QString& message) { log(LogLevel::Error, message); }

Q_SIGNALS:
    void logMessage(LogLevel level, const QString& message);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void openLogFile();
    QString levelToString(LogLevel level) const;
    QString levelToColor(LogLevel level) const;

    QString m_logFilePath;
    QFile m_logFile;
    QMutex m_mutex;
};

} // namespace Nuts
