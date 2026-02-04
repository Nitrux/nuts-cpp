// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include <QString>
#include <QObject>

namespace Nuts {

class Config : public QObject {
    Q_OBJECT

public:
    static Config& instance();

    bool load(const QString& configPath = "/etc/nuts.conf");

    QString downloadDir() const { return m_downloadDir; }
    QString squashfsDir() const { return m_squashfsDir; }
    QString backupDir() const { return m_backupDir; }
    QString xfsDir() const { return m_xfsDir; }
    QString logFile() const { return m_logFile; }
    QString branch() const { return m_branch; }

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
    QString m_logFile{"/var/log/nuts.log"};
    QString m_branch{"main"};
};

} // namespace Nuts
