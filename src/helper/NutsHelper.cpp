#include "NutsHelper.h"
#include "nuts/Config.h"
#include "nuts/Logger.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QFile>
#include <QThread>
#include <QtConcurrent>
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#include <unistd.h>
#include <sys/reboot.h>
#include <signal.h>

namespace Nuts {

// Static instance for signal handler
NutsHelper* NutsHelper::s_instance = nullptr;

NutsHelper::NutsHelper(QObject* parent)
    : QObject(parent) {
    s_instance = this;
    initialize();
}

NutsHelper::~NutsHelper() {
    s_instance = nullptr;
    cleanup();
}

void NutsHelper::initialize() {
    // Check if running as root
    if (getuid() != 0) {
        Logger::instance().error("NUTS helper must run as root");
        QCoreApplication::exit(1);
        return;
    }

    // Load configuration
    if (!Config::instance().load()) {
        Logger::instance().warning("Failed to load configuration, using defaults");
    }

    // Set up logging
    Logger::instance().setLogFile(Config::instance().logFile());

    // Create system interface
    m_sysInterface = new SystemInterface(this);

    // Create or secure work directory
    QString workDir = Config::instance().workDir();
    if (!m_sysInterface->directoryExists(workDir)) {
        // Securely create directory (0700 + root ownership)
        if (!m_sysInterface->createSecureDirectory(workDir)) {
            Logger::instance().error("Failed to create secure work directory");
            QCoreApplication::exit(1);
            return;
        }
    } else {
        // Directory exists - enforce permissions/ownership to prevent tampering
        if (!m_sysInterface->enforceSecurePermissions(workDir)) {
            Logger::instance().error("Failed to enforce secure permissions on work directory");
            QCoreApplication::exit(1);
            return;
        }
    }

    // Create managers
    m_backupManager = new BackupManager(m_sysInterface, this);
    m_updateManager = new UpdateManager(m_sysInterface, this);

    // Connect signals
    connectSignals();

    // Set up signal handlers for graceful shutdown
    struct sigaction sa;
    sa.sa_handler = &NutsHelper::signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    Logger::instance().info("NUTS helper initialized");
}

void NutsHelper::cleanup() {
    Logger::instance().info("NUTS helper shutting down");
}

void NutsHelper::connectSignals() {
    // Forward logger messages to D-Bus
    connect(&Logger::instance(), &Logger::logMessage, this,
            [this](LogLevel level, const QString& message) {
                Q_EMIT LogMessage(static_cast<int>(level), message);
            });

    // Forward backup progress
    connect(m_backupManager, &BackupManager::backupProgress, this,
            [this](int percentage, const QString& message) {
                emitProgress(OperationStatus::CreatingBackup, percentage, message);
            });

    connect(m_backupManager, &BackupManager::compressionProgress, this,
            [this](int percentage) {
                emitProgress(OperationStatus::CompressingBackup, percentage, "Compressing backup");
            });

    connect(m_backupManager, &BackupManager::decompressionProgress, this,
            [this](int percentage) {
                emitProgress(OperationStatus::DecompressingBackup, percentage, "Decompressing backup");
            });

    connect(m_backupManager, &BackupManager::restorationProgress, this,
            [this](int percentage) {
                emitProgress(OperationStatus::RestoringBackup, percentage, "Restoring backup");
            });

    // Forward update progress
    connect(m_updateManager, &UpdateManager::downloadProgress, this,
            [this](int percentage, qint64 bytesReceived, qint64 bytesTotal) {
                QString details = QString("Downloaded: %1 MB / %2 MB")
                                     .arg(bytesReceived / 1024.0 / 1024.0, 0, 'f', 2)
                                     .arg(bytesTotal / 1024.0 / 1024.0, 0, 'f', 2);
                emitProgress(OperationStatus::DownloadingUpdate, percentage,
                            "Downloading update", details);
            });

    connect(m_updateManager, &UpdateManager::verificationProgress, this,
            [this](int percentage) {
                emitProgress(OperationStatus::VerifyingUpdate, percentage, "Verifying update");
            });

    connect(m_updateManager, &UpdateManager::updateProgress, this,
            [this](int percentage, const QString& message) {
                emitProgress(OperationStatus::ApplyingUpdate, percentage, message);
            });
}

void NutsHelper::emitProgress(OperationStatus status, int percentage,
                              const QString& message, const QString& details) {
    // Use QMetaObject::invokeMethod to ensure signal is emitted from main thread
    // This is required for D-Bus signals to work properly when called from worker threads
    QMetaObject::invokeMethod(this, [this, status, percentage, message, details]() {
        Q_EMIT ProgressChanged(static_cast<int>(status), percentage, message, details);
    }, Qt::QueuedConnection);
}

void NutsHelper::emitOperationCompleted(bool success, const QString& message) {
    // Emit from main thread for D-Bus compatibility
    QMetaObject::invokeMethod(this, [this, success, message]() {
        Q_EMIT OperationCompleted(success, message);
    }, Qt::QueuedConnection);
}

void NutsHelper::emitOperationFailed(const QString& error) {
    // Emit from main thread for D-Bus compatibility
    QMetaObject::invokeMethod(this, [this, error]() {
        Q_EMIT OperationFailed(error);
    }, Qt::QueuedConnection);
}

bool NutsHelper::PerformUpdate() {

    // Check authorization before proceeding
    if (!checkAuthorization("org.nxos.nuts.update")) {
        return false;  // Error already sent by checkAuthorization
    }

    Logger::instance().info("Starting update operation");

    m_currentOperation = OperationType::Update;


    // Run asynchronously using QtConcurrent to not block D-Bus
    (void)QtConcurrent::run([this]() {
        handleUpdateOperation();
    });

    return true;
}

void NutsHelper::handleUpdateOperation() {
    // Check connectivity
    emitProgress(OperationStatus::CheckingConnectivity, 5, "Checking connectivity");

    if (!m_sysInterface->checkInternetConnectivity()) {
        emitOperationFailed("No internet connectivity");
        return;
    }

    if (!m_sysInterface->checkGitHubConnectivity()) {
        emitOperationFailed("Cannot reach GitHub");
        return;
    }

    // Get system info
    SystemInfo sysInfo = m_sysInterface->getSystemInfo();

    // Download query file
    emitProgress(OperationStatus::DownloadingUpdate, 10, "Checking for updates");

    if (!m_updateManager->downloadQueryFile(Config::instance().branch())) {
        emitOperationFailed("Failed to download update information");
        return;
    }

    // Check if update is available
    if (!m_updateManager->isUpdateAvailable(sysInfo.version)) {
        emitOperationCompleted(true, "No update available");
        return;
    }

    // Create backup
    emitProgress(OperationStatus::CreatingBackup, 15, "Creating system backup");

    // Ensure required directories exist before attempting backup
    const QString xfsDir = Config::instance().xfsDir();
    const QString backupDir = Config::instance().backupDir();

    if (!m_sysInterface->directoryExists(xfsDir)) {
        if (!m_sysInterface->createSecureDirectory(xfsDir)) {
            emitOperationFailed("Failed to create XFS directory: " + xfsDir);
            return;
        }
    }

    if (!m_sysInterface->directoryExists(backupDir)) {
        if (!m_sysInterface->createSecureDirectory(backupDir)) {
            emitOperationFailed("Failed to create backup directory: " + backupDir);
            return;
        }
    }

    QString xfsBackupFile = xfsDir + "/xfs-backup.xfs";
    QString compressedBackup = xfsBackupFile + ".zst";

    if (QFile::exists(compressedBackup)) {
        Logger::instance().info("Backup already exists, skipping");
    } else {
        if (!m_backupManager->createBackup(sysInfo.rootPartition, xfsBackupFile)) {
            emitOperationFailed("Failed to create backup");
            return;
        }

        emitProgress(OperationStatus::CompressingBackup, 40, "Compressing backup");

        if (!m_backupManager->compressBackup(xfsBackupFile, compressedBackup)) {
            emitOperationFailed("Failed to compress backup");
            return;
        }
    }

    // Apply update
    emitProgress(OperationStatus::ApplyingUpdate, 60, "Applying update");

    if (!m_updateManager->applyUpdate()) {
        emitOperationFailed("Failed to apply update");
        return;
    }

    // Success
    emitOperationCompleted(true, "Update completed successfully. System will reboot in 30 seconds.");

    // Schedule reboot
    QThread::sleep(30);

    // Perform standard reboot sequence
    Logger::instance().info("Initiating system reboot");

    // Sync filesystem buffers (twice for safety)
    sync();
    QThread::msleep(500);
    sync();
    QThread::msleep(500);

    // Use standard reboot command with absolute path to prevent PATH injection
    QString output, error;
    if (!m_sysInterface->executeCommand("/usr/sbin/reboot", {}, output, error, 5000)) {
        Logger::instance().warning("Reboot command failed, trying direct syscall");

        // Fallback to direct kernel syscall (no shell, no PATH lookup)
        sync();
        ::reboot(RB_AUTOBOOT);
    }
}

bool NutsHelper::PerformRescue() {

    Logger::instance().info("PerformRescue D-Bus method called");

    // Check authorization before proceeding
    Logger::instance().info("Checking authorization for org.nxos.nuts.rescue");
    if (!checkAuthorization("org.nxos.nuts.rescue")) {
        Logger::instance().error("Authorization check failed");
        return false;  // Error already sent by checkAuthorization
    }

    Logger::instance().success("Authorization granted");
    Logger::instance().info("Starting rescue operation");

    m_currentOperation = OperationType::Rescue;


    // Run asynchronously using QtConcurrent to not block D-Bus
    Logger::instance().info("Launching rescue operation in background thread");
    (void)QtConcurrent::run([this]() {
        handleRescueOperation();
    });

    Logger::instance().info("PerformRescue returning true");
    return true;
}

void NutsHelper::handleRescueOperation() {
    Logger::instance().info("=== Starting Rescue Operation ===");
    emitProgress(OperationStatus::CheckingConnectivity, 5, "Checking environment");

    // Check if running from Live session
    Logger::instance().info("Checking for Live session (looking for /usr/bin/calamares)");
    if (!QFile::exists("/usr/bin/calamares")) {
        Logger::instance().error("Not running from Live session - calamares not found");
        emitOperationFailed("Rescue operation can only be run from a Live session");
        return;
    }
    Logger::instance().success("Live session detected");

    // Check if a rescue operation was already completed in this live session.
    // This prevents re-running the rescue when the user hasn't rebooted yet.
    const QString rescueMarkerPath = "/var/run/nuts-cpp-rescue-completed";
    if (QFile::exists(rescueMarkerPath)) {
        Logger::instance().warning("A rescue operation was already completed in this session");
        emitOperationFailed("Rescue operation already completed. Please reboot the system to verify the restoration.");
        return;
    }

    // Find partitions with absolute path to prevent PATH injection
    Logger::instance().info("Searching for NX_ROOT partition");
    QString output, error;
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_ROOT"}, output, error)) {
        Logger::instance().error("Failed to find NX_ROOT partition");
        Logger::instance().error("findfs error: " + error);
        emitOperationFailed("Cannot find NX_ROOT partition. Error: " + error);
        return;
    }
    QString rootPartition = output.trimmed();
    Logger::instance().info("Found NX_ROOT: " + rootPartition);

