// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "nuts/UpdateManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QProcess>

namespace Nuts {

UpdateManager::UpdateManager(SystemInterface* sysInterface, QObject* parent)
    : QObject(parent)
    , m_sysInterface(sysInterface) {
}

bool UpdateManager::downloadQueryFile(const QString& branch) {
    QString url = QString("https://raw.githubusercontent.com/Nitrux/nuts/%1/tmp/nuts-query.info").arg(branch);
    QString destination = "/tmp/nuts-query.info";

    // Remove existing file
    if (QFile::exists(destination)) {
        QFile::remove(destination);
        Logger::instance().info("Overwriting existing nuts-query.info");
    }

    if (!m_sysInterface->downloadFile(url, destination)) {
        Logger::instance().error("Failed to download nuts-query.info");
        return false;
    }

    return parseQueryFile(destination);
}

bool UpdateManager::parseQueryFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Logger::instance().error("Failed to open nuts-query.info");
        return false;
    }

    m_queryData.clear();

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();

        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        int equalPos = line.indexOf('=');
        if (equalPos == -1) {
            continue;
        }

        QString key = line.left(equalPos).trimmed();
        QString value = line.mid(equalPos + 1).trimmed();

        // Remove quotes
        value.remove('"');

        m_queryData[key] = value;

        if (key == "MINTARGET") {
            m_minTarget = value;
        } else if (key == "UPDATE_URL") {
            m_updateUrl = value;
        } else if (key == "UPDATE_CHECKSUM") {
            m_updateChecksum = value;
        }
    }

    file.close();

    Logger::instance().info("Query file parsed successfully");
    Logger::instance().info("Minimum target version: " + m_minTarget);

    return true;
}

bool UpdateManager::isUpdateAvailable(const QString& currentVersion) {
    if (m_minTarget.isEmpty()) {
        Logger::instance().error("No minimum target version available");
        return false;
    }

    bool updateAvailable = (currentVersion == m_minTarget);

    if (updateAvailable) {
        Logger::instance().info("Update is available for version " + currentVersion);
    } else {
        Logger::instance().info("No update available for version " + currentVersion);
    }

    return updateAvailable;
}

bool UpdateManager::downloadUpdateArchive(const QString& url, const QString& destination) {
    Logger::instance().info("Downloading update archive");

    Q_EMIT downloadProgress(0, 0, 0);

    if (!m_sysInterface->downloadFile(url, destination)) {
        Logger::instance().error("Failed to download update archive");
        return false;
    }

    Q_EMIT downloadProgress(100, 0, 0);

    return true;
}

bool UpdateManager::verifyUpdateArchive(const QString& filePath, const QString& expectedChecksum) {
    Logger::instance().info("Verifying update archive");

    Q_EMIT verificationProgress(50);

    bool result = m_sysInterface->verifyChecksum(filePath, expectedChecksum);

    Q_EMIT verificationProgress(100);

    return result;
}

bool UpdateManager::applyUpdate() {
    Logger::instance().info("Applying update");

    Q_EMIT updateProgress(0, "Preparing to apply update");

    // The actual update is performed by nuts-cru component
    // We download it and execute it in the overlay

    QString branch = Config::instance().branch();

    Q_EMIT updateProgress(10, "Mounting overlay");

    QString output, error;

    // Mount dev in overlay
    if (!m_sysInterface->executeInOverlay({"mount", "-t", "devtmpfs", "dev", "/dev"}, output, error)) {
        Logger::instance().warning("Failed to mount /dev in overlay (might already be mounted)");
    }

    Q_EMIT updateProgress(20, "Downloading update component");

    // Download nuts-cru
    if (!downloadComponent("nuts-cru", branch)) {
        Logger::instance().error("Failed to download nuts-cru component");
        return false;
    }

    Q_EMIT updateProgress(40, "Downloading query info");

    // Download nuts-query.info to the overlay
    if (!m_sysInterface->executeInOverlay(
            {"axel", "-o", "/tmp", "-c", "-n", "10",
             QString("https://raw.githubusercontent.com/Nitrux/nuts/%1/tmp/nuts-query.info").arg(branch)},
            output, error)) {
        Logger::instance().error("Failed to download nuts-query.info to overlay");
        return false;
    }

    Q_EMIT updateProgress(60, "Executing update");

    // Execute nuts-cru
    if (!m_sysInterface->executeInOverlay({"nuts-cru"}, output, error)) {
        Logger::instance().error("Update execution failed: " + error);
        return false;
    }

    Q_EMIT updateProgress(100, "Update completed");

    Logger::instance().success("Update applied successfully");

    return true;
}

bool UpdateManager::downloadComponent(const QString& componentName, const QString& branch) {
    QString url = QString("https://raw.githubusercontent.com/Nitrux/nuts/%1/tmp/%2").arg(branch, componentName);
    QString destination = "/usr/bin/" + componentName;

    // Remove old component in overlay
    QString output, error;
    m_sysInterface->executeInOverlay({"find", "/usr/bin", "-type", "f", "-name", componentName, "-exec", "rm", "-v", "{}", ";"}, output, error);

    // Download component in overlay
    if (!m_sysInterface->executeInOverlay(
            {"axel", "-o", "/usr/bin", "-c", "-n", "10", url},
            output, error)) {
        Logger::instance().error("Failed to download " + componentName);
        return false;
    }

    // Make executable
    if (!m_sysInterface->executeInOverlay({"chmod", "+x", destination}, output, error)) {
        Logger::instance().error("Failed to make " + componentName + " executable");
        return false;
    }

    Logger::instance().info("Downloaded and installed: " + componentName);

    return true;
}

} // namespace Nuts
