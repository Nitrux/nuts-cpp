// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#include "nuts/BackupManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QRegularExpression>
#include <QTextStream>

namespace Nuts {

BackupManager::BackupManager(SystemInterface* sysInterface, QObject* parent)
    : QObject(parent)
    , m_sysInterface(sysInterface) {
}

bool BackupManager::createBackup(const QString& partition, const QString& outputPath) {
    Logger::instance().info(QStringLiteral("Creating XFS backup of ") + partition);

    // Validate output path to prevent path traversal (backup files go into xfsDir)
    QString xfsDir = Config::instance().xfsDir();
    if (!m_sysInterface->validatePath(outputPath, xfsDir)) {
        Logger::instance().error("SECURITY: Invalid backup output path");
        return false;
    }

    Q_EMIT backupProgress(0, QStringLiteral("Starting backup..."));

    if (!executeXfsdump(partition, outputPath)) {
        Logger::instance().error("XFS backup failed");
        return false;
    }

    Q_EMIT backupProgress(100, QStringLiteral("Backup completed"));
    Logger::instance().success(QStringLiteral("XFS backup created: ") + outputPath);

    return true;
}

bool BackupManager::executeXfsdump(const QString& device, const QString& outputFile) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args = {
        QStringLiteral("-l0"),  // Level 0 (full backup)
        QStringLiteral("-L"), QStringLiteral("Nitrux_Update_Tool_System-XFS_Backup"),
        QStringLiteral("-M"), QStringLiteral("NX_BAK-Vol-1"),
        QStringLiteral("-f"), outputFile,
        device
    };

    // SECURITY: Use absolute path to prevent PATH injection
    process.start(QStringLiteral("/usr/sbin/xfsdump"), args);

    if (!process.waitForStarted()) {
        Logger::instance().error("Failed to start xfsdump");
        return false;
    }

    while (process.state() == QProcess::Running) {
        process.waitForReadyRead(100);
        QString output = QString::fromUtf8(process.readAll());

        // Emit periodic progress updates
        // xfsdump doesn't provide exact progress, so we estimate based on time
        if (!output.isEmpty()) {
            Q_EMIT commandOutput(output);
        }

        QThread::msleep(100);
    }

    process.waitForFinished(-1);

    return process.exitCode() == 0;
}

bool BackupManager::compressBackup(const QString& inputPath, const QString& outputPath) {
    Logger::instance().info(QStringLiteral("Compressing backup with zstd"));

    // Validate paths to prevent path traversal
    QString backupDir = Config::instance().backupDir();
    QString xfsDir = Config::instance().xfsDir();

    // The XFS backup file always lives in xfsDir; backupDir is a secondary
    // allowed location.  The first validatePath() call is silent so that it
    // doesn't log a spurious security error when the file is legitimately in
    // xfsDir and the backupDir check would never be reached.
    if (!m_sysInterface->validatePath(inputPath, xfsDir, /*logError=*/false) &&
        !m_sysInterface->validatePath(inputPath, backupDir)) {
        Logger::instance().error("SECURITY: Invalid input path for compression");
        return false;
    }

    if (!m_sysInterface->validatePath(outputPath, xfsDir)) {
        Logger::instance().error("SECURITY: Invalid output path for compression");
        return false;
    }

    Q_EMIT compressionProgress(0);

    if (!executeZstdCompress(inputPath, outputPath)) {
        Logger::instance().error("Backup compression failed");
        return false;
    }

    Q_EMIT compressionProgress(100);
    Logger::instance().success(QStringLiteral("Backup compressed: ") + outputPath);

    // Remove uncompressed backup atomically (avoid TOCTOU)
    QFile inputFile(inputPath);
    if (inputFile.exists()) {
        inputFile.remove();
        Logger::instance().info("Removed uncompressed backup");
    }

    // Create checksum
    QString checksumPath = Config::instance().xfsDir()  + QStringLiteral("/xfs-backup.md5sum");
    if (!createChecksum(outputPath, checksumPath)) {
        Logger::instance().warning("Failed to create checksum file");
    }

    return true;
}

bool BackupManager::executeZstdCompress(const QString& inputFile, const QString& outputFile) {
    int threads = QThread::idealThreadCount();
    Logger::instance().info(QStringLiteral("Using %1 threads for compression").arg(threads));

    QProcess process;
    QStringList args = {
        QStringLiteral("-T%1").arg(threads),
        QStringLiteral("-3"),  // Compression level
        inputFile,
        QStringLiteral("-o"), outputFile
    };

    process.start(QStringLiteral("/usr/bin/zstd"), args);

    if (!process.waitForStarted()) {
        Logger::instance().error("Failed to start zstd");
        return false;
    }

    process.waitForFinished(-1);

    return process.exitCode() == 0;
}

