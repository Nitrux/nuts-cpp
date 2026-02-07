// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include "NutsExport.h"
#include <QString>
#include <QObject>

namespace Nuts {

class NUTS_EXPORT Config : public QObject {
    Q_OBJECT

public:
    static Config& instance();

    bool load(const QString& configPath = "/etc/nuts-cpp.conf");

    QString downloadDir() const { return m_downloadDir; }
    QString squashfsDir() const { return m_squashfsDir; }
    QString backupDir() const { return m_backupDir; }
    QString xfsDir() const { return m_xfsDir; }
    QString logFile() const { return m_logFile; }
    QString branch() const { return m_branch; }
    QString workDir() const { return m_workDir; }

    // URL configuration getters
    QString queryFileUrl() const;
    QString componentBaseUrl() const;
    QString osReleaseUrl() const { return m_osReleaseUrl; }
    QString releaseNotesUrl() const;
    QString connectivityCheckUrl() const { return m_connectivityCheckUrl; }
    QString githubConnectivityCheckUrl() const { return m_githubConnectivityCheckUrl; }

    void setDownloadDir(const QString& dir) { m_downloadDir = dir; }
    void setSquashfsDir(const QString& dir) { m_squashfsDir = dir; }
    void setBackupDir(const QString& dir) { m_backupDir = dir; }
    void setXfsDir(const QString& dir) { m_xfsDir = dir; }
    void setLogFile(const QString& path) { m_logFile = path; }
    void setBranch(const QString& branch) { m_branch = branch; }

private:
    Config() = default;
    ~Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    QString m_downloadDir{"/home/.nuts/downloads"};
    QString m_squashfsDir{"/home/.nuts/squashfs"};
    QString m_backupDir{"/home/.nuts/backup"};
    QString m_xfsDir{"/home/.nuts/xfs"};
    QString m_logFile{"/var/log/nuts-cpp.log"};
    QString m_branch{"main"};
    QString m_workDir{"/var/cache/nuts-cpp"};

    // Centralized URL configuration
    QString m_queryFileUrlTemplate{"https://raw.githubusercontent.com/Nitrux/nuts-cpp-components/refs/heads/{branch}/query/nuts-cpp-query.info"};
    QString m_componentBaseUrlTemplate{"https://raw.githubusercontent.com/Nitrux/nuts-cpp-components/refs/heads/{branch}/components/"};
    QString m_osReleaseUrl{"https://raw.githubusercontent.com/Nitrux/nitrux-base-files/refs/heads/{branch}/etc/os-release"};
    QString m_releaseNotesUrlTemplate{"https://raw.githubusercontent.com/Nitrux/nuts-cpp-components/refs/heads/{branch}/summary/{version}/nuts-cpp-summary.md"};
    QString m_connectivityCheckUrl{"http://1.1.1.1"};
    QString m_githubConnectivityCheckUrl{"https://raw.githubusercontent.com/Nitrux/storage/refs/heads/master/Other/sample1.txt"};
};

} // namespace Nuts
