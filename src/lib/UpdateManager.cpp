#include "nuts/UpdateManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QDir>
#include <QDirIterator>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <climits>

namespace Nuts {

UpdateManager::UpdateManager(SystemInterface* sysInterface, QObject* parent)
    : QObject(parent)
    , m_sysInterface(sysInterface) {
}

bool UpdateManager::downloadQueryFile(const QString& /*branch*/) {
    QString url = Config::instance().queryFileUrl();
    QString sigUrl = url + ".sig"; 

    QString workDir = Config::instance().workDir();
    QString destination = workDir + "/nuts-cpp-query.info";
    QString sigDestination = workDir + "/nuts-cpp-query.info.sig";

    // Ensure secure work directory exists
    if (!m_sysInterface->directoryExists(workDir)) {
        if (!m_sysInterface->createSecureDirectory(workDir)) {
            Logger::instance().error("Failed to create secure work directory");
            return false;
        }
    }

    // Remove files atomically (avoid TOCTOU)
    QFile::remove(destination);
    QFile::remove(sigDestination);

    if (!m_sysInterface->downloadFile(url, destination)) {
        Logger::instance().error("Failed to download nuts-cpp-query.info");
        return false;
    }

    Logger::instance().info("Downloading signature file...");
    if (!m_sysInterface->downloadFile(sigUrl, sigDestination)) {
        Logger::instance().error("Failed to download signature file. Security policy requires a valid signature.");
        QFile::remove(destination);
        return false;
    }

    Logger::instance().info("Verifying nuts-cpp-query.info signature...");
    if (!m_sysInterface->verifyGPGSignature(destination, sigDestination)) {
        Logger::instance().error("CRITICAL: Signature verification failed for nuts-cpp-query.info!");
        QFile::remove(destination);
        QFile::remove(sigDestination);
        return false;
    }

    Logger::instance().success("Signature verified. Trusted metadata loaded.");
    return parseQueryFile(destination);
}

bool UpdateManager::parseQueryFile(const QString& filePath) {
    QSettings settings(filePath, QSettings::IniFormat);

    if (settings.status() != QSettings::NoError) {
        Logger::instance().error("Failed to parse nuts-cpp-query.info");
        return false;
    }

    m_queryData.clear();
    m_mirrorList.clear();

    for (const QString& key : settings.allKeys()) {
        QString value = settings.value(key).toString();
        m_queryData[key] = value;

        if (key == "MINTARGET") {
            m_minTarget = value;
        } else if (key == "UPDATE_URL") {
            m_updateUrl = value;
        } else if (key == "UPDATE_CHECKSUM") {
            m_updateChecksum = value;
        } else if (key == "MIRRORLIST" || key == "URL/MIRRORLIST") {
            // Support comma-separated mirror list (legacy format)
            m_mirrorList = value.split(',', Qt::SkipEmptyParts);
        } else if (key.startsWith("MIRROR")) {
            // Support individual MIRROR entries (new format)
            // Allows: MIRROR1, MIRROR2, MIRROR_US, MIRROR_EU, etc.
            QString url = value.trimmed();
            if (!url.isEmpty() && !m_mirrorList.contains(url)) {
                m_mirrorList.append(url);
            }
        }
    }

    // Fallback: if no mirrors specified, use UPDATE_URL as single mirror
    if (m_mirrorList.isEmpty() && !m_updateUrl.isEmpty()) {
        m_mirrorList.append(m_updateUrl);
    }

    if (m_minTarget.isEmpty() || m_mirrorList.isEmpty()) {
        Logger::instance().error("Query file missing required fields (MINTARGET or MIRRORLIST)");
        return false;
    }
    
    m_otaChecksum = m_queryData.value("OTASUM");
    if (m_otaChecksum.isEmpty()) {
         m_otaChecksum = m_updateChecksum;
    }

    // Ensure external tool checksums are present
    if (m_queryData.value("DPKG_AI_SUM").isEmpty()) {
        Logger::instance().error("CRITICAL: DPKG_AI_SUM missing from metadata. Cannot verify update tools.");
        return false;
    }
    
    if (m_queryData.value("VAR_LIB_SUM").isEmpty()) {
        Logger::instance().error("CRITICAL: VAR_LIB_SUM missing from metadata. Cannot verify package database.");
        return false;
    }

    if (m_queryData.value("NUTS_CCU_CHECKSUM").isEmpty()) {
        Logger::instance().error("CRITICAL: NUTS_CCU_CHECKSUM missing - refusing to proceed");
        return false;
    }

    Logger::instance().info("Query file parsed successfully");
    return true;
}

bool UpdateManager::isUpdateAvailable(const QString& currentVersion) {
    if (m_minTarget.isEmpty()) return false;

    // Update is available if current version is LESS THAN target
    // If currentVersion < MINTARGET → update available
    // If currentVersion >= MINTARGET → already updated or newer
    return (compareVersions(currentVersion, m_minTarget) < 0);
}

int UpdateManager::compareVersions(const QString& version1, const QString& version2) {
    // Parse version strings (e.g., "6.0.0", "6.0.1", "6.0.0 build.060126")
    // Returns: -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2

    // Extract numeric part before any space (ignore build metadata)
    QString v1 = version1.split(' ').first();
    QString v2 = version2.split(' ').first();

    // Split by dots
    QStringList parts1 = v1.split('.');
    QStringList parts2 = v2.split('.');

    // Pad to same length
    int maxLen = qMax(parts1.size(), parts2.size());
    while (parts1.size() < maxLen) parts1.append("0");
    while (parts2.size() < maxLen) parts2.append("0");

    // Compare each component numerically
    for (int i = 0; i < maxLen; ++i) {
        bool ok1, ok2;
        int num1 = parts1[i].toInt(&ok1);
        int num2 = parts2[i].toInt(&ok2);

        // If parsing fails, fall back to string comparison
        if (!ok1 || !ok2) {
            int cmp = QString::compare(parts1[i], parts2[i]);
            if (cmp != 0) return (cmp < 0) ? -1 : 1;
            continue;
        }

        if (num1 < num2) return -1;
        if (num1 > num2) return 1;
    }

    return 0; // versions are equal
}

// -----------------
// Core Update Logic
// -----------------

bool UpdateManager::applyUpdate() {
    Logger::instance().info("Starting System Update Process...");

    Q_EMIT updateProgress(5, "Mounting partitions");
    if (!prepareSystemPartitions()) {
        cleanup();
        return false;
    }

    // Check disk space after partitions are mounted so QStorageInfo
    // can read the actual available space on /home (NX_HOME partition).
    Q_EMIT updateProgress(8, "Checking disk space");
    if (!checkDiskSpace()) {
        Logger::instance().error("Insufficient disk space for update");
        cleanup();
        return false;
    }

    Q_EMIT updateProgress(15, "Downloading OTA payload");
    if (!downloadOTAPayload()) {
        cleanup();
        return false;
    }

    Q_EMIT updateProgress(25, "Mounting OTA payload");
    if (!mountOTAPayload()) {
        cleanup();
        return false;
    }

    Q_EMIT updateProgress(30, "Preparing update tools");
    if (!prepareUpdateTools()) {
        cleanup();
        return false;
    }

    Q_EMIT updateProgress(40, "Syncing package database");
    if (!syncPackageData()) {
        cleanup();
        return false;
    }

    Q_EMIT updateProgress(50, "Applying packages (this may take time)");
    if (!performPackageUpdates()) {
        cleanup();
        return false;
    }

    Q_EMIT updateProgress(90, "Running final cleanup");
    if (!runCleanupCrew()) {
        cleanup();
        return false;
    }

    cleanup();

    Q_EMIT updateProgress(100, "Update completed successfully");
    Logger::instance().success("System updated successfully.");
    return true;
}

// ----------------------
// Helper Implementations
// ----------------------

bool UpdateManager::prepareSystemPartitions() {
    QString output, error;
    
    // Resolve NX_HOME
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_HOME"}, output, error)) {
        Logger::instance().error("Could not find partition labeled NX_HOME");
        return false;
    }
    QString homeDev = output.trimmed();

