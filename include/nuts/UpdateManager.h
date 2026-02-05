// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include "NutsExport.h"
#include "Types.h"
#include "SystemInterface.h"
#include <QObject>
#include <QString>
#include <QMap>

namespace Nuts {

class NUTS_EXPORT UpdateManager : public QObject {
    Q_OBJECT

public:
    explicit UpdateManager(SystemInterface* sysInterface, QObject* parent = nullptr);

    // Update query operations
    bool downloadQueryFile(const QString& branch);
    bool parseQueryFile(const QString& filePath);
    bool isUpdateAvailable(const QString& currentVersion);

    // Update operations
    bool downloadUpdateArchive(const QString& url, const QString& destination);
    bool verifyUpdateArchive(const QString& filePath, const QString& expectedChecksum);
    bool applyUpdate();

    // Getters for query info
    QString getMinTarget() const { return m_minTarget; }
    QString getUpdateUrl() const { return m_updateUrl; }
    QString getUpdateChecksum() const { return m_updateChecksum; }

Q_SIGNALS:
    void downloadProgress(int percentage, qint64 bytesReceived, qint64 bytesTotal);
    void verificationProgress(int percentage);
    void updateProgress(int percentage, const QString& message);

private:
    SystemInterface* m_sysInterface;

    QString m_minTarget;
    QString m_updateUrl;
    QString m_updateChecksum;
    QString m_otaChecksum;
    QString m_pkgManagerPath;
    QStringList m_mirrorList;
    QMap<QString, QString> m_queryData;

    // Helper methods for update process
    bool checkDiskSpace();
    bool prepareSystemPartitions();
    bool downloadOTAPayload();
    bool verifySquashFSIntegrity(const QString& squashfsPath);
    bool mountOTAPayload();
    bool prepareUpdateTools();
    bool syncPackageData();
    bool performPackageUpdates();
    bool runCleanupCrew();
    void cleanup();

    bool downloadAndVerifyComponent(const QString& componentName, const QString& expectedChecksum);
};

} // namespace Nuts
