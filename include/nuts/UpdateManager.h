// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include "NutsExport.h"
#include "Types.h"
#include "SystemInterface.h"
#include <QObject>
#include <QString>
#include <QStringList>
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
    qint64 getOtaSize() const { return m_otaSize; }
    QStringList getArchiveUrls() const {
        if (!m_mirrorList.isEmpty())
            return m_mirrorList;
        if (!m_updateUrl.isEmpty())
            return QStringList{m_updateUrl};
        return {};
    }

Q_SIGNALS:
    void downloadProgress(int percentage, qint64 bytesReceived, qint64 bytesTotal);
    void verificationProgress(int percentage);
    void updateProgress(int percentage, const QString& message);

private:
    SystemInterface* m_sysInterface;

    QString m_minTarget;
    QString m_otaFile;
    QString m_otaBranch;
    QString m_updateUrl;
    QString m_updateChecksum;
    QString m_otaChecksum;
    qint64 m_otaSize{0};
    QStringList m_mirrorList;
    QMap<QString, QString> m_queryData;

    // Pre-chroot helpers (run on host)
    bool checkDiskSpace();

    // Write the parameter file for nuts-agent and launch it in a single chroot session.
    bool writeAgentParams(const QString& filePath, bool hasNvidia) const;

    // Parse structured output from nuts-agent and relay progress signals.
    void parseAndRelayAgentOutput(const QString& output);

    // Version comparison helper
    static int compareVersions(const QString& version1, const QString& version2);
};

} // namespace Nuts
