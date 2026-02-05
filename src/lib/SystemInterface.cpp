// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "nuts/SystemInterface.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QStorageInfo>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QFileInfo>
#include <unistd.h> // Required for chown

namespace Nuts {

SystemInterface::SystemInterface(QObject* parent)
    : QObject(parent) {
    // m_networkManager removed: We now use local instances for thread safety
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

    // Use absolute path for findmnt
    if (executeCommand("/usr/bin/findmnt", {"-n", "-o", "SOURCE", "/media/root-ro"}, output, error)) {
        return output.trimmed();
    }

    if (executeCommand("/usr/bin/findmnt", {"-n", "-o", "SOURCE", "/"}, output, error)) {
        return output.trimmed();
    }

    return QString();
}

QString SystemInterface::getPartitionLabel(const QString& device) {
    QString output, error;
    // Use absolute path for blkid
    if (executeCommand("/usr/sbin/blkid", {"-o", "value", "-s", "LABEL", device}, output, error)) {
        return output.trimmed();
    }
    return QString();
}

QString SystemInterface::getPartitionFilesystem(const QString& device) {
    QString output, error;
    // Use absolute path for blkid
    if (executeCommand("/usr/sbin/blkid", {"-o", "value", "-s", "TYPE", device}, output, error)) {
        return output.trimmed();
    }
    return QString();
}

bool SystemInterface::isOverlayActive() {
    QString output, error;
    // Use absolute path for mount
    return executeCommand("/usr/bin/mount", {}, output, error) && output.contains("overlayroot");
}

bool SystemInterface::checkInternetConnectivity() {
    // Thread Safety: Create QNAM locally
    QNetworkAccessManager manager;
    
    QUrl url(Config::instance().connectivityCheckUrl());
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = manager.head(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(15000);

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    bool result = (reply->error() == QNetworkReply::NoError && timeoutTimer.isActive());
    timeoutTimer.stop();
    reply->deleteLater();

    if (result) {
        Logger::instance().success("Internet connectivity check passed");
    } else {
        Logger::instance().error("Internet connectivity check failed");
    }

    return result;
}

bool SystemInterface::checkGitHubConnectivity() {
    // Thread Safety: Create QNAM locally
    QNetworkAccessManager manager;

    QUrl url(Config::instance().githubConnectivityCheckUrl());
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = manager.head(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(15000);

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    bool result = (reply->error() == QNetworkReply::NoError && timeoutTimer.isActive());
    timeoutTimer.stop();
    reply->deleteLater();

    if (result) {
        Logger::instance().success("GitHub connectivity check passed");
    } else {
        Logger::instance().error("GitHub connectivity check failed");
    }

    return result;
}

bool SystemInterface::mountPartition(const QString& device, const QString& mountPoint) {
    QString output, error;

    if (!directoryExists(mountPoint)) {
        if (!createDirectory(mountPoint)) {
            Logger::instance().error("Failed to create mount point: " + mountPoint);
            return false;
        }
    }

    // Use absolute path for mount
    bool result = executeCommand("/usr/bin/mount", {"-t", "auto", device, mountPoint}, output, error);

    if (result) {
        Logger::instance().info("Mounted " + device + " to " + mountPoint);
    } else {
        Logger::instance().error("Failed to mount " + device + ": " + error);
    }

    return result;
}

bool SystemInterface::unmountPartition(const QString& mountPoint) {
    QString output, error;
    // Use absolute path for umount
    bool result = executeCommand("/usr/bin/umount", {mountPoint}, output, error);

    if (result) {
        Logger::instance().info("Unmounted " + mountPoint);
    } else {
        Logger::instance().error("Failed to unmount " + mountPoint + ": " + error);
    }

    return result;
}

bool SystemInterface::isMounted(const QString& mountPoint) {
    QString output, error;
    // Use absolute path for mountpoint
    return executeCommand("/usr/bin/mountpoint", {"-q", mountPoint}, output, error);
}

bool SystemInterface::executeInOverlay(const QStringList& command, QString& output, QString& error) {
    // Use absolute path for overlayroot-chroot
    QString program = "/usr/sbin/overlayroot-chroot";
    return executeCommand(program, command, output, error);
}

bool SystemInterface::downloadFile(const QString& url, const QString& destination) {
    Logger::instance().info("Downloading: " + url);

    // Thread Safety: Create QNAM locally
    QNetworkAccessManager manager;

    QUrl qurl(url);
    QNetworkRequest request(qurl);
    request.setTransferTimeout(600000); 
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(600000);

    qint64 lastBytesReceived = 0;
    qint64 bytesTotal = 0;

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, &lastBytesReceived, &bytesTotal](qint64 bytesReceived, qint64 totalBytes) {
        lastBytesReceived = bytesReceived;
        bytesTotal = totalBytes;

        int percentage = (totalBytes > 0) ? (bytesReceived * 100 / totalBytes) : 0;
        Q_EMIT downloadProgress(percentage, bytesReceived, totalBytes);
    });

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    bool result = false;

    if (reply->error() == QNetworkReply::NoError && timeoutTimer.isActive()) {
        QFile file(destination);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(reply->readAll());
            file.close();
            result = true;
            Logger::instance().success("Downloaded: " + QFileInfo(destination).fileName());
        } else {
            Logger::instance().error("Failed to write file: " + destination);
        }
    } else {
        QString error = reply->errorString();
        if (!timeoutTimer.isActive()) {
            error = "Download timed out";
        }
        Logger::instance().error("Download failed: " + error);
    }

