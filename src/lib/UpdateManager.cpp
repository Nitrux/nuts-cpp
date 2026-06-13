#include "nuts/UpdateManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
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
    m_otaSize = 0;

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
        } else if (key == "OTASIZE") {
            bool ok = false;
            qint64 parsedSize = value.trimmed().toLongLong(&ok);
            if (ok && parsedSize > 0) {
                m_otaSize = parsedSize;
            } else if (!value.trimmed().isEmpty()) {
                Logger::instance().warning("Invalid OTASIZE value in query file: " + value);
            }
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

    // Check if an update was recently applied but system hasn't rebooted yet.
    // This prevents re-running the update process when /etc/lsb-release hasn't
    // been updated on the read-only root filesystem.
    const QString rebootMarkerPath = "/var/run/nuts-cpp-pending-reboot";
    QFile rebootMarkerFile(rebootMarkerPath);
    if (rebootMarkerFile.exists() && rebootMarkerFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString appliedVersion = QString::fromUtf8(rebootMarkerFile.readAll()).trimmed();
        rebootMarkerFile.close();

        // If the marker indicates we've already applied this target version,
        // don't show the update as available (reboot pending)
        if (!appliedVersion.isEmpty() && appliedVersion == m_minTarget) {
            Logger::instance().info("Update to version " + m_minTarget +
                                  " already applied. Reboot required to complete.");
            return false;
        }
    }

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

    // File paths used throughout this function
    const QString lockPath = "/var/run/nuts-cpp.lock";
    const QString rebootMarkerPath = "/var/run/nuts-cpp-pending-reboot";

    // Acquire lock file to prevent concurrent update runs.
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

    // Clear any existing reboot marker from a previous update.
    // This allows applying a newer update if one becomes available.
    if (QFile::exists(rebootMarkerPath)) {
        QFile::remove(rebootMarkerPath);
        Logger::instance().info("Cleared previous reboot marker");
    }

    // --- PRE-CHROOT PHASE (runs on host) ---

    Q_EMIT updateProgress(5, "Checking disk space");
    Logger::instance().info("--- Step: Checking disk space ---");
    if (!checkDiskSpace()) {
        Logger::instance().error("Insufficient disk space for update");
        QFile::remove(lockPath);
        return false;
    }

    // --- PARTITION SAFETY CHECKS ---
    // These three checks prevent common catastrophic errors during updates:
    // 1. XFS filesystem verification (xfsrestore requires XFS root)
    // 2. Duplicate label detection (prevent updating wrong drive)
    // 3. Pre-mount cleanup (handle stale mounts from interrupted operations)

    Q_EMIT updateProgress(6, "Verifying system partitions");
    Logger::instance().info("--- Step: Verifying system partitions ---");

    // Safety check 1: Verify root partition filesystem type
    Logger::instance().info("Searching for NX_ROOT partition");
    QString output, error;
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_ROOT"}, output, error)) {
        Logger::instance().error("Failed to find NX_ROOT partition");
        Logger::instance().error("findfs error: " + error);
        QFile::remove(lockPath);
        return false;
    }
    QString rootPartition = output.trimmed();
    Logger::instance().info("Found NX_ROOT: " + rootPartition);

    Logger::instance().info("Verifying NX_ROOT filesystem type");
    if (!m_sysInterface->executeCommand("/usr/sbin/blkid", {"-o", "value", "-s", "TYPE", rootPartition}, output, error)) {
        Logger::instance().error("Failed to determine filesystem type of NX_ROOT");
        Logger::instance().error("blkid error: " + error);
        QFile::remove(lockPath);
        return false;
    }
    QString rootFilesystem = output.trimmed();
    Logger::instance().info("NX_ROOT filesystem: " + rootFilesystem);

    if (rootFilesystem != "xfs") {
        Logger::instance().error("The filesystem of NX_ROOT is " + rootFilesystem + ", not XFS");
        Logger::instance().error("The backup/restore system requires an XFS root partition");
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().success("Filesystem type verified: XFS");

    // Safety check 2: Check for duplicate NX_ROOT labels
    Logger::instance().info("Checking for duplicate NX_ROOT labels");
    if (!m_sysInterface->executeCommand("/usr/sbin/blkid", {"-o", "device", "-t", "LABEL=NX_ROOT"}, output, error)) {
        Logger::instance().warning("Failed to enumerate NX_ROOT labels (blkid error: " + error + "), proceeding with caution");
    } else {
        QStringList devices = output.trimmed().split('\n', Qt::SkipEmptyParts);
        int labelCount = devices.size();
        Logger::instance().info(QString("Found %1 device(s) with NX_ROOT label").arg(labelCount));

        if (labelCount > 1) {
            QString deviceList = devices.join(", ");
            Logger::instance().error("CRITICAL: Duplicate NX_ROOT partition labels detected!");
            Logger::instance().error("We found " + QString::number(labelCount) +
                                    " devices with the label 'NX_ROOT': " + deviceList);
            Logger::instance().error("Please disconnect the external/secondary drive before updating");
            QFile::remove(lockPath);
            return false;
        }
    }
    Logger::instance().success("No duplicate labels detected");

    // Safety check 3: Verify NX_HOME and NX_VAR_LIB exist and check for stale mounts
    // The agent will mount these inside the chroot, but we verify them on the host first.
    Logger::instance().info("Verifying NX_HOME partition exists");
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_HOME"}, output, error)) {
        Logger::instance().error("Failed to find NX_HOME partition");
        Logger::instance().error("findfs error: " + error);
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().info("Found NX_HOME: " + output.trimmed());

    Logger::instance().info("Verifying NX_VAR_LIB partition exists");
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_VAR_LIB"}, output, error)) {
        Logger::instance().error("Failed to find NX_VAR_LIB partition");
        Logger::instance().error("findfs error: " + error);
        QFile::remove(lockPath);
        return false;
    }
    Logger::instance().info("Found NX_VAR_LIB: " + output.trimmed());

    Logger::instance().success("All partition safety checks passed");

    // Detect NVIDIA on the HOST before entering the chroot.
    // The chroot lower layer is a snapshot of the read-only root; it cannot
    // reliably access host kernel state such as /proc/driver/nvidia after
    // session boundaries.  The result is passed to the agent via the param file.
    bool hasNvidia = QDir("/proc/driver/nvidia").exists();
    if (!hasNvidia) {
        QString out, err;
        m_sysInterface->executeCommand("/usr/bin/lspci", {}, out, err);
        if (out.contains("NVIDIA", Qt::CaseInsensitive))
            hasNvidia = true;
    }
    Logger::instance().info(QString("NVIDIA hardware detected on host: %1")
                            .arg(hasNvidia ? "yes" : "no"));

    // Write the parameter file that nuts-agent reads when it starts.
    //
    // Path constraints: the file must be visible inside overlayroot-chroot,
    // which only bind-mounts /proc, /run, and /sys from the host into the
    // chroot at /media/root-ro/{proc,run,sys}.  /var/cache (workDir) lives on
    // the overlay tmpfs upperdir and is NOT visible inside the chroot.
    // /run IS bind-mounted, so /run/nuts-agent-params.ini is reachable from
    // both the host (writer) and the agent (reader).
    Q_EMIT updateProgress(7, "Preparing agent parameters");
    const QString paramFile = QStringLiteral("/run/nuts-agent-params.ini");
    QFile::remove(paramFile);  // Remove any stale file from a previous run.

    if (!writeAgentParams(paramFile, hasNvidia)) {
        Logger::instance().error("Failed to write agent parameter file");
        QFile::remove(lockPath);
        return false;
    }

    // --- SINGLE CHROOT CALL ---
    // Everything from partition mounting through dpkg configuration and
    // cleanup runs inside one persistent nuts-agent process.  Mounts created
    // by the agent remain alive for its entire lifetime — this is the fix
    // for the "session amnesia" problem.
    Q_EMIT updateProgress(10, "Entering update environment");
    Logger::instance().info("--- Launching nuts-agent in chroot (single session) ---");

    QString agentOutput, agentError;
    bool agentOk = m_sysInterface->executeInOverlayAgent(
        {"/usr/sbin/nuts-agent", paramFile},
        agentOutput,
        agentError);

    // Parse the agent's structured output and relay progress/log lines.
    parseAndRelayAgentOutput(agentOutput);

    // Clean up the parameter file regardless of outcome.
    QFile::remove(paramFile);

    if (!agentOk) {
        Logger::instance().error("nuts-agent exited with failure");
        if (!agentError.trimmed().isEmpty())
            Logger::instance().error("Agent stderr: " + agentError.trimmed());
        QFile::remove(lockPath);
        return false;
    }

    QFile::remove(lockPath);

    // Create a marker file to track that this update has been applied.
    // This prevents re-running the update when /etc/lsb-release hasn't been
    // updated yet (because the root filesystem is read-only until reboot).
    QFile rebootMarkerFile(rebootMarkerPath);
    if (rebootMarkerFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        rebootMarkerFile.write(m_minTarget.toUtf8());
        rebootMarkerFile.close();
        Logger::instance().info("Created reboot marker for version: " + m_minTarget);
    } else {
        Logger::instance().warning("Failed to create reboot marker file (non-critical)");
    }

    Q_EMIT updateProgress(100, "Update completed successfully");
    Logger::instance().success("=== System updated successfully. Please reboot. ===");
    return true;
}