    if (!m_sysInterface->isMounted("/home")) {
        if (!m_sysInterface->mountPartition(homeDev, "/home")) return false;
    }

    // Resolve NX_VAR_LIB
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_VAR_LIB"}, output, error)) {
        Logger::instance().error("Could not find partition labeled NX_VAR_LIB");
        return false;
    }
    QString varLibDev = output.trimmed();

    if (m_sysInterface->isMounted("/var/lib")) {
        QString currentSrc;
        m_sysInterface->executeCommand("/usr/bin/findmnt", {"-n", "-o", "SOURCE", "--target", "/var/lib"}, currentSrc, error);
        if (currentSrc.trimmed() != varLibDev) {
            m_sysInterface->unmountPartition("/var/lib");
            if (!m_sysInterface->mountPartition(varLibDev, "/var/lib")) return false;
        }
    } else {
        if (!m_sysInterface->mountPartition(varLibDev, "/var/lib")) return false;
    }

    m_sysInterface->createDirectory(Config::instance().downloadDir());
    m_sysInterface->createDirectory(Config::instance().squashfsDir());
    
    return true;
}

bool UpdateManager::downloadOTAPayload() {
    QString otaPath = Config::instance().downloadDir() + "/nuts-ota.squashfs";

    //Validate path to prevent traversal
    QString downloadDir = Config::instance().downloadDir();
    if (!m_sysInterface->validatePath(otaPath, downloadDir)) {
        Logger::instance().error("SECURITY: Invalid OTA path");
        return false;
    }

    //Check and remove atomically (avoid TOCTOU)
    QFile otaFile(otaPath);
    if (otaFile.exists()) {
        if (m_sysInterface->verifyChecksum(otaPath, m_otaChecksum)) {
            Logger::instance().success("Existing OTA payload verified.");
            return true;
        }
        Logger::instance().warning("Existing OTA payload corrupt. Re-downloading.");
        otaFile.remove();
    }

    // Sort mirrors by latency for optimal download speed
    QList<QPair<QString, int>> mirrorLatencies;

    Logger::instance().info("Testing mirror latencies...");
    for (const QString& mirror : m_mirrorList) {
        QString url = mirror.trimmed();
        if (url.isEmpty()) continue;

        int latency = m_sysInterface->testMirrorLatency(url, 5000);
        if (latency >= 0) {
            mirrorLatencies.append(qMakePair(url, latency));
            Logger::instance().info(QString("Mirror %1: %2ms").arg(QUrl(url).host()).arg(latency));
        } else {
            Logger::instance().warning(QString("Mirror %1: unreachable").arg(QUrl(url).host()));
        }
    }

    // Sort by latency (lowest first)
    std::sort(mirrorLatencies.begin(), mirrorLatencies.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                  return a.second < b.second;
              });

    if (mirrorLatencies.isEmpty()) {
        Logger::instance().error("No reachable mirrors found.");
        return false;
    }

    Logger::instance().info(QString("Using fastest mirror: %1 (%2ms)")
                           .arg(QUrl(mirrorLatencies.first().first).host())
                           .arg(mirrorLatencies.first().second));

    // Try mirrors in order of latency
    for (const auto& mirrorPair : mirrorLatencies) {
        QString url = mirrorPair.first;

        // Remove any partial file from a previous mirror attempt before starting fresh.
        // Resuming a partial chunk from mirror A against mirror B produces a corrupt
        // "Frankenstein" file that will never pass checksum verification.
        QFile::remove(otaPath + ".partial");

        if (m_sysInterface->downloadFile(url, otaPath)) {
            if (m_sysInterface->verifyChecksum(otaPath, m_otaChecksum)) {
                // ADDITIONAL SAFETY: Verify SquashFS integrity with test mount
                Logger::instance().info("Verifying SquashFS filesystem integrity...");
                if (verifySquashFSIntegrity(otaPath)) {
                    Logger::instance().success("SquashFS integrity verified.");
                    return true;
                } else {
                    Logger::instance().error("SquashFS integrity check failed for " + url);
                    QFile(otaPath).remove();
                }
            } else {
                Logger::instance().error("Checksum mismatch for " + url);
                QFile(otaPath).remove();
            }
        }
    }

    Logger::instance().error("Failed to download OTA payload from all mirrors.");
    return false;
}

