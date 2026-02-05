// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "nuts/Config.h"
#include <QFile>
#include <QFileInfo>  // <--- Added this required header
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

    // Security Check: Verify ownership and permissions
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

    // Load directory settings
    m_downloadDir = settings.value("NUTS_DIR_DLS", m_downloadDir).toString();
    m_squashfsDir = settings.value("NUTS_DIR_SQS", m_squashfsDir).toString();
    m_backupDir = settings.value("NUTS_DIR_BAK", m_backupDir).toString();
    m_xfsDir = settings.value("NUTS_DIR_XFS", m_xfsDir).toString();
    m_logFile = settings.value("NUTS_LOG", m_logFile).toString();
    m_branch = settings.value("NUTS_BRANCH", m_branch).toString();

    // Load URL settings (with existing defaults)
    m_queryFileUrlTemplate = settings.value("NUTS_QUERY_URL", m_queryFileUrlTemplate).toString();
    m_componentBaseUrlTemplate = settings.value("NUTS_COMPONENT_URL", m_componentBaseUrlTemplate).toString();
    m_osReleaseUrl = settings.value("NUTS_OS_RELEASE_URL", m_osReleaseUrl).toString();
    m_otaBaseUrl = settings.value("NUTS_OTA_BASE_URL", m_otaBaseUrl).toString();
    m_releaseNotesUrlTemplate = settings.value("NUTS_RELEASE_NOTES_URL", m_releaseNotesUrlTemplate).toString();

    return true;
}

QString Config::queryFileUrl() const {
    QString url = m_queryFileUrlTemplate;
    return url.replace("{branch}", m_branch);
}

QString Config::componentBaseUrl() const {
    QString url = m_componentBaseUrlTemplate;
    return url.replace("{branch}", m_branch);
}

QString Config::releaseNotesUrl() const {
    return m_releaseNotesUrlTemplate;
}

} // namespace Nuts
