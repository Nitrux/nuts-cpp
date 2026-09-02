// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#pragma once

#include <QString>
#include <QObject>

namespace Nuts {

enum class OperationType {
    Update,
    Rescue,
    SelfUpdate
};

enum class OperationStatus {
    Idle,
    CheckingConnectivity,
    CreatingBackup,
    CompressingBackup,
    DownloadingUpdate,
    VerifyingUpdate,
    ApplyingUpdate,
    RestoringBackup,
    DecompressingBackup,
    Completed,
    Failed
};

struct OperationProgress {
    OperationStatus status;
    int percentage{0};
    QString message;
    QString details;

    OperationProgress() : status(OperationStatus::Idle) {}
    OperationProgress(OperationStatus s, int p = 0, const QString& m = QString(), const QString& d = QString())
        : status(s), percentage(p), message(m), details(d) {}
};

struct SystemInfo {
    QString distribution;
    QString version;
    QString rootPartition;
    QString rootLabel;
    QString rootFilesystem;
    QString homePartition;
    QString homeLabel;
    bool overlayActive{false};
};

struct BackupInfo {
    QString filePath;
    QString checksum;
    qint64 size{0};
    bool exists{false};
    bool valid{false};
};

enum class LogLevel {
    Debug,
    Info,
    Success,
    Warning,
    Error
};

} // namespace Nuts

Q_DECLARE_METATYPE(Nuts::OperationProgress)
Q_DECLARE_METATYPE(Nuts::OperationStatus)
Q_DECLARE_METATYPE(Nuts::SystemInfo)
Q_DECLARE_METATYPE(Nuts::BackupInfo)
