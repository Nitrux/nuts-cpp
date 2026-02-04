// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "NutsClient.h"
#include <QDBusReply>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QProcess>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>

namespace Nuts {

NutsClient::NutsClient(QObject* parent)
    : QObject(parent) {
    connectToHelper();
}

NutsClient::~NutsClient() {
    delete m_helperInterface;
}

void NutsClient::connectToHelper() {
    // Check if helper is running
    QDBusConnectionInterface* interface = QDBusConnection::systemBus().interface();
    if (!interface->isServiceRegistered("org.nxos.nuts")) {
        qDebug() << "Helper not running, will start on first operation";
    }

    // Create interface (will auto-start via D-Bus activation)
    m_helperInterface = new QDBusInterface("org.nxos.nuts",
                                           "/org/nxos/nuts",
                                           "org.nxos.nuts",
                                           QDBusConnection::systemBus(),
                                           this);

    if (!m_helperInterface->isValid()) {
        qWarning() << "Failed to connect to NUTS helper:" << m_helperInterface->lastError().message();
        m_connected = false;
    } else {
        m_connected = true;

        // Connect signals
        QDBusConnection::systemBus().connect("org.nxos.nuts", "/org/nxos/nuts", "org.nxos.nuts",
                                            "ProgressChanged", this,
                                            SLOT(onProgressChanged(int, int, QString, QString)));

        QDBusConnection::systemBus().connect("org.nxos.nuts", "/org/nxos/nuts", "org.nxos.nuts",
                                            "OperationCompleted", this,
                                            SLOT(onOperationCompleted(bool, QString)));

        QDBusConnection::systemBus().connect("org.nxos.nuts", "/org/nxos/nuts", "org.nxos.nuts",
                                            "OperationFailed", this,
                                            SLOT(onOperationFailed(QString)));

        QDBusConnection::systemBus().connect("org.nxos.nuts", "/org/nxos/nuts", "org.nxos.nuts",
                                            "LogMessage", this,
                                            SLOT(onLogMessage(int, QString)));

        qDebug() << "Connected to NUTS helper";
    }

    Q_EMIT connectedChanged();
}

void NutsClient::performUpdate() {
    if (!m_connected || m_busy) {
        return;
    }

    m_busy = true;
    Q_EMIT busyChanged();

    m_statusMessage = "Starting update...";
    Q_EMIT statusMessageChanged();

    showNotification("NUTS Update", "System update starting...", KNotification::Persistent);

    // Use pkexec to start the helper with elevated privileges
    QProcess* process = new QProcess(this);
    process->start("pkexec", {"/usr/libexec/nuts-helper"});

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        process->deleteLater();

        if (exitCode == 0) {
            // Helper started, now call the D-Bus method
            QDBusReply<bool> reply = m_helperInterface->call("PerformUpdate");
            if (!reply.isValid()) {
                qWarning() << "Failed to call PerformUpdate:" << reply.error().message();
                m_busy = false;
                Q_EMIT busyChanged();
                onOperationFailed("Failed to start update: " + reply.error().message());
            }
        } else {
            m_busy = false;
            Q_EMIT busyChanged();
            onOperationFailed("Authentication cancelled or failed");
        }
    });
}

void NutsClient::performRescue() {
    if (!m_connected || m_busy) {
        return;
    }

    m_busy = true;
    Q_EMIT busyChanged();

    m_statusMessage = "Starting rescue...";
    Q_EMIT statusMessageChanged();

    showNotification("NUTS Rescue", "System rescue starting...", KNotification::Persistent);

    QProcess* process = new QProcess(this);
    process->start("pkexec", {"/usr/libexec/nuts-helper"});

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        process->deleteLater();

        if (exitCode == 0) {
            QDBusReply<bool> reply = m_helperInterface->call("PerformRescue");
            if (!reply.isValid()) {
                qWarning() << "Failed to call PerformRescue:" << reply.error().message();
                m_busy = false;
                Q_EMIT busyChanged();
                onOperationFailed("Failed to start rescue: " + reply.error().message());
            }
        } else {
            m_busy = false;
            Q_EMIT busyChanged();
            onOperationFailed("Authentication cancelled or failed");
        }
    });
}

void NutsClient::cancel() {
    if (!m_connected || !m_busy) {
        return;
    }

    m_helperInterface->call("Cancel");
}