// ----------------------
// Helper Implementations
// ----------------------

bool UpdateManager::writeAgentParams(const QString& filePath, bool hasNvidia) const {
    // Build the URLs that the agent needs.
    QString varLibUrl = QString(
        "https://raw.githubusercontent.com/Nitrux/storage/master/Other/var-lib-dpkg-%1.tar.xz")
        .arg(m_minTarget);

    // The dpkg AppImage URL is currently fixed; it can be promoted to a query
    // file field in a future update so it can be changed without recompiling.
    const QString dpkgAiUrl = QStringLiteral(
        "https://raw.githubusercontent.com/Nitrux/storage/master/Other/AppImages/"
        "dpkg-1.22.21-x86_64.AppImage");

    QString baseUrl = Config::instance().componentBaseUrl();
    if (!baseUrl.endsWith('/'))
        baseUrl += '/';
    const QString ccuUrl = baseUrl + "nuts-cpp-ccu";

    QSettings s(filePath, QSettings::IniFormat);
    s.beginGroup("Agent");
    s.setValue("MINTARGET",           m_minTarget);
    s.setValue("OTA_CHECKSUM",        m_otaChecksum);
    s.setValue("DPKG_AI_URL",         dpkgAiUrl);
    s.setValue("DPKG_AI_SUM",         m_queryData.value("DPKG_AI_SUM"));
    s.setValue("VAR_LIB_URL",         varLibUrl);
    s.setValue("VAR_LIB_SUM",         m_queryData.value("VAR_LIB_SUM"));
    s.setValue("NUTS_CCU_URL",        ccuUrl);
    s.setValue("NUTS_CCU_CHECKSUM",   m_queryData.value("NUTS_CCU_CHECKSUM"));
    s.setValue("DOWNLOAD_DIR",        Config::instance().downloadDir());
    s.setValue("SQUASHFS_DIR",        Config::instance().squashfsDir());
    s.setValue("HAS_NVIDIA",          hasNvidia);
    // Mirrors are pipe-delimited in a single value to keep the INI format simple.
    s.setValue("MIRRORS",             m_mirrorList.join('|'));
    s.endGroup();
    s.sync();

    if (s.status() != QSettings::NoError) {
        Logger::instance().error("Failed to write agent parameter file: " + filePath);
        return false;
    }

    // Restrict access to root only (mode 0600).
    QFile f(filePath);
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    Logger::instance().info("Agent parameter file written: " + filePath);
    return true;
}

