// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#include "nuts/Logger.h"
#include <QDateTime>
#include <QMutexLocker>
#include <QDebug>
#include <iostream>

namespace Nuts {

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
}

void Logger::setLogFile(const QString& path) {
    QMutexLocker locker(&m_mutex);

    if (m_logFile.isOpen()) {
        m_logFile.close();
    }

    m_logFilePath = path;
    openLogFile();
}

void Logger::openLogFile() {
    if (m_logFilePath.isEmpty()) {
        return;
    }

    m_logFile.setFileName(m_logFilePath);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Failed to open log file:" << m_logFilePath;
    }
}

QString Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug: return QStringLiteral("Debug");
        case LogLevel::Info: return QStringLiteral("Info");
        case LogLevel::Success: return QStringLiteral("Success");
        case LogLevel::Warning: return QStringLiteral("Warning");
        case LogLevel::Error: return QStringLiteral("Error");
        default: return QStringLiteral("Unknown");
    }
}

QString Logger::levelToColor(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug: return QStringLiteral("\033[36m");     // Cyan
        case LogLevel::Info: return QStringLiteral("\033[34m");      // Blue
        case LogLevel::Success: return QStringLiteral("\033[32m");   // Green
        case LogLevel::Warning: return QStringLiteral("\033[33m");   // Yellow
        case LogLevel::Error: return QStringLiteral("\033[31m");     // Red
        default: return QStringLiteral("\033[0m");                   // Reset
    }
}

void Logger::log(LogLevel level, const QString& message) {
    QMutexLocker locker(&m_mutex);

    QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    QString levelStr = levelToString(level);
    QString logEntry = QStringLiteral("[%1] %2: %3").arg(timestamp, levelStr, message);

    // Write to console with color
    QString coloredOutput = QStringLiteral("%1%2:\033[0m %3")
                               .arg(levelToColor(level), levelStr, message);

    if (level == LogLevel::Error) {
        std::cerr << coloredOutput.toStdString() << std::endl;
    } else {
        std::cout << coloredOutput.toStdString() << std::endl;
    }

    // Write to log file
    if (m_logFile.isOpen()) {
        QTextStream out(&m_logFile);
        out << logEntry << "\n";
        m_logFile.flush();
    }

    // Emit signal for GUI
    Q_EMIT logMessage(level, message);
}

} // namespace Nuts