void NutsClient::checkForUpdates() {
    if (!m_connected || m_busy) {
        return;
    }

    m_statusMessage = "Checking for updates...";
    Q_EMIT statusMessageChanged();

    // Read local VERSION from /etc/os-release
    QFile localFile("/etc/os-release");
    if (!localFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to read /etc/os-release";
        m_statusMessage = "Error reading system version";
        Q_EMIT statusMessageChanged();
        return;
    }

    QString localVersion;
    QTextStream in(&localFile);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.startsWith("VERSION=")) {
            localVersion = line.mid(8).remove('"');
            break;
        }
    }
    localFile.close();

    if (localVersion.isEmpty()) {
        qWarning() << "VERSION not found in /etc/os-release";
        return;
    }

    // Fetch remote os-release from GitHub
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QUrl remoteOsReleaseUrl("https://raw.githubusercontent.com/Nitrux/nitrux-base-files/refs/heads/main/etc/os-release");

    QNetworkReply* reply = manager->get(QNetworkRequest(remoteOsReleaseUrl));
    connect(reply, &QNetworkReply::finished, this, [this, reply, localVersion, manager]() {
        reply->deleteLater();
        manager->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Failed to fetch remote os-release:" << reply->errorString();
            m_statusMessage = "Failed to check for updates";
            Q_EMIT statusMessageChanged();
            return;
        }

        QString remoteContent = reply->readAll();
        QString remoteVersion;

        QTextStream stream(&remoteContent);
        while (!stream.atEnd()) {
            QString line = stream.readLine();
            if (line.startsWith("VERSION=")) {
                remoteVersion = line.mid(8).remove('"');
                break;
            }
        }

        if (remoteVersion.isEmpty() || remoteVersion == localVersion) {
            m_updateAvailable = false;
            Q_EMIT updateAvailableChanged();
            m_statusMessage = "No updates available";
            Q_EMIT statusMessageChanged();
            return;
        }

        // Check if OTA archive exists at SourceForge
        QNetworkAccessManager* sfManager = new QNetworkAccessManager(this);
        QString otaFilename = QString("nitrux-nx-desktop-plasma-amd64-%1.zsync").arg(remoteVersion);
        QUrl sourceForgeUrl(QString("https://master.dl.sourceforge.net/project/nitruxos/Updates/%1").arg(otaFilename));

        QNetworkReply* sfReply = sfManager->head(QNetworkRequest(sourceForgeUrl));
        connect(sfReply, &QNetworkReply::finished, this, [this, sfReply, sfManager, remoteVersion]() {
            sfReply->deleteLater();
            sfManager->deleteLater();

            if (sfReply->error() != QNetworkReply::NoError) {
                m_updateAvailable = false;
                Q_EMIT updateAvailableChanged();
                m_statusMessage = "Update metadata found but OTA archive not available yet";
                Q_EMIT statusMessageChanged();
                return;
            }

            // Both conditions met - update is available
            m_updateVersion = remoteVersion;

            // Get file size
            qint64 bytes = sfReply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            m_updateSize = QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";

            // Fetch release notes
            QNetworkAccessManager* notesManager = new QNetworkAccessManager(this);
            QUrl notesUrl(QString("https://raw.githubusercontent.com/Nitrux/storage/master/Updates/nuts-summary-%1.md").arg(remoteVersion));

            QNetworkReply* notesReply = notesManager->get(QNetworkRequest(notesUrl));
            connect(notesReply, &QNetworkReply::finished, this, [this, notesReply, notesManager]() {
                notesReply->deleteLater();
                notesManager->deleteLater();

                if (notesReply->error() == QNetworkReply::NoError) {
                    m_updateNotes = notesReply->readAll();
                } else {
                    m_updateNotes = "Release notes not available.";
                }

                m_updateAvailable = true;
                Q_EMIT updateAvailableChanged();
                Q_EMIT updateInfoChanged();
                m_statusMessage = QString("Update available: %1").arg(m_updateVersion);
                Q_EMIT statusMessageChanged();
            });
        });
    });
}

void NutsClient::refreshSystemInfo() {
    if (!m_connected) {
        return;
    }

    QDBusReply<QVariantMap> reply = m_helperInterface->call("GetSystemInfo");
    if (reply.isValid()) {
        QVariantMap info = reply.value();
        m_distributionInfo = QString("%1 %2")
                                .arg(info["distribution"].toString())
                                .arg(info["version"].toString());
        Q_EMIT systemInfoChanged();
    }
}

void NutsClient::onProgressChanged(int status, int percentage, const QString& message, const QString& details) {
    m_progressPercentage = percentage;
    m_progressMessage = statusToString(status);

    if (!message.isEmpty()) {
        m_statusMessage = message;
    }

    if (!details.isEmpty()) {
        m_statusMessage += "\n" + details;
    }

    Q_EMIT progressChanged();
    Q_EMIT statusMessageChanged();
}

void NutsClient::onOperationCompleted(bool success, const QString& message) {
    m_busy = false;
    Q_EMIT busyChanged();

    m_statusMessage = message;
    Q_EMIT statusMessageChanged();

    showNotification("NUTS", message, KNotification::CloseOnTimeout);

    Q_EMIT operationCompleted(success, message);
}

void NutsClient::onOperationFailed(const QString& error) {
    m_busy = false;
    Q_EMIT busyChanged();

    m_statusMessage = "Error: " + error;
    Q_EMIT statusMessageChanged();

    showNotification("NUTS Error", error, KNotification::CloseOnTimeout | KNotification::Persistent);

    Q_EMIT operationFailed(error);
}

void NutsClient::onLogMessage(int level, const QString& message) {
    qDebug() << "NUTS Log [" << level << "]:" << message;
}

void NutsClient::showNotification(const QString& title, const QString& message,
                                  KNotification::NotificationFlags flags) {
    KNotification* notification = new KNotification("nutsEvent", flags);
    notification->setTitle(title);
    notification->setText(message);
    notification->setIconName("system-software-update");
    notification->sendEvent();
}

QString NutsClient::statusToString(int status) const {
    switch (status) {
        case 0: return "Idle";
        case 1: return "Checking connectivity";
        case 2: return "Creating backup";
        case 3: return "Compressing backup";
        case 4: return "Downloading update";
        case 5: return "Verifying update";
        case 6: return "Applying update";
        case 7: return "Restoring backup";
        case 8: return "Decompressing backup";
        case 9: return "Completed";
        case 10: return "Failed";
        default: return "Unknown";
    }
}

} // namespace Nuts