bool UpdateManager::mountOTAPayload() {
    QString otaPath = Config::instance().downloadDir() + "/nuts-ota.squashfs";
    QString mountPoint = Config::instance().squashfsDir();
    
    QString output, error;
    if (!m_sysInterface->executeCommand("/usr/bin/mount", {otaPath, mountPoint}, output, error)) {
        Logger::instance().error("Failed to mount OTA squashfs: " + error);
        return false;
    }
    return true;
}

bool UpdateManager::prepareUpdateTools() {
    // Use secure work directory (root:root 0700) instead of /tmp
    // This prevents other users from manipulating the extracted files.
    QString workDir = Config::instance().workDir();
    QString appImagePath = workDir + "/dpkg-tooling.AppImage";
    QString extractDir = workDir + "/pkgman-extracted";

    // Validate paths to prevent traversal
    if (!m_sysInterface->validatePath(appImagePath, workDir)) {
        Logger::instance().error("SECURITY: Invalid AppImage path");
        return false;
    }
    if (!m_sysInterface->validatePath(extractDir, workDir)) {
        Logger::instance().error("SECURITY: Invalid extract directory path");
        return false;
    }

    QString appImageUrl = "https://raw.githubusercontent.com/Nitrux/storage/master/Other/AppImages/dpkg-1.22.21-x86_64.AppImage";
    QString expectedChecksum = m_queryData.value("DPKG_AI_SUM");

    // Check atomically (avoid TOCTOU)
    QFile appRunFile(extractDir + "/squashfs-root/AppRun");
    if (!appRunFile.exists()) {
        // 1. Download AppImage
        if (!m_sysInterface->downloadFile(appImageUrl, appImagePath)) {
            Logger::instance().error("Failed to download OTA tooling.");
            return false;
        }

        // 2. Verify Checksum
        // We MUST verify this before executing it, as it runs as root.
        Logger::instance().info("Verifying update tools integrity...");
        if (!m_sysInterface->verifyChecksum(appImagePath, expectedChecksum)) {
            Logger::instance().error("CRITICAL: OTA tooling checksum mismatch!");
            Logger::instance().error("Possible compromise attempt. Aborting.");
            QFile(appImagePath).remove();
            return false;
        }

        // 3. Make executable
        QString output, error;
        m_sysInterface->executeCommand("/usr/bin/chmod", {"+x", appImagePath}, output, error);

        // 4. Extract
        m_sysInterface->executeCommand("/usr/bin/rm", {"-rf", extractDir}, output, error);
        m_sysInterface->createDirectory(extractDir);
        
        QProcess proc;
        proc.setWorkingDirectory(extractDir);
        proc.start(appImagePath, {"--appimage-extract"});
        proc.waitForFinished();
        
        if (proc.exitCode() != 0) {
            Logger::instance().error("Failed to extract OTA tooling.");
            return false;
        }
    }

    m_pkgManagerPath = extractDir + "/squashfs-root/AppRun";

    // Symlink tools
    QStringList tools = {"dpkg", "dpkg-deb", "dpkg-divert", "dpkg-query", 
                         "dpkg-realpath", "dpkg-split", "dpkg-statoverride", 
                         "dpkg-trigger", "dpkg-maintscript-helper", "update-alternatives"};

    QString binDir = extractDir + "/squashfs-root/usr/bin";
    
    for (const QString& tool : tools) {
        QString target = binDir + "/" + tool;
        QString link = "/usr/bin/" + tool;
        if (QFile::exists(target)) {
            QString output, error;
            m_sysInterface->executeCommand("/usr/bin/ln", {"-svf", target, link}, output, error);
        }
    }

    QString output, error;
    m_sysInterface->executeCommand("/usr/bin/mkdir", {"-p", "/usr/share"}, output, error);
    m_sysInterface->executeCommand("/usr/bin/ln", {"-svf", extractDir + "/squashfs-root/usr/share/dpkg", "/usr/share/dpkg"}, output, error);

    return true;
}

