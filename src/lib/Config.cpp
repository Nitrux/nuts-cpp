// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#include "nuts/Config.h"
#include <QFile>
#include <QFileInfo> 
#include <QTextStream>
#include <QDebug>
#include <QSettings>

namespace Nuts {

Config& Config::instance() {
    static Config instance;
    return instance;
}

bool Config::load(const QString& configPath) {
    if (!QFile::exists(configPath)) {
        qWarning() << "Config file does not exist:" << configPath;
        return false;
    }

    // Verify ownership and permissions
    QFileInfo configInfo(configPath);
    
    // Ensure file is owned by root (ID 0)
    if (configInfo.ownerId() != 0) {
        qWarning() << "SECURITY ERROR: Config file must be owned by root!";
        return false;
    }

    // Ensure file is not writable by others (World Write)
    QFile::Permissions perms = configInfo.permissions();
    if (perms & QFile::WriteOther) {
         qWarning() << "SECURITY ERROR: Config file is world-writable!";
         return false;
    }

    QSettings settings(configPath, QSettings::IniFormat);

    if (settings.status() != QSettings::NoError) {
        qWarning() << "Failed to parse config file:" << configPath;
        return false;
    }

    // Load directory settings with path validation
    QString downloadDir = settings.value(QStringLiteral("NUTS_DIR_DLS"), m_downloadDir).toString();
    QString squashfsDir = settings.value(QStringLiteral("NUTS_DIR_SQS"), m_squashfsDir).toString();
    QString backupDir = settings.value(QStringLiteral("NUTS_DIR_BAK"), m_backupDir).toString();
    QString xfsDir = settings.value(QStringLiteral("NUTS_DIR_XFS"), m_xfsDir).toString();
    QString logFile = settings.value(QStringLiteral("NUTS_LOG"), m_logFile).toString();
    QString branch = settings.value(QStringLiteral("NUTS_BRANCH"), m_branch).toString();

    // SECURITY: Validate paths don't contain traversal sequences
    if (downloadDir.contains(QStringLiteral("../")) || downloadDir.contains(QStringLiteral("..\\"))) {
        qWarning() << "SECURITY ERROR: Download directory contains path traversal sequence!";
        return false;
    }
    if (squashfsDir.contains(QStringLiteral("../")) || squashfsDir.contains(QStringLiteral("..\\"))) {
        qWarning() << "SECURITY ERROR: SquashFS directory contains path traversal sequence!";
        return false;
    }
    if (backupDir.contains(QStringLiteral("../")) || backupDir.contains(QStringLiteral("..\\"))) {
        qWarning() << "SECURITY ERROR: Backup directory contains path traversal sequence!";
        return false;
    }
    if (xfsDir.contains(QStringLiteral("../")) || xfsDir.contains(QStringLiteral("..\\"))) {
        qWarning() << "SECURITY ERROR: XFS directory contains path traversal sequence!";
        return false;
    }
    if (logFile.contains(QStringLiteral("../")) || logFile.contains(QStringLiteral("..\\"))) {
        qWarning() << "SECURITY ERROR: Log file path contains path traversal sequence!";
        return false;
    }

    // Assign validated paths
    m_downloadDir = downloadDir;
    m_squashfsDir = squashfsDir;
    m_backupDir = backupDir;
    m_xfsDir = xfsDir;
    m_logFile = logFile;
    m_branch = branch;

    return true;
}

QString Config::queryFileUrl() const {
    QString url = m_queryFileUrlTemplate;
    return url.replace(QStringLiteral("{branch}"), m_branch);
}

QString Config::componentBaseUrl() const {
    QString url = m_componentBaseUrlTemplate;
    return url.replace(QStringLiteral("{branch}"), m_branch);
}

QString Config::releaseNotesUrl() const {
    return m_releaseNotesUrlTemplate;
}

} // namespace Nuts
