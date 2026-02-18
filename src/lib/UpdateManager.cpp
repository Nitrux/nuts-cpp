#include "nuts/UpdateManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QSettings>
#include <QDir>
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
    Logger::instance().info("Starting System Update Process...");

    // Acquire lock file to prevent concurrent update runs.
    // This matches the original nuts shell script behaviour.
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
    if (!checkDiskSpace()) {
        Logger::instance().error("Insufficient disk space for update");
        QFile::remove(lockPath);
        return false;
    }

    // Everything from here runs inside overlayroot-chroot.

    // Step 1: Prepare chroot environment.
    Q_EMIT updateProgress(8, "Preparing chroot environment");
    {
        QString output, error;
        m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "devtmpfs", "dev", "/dev"}, output, error);
        // Non-fatal: may already be mounted; proceed regardless.
        m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", "/tmp"}, output, error);
        m_sysInterface->executeInOverlay({"/usr/bin/chmod", "1777", "/tmp"}, output, error);
    }

    Q_EMIT updateProgress(10, "Mounting partitions");
    if (!prepareSystemPartitions()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    Q_EMIT updateProgress(15, "Downloading OTA payload");
    if (!downloadOTAPayload()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    Q_EMIT updateProgress(25, "Mounting OTA payload");
    if (!mountOTAPayload()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    Q_EMIT updateProgress(30, "Preparing update tools");
    if (!prepareUpdateTools()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    Q_EMIT updateProgress(40, "Syncing package database");
    if (!syncPackageData()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    Q_EMIT updateProgress(50, "Applying packages (this may take time)");
    if (!performPackageUpdates()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    Q_EMIT updateProgress(90, "Running final cleanup");
    if (!runCleanupCrew()) {
        cleanup();
        QFile::remove(lockPath);
        return false;
    }

    // Create nx-pkgmgr-policy symlinks.
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

        // Flush all pending writes to disk before exiting the chroot.
        m_sysInterface->executeInOverlay({"/usr/bin/sync"}, output, error);
    }

    cleanup();
    QFile::remove(lockPath);

    Q_EMIT updateProgress(100, "Update completed successfully");
    Logger::instance().success("System updated successfully.");
    return true;
}

// ----------------------
// Helper Implementations
// ----------------------

bool UpdateManager::prepareSystemPartitions() {
    // All mount operations run inside the chroot (lower layer).
    QString output, error;

    // Resolve and mount NX_HOME → /home
    if (!m_sysInterface->executeInOverlay({"/usr/sbin/findfs", "LABEL=NX_HOME"}, output, error)) {
        Logger::instance().error("Could not find partition labeled NX_HOME");
        return false;
    }
    QString homeDev = output.trimmed();

    m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "auto", homeDev, "/home"}, output, error);
    // Non-fatal if already mounted; proceed.

    // Resolve and mount NX_VAR_LIB → /var/lib
    if (!m_sysInterface->executeInOverlay({"/usr/sbin/findfs", "LABEL=NX_VAR_LIB"}, output, error)) {
        Logger::instance().error("Could not find partition labeled NX_VAR_LIB");
        return false;
    }
    QString varLibDev = output.trimmed();

    m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "auto", varLibDev, "/var/lib"}, output, error);
    // Non-fatal if already mounted.

    // Create working directories inside the chroot
    m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", Config::instance().downloadDir()}, output, error);
    m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", Config::instance().squashfsDir()}, output, error);

    return true;
}

bool UpdateManager::downloadOTAPayload() {
    // Runs inside the chroot (lower layer).
    QString otaPath = Config::instance().downloadDir() + "/nuts-ota.squashfs";
    QString output, error;

    // Check if a valid file already exists in the lower layer
    if (m_sysInterface->executeInOverlay({"/usr/bin/test", "-f", otaPath}, output, error)) {
        if (m_sysInterface->executeInOverlay(
                {"/bin/sh", "-c",
                 QString("[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]").arg(otaPath, m_otaChecksum)},
                output, error)) {
            Logger::instance().success("Existing OTA payload verified.");
            return true;
        }
        Logger::instance().warning("Existing OTA payload corrupt. Re-downloading.");
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", otaPath}, output, error);
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

        Logger::instance().info("Trying mirror: " + QUrl(url).host());
        bool ok = m_sysInterface->executeInOverlay(
            {"/usr/bin/axel", "-n", "10", "-o", otaPath, url},
            output, error);

        if (!ok) {
            Logger::instance().warning("Download from " + QUrl(url).host() + " failed. Trying next mirror.");
            continue;
        }

        // Verify checksum inside the chroot
        bool checksumOk = m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c",
             QString("[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]").arg(otaPath, m_otaChecksum)},
            output, error);

        if (checksumOk) {
            Logger::instance().success("OTA payload downloaded and verified from " + QUrl(url).host());
            return true;
        }

        Logger::instance().warning("Checksum mismatch from " + QUrl(url).host() + ". Trying next mirror.");
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", otaPath}, output, error);
    }

    Logger::instance().error("Failed to download OTA payload from all mirrors.");
    return false;
}