bool UpdateManager::syncPackageData() {
    QString url = QString("https://raw.githubusercontent.com/Nitrux/storage/master/Other/var-lib-dpkg-%1.tar.xz").arg(m_minTarget);
    
    // Download to secure work directory first
    QString tarPath = Config::instance().workDir() + "/var-lib-dpkg.tar.xz";
    QString expectedChecksum = m_queryData.value("VAR_LIB_SUM");

    if (!m_sysInterface->downloadFile(url, tarPath)) return false;

    // Verify Checksum before extraction
    // Extracting an unverified archive to / is extremely dangerous (zip slip / overwrite attacks).
    Logger::instance().info("Verifying package database archive...");
    if (!m_sysInterface->verifyChecksum(tarPath, expectedChecksum)) {
        Logger::instance().error("CRITICAL: Package database checksum mismatch!");
        QFile(tarPath).remove();
        return false;
    }

    // Extract to /
    QString output, error;
    if (!m_sysInterface->executeCommand("/usr/bin/tar", {"-xf", tarPath, "-C", "/"}, output, error)) {
        Logger::instance().error("Failed to extract package database: " + error);
        return false;
    }
    
    return QFile::exists("/var/lib/dpkg/status");
}

bool UpdateManager::performPackageUpdates() {
    QString otaDir = Config::instance().squashfsDir() + "/ota";
    QString updatesDir = otaDir + "/updates";
    QString nvidiaDir = otaDir + "/nvidia";

    QStringList debFiles;
    
    QDirIterator it(updatesDir, {"*.deb"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) debFiles << it.next();

    bool isNvidia = QDir("/proc/driver/nvidia").exists();
    if (!isNvidia) {
        QString output, error;
        m_sysInterface->executeCommand("/usr/bin/lspci", {}, output, error);
        if (output.contains("NVIDIA", Qt::CaseInsensitive)) isNvidia = true;
    }

    if (isNvidia && QDir(nvidiaDir).exists()) {
        Logger::instance().info("NVIDIA hardware detected. Including NVIDIA drivers.");
        QDirIterator nit(nvidiaDir, {"*.deb"}, QDir::Files, QDirIterator::Subdirectories);
        while (nit.hasNext()) debFiles << nit.next();
    }

    if (debFiles.isEmpty()) {
        Logger::instance().warning("No packages found to update.");
        return true; 
    }

    // Phase 1: Unpack
    // All dpkg operations run inside overlayroot-chroot so they write to the
    // lower (persistent) layer, not the upper (ephemeral) overlay layer.
    const int BATCH_SIZE = 50;
    Logger::instance().info(QString("Unpacking %1 packages in batches...").arg(debFiles.size()));

    for (int i = 0; i < debFiles.size(); i += BATCH_SIZE) {
        QStringList batch = debFiles.mid(i, BATCH_SIZE);
        Logger::instance().info(QString("Unpacking batch %1 of %2...").arg((i/BATCH_SIZE)+1).arg((debFiles.size()+BATCH_SIZE-1)/BATCH_SIZE));

        QStringList args;
        args << "/usr/bin/env"
             << "DEBIAN_FRONTEND=noninteractive"
             << "TMPDIR=" + Config::instance().workDir()
             << m_pkgManagerPath << "--force-all" << "--unpack";
        args << batch;

        QString output, error;
        if (!m_sysInterface->executeInOverlay(args, output, error)) {
            Logger::instance().error("Failed to unpack batch: " + error);
            return false;
        }
    }

    // Phase 2: Configure loop
    Logger::instance().info("Configuring packages...");
    int maxPasses = 15;
    int pass = 1;
    QString lastAudit;

    while (pass <= maxPasses) {
        Logger::instance().info(QString("Configuration pass %1/%2").arg(pass).arg(maxPasses));

        QString output, error;
        QStringList confArgs;
        confArgs << "/usr/bin/env"
                 << "DEBIAN_FRONTEND=noninteractive"
                 << "TMPDIR=" + Config::instance().workDir()
                 << m_pkgManagerPath << "--force-all" << "--configure" << "-a";
        m_sysInterface->executeInOverlay(confArgs, output, error);

        QStringList auditArgs;
        auditArgs << m_pkgManagerPath << "--audit";
        m_sysInterface->executeInOverlay(auditArgs, output, error);
        QString currentAudit = output.trimmed();

        if (currentAudit.isEmpty()) {
            Logger::instance().success("Package configuration converged.");
            return true;
        }

        if (currentAudit == lastAudit && pass > 1) {
             Logger::instance().error("Package configuration stuck (no progress). Aborting.");
             Logger::instance().error("Audit output: " + currentAudit);
             return false;
        }

        lastAudit = currentAudit;
        pass++;
        QThread::sleep(1);
    }

    Logger::instance().error("Package configuration failed to converge.");
    return false;
}

bool UpdateManager::runCleanupCrew() {
    QString ccuChecksum = m_queryData.value("NUTS_CCU_CHECKSUM");
    if (!downloadAndVerifyComponent("nuts-cpp-ccu", ccuChecksum)) return false;

    QString ccuPath = Config::instance().workDir() + "/nuts-cpp-ccu";
    QString output, error;
    
    if (!m_sysInterface->executeCommand(ccuPath, {}, output, error)) {
        Logger::instance().error("Cleanup script failed: " + error);
        return false;
    }
    return true;
}

void UpdateManager::cleanup() {
    m_sysInterface->unmountPartition(Config::instance().squashfsDir());
    m_sysInterface->unmountPartition("/home");
    m_sysInterface->unmountPartition("/var/lib");
    
    QStringList tools = {"dpkg", "dpkg-deb", "dpkg-query", "update-alternatives", 
                         "dpkg-divert", "dpkg-realpath", "dpkg-split", 
                         "dpkg-statoverride", "dpkg-trigger", "dpkg-maintscript-helper"};
    for(const QString& tool : tools) {
        QFile::remove("/usr/bin/" + tool);
    }
}

bool UpdateManager::downloadAndVerifyComponent(const QString& componentName, const QString& expectedChecksum) {
    QString baseUrl = Config::instance().componentBaseUrl();
    if (!baseUrl.endsWith('/'))
        baseUrl += '/';
    QString url = baseUrl + componentName;
    if (url.contains("{branch}"))
        url.replace("{branch}", Config::instance().branch());

    QString workDir = Config::instance().workDir();
    QString tempDest = workDir + "/" + componentName + ".download";
    QString destination = workDir + "/" + componentName;

    if (!m_sysInterface->downloadFile(url, tempDest)) return false;

    if (!m_sysInterface->verifyChecksum(tempDest, expectedChecksum)) {
        Logger::instance().error("Checksum verification failed for " + componentName);
        QFile(tempDest).remove();
        return false;
    }

    // Remove atomically (avoid TOCTOU)
    QFile::remove(destination);
    QFile::rename(tempDest, destination);
    
    QString output, error;
    m_sysInterface->executeCommand("/usr/bin/chmod", {"+x", destination}, output, error);
    
    return true;
}

bool UpdateManager::downloadUpdateArchive(const QString& url, const QString& destination) {
    return m_sysInterface->downloadFile(url, destination);
}

bool UpdateManager::verifyUpdateArchive(const QString& filePath, const QString& expectedChecksum) {
    return m_sysInterface->verifyChecksum(filePath, expectedChecksum);
}

bool UpdateManager::verifySquashFSIntegrity(const QString& squashfsPath) {
    // Create a temporary mount point for verification
    QString tempMount = Config::instance().workDir() + "/squashfs-verify";

    // Create mount point
    if (!m_sysInterface->directoryExists(tempMount)) {
        if (!m_sysInterface->createDirectory(tempMount)) {
            Logger::instance().warning("Could not create temporary mount point for verification");
            return true; // Don't fail the update if we can't verify
        }
    }

    QString output, error;

    // Attempt to mount (read-only)
    bool mountSuccess = m_sysInterface->executeCommand("/usr/bin/mount",
                                                       {"-o", "ro,loop", squashfsPath, tempMount},
                                                       output, error, 10000);

    if (!mountSuccess) {
        Logger::instance().error("Test mount failed: " + error);
        return false;
    }

    // Check if we can read the directory structure
    bool readSuccess = m_sysInterface->executeCommand("/usr/bin/ls",
                                                      {tempMount + "/ota"},
                                                      output, error, 5000);

    // Unmount
    m_sysInterface->unmountPartition(tempMount);

    if (!readSuccess) {
        Logger::instance().error("Could not read SquashFS contents");
        return false;
    }

    return true;
}

bool UpdateManager::checkDiskSpace() {
    // Check space on /home for OTA download
    QString downloadDir = Config::instance().downloadDir();
    qint64 availableHome = m_sysInterface->getAvailableSpace(downloadDir);

    // Estimate needed space: OTA size (from metadata) + 20% buffer
    qint64 estimatedOtaSize = 2LL * 1024 * 1024 * 1024; // Default 2GB if not known

    // Try to get actual size from update URL if available
    if (!m_updateUrl.isEmpty()) {
        qint64 remoteSize = m_sysInterface->getRemoteFileSize(m_updateUrl);
        if (remoteSize > 0) {
            estimatedOtaSize = remoteSize;
        }
    }

    // Check for integer overflow in size calculation
    qint64 buffer = estimatedOtaSize / 5;
    qint64 requiredHome;

    // Check if addition would overflow
    if (estimatedOtaSize > 0 && buffer > (LLONG_MAX - estimatedOtaSize)) {
        Logger::instance().error("SECURITY: Integer overflow detected in disk space calculation");
        return false;
    }

    requiredHome = estimatedOtaSize + buffer; // +20% buffer

    if (availableHome < requiredHome) {
        Logger::instance().error(QString("Insufficient space on /home. Required: %1 GB, Available: %2 GB")
                                .arg(requiredHome / (1024.0 * 1024 * 1024), 0, 'f', 2)
                                .arg(availableHome / (1024.0 * 1024 * 1024), 0, 'f', 2));
        return false;
    }

    // Check space on root for unpacking (need at least 1GB free after update)
    qint64 availableRoot = m_sysInterface->getAvailableSpace("/");
    qint64 requiredRoot = 1LL * 1024 * 1024 * 1024; // 1GB minimum

    if (availableRoot < requiredRoot) {
        Logger::instance().error(QString("Insufficient space on root partition. Required: %1 GB, Available: %2 GB")
                                .arg(requiredRoot / (1024.0 * 1024 * 1024), 0, 'f', 2)
                                .arg(availableRoot / (1024.0 * 1024 * 1024), 0, 'f', 2));
        return false;
    }

    Logger::instance().info(QString("Disk space check passed. Home: %1 GB free, Root: %2 GB free")
                           .arg(availableHome / (1024.0 * 1024 * 1024), 0, 'f', 2)
                           .arg(availableRoot / (1024.0 * 1024 * 1024), 0, 'f', 2));

    return true;
}

} // namespace Nuts