    // Safety check 1: Verify filesystem type (must be XFS for xfsrestore to work)
    Logger::instance().info("Verifying NX_ROOT filesystem type");
    if (!m_sysInterface->executeCommand("/usr/sbin/blkid", {"-o", "value", "-s", "TYPE", rootPartition}, output, error)) {
        Logger::instance().error("Failed to determine filesystem type of NX_ROOT");
        Logger::instance().error("blkid error: " + error);
        emitOperationFailed("Cannot determine NX_ROOT filesystem type. Error: " + error);
        return;
    }
    QString rootFilesystem = output.trimmed();
    Logger::instance().info("NX_ROOT filesystem: " + rootFilesystem);

    if (rootFilesystem != "xfs") {
        Logger::instance().error("The filesystem of NX_ROOT is " + rootFilesystem + ", not XFS");
        emitOperationFailed("The filesystem of NX_ROOT is " + rootFilesystem +
                           ", not XFS. xfsrestore requires an XFS partition.");
        return;
    }
    Logger::instance().success("Filesystem type verified: XFS");

    // Safety check 2: Check for duplicate NX_ROOT labels to prevent restoring to the wrong drive
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
            emitOperationFailed("Duplicate NX_ROOT partition labels detected! Found " +
                               QString::number(labelCount) + " devices: " + deviceList +
                               ". Please disconnect the external/secondary drive to ensure safe restoration.");
            return;
        }
    }
    Logger::instance().success("No duplicate labels detected");

    Logger::instance().info("Searching for NX_HOME partition");
    if (!m_sysInterface->executeCommand("/usr/sbin/findfs", {"LABEL=NX_HOME"}, output, error)) {
        Logger::instance().error("Failed to find NX_HOME partition");
        Logger::instance().error("findfs error: " + error);
        emitOperationFailed("Cannot find NX_HOME partition. Error: " + error);
        return;
    }
    QString homePartition = output.trimmed();
    Logger::instance().info("Found NX_HOME: " + homePartition);

    // Mount partitions
    emitProgress(OperationStatus::RestoringBackup, 10, "Mounting partitions");

    QString rootMount = "/media/nitrux/NX_ROOT";
    QString homeMount = "/media/nitrux/NX_HOME";

    // Safety check 3: Unmount if already mounted (from previous interrupted operation)
    Logger::instance().info("Checking for stale mounts at " + rootMount);
    if (m_sysInterface->isMounted(rootMount)) {
        Logger::instance().warning(rootMount + " is already mounted. Unmounting...");
        if (!m_sysInterface->unmountPartition(rootMount)) {
            Logger::instance().error("Failed to unmount stale mount at " + rootMount);
            emitOperationFailed("Failed to unmount stale mount at " + rootMount +
                               ". Please unmount manually before retrying.");
            return;
        }
        Logger::instance().success("Stale mount unmounted");
    }

    Logger::instance().info("Checking for stale mounts at " + homeMount);
    if (m_sysInterface->isMounted(homeMount)) {
        Logger::instance().warning(homeMount + " is already mounted. Unmounting...");
        if (!m_sysInterface->unmountPartition(homeMount)) {
            Logger::instance().error("Failed to unmount stale mount at " + homeMount);
            emitOperationFailed("Failed to unmount stale mount at " + homeMount +
                               ". Please unmount manually before retrying.");
            return;
        }
        Logger::instance().success("Stale mount unmounted");
    }

    Logger::instance().info("Creating mount point: " + rootMount);
    m_sysInterface->createDirectory(rootMount);

    Logger::instance().info("Mounting " + rootPartition + " to " + rootMount);
    if (!m_sysInterface->mountPartition(rootPartition, rootMount)) {
        Logger::instance().error("Failed to mount root partition");
        emitOperationFailed("Failed to mount root partition");
        return;
    }
    Logger::instance().success("Root partition mounted");

    Logger::instance().info("Creating mount point: " + homeMount);
    m_sysInterface->createDirectory(homeMount);

    Logger::instance().info("Mounting " + homePartition + " to " + homeMount);
    if (!m_sysInterface->mountPartition(homePartition, homeMount)) {
        Logger::instance().error("Failed to mount home partition");
        m_sysInterface->unmountPartition(rootMount);
        emitOperationFailed("Failed to mount home partition");
        return;
    }
    Logger::instance().success("Home partition mounted");

    // Locate backup
    QString compressedBackup = homeMount + "/.nuts/xfs/xfs-backup.xfs.zst";
    QString checksumFile = homeMount + "/.nuts/xfs/xfs-backup.md5sum";

    Logger::instance().info("Looking for backup file: " + compressedBackup);
    if (!QFile::exists(compressedBackup)) {
        Logger::instance().error("Backup file not found at: " + compressedBackup);
        Logger::instance().info("Listing contents of " + homeMount + "/.nuts/xfs/");
        QString lsOutput, lsError;
        m_sysInterface->executeCommand("/usr/bin/ls", {"-la", homeMount + "/.nuts/xfs/"}, lsOutput, lsError);
        Logger::instance().info("Directory contents:\n" + lsOutput);

        m_sysInterface->unmountPartition(homeMount);
        m_sysInterface->unmountPartition(rootMount);
        emitOperationFailed("Backup file not found at: " + compressedBackup);
        return;
    }
    Logger::instance().success("Backup file found");

    // Verify backup
    emitProgress(OperationStatus::VerifyingUpdate, 20, "Verifying backup");
    Logger::instance().info("Verifying backup checksum");

    if (!m_backupManager->verifyBackup(compressedBackup, checksumFile)) {
        Logger::instance().error("Backup verification failed");
        m_sysInterface->unmountPartition(homeMount);
        m_sysInterface->unmountPartition(rootMount);
        emitOperationFailed("Backup verification failed");
        return;
    }
    Logger::instance().success("Backup verified successfully");

    // Decompress backup
    emitProgress(OperationStatus::DecompressingBackup, 30, "Decompressing backup");
    Logger::instance().info("Decompressing backup");

    QString decompressedBackup = homeMount + "/.nuts/xfs/xfs-backup.xfs";
    if (!m_backupManager->decompressBackup(compressedBackup, decompressedBackup)) {
        Logger::instance().error("Failed to decompress backup");
        m_sysInterface->unmountPartition(homeMount);
        m_sysInterface->unmountPartition(rootMount);
        emitOperationFailed("Failed to decompress backup");
        return;
    }
    Logger::instance().success("Backup decompressed successfully");

    // Restore backup
    emitProgress(OperationStatus::RestoringBackup, 50, "Restoring system");
    Logger::instance().info("Restoring backup to " + rootMount);

    if (!m_backupManager->restoreBackup(decompressedBackup, rootMount)) {
        Logger::instance().error("Failed to restore backup");
        QFile::remove(decompressedBackup);
        m_sysInterface->unmountPartition(homeMount);
        m_sysInterface->unmountPartition(rootMount);
        emitOperationFailed("Failed to restore backup");
        return;
    }
    Logger::instance().success("Backup restored successfully");

    // Cleanup
    Logger::instance().info("Cleaning up temporary files");
    QFile::remove(decompressedBackup);

    Logger::instance().info("Unmounting partitions");
    m_sysInterface->unmountPartition(rootMount);
    m_sysInterface->unmountPartition(homeMount);

    // Create a marker file to track that the rescue operation completed successfully.
    // This prevents re-running the rescue operation in the same live session.
    const QString rescueMarkerPath = "/var/run/nuts-cpp-rescue-completed";
    QFile rescueMarkerFile(rescueMarkerPath);
    if (rescueMarkerFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QDateTime now = QDateTime::currentDateTime();
        rescueMarkerFile.write(now.toString(Qt::ISODate).toUtf8());
        rescueMarkerFile.close();
        Logger::instance().info("Created rescue completion marker");
    } else {
        Logger::instance().warning("Failed to create rescue completion marker (non-critical)");
    }

    Logger::instance().success("=== Rescue Operation Completed Successfully ===");
    emitOperationCompleted(true, "System restored successfully. Please reboot to verify the restoration.");
}