bool UpdateManager::mountOTAPayload() {
    QString otaPath = Config::instance().downloadDir() + "/nuts-ota.squashfs";
    QString mountPoint = Config::instance().squashfsDir();
    QString output, error;

    if (!m_sysInterface->executeInOverlay({"/usr/bin/mount", otaPath, mountPoint}, output, error)) {
        Logger::instance().error("Failed to mount OTA squashfs: " + error);
        return false;
    }
    return true;
}

bool UpdateManager::prepareUpdateTools() {
    // Use /tmp inside the chroot.
    QString appImagePath = "/tmp/dpkg-1.22.21-x86_64.AppImage";
    QString extractDir = "/tmp/pkgman-extracted";
    QString appRunPath = extractDir + "/squashfs-root/AppRun";

    QString appImageUrl = "https://raw.githubusercontent.com/Nitrux/storage/master/Other/AppImages/dpkg-1.22.21-x86_64.AppImage";
    QString expectedChecksum = m_queryData.value("DPKG_AI_SUM");

    // Check if already extracted (test inside chroot)
    QString output, error;
    bool alreadyExtracted = m_sysInterface->executeInOverlay(
        {"/usr/bin/test", "-f", appRunPath}, output, error);

    if (!alreadyExtracted) {
        // 1. Download AppImage inside chroot using axel
        Logger::instance().info("Downloading dpkg AppImage tooling...");
        if (!m_sysInterface->executeInOverlay(
                {"/usr/bin/axel", "-n", "10", "-o", appImagePath, appImageUrl}, output, error)) {
            Logger::instance().error("Failed to download OTA tooling: " + error);
            return false;
        }

        // 2. Verify checksum inside chroot
        Logger::instance().info("Verifying update tools integrity...");
        QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                                  .arg(expectedChecksum, appImagePath);
        if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", checksumCmd}, output, error)) {
            Logger::instance().error("CRITICAL: OTA tooling checksum mismatch! Aborting.");
            m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", appImagePath}, output, error);
            return false;
        }

        // 3. Make executable inside chroot
        m_sysInterface->executeInOverlay({"/usr/bin/chmod", "+x", appImagePath}, output, error);

        // 4. Extract inside chroot
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-rf", extractDir}, output, error);
        m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", extractDir}, output, error);
        if (!m_sysInterface->executeInOverlay(
                {"/bin/sh", "-c", "cd " + extractDir + " && " + appImagePath + " --appimage-extract"},
                output, error)) {
            Logger::instance().error("Failed to extract OTA tooling: " + error);
            return false;
        }
    }

    m_pkgManagerPath = appRunPath;

    m_sysInterface->executeInOverlay({"/usr/bin/ln", "-svf", appRunPath, "/usr/bin/dpkg"}, output, error);

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
        }
    }

    m_sysInterface->executeInOverlay({"/usr/bin/mkdir", "-p", "/usr/share"}, output, error);
    m_sysInterface->executeInOverlay({"/usr/bin/ln", "-svf", extractDir + "/squashfs-root/usr/share/dpkg", "/usr/share/dpkg"}, output, error);

    return true;
}

bool UpdateManager::syncPackageData() {
    QString url = QString("https://raw.githubusercontent.com/Nitrux/storage/master/Other/var-lib-dpkg-%1.tar.xz").arg(m_minTarget);
    QString tarPath = QString("/tmp/var-lib-dpkg-%1.tar.xz").arg(m_minTarget);
    QString expectedChecksum = m_queryData.value("VAR_LIB_SUM");
    QString output, error;

    // Remove any stale file first
    m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", tarPath}, output, error);

    Logger::instance().info("Downloading package database archive...");
    if (!m_sysInterface->executeInOverlay(
            {"/usr/bin/axel", "-n", "10", "-o", tarPath, url}, output, error)) {
        Logger::instance().error("Failed to download package database: " + error);
        return false;
    }

    // Verify checksum inside chroot before extraction.
    // Extracting an unverified archive to / is extremely dangerous (zip slip / overwrite attacks).
    Logger::instance().info("Verifying package database archive...");
    QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(expectedChecksum, tarPath);
    if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", checksumCmd}, output, error)) {
        Logger::instance().error("CRITICAL: Package database checksum mismatch!");
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", tarPath}, output, error);
        return false;
    }

    // Extract into the lower layer — original uses: cd / && tar -xf $TARFILE
    if (!m_sysInterface->executeInOverlay(
            {"/bin/sh", "-c", "mkdir -p /var/lib/dpkg && cd / && /usr/bin/tar -xf " + tarPath},
            output, error)) {
        Logger::instance().error("Failed to extract package database: " + error);
        return false;
    }

    // Confirm extraction succeeded.
    if (!m_sysInterface->executeInOverlay({"/usr/bin/test", "-f", "/var/lib/dpkg/status"}, output, error)) {
        Logger::instance().error("Package database extraction failed: /var/lib/dpkg/status not found");
        return false;
    }

    return true;
}

