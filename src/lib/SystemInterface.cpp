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
#include <QRegularExpression>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace Nuts {

SystemInterface::SystemInterface(QObject* parent)
    : QObject(parent) {
}

bool SystemInterface::executeCommand(const QString& program, const QStringList& arguments,
                                     QString& output, QString& error, int timeout) {
    // Log the full command being run for troubleshooting
    QString cmdLine = program + (arguments.isEmpty() ? QString() : " " + arguments.join(' '));
    Logger::instance().debug("EXEC: " + cmdLine);

    QProcess process;
    process.start(program, arguments);

    if (!process.waitForFinished(timeout)) {
        error = "Command timed out";
        process.kill();
        Logger::instance().debug("EXEC TIMEOUT: " + cmdLine);
        return false;
    }

    output = QString::fromUtf8(process.readAllStandardOutput());
    error = QString::fromUtf8(process.readAllStandardError());

    int exitCode = process.exitCode();
    if (!output.trimmed().isEmpty())
        Logger::instance().debug("STDOUT: " + output.trimmed());
    if (!error.trimmed().isEmpty())
        Logger::instance().debug("STDERR: " + error.trimmed());
    Logger::instance().debug(QString("EXIT(%1): %2").arg(exitCode).arg(cmdLine));

    return exitCode == 0;
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
    if (executeCommand("/usr/sbin/blkid", {"-o", "value", "-s", "LABEL", device}, output, error)) {
        return output.trimmed();
    }
    return QString();
}

QString SystemInterface::getPartitionFilesystem(const QString& device) {
    QString output, error;
    if (executeCommand("/usr/sbin/blkid", {"-o", "value", "-s", "TYPE", device}, output, error)) {
        return output.trimmed();
    }
    return QString();
}

bool SystemInterface::isOverlayActive() {
    QString output, error;
    return executeCommand("/usr/bin/mount", {}, output, error) && output.contains("overlayroot");
}

bool SystemInterface::checkInternetConnectivity() {
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
    return executeCommand("/usr/bin/mountpoint", {"-q", mountPoint}, output, error);
}

bool SystemInterface::executeInOverlay(const QStringList& command, QString& output, QString& error) {
    QString program = "/usr/sbin/overlayroot-chroot";
    return executeCommand(program, command, output, error, 600000);
}

bool SystemInterface::executeInOverlayAgent(const QStringList& command,
                                            QString& output, QString& error,
                                            int timeout) {
    // One long-running call that executes nuts-agent inside the chroot.
    // The extended timeout (default 2 hours) covers the full OTA pipeline:
    // partition mounting, multi-gigabyte download, dpkg unpack + configure.
    Logger::instance().info(
        QString("Launching nuts-agent in chroot (timeout %1s)").arg(timeout / 1000));
    return executeCommand("/usr/sbin/overlayroot-chroot", command, output, error, timeout);
}

