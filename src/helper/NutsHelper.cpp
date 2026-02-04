// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "NutsHelper.h"
#include "nuts/Config.h"
#include "nuts/Logger.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QFile>
#include <QThread>
#include <unistd.h>

namespace Nuts {

NutsHelper::NutsHelper(QObject* parent)
    : QObject(parent) {
    initialize();
}

NutsHelper::~NutsHelper() {
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

    // Create managers
    m_backupManager = new BackupManager(m_sysInterface, this);
    m_updateManager = new UpdateManager(m_sysInterface, this);

    // Connect signals
    connectSignals();

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
    Q_EMIT ProgressChanged(static_cast<int>(status), percentage, message, details);
}

bool NutsHelper::PerformUpdate() {
    Logger::instance().info("Starting update operation");

    m_currentOperation = OperationType::Update;
    m_cancelled = false;

    // Run in separate thread to not block D-Bus
    QThread* thread = QThread::create([this]() {
        handleUpdateOperation();
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();

    return true;
}

void NutsHelper::handleUpdateOperation() {
    // Check connectivity
        emitProgress(OperationStatus::CheckingConnectivity, 5, "Checking connectivity");

        if (!m_sysInterface->checkInternetConnectivity()) {
            Q_EMIT OperationFailed("No internet connectivity");
            return;
        }

        if (!m_sysInterface->checkGitHubConnectivity()) {
            Q_EMIT OperationFailed("Cannot reach GitHub");
            return;
        }

        // Get system info
        SystemInfo sysInfo = m_sysInterface->getSystemInfo();

        // Download query file
        emitProgress(OperationStatus::DownloadingUpdate, 10, "Checking for updates");

        if (!m_updateManager->downloadQueryFile(Config::instance().branch())) {
            Q_EMIT OperationFailed("Failed to download update information");
            return;
        }

        // Check if update is available
        if (!m_updateManager->isUpdateAvailable(sysInfo.version)) {
            Q_EMIT OperationCompleted(true, "No update available");
            return;
        }

        // Create backup
        emitProgress(OperationStatus::CreatingBackup, 15, "Creating system backup");

        QString xfsBackupFile = Config::instance().xfsDir() + "/xfs-backup.xfs";
        QString compressedBackup = xfsBackupFile + ".zst";

        if (QFile::exists(compressedBackup)) {
            Logger::instance().info("Backup already exists, skipping");
        } else {
            if (!m_backupManager->createBackup(sysInfo.rootPartition, xfsBackupFile)) {
                Q_EMIT OperationFailed("Failed to create backup");
                return;
            }

            emitProgress(OperationStatus::CompressingBackup, 40, "Compressing backup");

            if (!m_backupManager->compressBackup(xfsBackupFile, compressedBackup)) {
                Q_EMIT OperationFailed("Failed to compress backup");
                return;
            }
        }

        // Apply update
        emitProgress(OperationStatus::ApplyingUpdate, 60, "Applying update");

        if (!m_updateManager->applyUpdate()) {
            Q_EMIT OperationFailed("Failed to apply update");
            return;
        }

        // Success
        Q_EMIT OperationCompleted(true, "Update completed successfully. System will reboot in 30 seconds.");

        // Schedule reboot
        QThread::sleep(30);

        // Emergency sync
        QFile::setPermissions("/proc/sysrq-trigger", QFileDevice::WriteUser | QFileDevice::WriteOwner);
        QFile sysrq("/proc/sys/kernel/sysrq");
        if (sysrq.open(QIODevice::WriteOnly)) {
            sysrq.write("1");
            sysrq.close();
        }

        QFile trigger("/proc/sysrq-trigger");
        if (trigger.open(QIODevice::WriteOnly)) {
            trigger.write("s");  // Emergency sync
            trigger.close();
        }

        sync();
        system("reboot");
}

bool NutsHelper::PerformRescue() {
    Logger::instance().info("Starting rescue operation");

    m_currentOperation = OperationType::Rescue;
    m_cancelled = false;

    QThread* thread = QThread::create([this]() {
        handleRescueOperation();
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();

    return true;
}

void NutsHelper::handleRescueOperation() {
    emitProgress(OperationStatus::CheckingConnectivity, 5, "Checking environment");

        // Check if running from Live session
        if (!QFile::exists("/usr/bin/calamares")) {
            Q_EMIT OperationFailed("Rescue operation can only be run from a Live session");
            return;
        }

        // Find partitions
        QString output, error;
        if (!m_sysInterface->executeCommand("findfs", {"LABEL=NX_ROOT"}, output, error)) {
            Q_EMIT OperationFailed("Cannot find NX_ROOT partition");
            return;
        }
        QString rootPartition = output.trimmed();

        if (!m_sysInterface->executeCommand("findfs", {"LABEL=NX_HOME"}, output, error)) {
            Q_EMIT OperationFailed("Cannot find NX_HOME partition");
            return;
        }
        QString homePartition = output.trimmed();

        // Mount partitions
        emitProgress(OperationStatus::RestoringBackup, 10, "Mounting partitions");

        QString rootMount = "/media/nitrux/NX_ROOT";
        QString homeMount = "/media/nitrux/NX_HOME";

        if (!m_sysInterface->mountPartition(rootPartition, rootMount)) {
            Q_EMIT OperationFailed("Failed to mount root partition");
            return;
        }

        if (!m_sysInterface->mountPartition(homePartition, homeMount)) {
            Q_EMIT OperationFailed("Failed to mount home partition");
            return;
        }

        // Locate backup
        QString compressedBackup = homeMount + "/.nuts/xfs/xfs-backup.xfs.zst";
        QString checksumFile = homeMount + "/.nuts/xfs/xfs-backup.md5sum";

        if (!QFile::exists(compressedBackup)) {
            Q_EMIT OperationFailed("Backup file not found");
            return;
        }

        // Verify backup
        emitProgress(OperationStatus::VerifyingUpdate, 20, "Verifying backup");

        if (!m_backupManager->verifyBackup(compressedBackup, checksumFile)) {
            Q_EMIT OperationFailed("Backup verification failed");
            return;
        }

        // Decompress backup
        emitProgress(OperationStatus::DecompressingBackup, 30, "Decompressing backup");

        QString decompressedBackup = homeMount + "/.nuts/xfs/xfs-backup.xfs";
        if (!m_backupManager->decompressBackup(compressedBackup, decompressedBackup)) {
            Q_EMIT OperationFailed("Failed to decompress backup");
            return;
        }

        // Restore backup
        emitProgress(OperationStatus::RestoringBackup, 50, "Restoring system");

        if (!m_backupManager->restoreBackup(decompressedBackup, rootMount)) {
            Q_EMIT OperationFailed("Failed to restore backup");
            return;
        }

        // Cleanup
        QFile::remove(decompressedBackup);

        m_sysInterface->unmountPartition(rootMount);
        m_sysInterface->unmountPartition(homeMount);

        Q_EMIT OperationCompleted(true, "System restored successfully");
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

bool NutsHelper::CheckConnectivity() {
    bool internet = m_sysInterface->checkInternetConnectivity();
    bool github = m_sysInterface->checkGitHubConnectivity();

    return internet && github;
}

void NutsHelper::Cancel() {
    Logger::instance().warning("Operation cancelled by user");
    m_cancelled = true;
}

} // namespace Nuts
