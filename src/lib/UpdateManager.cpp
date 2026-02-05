// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "nuts/UpdateManager.h"
#include "nuts/Logger.h"
#include "nuts/Config.h"
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>

namespace Nuts {

UpdateManager::UpdateManager(SystemInterface* sysInterface, QObject* parent)
    : QObject(parent)
    , m_sysInterface(sysInterface) {
}

bool UpdateManager::downloadQueryFile(const QString& /*branch*/) {
    QString url = Config::instance().queryFileUrl();
    QString sigUrl = url + ".sig"; // Append .sig for the signature URL

    QString workDir = Config::instance().workDir();
    QString destination = workDir + "/nuts-query.info";
    QString sigDestination = workDir + "/nuts-query.info.sig";

    // Ensure secure work directory exists
    if (!m_sysInterface->directoryExists(workDir)) {
        if (!m_sysInterface->createSecureDirectory(workDir)) {
            Logger::instance().error("Failed to create secure work directory");
            return false;
        }
    }

    // Remove existing files to ensure fresh downloads
    if (QFile::exists(destination)) {
        QFile::remove(destination);
    }
    if (QFile::exists(sigDestination)) {
        QFile::remove(sigDestination);
    }

    // 1. Download the Info File
    if (!m_sysInterface->downloadFile(url, destination)) {
        Logger::instance().error("Failed to download nuts-query.info");
        return false;
    }

    // 2. Download the Signature
    // We treat the signature as critical. If it's missing, we fail.
    Logger::instance().info("Downloading signature file...");
    if (!m_sysInterface->downloadFile(sigUrl, sigDestination)) {
        Logger::instance().error("Failed to download signature file (nuts-query.info.sig)");
        Logger::instance().error("Security policy requires a valid signature.");
        QFile::remove(destination); // Cleanup isolated file
        return false;
    }

    // 3. Verify Signature
    // Relies on verifyGPGSignature being implemented in SystemInterface
    Logger::instance().info("Verifying nuts-query.info signature...");
    if (!m_sysInterface->verifyGPGSignature(destination, sigDestination)) {
        Logger::instance().error("CRITICAL: Signature verification failed for nuts-query.info!");
        Logger::instance().error("The file may have been tampered with or is not signed by a trusted key.");
        
        // Cleanup potentially malicious files
        QFile::remove(destination);
        QFile::remove(sigDestination);
        
        return false;
    }

    Logger::instance().success("Signature verified. Trusted metadata loaded.");

    return parseQueryFile(destination);
}

bool UpdateManager::parseQueryFile(const QString& filePath) {
    QSettings settings(filePath, QSettings::IniFormat);

    if (settings.status() != QSettings::NoError) {
        Logger::instance().error("Failed to parse nuts-query.info");
        return false;
    }

    m_queryData.clear();

    // Read all keys and values
    for (const QString& key : settings.allKeys()) {
        QString value = settings.value(key).toString();
        m_queryData[key] = value;

        if (key == "MINTARGET") {
            m_minTarget = value;
        } else if (key == "UPDATE_URL") {
            m_updateUrl = value;
        } else if (key == "UPDATE_CHECKSUM") {
            m_updateChecksum = value;
        }
    }

    if (m_minTarget.isEmpty() || m_updateUrl.isEmpty() || m_updateChecksum.isEmpty()) {
        Logger::instance().error("Query file missing required fields (MINTARGET, UPDATE_URL, UPDATE_CHECKSUM)");
        return false;
    }

    // Verify NUTS_CCU_CHECKSUM exists for security
    // Since the file is now signed, we trust this checksum implicitly
    QString ccuChecksum = m_queryData.value("NUTS_CCU_CHECKSUM");
    if (ccuChecksum.isEmpty()) {
        Logger::instance().error("CRITICAL: NUTS_CCU_CHECKSUM missing - refusing to proceed for security");
        return false;
    }

    Logger::instance().info("Query file parsed successfully");
    Logger::instance().info("Minimum target version: " + m_minTarget);
    Logger::instance().info("CCU checksum present: " + ccuChecksum.left(16) + "...");

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

    // The actual update is performed by nuts-ccu component
    // We download it with checksum verification and execute it

    QString branch = Config::instance().branch();

    Q_EMIT updateProgress(10, "Mounting overlay");

    QString output, error;

    // Mount dev in overlay
    if (!m_sysInterface->executeInOverlay({"/usr/bin/mount", "-t", "devtmpfs", "dev", "/dev"}, output, error)) {
        Logger::instance().warning("Failed to mount /dev in overlay (might already be mounted)");
    }

    Q_EMIT updateProgress(20, "Downloading update component");

    // Download nuts-ccu with checksum verification
    // This checksum is trusted because it came from the Signed nuts-query.info
    QString ccuChecksum = m_queryData.value("NUTS_CCU_CHECKSUM");
    if (ccuChecksum.isEmpty()) {
        Logger::instance().error("CRITICAL: NUTS_CCU_CHECKSUM not found - aborting update");
        return false;
    }

    if (!downloadAndVerifyComponent("nuts-ccu", ccuChecksum)) {
        Logger::instance().error("Failed to securely download nuts-ccu component");
        return false;
    }

    Q_EMIT updateProgress(40, "Downloading query info");

    // Download nuts-query.info to secure work directory
    // We already have it from the check phase, but we download it again to ensure it's fresh/present
    // and verify the signature again implicitly via the same rigorous process if needed, 
    // OR simply copy the one we just verified.
    // For simplicity/robustness, we re-download (or you could optimize to copy).
    // Given the previous verification, downloading it again without verification implies trusting the server again.
    // OPTIMIZATION: Use the one we already verified in downloadQueryFile.
    
    // NOTE: In the original flow, it downloaded it again.
    // Ideally, we should just use the existing one at Config::instance().workDir() + "/nuts-query.info"
    // Since we verified it in downloadQueryFile, and it's in a secure directory (0700), it's safe.
    
    QString workDir = Config::instance().workDir();
    QString queryDest = workDir + "/nuts-query.info";
    
    if (!QFile::exists(queryDest)) {
         // If it's missing (unexpected), we must re-download AND re-verify
         if (!downloadQueryFile(branch)) { // Re-uses the secure download+verify logic
             Logger::instance().error("Failed to retrieve verified nuts-query.info");
             return false;
         }
    }

    Q_EMIT updateProgress(60, "Executing update");

    // Execute nuts-ccu from secure work directory
    QString ccuPath = workDir + "/nuts-ccu";

    if (!m_sysInterface->executeCommand(ccuPath, {}, output, error)) {
        Logger::instance().error("Update execution failed: " + error);
        return false;
    }

    Q_EMIT updateProgress(100, "Update completed");

    Logger::instance().success("Update applied successfully");

    return true;
}

bool UpdateManager::downloadAndVerifyComponent(const QString& componentName, const QString& expectedChecksum) {
    Logger::instance().info("Securely downloading " + componentName + " with checksum verification");

    QString url = Config::instance().componentBaseUrl() + componentName;
    QString workDir = Config::instance().workDir();
    QString tempDest = workDir + "/" + componentName + ".download";

    // Ensure secure work directory exists
    if (!m_sysInterface->directoryExists(workDir)) {
        if (!m_sysInterface->createSecureDirectory(workDir)) {
            Logger::instance().error("Failed to create secure work directory");
            return false;
        }
    }

    // Step 1: Download to secure work directory
    if (!m_sysInterface->downloadFile(url, tempDest)) {
        Logger::instance().error("Failed to download " + componentName);
        return false;
    }

    // Step 2: CRITICAL - Verify checksum BEFORE making executable
    Logger::instance().info("Verifying " + componentName + " checksum...");
    if (!m_sysInterface->verifyChecksum(tempDest, expectedChecksum)) {
        Logger::instance().error("SECURITY ALERT: " + componentName + " checksum verification FAILED!");
        Logger::instance().error("Possible compromise attempt detected - aborting update");
        QFile::remove(tempDest);
        return false;
    }

    Logger::instance().success(componentName + " checksum verified successfully");

    // Step 3: Only now is it safe to rename and make executable
    QString destination = workDir + "/" + componentName;
    QFile::remove(destination);
    QFile::rename(tempDest, destination);

    QString output, error;

    // Make executable
    if (!m_sysInterface->executeCommand("chmod", {"+x", destination}, output, error)) {
        Logger::instance().error("Failed to make " + componentName + " executable");
        QFile::remove(destination);
        return false;
    }

    Logger::instance().success("Securely installed: " + componentName);

    return true;
}

} // namespace Nuts