bool UpdateManager::performPackageUpdates() {
    QString otaDir = Config::instance().squashfsDir() + "/ota";
    QString updatesDir = otaDir + "/updates";
    QString nvidiaDir = otaDir + "/nvidia";

    // Detect NVIDIA on the host via /proc (shared with chroot).
    bool isNvidia = QDir("/proc/driver/nvidia").exists();
    if (!isNvidia) {
        QString output, error;
        m_sysInterface->executeCommand("/usr/bin/lspci", {}, output, error);
        if (output.contains("NVIDIA", Qt::CaseInsensitive)) isNvidia = true;
    }
    if (isNvidia)
        Logger::instance().info("NVIDIA hardware detected. Including NVIDIA drivers.");

    // Phase 1: Unpack.
    Logger::instance().info("Phase 1: Unpacking OTA content...");
    {
        QString searchPaths = updatesDir;
        if (isNvidia)
            searchPaths += " " + nvidiaDir;

        QString unpackCmd = QString(
            "export DEBIAN_FRONTEND=noninteractive && "
            "export TMPDIR=/tmp && "
            "find %1 -name '*.deb' -print0 | "
            "xargs -0 -n 120 %2 --force-all --unpack"
        ).arg(searchPaths, m_pkgManagerPath);

        QString output, error;
        if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", unpackCmd}, output, error)) {
            Logger::instance().error("Failed to unpack OTA content: " + error);
            return false;
        }
    }

    // Phase 2: Configure loop audit reports clean.
    Logger::instance().info("Configuring packages...");
    int maxPasses = 15;
    int pass = 1;
    QString lastAudit;

    while (pass <= maxPasses) {
        Logger::instance().info(QString("Configuration pass %1/%2").arg(pass).arg(maxPasses));

        QString output, error;
        QStringList confArgs;
        confArgs << "/bin/sh" << "-c"
                 << QString("export DEBIAN_FRONTEND=noninteractive && export TMPDIR=/tmp && "
                            "%1 --force-all --configure -a").arg(m_pkgManagerPath);
        m_sysInterface->executeInOverlay(confArgs, output, error);

        QStringList auditArgs;
        auditArgs << m_pkgManagerPath << "--audit";
        m_sysInterface->executeInOverlay(auditArgs, output, error);
        QString currentAudit = output.trimmed();

        if (currentAudit.isEmpty()) {
            Logger::instance().success("Package configuration converged.");
            m_sysInterface->executeInOverlay({"/usr/bin/rm", "-rf", "/tmp/pkgman-extracted"}, output, error);
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
    QString ccuPath = "/tmp/nuts-cpp-ccu";
    QString output, error;

    // Build the component URL from internal config
    QString baseUrl = Config::instance().componentBaseUrl();
    if (!baseUrl.endsWith('/')) baseUrl += '/';
    QString ccuUrl = baseUrl + "nuts-cpp-ccu";

    // Download inside chroot using axel
    Logger::instance().info("Downloading cleanup crew...");
    if (!m_sysInterface->executeInOverlay(
            {"/usr/bin/axel", "-n", "10", "-o", ccuPath, ccuUrl}, output, error)) {
        Logger::instance().error("Failed to download cleanup crew: " + error);
        return false;
    }

    // Verify checksum inside chroot
    QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(ccuChecksum, ccuPath);
    if (!m_sysInterface->executeInOverlay({"/bin/sh", "-c", checksumCmd}, output, error)) {
        Logger::instance().error("CRITICAL: Cleanup crew checksum mismatch! Aborting.");
        m_sysInterface->executeInOverlay({"/usr/bin/rm", "-f", ccuPath}, output, error);
        return false;
    }

    // Make executable and run inside chroot
    m_sysInterface->executeInOverlay({"/usr/bin/chmod", "+x", ccuPath}, output, error);
    if (!m_sysInterface->executeInOverlay({ccuPath}, output, error)) {
        Logger::instance().error("Cleanup script failed: " + error);
        return false;
    }
    return true;
}

void UpdateManager::cleanup() {
    QString output, error;

    // Unmount inside chroot — mounts were created inside the chroot.
    m_sysInterface->executeInOverlay({"/usr/bin/umount", Config::instance().squashfsDir()}, output, error);
    m_sysInterface->executeInOverlay({"/usr/bin/umount", "/home"}, output, error);
    m_sysInterface->executeInOverlay({"/usr/bin/umount", "/var/lib"}, output, error);
    m_sysInterface->executeInOverlay({"/usr/bin/umount", "/dev"}, output, error);
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