QVariantMap NutsHelper::GetSystemInfo() {

    SystemInfo info = m_sysInterface->getSystemInfo();

    QVariantMap map;
    map["distribution"] = info.distribution;
    map["version"] = info.version;
    map["rootPartition"] = info.rootPartition;
    map["rootLabel"] = info.rootLabel;
    map["rootFilesystem"] = info.rootFilesystem;
    map["homePartition"] = info.homePartition;
    map["homeLabel"] = info.homeLabel;
    map["overlayActive"] = info.overlayActive;

    return map;
}

QVariantMap NutsHelper::CheckForUpdates() {

    QVariantMap result;

    Logger::instance().info("Checking for updates");

    // Check connectivity first
    if (!m_sysInterface->checkInternetConnectivity()) {
        result["available"] = false;
        result["error"] = "No internet connectivity";
        return result;
    }

    if (!m_sysInterface->checkGitHubConnectivity()) {
        result["available"] = false;
        result["error"] = "Cannot reach GitHub";
        return result;
    }

    // Get current system info
    SystemInfo sysInfo = m_sysInterface->getSystemInfo();
    result["currentVersion"] = sysInfo.version;

    // Download and parse query file
    if (!m_updateManager->downloadQueryFile(Config::instance().branch())) {
        result["available"] = false;
        result["error"] = "Failed to download update information";
        return result;
    }

    // Check if update is available
    bool available = m_updateManager->isUpdateAvailable(sysInfo.version);
    result["available"] = available;

    if (available) {
        QString targetVersion = m_updateManager->getMinTarget();
        QString updateUrl = m_updateManager->getUpdateUrl();

        result["targetVersion"] = targetVersion;
        result["updateUrl"] = updateUrl;
        result["updateChecksum"] = m_updateManager->getUpdateChecksum();

        // Fetch file size (cached for this check)
        qint64 fileSize = m_sysInterface->getRemoteFileSize(updateUrl);
        result["updateSize"] = fileSize;

        // Build release notes URL
        // Sanitize version string to prevent URL injection
        QString urlVersion = m_sysInterface->sanitizeVersionString(targetVersion);
        if (urlVersion.isEmpty()) {
            Logger::instance().error("SECURITY: Invalid version string from update metadata");
            result["releaseNotesUrl"] = "";
        } else {
            urlVersion.replace(" ", "-");

            QString releaseNotesUrl = Config::instance().releaseNotesUrl();
            releaseNotesUrl.replace("{branch}", Config::instance().branch());
            releaseNotesUrl.replace("{version}", urlVersion);
            result["releaseNotesUrl"] = releaseNotesUrl;
        }

        Logger::instance().info("Update available: " + targetVersion);
        Logger::instance().info("Update size: " + QString::number(fileSize / (1024.0 * 1024.0), 'f', 2) + " MB");
    } else {
        Logger::instance().info("No update available");
    }

    return result;
}

