// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "nuts/SystemInterface.h"
#include "nuts/Logger.h"
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QStorageInfo>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>

namespace Nuts {

SystemInterface::SystemInterface(QObject* parent)
    : QObject(parent) {
}

bool SystemInterface::executeCommand(const QString& program, const QStringList& arguments,
                                     QString& output, QString& error, int timeout) {
    QProcess process;
    process.start(program, arguments);

    if (!process.waitForFinished(timeout)) {
        error = "Command timed out";
        process.kill();
        return false;
    }

    output = QString::fromUtf8(process.readAllStandardOutput());
    error = QString::fromUtf8(process.readAllStandardError());

    return process.exitCode() == 0;
}

SystemInfo SystemInterface::getSystemInfo() {
    SystemInfo info;

    // Read /etc/lsb-release
    QFile lsbFile("/etc/lsb-release");
    if (lsbFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&lsbFile);
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.startsWith("DISTRIB_ID=")) {
                info.distribution = line.mid(11).remove('"');
            } else if (line.startsWith("DISTRIB_RELEASE=")) {
                info.version = line.mid(16).remove('"');
            }
        }
        lsbFile.close();
    }

    info.rootPartition = getRootPartition();
    if (!info.rootPartition.isEmpty()) {
        info.rootLabel = getPartitionLabel(info.rootPartition);
        info.rootFilesystem = getPartitionFilesystem(info.rootPartition);
    }

    info.overlayActive = isOverlayActive();

    return info;
}

QString SystemInterface::getRootPartition() {
    QString output, error;

    // Try /media/root-ro first (when overlay is active)
    if (executeCommand("findmnt", {"-n", "-o", "SOURCE", "/media/root-ro"}, output, error)) {
        return output.trimmed();
    }

    // Fallback to root
    if (executeCommand("findmnt", {"-n", "-o", "SOURCE", "/"}, output, error)) {
        return output.trimmed();
    }

    return QString();
}

QString SystemInterface::getPartitionLabel(const QString& device) {
    QString output, error;
    if (executeCommand("blkid", {"-o", "value", "-s", "LABEL", device}, output, error)) {
        return output.trimmed();
    }
    return QString();
}

QString SystemInterface::getPartitionFilesystem(const QString& device) {
    QString output, error;
    if (executeCommand("blkid", {"-o", "value", "-s", "TYPE", device}, output, error)) {
        return output.trimmed();
    }
    return QString();
}

bool SystemInterface::isOverlayActive() {
    QString output, error;
    return executeCommand("mount", {}, output, error) && output.contains("overlayroot");
}

bool SystemInterface::checkInternetConnectivity() {
    QString output, error;
    bool result = executeCommand("wget", {"-q", "--spider", "--timeout=10", "http://1.1.1.1"},
                                 output, error, 15000);

    if (result) {
        Logger::instance().success("Internet connectivity check passed");
    } else {
        Logger::instance().error("Internet connectivity check failed");
    }

    return result;
}

bool SystemInterface::checkGitHubConnectivity() {
    QString testUrl = "https://raw.githubusercontent.com/Nitrux/storage/refs/heads/master/Other/sample1.txt";
    QString output, error;

    bool result = executeCommand("wget", {"-q", "--spider", "--timeout=10", testUrl},
                                 output, error, 15000);

    if (result) {
        Logger::instance().success("GitHub connectivity check passed");
    } else {
        Logger::instance().error("GitHub connectivity check failed");
    }

    return result;
}

bool SystemInterface::mountPartition(const QString& device, const QString& mountPoint) {
    QString output, error;

    // Create mount point if it doesn't exist
    if (!directoryExists(mountPoint)) {
        if (!createDirectory(mountPoint)) {
            Logger::instance().error("Failed to create mount point: " + mountPoint);
            return false;
        }
    }

    bool result = executeCommand("mount", {"-t", "auto", device, mountPoint}, output, error);

    if (result) {
        Logger::instance().info("Mounted " + device + " to " + mountPoint);
    } else {
        Logger::instance().error("Failed to mount " + device + ": " + error);
    }

    return result;
}

bool SystemInterface::unmountPartition(const QString& mountPoint) {
    QString output, error;
    bool result = executeCommand("umount", {mountPoint}, output, error);

    if (result) {
        Logger::instance().info("Unmounted " + mountPoint);
    } else {
        Logger::instance().error("Failed to unmount " + mountPoint + ": " + error);
    }

    return result;
}

bool SystemInterface::isMounted(const QString& mountPoint) {
    QString output, error;
    return executeCommand("mountpoint", {"-q", mountPoint}, output, error);
}

bool SystemInterface::executeInOverlay(const QStringList& command, QString& output, QString& error) {
    QStringList args = command;
    args.prepend("overlayroot-chroot");

    return executeCommand("overlayroot-chroot", command, output, error);
}

bool SystemInterface::downloadFile(const QString& url, const QString& destination) {
    QString output, error;
    QStringList args = {"-q", "-n", "10", "-o", QFileInfo(destination).path(), url};

    Logger::instance().info("Downloading: " + url);

    bool result = executeCommand("axel", args, output, error, 600000); // 10 minute timeout

    if (result) {
        Logger::instance().success("Downloaded: " + QFileInfo(destination).fileName());
    } else {
        Logger::instance().error("Download failed: " + error);
    }

    return result;
}

QString SystemInterface::calculateMD5(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::instance().error("Failed to open file for checksum: " + filePath);
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Md5);
    if (hash.addData(&file)) {
        return QString(hash.result().toHex());
    }

    return QString();
}

bool SystemInterface::verifyChecksum(const QString& filePath, const QString& expectedChecksum) {
    QString actualChecksum = calculateMD5(filePath);

    if (actualChecksum.isEmpty()) {
        return false;
    }

    bool matches = actualChecksum.compare(expectedChecksum, Qt::CaseInsensitive) == 0;

    if (matches) {
        Logger::instance().success("Checksum verification passed for " + QFileInfo(filePath).fileName());
    } else {
        Logger::instance().error("Checksum verification failed for " + QFileInfo(filePath).fileName());
        Logger::instance().error("Expected: " + expectedChecksum);
        Logger::instance().error("Actual: " + actualChecksum);
    }

    return matches;
}

bool SystemInterface::createDirectory(const QString& path) {
    QDir dir;
    bool result = dir.mkpath(path);

    if (result) {
        Logger::instance().info("Created directory: " + path);
    } else {
        Logger::instance().error("Failed to create directory: " + path);
    }

    return result;
}

bool SystemInterface::directoryExists(const QString& path) {
    return QDir(path).exists();
}

qint64 SystemInterface::getDirectorySize(const QString& path) {
    qint64 totalSize = 0;
    QDir dir(path);

    if (!dir.exists()) {
        return 0;
    }

    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& entry : entries) {
        if (entry.isFile()) {
            totalSize += entry.size();
        } else if (entry.isDir()) {
            totalSize += getDirectorySize(entry.absoluteFilePath());
        }
    }

    return totalSize;
}

qint64 SystemInterface::getAvailableSpace(const QString& path) {
    QStorageInfo storage(path);
    return storage.bytesAvailable();
}

} // namespace Nuts
