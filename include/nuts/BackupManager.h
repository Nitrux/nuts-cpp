// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#pragma once

#include "NutsExport.h"
#include "Types.h"
#include "SystemInterface.h"
#include <QObject>
#include <QString>

namespace Nuts {

class NUTS_EXPORT BackupManager : public QObject {
    Q_OBJECT

public:
    explicit BackupManager(SystemInterface* sysInterface, QObject* parent = nullptr);

    // Backup operations
    bool createBackup(const QString& partition, const QString& outputPath);
    bool compressBackup(const QString& inputPath, const QString& outputPath);
    bool restoreBackup(const QString& backupPath, const QString& targetMount);
    bool decompressBackup(const QString& compressedPath, const QString& outputPath);

    // Backup information
    BackupInfo getBackupInfo(const QString& backupPath);
    bool backupExists(const QString& backupPath);
    bool verifyBackup(const QString& backupPath, const QString& checksumPath);

    // Checksum operations
    bool createChecksum(const QString& filePath, const QString& checksumPath);
    QString readChecksum(const QString& checksumPath);

Q_SIGNALS:
    void backupProgress(int percentage, const QString& message);
    void compressionProgress(int percentage);
    void decompressionProgress(int percentage);
    void restorationProgress(int percentage);
    void commandOutput(const QString& output);

private:
    SystemInterface* m_sysInterface;

    bool executeXfsdump(const QString& device, const QString& outputFile);
    bool executeXfsrestore(const QString& backupFile, const QString& targetMount);
    bool executeZstdCompress(const QString& inputFile, const QString& outputFile);
    bool executeZstdDecompress(const QString& inputFile, const QString& outputFile);
};

} // namespace Nuts