bool NutsHelper::CheckConnectivity() {

    bool internet = m_sysInterface->checkInternetConnectivity();
    bool github = m_sysInterface->checkGitHubConnectivity();

    return internet && github;
}


bool NutsHelper::checkAuthorization(const QString& actionId) {
    // Get the D-Bus caller's service name
    QString callerService = message().service();

    if (callerService.isEmpty()) {
        Logger::instance().error("Authorization failed: Cannot identify caller");
        sendErrorReply(QDBusError::AccessDenied, "Cannot identify D-Bus caller");
        return false;
    }

    // Create Polkit subject from D-Bus caller
    PolkitQt1::SystemBusNameSubject subject(callerService);

    // Check authorization
    PolkitQt1::Authority::Result result = PolkitQt1::Authority::instance()->checkAuthorizationSync(
        actionId,
        subject,
        PolkitQt1::Authority::AllowUserInteraction  // Allow password prompt
    );

    if (result != PolkitQt1::Authority::Yes) {
        Logger::instance().error("Authorization failed for action: " + actionId);
        Logger::instance().error("Caller: " + callerService);
        sendErrorReply(QDBusError::AccessDenied,
                      "Authorization required for " + actionId);
        return false;
    }

    Logger::instance().info("Authorization granted for action: " + actionId);
    return true;
}


