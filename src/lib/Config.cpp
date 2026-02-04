// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>

namespace Nuts {

Config& Config::instance() {
    static Config instance;
    return instance;
}

bool Config::load(const QString& configPath) {
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open config file:" << configPath;
        return false;
    }

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();

        // Skip comments and empty lines
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        // Parse key=value pairs
        int equalPos = line.indexOf('=');
        if (equalPos == -1) {
            continue;
        }

        QString key = line.left(equalPos).trimmed();
        QString value = line.mid(equalPos + 1).trimmed();

        if (key == "NUTS_DIR_DLS") {
            m_downloadDir = value;
        } else if (key == "NUTS_DIR_SQS") {
            m_squashfsDir = value;
        } else if (key == "NUTS_DIR_BAK") {
            m_backupDir = value;
        } else if (key == "NUTS_DIR_XFS") {
            m_xfsDir = value;
        } else if (key == "NUTS_LOG") {
            m_logFile = value;
        } else if (key == "NUTS_BRANCH") {
            m_branch = value;
        }
    }

    file.close();
    return true;
}

} // namespace Nuts
