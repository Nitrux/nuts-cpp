#include "nuts/UpdateManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QSettings>
#include <QDir>
#include <QUrl>
#include <QThread>
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

    // Collect raw mirror base URLs keyed by MIRROR{n}; assembled after
    // OTAFILE and OTABRANCH are known.
    QStringList mirrorBases;

    for (const QString& key : settings.allKeys()) {
        QString value = settings.value(key).toString();
        m_queryData[key] = value;

        if (key == "MINTARGET") {
            m_minTarget = value;
        } else if (key == "OTAFILE") {
            m_otaFile = value.trimmed();
        } else if (key == "OTABRANCH") {
            m_otaBranch = value.trimmed();
        } else if (key == "UPDATE_URL") {
            m_updateUrl = value;
        } else if (key == "UPDATE_CHECKSUM") {
            m_updateChecksum = value;
        } else if (key.startsWith("MIRROR")) {
            QString base = value.trimmed();
            if (!base.isEmpty())
                mirrorBases.append(base);
        }
    }

    // Validate required fields before assembling URLs
    if (m_otaFile.isEmpty()) {
        Logger::instance().error("Query file missing required field: OTAFILE");
        return false;
    }
    if (m_otaBranch.isEmpty()) {
        Logger::instance().error("Query file missing required field: OTABRANCH");
        return false;
    }

    // Assemble full mirror URLs: base + OTABRANCH + "/" + OTAFILE
    for (const QString& base : mirrorBases) {
        QString normalised = base.endsWith('/') ? base : base + '/';
        QString url = normalised + m_otaBranch + "/" + m_otaFile;
        if (!m_mirrorList.contains(url))
            m_mirrorList.append(url);
    }

    // Fallback: if no mirrors specified, use UPDATE_URL as single mirror
    if (m_mirrorList.isEmpty() && !m_updateUrl.isEmpty()) {
        m_mirrorList.append(m_updateUrl);
    }

    if (m_minTarget.isEmpty() || m_mirrorList.isEmpty()) {
        Logger::instance().error("Query file missing required fields (MINTARGET or mirrors)");
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
    Logger::instance().info("=== Starting System Update Process ===");

    // Acquire lock file to prevent concurrent update runs.
    // This matches the original nuts behaviour.
    const QString lockPath = "/var/run/nuts-cpp.lock";
    QFile lockFile(lockPath);
    if (lockFile.exists()) {
        Logger::instance().error("Another instance of nuts-cpp is already running (" + lockPath + ")");
        return false;
    }
    if (!lockFile.open(QIODevice::WriteOnly)) {
        Logger::instance().error("Failed to create lock file: " + lockPath);
        return false;
    }
    lockFile.close();

    // Check disk space on the host before entering the chroot.
    Q_EMIT updateProgress(5, "Checking disk space");
    Logger::instance().info("--- Step: Checking disk space ---");
    if (!checkDiskSpace()) {
        Logger::instance().error("Insufficient disk space for update");
        QFile::remove(lockPath);
        return false;
    }

    // Everything from here runs inside overlayroot-chroot.

    // Step 1: Prepare chroot environment.
    Q_EMIT updateProgress(8, "Preparing chroot environment");
    Logger::instance().info("--- Step: Preparing chroot environment ---");
    {
        QString output, error;
        bool ok;
        ok = m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "devtmpfs", "dev", "/dev"}, output, error);
        Logger::instance().debug(QString("mount devtmpfs: %1").arg(ok ? "ok" : "failed (non-fatal, may already be mounted)"));
        ok = m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", "/tmp"}, output, error);
        Logger::instance().debug(QString("mkdir /tmp: %1").arg(ok ? "ok" : "failed"));
        ok = m_sysInterface->executeInOverlay({"/usr/bin/chmod", "1777", "/tmp"}, output, error);
        Logger::instance().debug(QString("chmod 1777 /tmp: %1").arg(ok ? "ok" : "failed"));
    }

    Q_EMIT updateProgress(10, "Mounting partitions");
    Logger::instance().info("--- Step: Mounting system partitions ---");
    if (!prepareSystemPartitions()) {
        Logger::instance().error("prepareSystemPartitions() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("System partitions ready.");

    Q_EMIT updateProgress(15, "Downloading OTA payload");
    Logger::instance().info("--- Step: Downloading OTA payload ---");
    if (!downloadOTAPayload()) {
        Logger::instance().error("downloadOTAPayload() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("OTA payload ready.");

    Q_EMIT updateProgress(25, "Mounting OTA payload");
    Logger::instance().info("--- Step: Mounting OTA squashfs ---");
    if (!mountOTAPayload()) {
        Logger::instance().error("mountOTAPayload() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("OTA squashfs mounted.");

    Q_EMIT updateProgress(30, "Preparing update tools");
    Logger::instance().info("--- Step: Preparing dpkg tooling ---");
    if (!prepareUpdateTools()) {
        Logger::instance().error("prepareUpdateTools() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("dpkg tooling ready.");

    Q_EMIT updateProgress(40, "Syncing package database");
    Logger::instance().info("--- Step: Syncing package database ---");
    if (!syncPackageData()) {
        Logger::instance().error("syncPackageData() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("Package database synced.");

    Q_EMIT updateProgress(50, "Applying packages (this may take time)");
    Logger::instance().info("--- Step: Applying package updates ---");
    if (!performPackageUpdates()) {
        Logger::instance().error("performPackageUpdates() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("Packages applied.");

    Q_EMIT updateProgress(90, "Running final cleanup");
    Logger::instance().info("--- Step: Running cleanup crew ---");
    if (!runCleanupCrew()) {
        Logger::instance().error("runCleanupCrew() FAILED");
        cleanup();
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("Cleanup crew finished.");

    // Create nx-pkgmgr-policy symlinks.
    Logger::instance().info("--- Step: Installing policy symlinks ---");
    {
        QString output, error;
        QStringList aptTools = {"apt", "apt-cache", "apt-cdrom", "apt-config", "apt-get", "apt-mark"};
        for (const QString& tool : aptTools)
            m_sysInterface->executeInOverlay({"/usr/bin/ln", "-sf", "/usr/bin/nx-pkgmgr-policy", "/usr/bin/" + tool}, output, error);

        QStringList dpkgTools = {
            "dpkg", "dpkg-deb", "dpkg-divert", "dpkg-maintscript-helper", "dpkg-query",
            "dpkg-realpath", "dpkg-split", "dpkg-statoverride", "dpkg-trigger",
            "dpkg-architecture", "dpkg-buildapi", "dpkg-buildflags", "dpkg-buildpackage",
            "dpkg-buildtree", "dpkg-checkbuilddeps", "dpkg-distaddfile",
            "dpkg-genbuildinfo", "dpkg-genchanges", "dpkg-gencontrol", "dpkg-gensymbols",
            "dpkg-mergechangelogs", "dpkg-name", "dpkg-parsechangelog",
            "dpkg-scanpackages", "dpkg-scansources", "dpkg-shlibdeps", "dpkg-source", "dpkg-vendor"
        };
        for (const QString& tool : dpkgTools)
            m_sysInterface->executeInOverlay({"/usr/bin/ln", "-sf", "/usr/bin/nx-pkgmgr-policy", "/usr/bin/" + tool}, output, error);

        Logger::instance().info("Flushing writes to disk...");
        // Flush all pending writes to disk before exiting the chroot.
        m_sysInterface->executeInOverlay({"/usr/bin/sync"}, output, error);
    }

    cleanup();
    QFile::remove(lockPath);

    Q_EMIT updateProgress(100, "Update completed successfully");
    Logger::instance().success("=== System updated successfully. Please reboot. ===");
    return true;
}

// ----------------------
// Helper Implementations
// ----------------------

bool UpdateManager::prepareSystemPartitions() {
    // All mount operations run inside the chroot (lower layer).
    QString output, error;

    // Resolve and mount NX_HOME → /home
    Logger::instance().debug("Resolving LABEL=NX_HOME...");
    if (!m_sysInterface->executeInOverlay({"/usr/sbin/findfs", "LABEL=NX_HOME"}, output, error)) {
        Logger::instance().error("Could not find partition labeled NX_HOME");
        if (!error.trimmed().isEmpty()) Logger::instance().error("findfs NX_HOME stderr: " + error.trimmed());
        return false;
    }
    QString homeDev = output.trimmed();
    Logger::instance().info("NX_HOME device: " + homeDev);

    bool mountOk = m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "auto", homeDev, "/home"}, output, error);
    Logger::instance().debug(QString("mount %1 /home: %2").arg(homeDev, mountOk ? "ok" : "failed (may already be mounted)"));
    if (!mountOk && !error.trimmed().isEmpty())
        Logger::instance().debug("mount /home stderr: " + error.trimmed());

    // Resolve and mount NX_VAR_LIB → /var/lib
    Logger::instance().debug("Resolving LABEL=NX_VAR_LIB...");
    if (!m_sysInterface->executeInOverlay({"/usr/sbin/findfs", "LABEL=NX_VAR_LIB"}, output, error)) {
        Logger::instance().error("Could not find partition labeled NX_VAR_LIB");
        if (!error.trimmed().isEmpty()) Logger::instance().error("findfs NX_VAR_LIB stderr: " + error.trimmed());
        return false;
    }
    QString varLibDev = output.trimmed();
    Logger::instance().info("NX_VAR_LIB device: " + varLibDev);

    mountOk = m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "auto", varLibDev, "/var/lib"}, output, error);
    Logger::instance().debug(QString("mount %1 /var/lib: %2").arg(varLibDev, mountOk ? "ok" : "failed (may already be mounted)"));
    if (!mountOk && !error.trimmed().isEmpty())
        Logger::instance().debug("mount /var/lib stderr: " + error.trimmed());

    // Create working directories inside the chroot
    m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", Config::instance().downloadDir()}, output, error);
    m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", Config::instance().squashfsDir()}, output, error);
    Logger::instance().debug("Working dirs: " + Config::instance().downloadDir() + ", " + Config::instance().squashfsDir());

    return true;
}

bool UpdateManager::downloadOTAPayload() {
    // Runs inside the chroot (lower layer).
    QString otaPath = Config::instance().downloadDir() + "/nuts-ota.squashfs";
    QString output, error;

    Logger::instance().info("OTA target path: " + otaPath);
    Logger::instance().info("OTA expected checksum: " + m_otaChecksum);
    Logger::instance().info("Mirrors available: " + QString::number(m_mirrorList.size()));

    // Check if a valid file already exists in the lower layer
    if (m_sysInterface->executeInOverlay({"/usr/bin/test", "-f", otaPath}, output, error)) {
        Logger::instance().info("Existing OTA file found, verifying checksum...");
        if (m_sysInterface->executeInOverlay(
                {"/bin/sh", "-c",
                 QString("[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]").arg(otaPath, m_otaChecksum)},
                output, error)) {
            Logger::instance().success("Existing OTA payload verified, skipping download.");
            return true;
        }
        Logger::instance().warning("Existing OTA payload checksum mismatch. Re-downloading.");
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", otaPath}, output, error);
    } else {
        Logger::instance().info("No existing OTA file found at " + otaPath);
    }

    if (m_mirrorList.isEmpty()) {
        Logger::instance().error("No mirrors available.");
        return false;
    }

    for (const QString& mirror : m_mirrorList) {
        QString url = mirror.trimmed();
        if (url.isEmpty()) continue;

        // Always start clean — never resume across mirrors
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", otaPath}, output, error);

        Logger::instance().info("Trying mirror: " + url);
        bool ok = m_sysInterface->executeInOverlay(
            {"/usr/bin/axel", "-n", "10", "-o", otaPath, url},
            output, error);

        if (!ok) {
            Logger::instance().warning("Download from " + url + " failed.");
            if (!error.trimmed().isEmpty()) Logger::instance().warning("axel stderr: " + error.trimmed());
            continue;
        }
        Logger::instance().debug("axel download complete, verifying checksum...");

        // Verify checksum inside the chroot
        bool checksumOk = m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c",
             QString("[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]").arg(otaPath, m_otaChecksum)},
            output, error);

        if (checksumOk) {
            Logger::instance().success("OTA payload downloaded and verified from " + url);
            return true;
        }

        // Log what the actual checksum was for comparison
        QString actualSum, sumErr;
        m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c", QString("sha256sum '%1' | awk '{print $1}'").arg(otaPath)},
            actualSum, sumErr);
        Logger::instance().warning("Checksum mismatch from " + url);
        Logger::instance().warning("  Expected: " + m_otaChecksum);
        Logger::instance().warning("  Got:      " + actualSum.trimmed());
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", otaPath}, output, error);
    }

    Logger::instance().error("Failed to download OTA payload from all mirrors.");
    return false;
}

bool UpdateManager::mountOTAPayload() {
    QString otaPath = Config::instance().downloadDir() + "/nuts-ota.squashfs";
    QString mountPoint = Config::instance().squashfsDir();
    QString output, error;

    Logger::instance().info("Mounting: " + otaPath + " -> " + mountPoint);
    if (!m_sysInterface->executeInOverlay({"/usr/bin/mount", otaPath, mountPoint}, output, error)) {
        Logger::instance().error("Failed to mount OTA squashfs");
        if (!error.trimmed().isEmpty()) Logger::instance().error("mount stderr: " + error.trimmed());
        return false;
    }
    // List top-level contents for verification
    m_sysInterface->executeInOverlay({"/usr/bin/ls", mountPoint}, output, error);
    Logger::instance().debug("OTA squashfs root contents: " + output.trimmed());
    return true;
}

bool UpdateManager::prepareUpdateTools() {
    // Use /tmp inside the chroot.
    QString appImagePath = "/tmp/dpkg-1.22.21-x86_64.AppImage";
    QString extractDir = "/tmp/pkgman-extracted";
    QString appRunPath = extractDir + "/squashfs-root/AppRun";

    QString appImageUrl = "https://raw.githubusercontent.com/Nitrux/storage/master/Other/AppImages/dpkg-1.22.21-x86_64.AppImage";
    QString expectedChecksum = m_queryData.value("DPKG_AI_SUM");

    Logger::instance().info("dpkg AppImage path: " + appImagePath);
    Logger::instance().info("dpkg AppRun path:   " + appRunPath);
    Logger::instance().info("Expected DPKG_AI_SUM: " + expectedChecksum);

    // Check if already extracted (test inside chroot)
    QString output, error;
    bool alreadyExtracted = m_sysInterface->executeInOverlay(
        {"/usr/bin/test", "-f", appRunPath}, output, error);

    if (alreadyExtracted) {
        Logger::instance().info("Extracted OTA tooling already present, skipping download.");
    } else {
        Logger::instance().info("OTA tooling not found, downloading...");

        // 1. Download AppImage inside chroot using axel
        if (!m_sysInterface->executeInOverlay(
                {"/usr/bin/axel", "-n", "10", "-o", appImagePath, appImageUrl}, output, error)) {
            Logger::instance().error("Failed to download OTA tooling");
            if (!error.trimmed().isEmpty()) Logger::instance().error("axel stderr: " + error.trimmed());
            return false;
        }
        Logger::instance().success("dpkg AppImage downloaded.");

        // 2. Verify checksum inside chroot
        Logger::instance().info("Verifying dpkg AppImage checksum...");
        QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                                  .arg(expectedChecksum, appImagePath);
        if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", checksumCmd}, output, error)) {
            Logger::instance().error("CRITICAL: dpkg AppImage checksum mismatch!");
            // Log actual checksum for diagnosis
            QString actualSum, sumErr;
            m_sysInterface->executeInOverlay(
                {"/bin/sh", "-c", QString("sha256sum '%1' | awk '{print $1}'").arg(appImagePath)},
                actualSum, sumErr);
            Logger::instance().error("  Expected: " + expectedChecksum);
            Logger::instance().error("  Got:      " + actualSum.trimmed());
            m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", appImagePath}, output, error);
            return false;
        }
        Logger::instance().success("dpkg AppImage checksum OK.");

        // 3. Make executable inside chroot
        m_sysInterface->executeInOverlay({"/usr/bin/chmod", "+x", appImagePath}, output, error);

        // 4. Extract inside chroot
        Logger::instance().info("Extracting dpkg AppImage...");
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-rf", extractDir}, output, error);
        m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", extractDir}, output, error);
        if (!m_sysInterface->executeInOverlay(
                {"/bin/sh", "-c", "cd " + extractDir + " && " + appImagePath + " --appimage-extract"},
                output, error)) {
            Logger::instance().error("Failed to extract dpkg AppImage");
            if (!error.trimmed().isEmpty()) Logger::instance().error("extract stderr: " + error.trimmed());
            return false;
        }

        // Verify AppRun exists after extraction
        bool appRunExists = m_sysInterface->executeInOverlay(
            {"/usr/bin/test", "-f", appRunPath}, output, error);
        if (!appRunExists) {
            Logger::instance().error("Extraction completed but AppRun not found at: " + appRunPath);
            // List what IS there to help diagnose
            m_sysInterface->executeInOverlay({"/usr/bin/ls", "-la", extractDir + "/squashfs-root/"}, output, error);
            Logger::instance().debug("squashfs-root contents: " + output.trimmed());
            return false;
        }
        Logger::instance().success("dpkg AppImage extracted. AppRun found.");
    }

    m_pkgManagerPath = appRunPath;
    Logger::instance().info("Using dpkg tooling at: " + m_pkgManagerPath);

    // dpkg → AppRun (matching original: ln -svf "$AIPKG_MANAGER" /usr/bin/dpkg)
    bool lnOk = m_sysInterface->executeInOverlay({"/usr/bin/ln", "-svf", appRunPath, "/usr/bin/dpkg"}, output, error);
    Logger::instance().debug(QString("ln dpkg -> AppRun: %1").arg(lnOk ? "ok" : "failed"));
    if (!lnOk && !error.trimmed().isEmpty()) Logger::instance().debug("ln stderr: " + error.trimmed());

    QStringList tools = {"dpkg-deb", "dpkg-divert", "dpkg-query",
                         "dpkg-realpath", "dpkg-split", "dpkg-statoverride",
                         "dpkg-trigger", "dpkg-maintscript-helper", "update-alternatives"};

    QString binDir = extractDir + "/squashfs-root/usr/bin";

    for (const QString& tool : tools) {
        QString target = binDir + "/" + tool;
        QString link = "/usr/bin/" + tool;
        QString testOut, testErr;
        if (m_sysInterface->executeInOverlay({"/usr/bin/test", "-f", target}, testOut, testErr)) {
            m_sysInterface->executeInOverlay({"/usr/bin/ln", "-svf", target, link}, output, error);
            Logger::instance().debug("Linked: " + link + " -> " + target);
        } else {
            Logger::instance().warning("Tool binary not found in AppImage, skipping: " + target);
        }
    }

    m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", "/usr/share"}, output, error);
    bool shareOk = m_sysInterface->executeInOverlay({"/usr/bin/ln", "-svf", extractDir + "/squashfs-root/usr/share/dpkg", "/usr/share/dpkg"}, output, error);
    Logger::instance().debug(QString("ln /usr/share/dpkg: %1").arg(shareOk ? "ok" : "failed (non-fatal)"));

    return true;
}

bool UpdateManager::syncPackageData() {
    QString url = QString("https://raw.githubusercontent.com/Nitrux/storage/master/Other/var-lib-dpkg-%1.tar.xz").arg(m_minTarget);
    QString tarPath = QString("/tmp/var-lib-dpkg-%1.tar.xz").arg(m_minTarget);
    QString expectedChecksum = m_queryData.value("VAR_LIB_SUM");
    QString output, error;

    Logger::instance().info("Package DB archive URL: " + url);
    Logger::instance().info("Package DB archive path: " + tarPath);
    Logger::instance().info("Expected VAR_LIB_SUM: " + expectedChecksum);

    // Remove any stale file first
    m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", tarPath}, output, error);

    Logger::instance().info("Downloading package database archive...");
    if (!m_sysInterface->executeInOverlay(
            {"/usr/bin/axel", "-n", "10", "-o", tarPath, url}, output, error)) {
        Logger::instance().error("Failed to download package database archive");
        if (!error.trimmed().isEmpty()) Logger::instance().error("axel stderr: " + error.trimmed());
        return false;
    }
    Logger::instance().success("Package database archive downloaded.");

    // Verify checksum inside chroot before extraction.
    // Extracting an unverified archive to / is extremely dangerous (zip slip / overwrite attacks).
    Logger::instance().info("Verifying package database archive checksum...");
    QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(expectedChecksum, tarPath);
    if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", checksumCmd}, output, error)) {
        Logger::instance().error("CRITICAL: Package database checksum mismatch!");
        QString actualSum, sumErr;
        m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c", QString("sha256sum '%1' | awk '{print $1}'").arg(tarPath)},
            actualSum, sumErr);
        Logger::instance().error("  Expected: " + expectedChecksum);
        Logger::instance().error("  Got:      " + actualSum.trimmed());
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", tarPath}, output, error);
        return false;
    }
    Logger::instance().success("Package database archive checksum OK.");

    // Extract into the lower layer — original uses: cd / && tar -xf $TARFILE
    Logger::instance().info("Extracting package database archive to /...");
    if (!m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c", "mkdir -p /var/lib/dpkg && cd / && /usr/bin/tar -xf " + tarPath},
            output, error)) {
        Logger::instance().error("Failed to extract package database archive");
        if (!error.trimmed().isEmpty()) Logger::instance().error("tar stderr: " + error.trimmed());
        return false;
    }

    // Confirm extraction succeeded.
    if (!m_sysInterface->executeInOverlay({"/usr/bin/test", "-f", "/var/lib/dpkg/status"}, output, error)) {
        Logger::instance().error("Package database extraction failed: /var/lib/dpkg/status not found");
        // List /var/lib/dpkg to see what is there
        m_sysInterface->executeInOverlay({"/usr/bin/ls", "-la", "/var/lib/dpkg/"}, output, error);
        Logger::instance().debug("/var/lib/dpkg/ contents: " + output.trimmed());
        return false;
    }
    Logger::instance().success("Package database extracted. /var/lib/dpkg/status confirmed.");

    return true;
}

bool UpdateManager::performPackageUpdates() {
    QString otaDir = Config::instance().squashfsDir() + "/ota";
    QString updatesDir = otaDir + "/updates";
    QString nvidiaDir = otaDir + "/nvidia";

    Logger::instance().info("OTA updates dir: " + updatesDir);
    Logger::instance().info("dpkg tooling path: " + m_pkgManagerPath);

    // Detect NVIDIA on the host via /proc (shared with chroot).
    bool isNvidia = QDir("/proc/driver/nvidia").exists();
    if (!isNvidia) {
        QString output, error;
        m_sysInterface->executeCommand("/usr/bin/lspci", {}, output, error);
        if (output.contains("NVIDIA", Qt::CaseInsensitive)) isNvidia = true;
    }
    Logger::instance().info(QString("NVIDIA hardware: %1").arg(isNvidia ? "yes" : "no"));

    QString output, error;

    // Verify the dpkg tooling is present inside the chroot before attempting anything.
    if (!m_sysInterface->executeInOverlay({"/usr/bin/test", "-f", m_pkgManagerPath}, output, error)) {
        Logger::instance().error("dpkg tooling not found inside chroot: " + m_pkgManagerPath);
        return false;
    }
    Logger::instance().info("dpkg tooling confirmed present: " + m_pkgManagerPath);

    // --- Phase 1: Collect .deb paths ---
    Logger::instance().info("--- Phase 1: Collecting .deb packages ---");

    QStringList findArgs = {"/usr/bin/find", updatesDir, "-name", "*.deb", "-print0"};
    if (isNvidia)
        findArgs << nvidiaDir;

    bool findOk = m_sysInterface->executeInOverlay(findArgs, output, error);
    if (!findOk && output.trimmed().isEmpty()) {
        Logger::instance().error("find failed to enumerate .deb packages");
        if (!error.trimmed().isEmpty()) Logger::instance().error("find stderr: " + error.trimmed());
        return false;
    }

    // Split null-delimited output into individual paths.
    QStringList debs = output.split('\0', Qt::SkipEmptyParts);
    Logger::instance().info(QString("Found %1 .deb package(s).").arg(debs.size()));
    for (const QString& deb : debs)
        Logger::instance().debug("  deb: " + deb);

    if (debs.isEmpty()) {
        Logger::instance().warning("No .deb packages found. Nothing to install.");
        return true;
    }

    // --- Phase 2: Unpack in batches of 120 ---
    Logger::instance().info("--- Phase 2: Unpacking packages ---");

    const int batchSize = 120;
    int totalBatches = (debs.size() + batchSize - 1) / batchSize;

    for (int i = 0; i < debs.size(); i += batchSize) {
        QStringList batch = debs.mid(i, batchSize);
        int batchNum = (i / batchSize) + 1;
        Logger::instance().info(QString("Unpacking batch %1/%2 (%3 package(s))...")
                                    .arg(batchNum).arg(totalBatches).arg(batch.size()));

        QStringList unpackArgs = {m_pkgManagerPath, "--force-all", "--unpack"};
        unpackArgs << batch;

        if (!m_sysInterface->executeInOverlay(unpackArgs, output, error)) {
            Logger::instance().error(QString("Unpack failed on batch %1/%2").arg(batchNum).arg(totalBatches));
            if (!error.trimmed().isEmpty()) Logger::instance().error("unpack stderr: " + error.trimmed());
            return false;
        }
        Logger::instance().success(QString("Batch %1/%2 unpacked.").arg(batchNum).arg(totalBatches));
    }

    Logger::instance().success("All packages unpacked.");

    // --- Phase 3: Configure + audit loop ---
    Logger::instance().info("--- Phase 3: Configuring packages ---");

    const int maxPasses = 15;
    QString lastAudit;

    for (int pass = 1; pass <= maxPasses; ++pass) {
        Logger::instance().info(QString("Configuration pass %1/%2...").arg(pass).arg(maxPasses));

        // --configure -a: configure all unpacked packages; ignore non-zero exit (may be partial)
        m_sysInterface->executeInOverlay({m_pkgManagerPath, "--force-all", "--configure", "-a"}, output, error);

        // --audit: report packages in inconsistent state
        m_sysInterface->executeInOverlay({m_pkgManagerPath, "--audit"}, output, error);
        QString currentAudit = output.trimmed();

        if (currentAudit.isEmpty()) {
            Logger::instance().success(QString("Package configuration converged after %1 pass(es).").arg(pass));
            break;
        }

        Logger::instance().info("dpkg --audit output:\n" + currentAudit);

        if (pass > 1 && currentAudit == lastAudit) {
            Logger::instance().error("Package configuration stuck — no progress between passes.");
            return false;
        }

        if (pass == maxPasses) {
            Logger::instance().error(QString("Package configuration failed to converge after %1 passes.").arg(maxPasses));
            return false;
        }

        lastAudit = currentAudit;
        QThread::sleep(1);
    }

    m_sysInterface->executeInOverlay({"/usr/bin/rm", "-rf", "/tmp/pkgman-extracted"}, output, error);
    Logger::instance().success("Package updates applied successfully.");
    return true;
}

bool UpdateManager::runCleanupCrew() {
    QString ccuChecksum = m_queryData.value("NUTS_CCU_CHECKSUM");
    QString ccuPath = "/tmp/nuts-cpp-ccu";
    QString output, error;

    // Build the component URL from internal config
    QString baseUrl = Config::instance().componentBaseUrl();
    if (!baseUrl.endsWith('/')) baseUrl += '/';
    QString ccuUrl = baseUrl + "nuts-cpp-ccu";

    Logger::instance().info("CCU URL:  " + ccuUrl);
    Logger::instance().info("CCU path: " + ccuPath);
    Logger::instance().info("Expected NUTS_CCU_CHECKSUM: " + ccuChecksum);

    // Download inside chroot using axel
    Logger::instance().info("Downloading cleanup crew...");
    if (!m_sysInterface->executeInOverlay(
            {"/usr/bin/axel", "-n", "10", "-o", ccuPath, ccuUrl}, output, error)) {
        Logger::instance().error("Failed to download cleanup crew");
        if (!error.trimmed().isEmpty()) Logger::instance().error("axel stderr: " + error.trimmed());
        return false;
    }
    Logger::instance().success("Cleanup crew downloaded.");

    // Verify checksum inside chroot
    Logger::instance().info("Verifying cleanup crew checksum...");
    QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(ccuChecksum, ccuPath);
    if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", checksumCmd}, output, error)) {
        Logger::instance().error("CRITICAL: Cleanup crew checksum mismatch!");
        QString actualSum, sumErr;
        m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c", QString("sha256sum '%1' | awk '{print $1}'").arg(ccuPath)},
            actualSum, sumErr);
        Logger::instance().error("  Expected: " + ccuChecksum);
        Logger::instance().error("  Got:      " + actualSum.trimmed());
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", ccuPath}, output, error);
        return false;
    }
    Logger::instance().success("Cleanup crew checksum OK.");

    // Make executable and run inside chroot
    m_sysInterface->executeInOverlay({"/usr/bin/chmod", "+x", ccuPath}, output, error);
    Logger::instance().info("Running cleanup crew...");
    bool ok = m_sysInterface->executeInOverlay({ccuPath}, output, error);
    if (!output.trimmed().isEmpty()) Logger::instance().info("CCU stdout: " + output.trimmed());
    if (!error.trimmed().isEmpty())  Logger::instance().info("CCU stderr: " + error.trimmed());
    if (!ok) {
        Logger::instance().error("Cleanup crew exited with non-zero status");
        return false;
    }
    Logger::instance().success("Cleanup crew completed successfully.");
    return true;
}

void UpdateManager::cleanup() {
    Logger::instance().info("--- Cleanup: unmounting chroot mounts ---");
    QString output, error;

    // Unmount inside chroot — mounts were created inside the chroot.
    QString sqfsDir = Config::instance().squashfsDir();
    bool ok;
    ok = m_sysInterface->executeInOverlay({"/usr/bin/umount", sqfsDir}, output, error);
    Logger::instance().debug(QString("umount %1: %2").arg(sqfsDir, ok ? "ok" : "failed (may not be mounted)"));

    ok = m_sysInterface->executeInOverlay({"/usr/bin/umount", "/home"}, output, error);
    Logger::instance().debug(QString("umount /home: %1").arg(ok ? "ok" : "failed (may not be mounted)"));

    ok = m_sysInterface->executeInOverlay({"/usr/bin/umount", "/var/lib"}, output, error);
    Logger::instance().debug(QString("umount /var/lib: %1").arg(ok ? "ok" : "failed (may not be mounted)"));

    ok = m_sysInterface->executeInOverlay({"/usr/bin/umount", "/dev"}, output, error);
    Logger::instance().debug(QString("umount /dev: %1").arg(ok ? "ok" : "failed (may not be mounted)"));
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