void NutsHelper::signalHandler(int signal) {
    if (s_instance) {
        Logger::instance().warning(QString("Received signal %1, performing emergency cleanup").arg(signal));
        s_instance->emergencyCleanup();
    }
    QCoreApplication::exit(128 + signal);
}

void NutsHelper::emergencyCleanup() {
    Logger::instance().warning("Emergency cleanup initiated");

    // Attempt to unmount any mounted filesystems
    if (m_sysInterface) {
        QString squashfsDir = Config::instance().squashfsDir();

        // Try to unmount in reverse order
        m_sysInterface->unmountPartition(squashfsDir);
        m_sysInterface->unmountPartition("/var/lib");
        m_sysInterface->unmountPartition("/home");
    }

    // Remove temporary dpkg symlinks
    QStringList tools = {"dpkg", "dpkg-deb", "dpkg-query", "update-alternatives",
                         "dpkg-divert", "dpkg-realpath", "dpkg-split",
                         "dpkg-statoverride", "dpkg-trigger", "dpkg-maintscript-helper"};

    for (const QString& tool : tools) {
        QString linkPath = "/usr/bin/" + tool;
        if (QFile::exists(linkPath)) {
            QFile::remove(linkPath);
        }
    }

    Logger::instance().info("Emergency cleanup completed");
}

} // namespace Nuts
