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
        case LogLevel::Info: return "Info";
        case LogLevel::Success: return "Success";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error: return "Error";
        default: return "Unknown";
    }
}

QString Logger::levelToColor(LogLevel level) const {
    switch (level) {
        case LogLevel::Info: return "\033[34m";      // Blue
        case LogLevel::Success: return "\033[32m";   // Green
        case LogLevel::Warning: return "\033[33m";   // Yellow
        case LogLevel::Error: return "\033[31m";     // Red
        default: return "\033[0m";                   // Reset
    }
}

void Logger::log(LogLevel level, const QString& message) {
    QMutexLocker locker(&m_mutex);

    QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    QString levelStr = levelToString(level);
    QString logEntry = QString("[%1] %2: %3").arg(timestamp, levelStr, message);

    // Write to console with color
    QString coloredOutput = QString("%1%2:\033[0m %3")
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