bool SystemInterface::downloadFile(const QString& url, const QString& destination) {
    Logger::instance().info("Downloading: " + url);

    // Validate URL scheme (only allow HTTP/HTTPS)
    if (!validateURL(url)) {
        Logger::instance().error("SECURITY: Download blocked - invalid URL");
        return false;
    }

    // Prefer axel for multi-connection downloading.
    if (QFile::exists("/usr/bin/axel")) {
        // Always remove any stale destination before axel writes it.
        QFile::remove(destination);
        QString output, error;
        bool ok = executeCommand("/usr/bin/axel",
                                 {"-n", "10", "-o", destination, url},
                                 output, error,
                                 3600000);
        if (ok && QFile::exists(destination)) {
            Logger::instance().success("Downloaded: " + QFileInfo(destination).fileName());
            return true;
        }
        Logger::instance().warning("axel download failed, falling back to QNetworkAccessManager");
        QFile::remove(destination);
    }

    // Fallback: single-connection streaming via QNetworkAccessManager.
    QFile::remove(destination + ".partial");

    QNetworkAccessManager manager;

    QUrl qurl(url);
    QNetworkRequest request(qurl);
    request.setTransferTimeout(600000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QString partialPath = destination + ".partial";
    QFile outFile(partialPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        Logger::instance().error("Failed to open partial file for writing: " + partialPath);
        return false;
    }

    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(600000);

    bool writeError = false;

    // Stream data to disk as it arrives — never buffer the full file in memory
    connect(reply, &QNetworkReply::readyRead, this, [&]() {
        QByteArray chunk = reply->read(1024 * 1024); // 1 MB chunks
        if (outFile.write(chunk) != chunk.size()) {
            Logger::instance().error("Disk write error while downloading");
            writeError = true;
            loop.quit();
        }
    });

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 bytesReceived, qint64 totalBytes) {
        int percentage = (totalBytes > 0) ? (bytesReceived * 100 / totalBytes) : 0;
        Q_EMIT downloadProgress(percentage, bytesReceived, totalBytes);
    });

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    outFile.close();

    bool result = false;

    if (!writeError && reply->error() == QNetworkReply::NoError && timeoutTimer.isActive()) {
        if (QFile::exists(destination)) {
            QFile::remove(destination);
        }
        if (QFile::rename(partialPath, destination)) {
            result = true;
            Logger::instance().success("Downloaded: " + QFileInfo(destination).fileName());
        } else {
            Logger::instance().error("Failed to rename partial file to: " + destination);
        }
    } else {
        QString err = reply->errorString();
        if (!timeoutTimer.isActive()) {
            err = "Download timed out";
        }
        if (!writeError) {
            Logger::instance().error("Download failed: " + err);
        }
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

int SystemInterface::testMirrorLatency(const QString& url, int timeout) {
    QNetworkAccessManager manager;
    QUrl qurl(url);
    QNetworkRequest request(qurl);
    request.setTransferTimeout(timeout);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    // Use HEAD request for minimal data transfer
    QElapsedTimer timer;
    timer.start();

    QNetworkReply* reply = manager.head(request);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(timeout);

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeoutTimer.start();
    loop.exec();

    int latency = -1; // -1 indicates failure/timeout

    if (reply->error() == QNetworkReply::NoError && timeoutTimer.isActive()) {
        latency = timer.elapsed();
    }

    timeoutTimer.stop();
    reply->deleteLater();

    return latency;
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
    QByteArray encodedPath = QFile::encodeName(path);
    const char* pathStr = encodedPath.constData();

    // Open file descriptor, failing if it's a symlink (O_NOFOLLOW)
    int fd = ::open(pathStr, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd == -1) {
        Logger::instance().error("SECURITY: Failed to open directory securely (possible symlink or non-existent): " + path);
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) == -1) {
        Logger::instance().error("Failed to fstat directory: " + path);
        ::close(fd);
        return false;
    }

    // Ensure ownership is root (0). Use fchown on the file descriptor.
    if (st.st_uid != 0) {
        Logger::instance().warning("Directory not owned by root! Attempting to seize ownership: " + path);
        
        if (::fchown(fd, 0, 0) != 0) {
             Logger::instance().error("SECURITY FAILURE: Failed to claim ownership of directory. Aborting.");
             ::close(fd);
             return false;
        }
    }

    // Set permissions to 0700 (owner read/write/execute only) using fchmod
    if (::fchmod(fd, S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
        Logger::instance().error("Failed to set secure permissions on: " + path);
        ::close(fd);
        return false;
    }

    ::close(fd);
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

bool SystemInterface::validatePath(const QString& path, const QString& allowedPrefix) {
    // Check for path traversal sequences
    if (path.contains("../") || path.contains("..\\")) {
        Logger::instance().error("SECURITY: Path traversal sequence detected: " + path);
        return false;
    }

    // Get canonical path to resolve any symlinks or relative components
    QFileInfo fileInfo(path);
    QString canonicalPath = fileInfo.canonicalFilePath();

    // If file doesn't exist yet, get canonical path of parent directory
    if (canonicalPath.isEmpty()) {
        QDir parentDir = fileInfo.dir();
        QString canonicalParent = parentDir.canonicalPath();
        if (!canonicalParent.isEmpty()) {
            canonicalPath = canonicalParent + "/" + fileInfo.fileName();
        } else {
            // If parent doesn't exist, use absolute path
            canonicalPath = fileInfo.absoluteFilePath();
        }
    }

    // Ensure path doesn't escape allowed directory
    if (!allowedPrefix.isEmpty() && !canonicalPath.startsWith(allowedPrefix)) {
        Logger::instance().error("SECURITY: Path escapes allowed directory: " + path);
        Logger::instance().error("Canonical: " + canonicalPath + ", Allowed: " + allowedPrefix);
        return false;
    }

    return true;
}

bool SystemInterface::validateURL(const QString& url) {
    QUrl qurl(url);

    if (!qurl.isValid()) {
        Logger::instance().error("SECURITY: Invalid URL: " + url);
        return false;
    }

    // Only allow HTTP and HTTPS protocols
    QString scheme = qurl.scheme().toLower();
    if (scheme != "http" && scheme != "https") {
        Logger::instance().error("SECURITY: Invalid URL scheme (only http/https allowed): " + scheme);
        return false;
    }

    // Prevent access to localhost and internal networks (optional based on requirements)
    QString host = qurl.host().toLower();
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" ||
        host.startsWith("192.168.") || host.startsWith("10.") ||
        host.startsWith("172.16.") || host.startsWith("172.17.") ||
        host.startsWith("172.18.") || host.startsWith("172.19.") ||
        host.startsWith("172.20.") || host.startsWith("172.21.") ||
        host.startsWith("172.22.") || host.startsWith("172.23.") ||
        host.startsWith("172.24.") || host.startsWith("172.25.") ||
        host.startsWith("172.26.") || host.startsWith("172.27.") ||
        host.startsWith("172.28.") || host.startsWith("172.29.") ||
        host.startsWith("172.30.") || host.startsWith("172.31.") ||
        host.startsWith("169.254.")) {
        Logger::instance().error("SECURITY: Internal network URL blocked: " + url);
        return false;
    }

    return true;
}

QString SystemInterface::sanitizeVersionString(const QString& version) {
    // Only allow alphanumeric characters, dots, hyphens, and underscores
    QRegularExpression validChars("^[a-zA-Z0-9._-]+$");

    if (!validChars.match(version).hasMatch()) {
        Logger::instance().error("SECURITY: Invalid version string contains disallowed characters: " + version);
        return QString();
    }

    // Additional check: prevent excessive length
    if (version.length() > 64) {
        Logger::instance().error("SECURITY: Version string too long: " + version);
        return QString();
    }

    return version;
}

} // namespace Nuts