void UpdateManager::parseAndRelayAgentOutput(const QString& output) {
    // The agent writes structured lines to stdout:
    //   [NUTS-AGENT] PROGRESS: <pct> <message>
    //   [NUTS-AGENT] INFO: <message>
    //   [NUTS-AGENT] SUCCESS: <message>
    //   [NUTS-AGENT] WARNING: <message>
    //   [NUTS-AGENT] ERROR: <message>
    //   [NUTS-AGENT] DEBUG: <message>

    static const QString progressPrefix = QStringLiteral("[NUTS-AGENT] PROGRESS: ");

    for (const QString& line : output.split('\n', Qt::SkipEmptyParts)) {
        if (line.startsWith(progressPrefix)) {
            // Parse "N message" from the remainder.
            QString rest = line.mid(progressPrefix.length()).trimmed();
            int spaceIdx = rest.indexOf(' ');
            if (spaceIdx > 0) {
                bool ok;
                int pct = rest.left(spaceIdx).toInt(&ok);
                if (ok) {
                    QString msg = rest.mid(spaceIdx + 1);
                    Q_EMIT updateProgress(pct, msg);
                }
            }
        } else if (line.contains("[NUTS-AGENT]")) {
            // Forward all other agent log lines verbatim.
            Logger::instance().info("Agent: " + line);
        }
    }
}

bool UpdateManager::downloadUpdateArchive(const QString& url, const QString& destination) {
    return m_sysInterface->downloadFile(url, destination);
}

bool UpdateManager::verifyUpdateArchive(const QString& filePath, const QString& expectedChecksum) {
    return m_sysInterface->verifyChecksum(filePath, expectedChecksum);
}

bool UpdateManager::checkDiskSpace() {
    // Check space on /home for OTA download.
    // downloadDir (/home/.nuts/downloads) may not exist yet — it is created
    // inside the chroot later.  Walk up to the nearest existing ancestor so
    // QStorageInfo can resolve the correct mount point.
    QString downloadDir = Config::instance().downloadDir();
    {
        QDir d(downloadDir);
        while (!d.exists() && !d.isRoot())
            d.cdUp();
        downloadDir = d.absolutePath();
    }
    qint64 availableHome = m_sysInterface->getAvailableSpace(downloadDir);

    // Estimate needed space: OTA size (from metadata) + 20% buffer
    qint64 estimatedOtaSize = 2LL * 1024 * 1024 * 1024; // Default 2GB if not known

    // Try to get actual size from the archive URLs if available.
    for (const QString& archiveUrl : getArchiveUrls()) {
        qint64 remoteSize = m_sysInterface->getRemoteFileSize(archiveUrl);
        if (remoteSize > 0) {
            estimatedOtaSize = remoteSize;
            break;
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
        Logger::instance().error(
            QString("Insufficient space on %1. Required: %2 GB, Available: %3 GB")
            .arg(downloadDir)
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