    timeoutTimer.stop();
    reply->deleteLater();

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

QString SystemInterface::calculateSHA256(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Logger::instance().error("Failed to open file for SHA256: " + filePath);
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&file)) {
        return QString(hash.result().toHex());
    }

    return QString();
}

bool SystemInterface::verifyChecksum(const QString& filePath, const QString& expectedChecksum) {
    QString actualChecksum;

    if (expectedChecksum.length() == 64) {
        actualChecksum = calculateSHA256(filePath);
    } else if (expectedChecksum.length() == 32) {
        actualChecksum = calculateMD5(filePath);
    } else {
        Logger::instance().error("Invalid checksum format (expected 32 or 64 hex chars)");
        return false;
    }

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

bool SystemInterface::verifyGPGSignature(const QString& dataFile, const QString& signatureFile) {
    const QString publicKeyRing = "/usr/share/nuts/keys/nitrux-updates.gpg";

    if (!QFile::exists(publicKeyRing)) {
        Logger::instance().error("CRITICAL: Public keyring not found at " + publicKeyRing);
        Logger::instance().error("Cannot verify update authenticity without the public key.");
        return false;
    }

    // Use absolute path for gpgv to prevent PATH injection
    QString program = "/usr/bin/gpgv";
    
    QStringList args;
    args << "--keyring" << publicKeyRing
         << signatureFile
         << dataFile;

    QString output, error;
    bool result = executeCommand(program, args, output, error);

    if (result) {
        Logger::instance().success("GPG signature verified successfully for " + QFileInfo(dataFile).fileName());
    } else {
        Logger::instance().error("SECURITY ALERT: GPG signature verification FAILED");
        Logger::instance().error("GPGv Output: " + error);
    }

    return result;
}

qint64 SystemInterface::getRemoteFileSize(const QString& url) {
    // Thread Safety: Create QNAM locally
    QNetworkAccessManager manager;

    QUrl qurl(url);
    QNetworkRequest request(qurl);
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = manager.head(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(15000);

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    qint64 fileSize = 0;

    if (reply->error() == QNetworkReply::NoError && timeoutTimer.isActive()) {
        fileSize = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    }

    timeoutTimer.stop();
    reply->deleteLater();

    return fileSize;
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

bool SystemInterface::createSecureDirectory(const QString& path) {
    QDir dir;
    // mkpath returns true if directory exists or was created
    if (!dir.mkpath(path)) {
        Logger::instance().error("Failed to create secure directory: " + path);
        return false;
    }
    
    // Immediately enforce permissions and ownership
    return enforceSecurePermissions(path);
}

bool SystemInterface::enforceSecurePermissions(const QString& path) {
    QFileInfo info(path);
    if (!info.exists()) {
        Logger::instance().error("Cannot enforce permissions on non-existent path: " + path);
        return false;
    }

    // CRITICAL: Ensure ownership is root (0)
    if (info.ownerId() != 0) {
        Logger::instance().warning("Directory not owned by root! Attempting to seize ownership: " + path);
        
        // chown(path, owner, group) - 0 is root
        if (::chown(QFile::encodeName(path).constData(), 0, 0) != 0) {
             Logger::instance().error("SECURITY FAILURE: Failed to claim ownership of directory. Aborting.");
             return false;
        }
    }

    // Set permissions to 0700 (owner read/write/execute only)
    QFile::Permissions perms = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    if (!QFile::setPermissions(path, perms)) {
        Logger::instance().error("Failed to set secure permissions on: " + path);
        return false;
    }

    Logger::instance().info("Enforced secure permissions (0700, root:root) on: " + path);
    return true;
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
