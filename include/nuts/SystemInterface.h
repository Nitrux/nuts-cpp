// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include "NutsExport.h"
#include "Types.h"
#include <QString>
#include <QObject>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace Nuts {

class NUTS_EXPORT SystemInterface : public QObject {
    Q_OBJECT

public:
    explicit SystemInterface(QObject* parent = nullptr);

    // System information
    SystemInfo getSystemInfo();
    QString getRootPartition();
    QString getPartitionLabel(const QString& device);
    QString getPartitionFilesystem(const QString& device);
    bool isOverlayActive();

    // Connectivity checks
    bool checkInternetConnectivity();
    bool checkGitHubConnectivity();

    // Partition operations
    bool mountPartition(const QString& device, const QString& mountPoint);
    bool unmountPartition(const QString& mountPoint);
    bool isMounted(const QString& mountPoint);

    // Overlay operations
    bool executeInOverlay(const QStringList& command, QString& output, QString& error);

    // File operations
    bool downloadFile(const QString& url, const QString& destination);
    QString calculateMD5(const QString& filePath);
    QString calculateSHA256(const QString& filePath);
    bool verifyChecksum(const QString& filePath, const QString& expectedChecksum);
    
    // NEW: Security Verification
    bool verifyGPGSignature(const QString& dataFile, const QString& signatureFile);

    qint64 getRemoteFileSize(const QString& url);

    // Directory operations
    bool createDirectory(const QString& path);
    bool createSecureDirectory(const QString& path);
    bool enforceSecurePermissions(const QString& path);
    bool directoryExists(const QString& path);
    qint64 getDirectorySize(const QString& path);
    qint64 getAvailableSpace(const QString& path);

    // Command execution
    bool executeCommand(const QString& program, const QStringList& arguments,
                       QString& output, QString& error, int timeout = 120000);

Q_SIGNALS:
    void downloadProgress(int percentage, qint64 bytesReceived, qint64 bytesTotal);
    void commandOutput(const QString& output);

private:
    bool executeCommandAsync(const QString& program, const QStringList& arguments,
                            std::function<void(QProcess*)> callback);

    QNetworkAccessManager* m_networkManager{nullptr};
};

} // namespace Nuts