bool BackupManager::restoreBackup(const QString& backupPath, const QString& targetMount) {
    Logger::instance().info(QStringLiteral("Restoring backup to ") + targetMount);

    // Validate backup path to prevent path traversal.
    // Allow two modes:
    // - Normal: /home/.nuts/xfs/xfs-backup.xfs (running system)
    // - Rescue: /media/nitrux/NX_HOME/.nuts/xfs/xfs-backup.xfs (Live session)
    QString xfsDir = Config::instance().xfsDir();
    if (!m_sysInterface->validatePath(backupPath, xfsDir, /*logError=*/false) &&
        !m_sysInterface->validatePath(backupPath, QStringLiteral("/media"), /*logError=*/false)) {
        Logger::instance().error(QStringLiteral("SECURITY: Invalid backup path for restoration: ") + backupPath);
        Logger::instance().error(QStringLiteral("Path must be under ") + xfsDir  + QStringLiteral(" or /media"));
        return false;
    }

    // Validate target mount point (should be under /mnt or /media for rescue mode).
    // - Normal: /mnt/... (manual restore)
    // - Rescue: /media/nitrux/NX_ROOT (Live session mount point)
    if (!m_sysInterface->validatePath(targetMount, QStringLiteral("/mnt"), /*logError=*/false) &&
        !m_sysInterface->validatePath(targetMount, QStringLiteral("/media"), /*logError=*/false)) {
        Logger::instance().error(QStringLiteral("SECURITY: Invalid target mount point for restoration: ") + targetMount);
        Logger::instance().error("Path must be under /mnt or /media");
        return false;
    }

    Q_EMIT restorationProgress(0);

    // First, wipe the target directory
    QProcess wipProcess;
    wipProcess.start(QStringLiteral("/usr/bin/find"), QStringList{targetMount, QStringLiteral("-mindepth"), QStringLiteral("1"), QStringLiteral("-delete")});
    wipProcess.waitForFinished(-1);

    if (!executeXfsrestore(backupPath, targetMount)) {
        Logger::instance().error("Backup restoration failed");
        return false;
    }

    Q_EMIT restorationProgress(100);
    Logger::instance().success("Backup restored successfully");

    return true;
}

bool BackupManager::executeXfsrestore(const QString& backupFile, const QString& targetMount) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);

    QStringList args = {
        QStringLiteral("-f"), backupFile,
        targetMount
    };

    process.start(QStringLiteral("/usr/sbin/xfsrestore"), args);

    if (!process.waitForStarted()) {
        Logger::instance().error("Failed to start xfsrestore");
        return false;
    }

    while (process.state() == QProcess::Running) {
        process.waitForReadyRead(100);
        QString output = QString::fromUtf8(process.readAll());

        if (!output.isEmpty()) {
            Q_EMIT commandOutput(output);
        }

        QThread::msleep(100);
    }

    process.waitForFinished(-1);

    return process.exitCode() == 0;
}

bool BackupManager::decompressBackup(const QString& compressedPath, const QString& outputPath) {
    Logger::instance().info("Decompressing backup");

    Q_EMIT decompressionProgress(0);

    if (!executeZstdDecompress(compressedPath, outputPath)) {
        Logger::instance().error("Backup decompression failed");
        return false;
    }

    Q_EMIT decompressionProgress(100);
    Logger::instance().success("Backup decompressed");

    return true;
}

bool BackupManager::executeZstdDecompress(const QString& inputFile, const QString& outputFile) {
    QProcess process;
    QStringList args = {
        QStringLiteral("-d"),
        QStringLiteral("-f"),
        QStringLiteral("-o"), outputFile,
        inputFile
    };

    process.start(QStringLiteral("/usr/bin/zstd"), args);

    if (!process.waitForStarted()) {
        Logger::instance().error("Failed to start zstd");
        return false;
    }

    process.waitForFinished(-1);

    return process.exitCode() == 0;
}

BackupInfo BackupManager::getBackupInfo(const QString& backupPath) {
    BackupInfo info;
    info.filePath = backupPath;
    info.exists = QFile::exists(backupPath);

    if (info.exists) {
        QFileInfo fileInfo(backupPath);
        info.size = fileInfo.size();

        QString checksumPath = Config::instance().xfsDir()  + QStringLiteral("/xfs-backup.md5sum");
        info.valid = verifyBackup(backupPath, checksumPath);

        if (info.valid) {
            info.checksum = readChecksum(checksumPath);
        }
    }

    return info;
}

bool BackupManager::backupExists(const QString& backupPath) {
    return QFile::exists(backupPath);
}

bool BackupManager::verifyBackup(const QString& backupPath, const QString& checksumPath) {
    if (!QFile::exists(checksumPath)) {
        Logger::instance().warning(QStringLiteral("Checksum file not found: ") + checksumPath);
        return false;
    }

    QString expectedChecksum = readChecksum(checksumPath);
    if (expectedChecksum.isEmpty()) {
        return false;
    }

    return m_sysInterface->verifyChecksum(backupPath, expectedChecksum);
}

bool BackupManager::createChecksum(const QString& filePath, const QString& checksumPath) {
    QString checksum = m_sysInterface->calculateMD5(filePath);
    if (checksum.isEmpty()) {
        return false;
    }

    QFile file(checksumPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Logger::instance().error(QStringLiteral("Failed to create checksum file: ") + checksumPath);
        return false;
    }

    QTextStream out(&file);
    out << checksum << "  " << QFileInfo(filePath).fileName() << "\n";
    file.close();

    Logger::instance().info(QStringLiteral("Checksum file created: ") + checksumPath);
    return true;
}

QString BackupManager::readChecksum(const QString& checksumPath) {
    QFile file(checksumPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Logger::instance().error(QStringLiteral("Failed to read checksum file: ") + checksumPath);
        return QString();
    }

    QTextStream in(&file);
    QString line = in.readLine();
    file.close();

    // Parse checksum from "checksum  filename" format
    QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")));
    if (parts.isEmpty()) {
        return QString();
    }

    return parts.first();
}

} // namespace Nuts
