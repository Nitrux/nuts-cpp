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
    // Detect Live session (same check as in NutsHelper)
    m_isLiveSession = QFile::exists(QStringLiteral("/usr/bin/calamares"));
    connectToHelper();
}

NutsClient::~NutsClient() {
    delete m_helperInterface;
}

void NutsClient::connectToHelper() {
    // Check if helper is running
    QDBusConnectionInterface* interface = QDBusConnection::systemBus().interface();
    if (!interface->isServiceRegistered(QStringLiteral("org.nxos.nuts"))) {
        qDebug() << "Helper not running, will start on first operation";
    }

    // Create interface (will auto-start via D-Bus activation)
    m_helperInterface = new QDBusInterface(QStringLiteral("org.nxos.nuts"),
                                           QStringLiteral("/org/nxos/nuts"),
                                           QStringLiteral("org.nxos.nuts"),
                                           QDBusConnection::systemBus(),
                                           this);

    if (!m_helperInterface->isValid()) {
        qWarning() << "Failed to connect to NUTS helper:" << m_helperInterface->lastError().message();
        m_connected = false;
    } else {
        m_connected = true;

        // Connect signals
        QDBusConnection::systemBus().connect(QStringLiteral("org.nxos.nuts"), QStringLiteral("/org/nxos/nuts"), QStringLiteral("org.nxos.nuts"),
                                            QStringLiteral("ProgressChanged"), this,
                                            SLOT(onProgressChanged(int, int, QString, QString)));

        QDBusConnection::systemBus().connect(QStringLiteral("org.nxos.nuts"), QStringLiteral("/org/nxos/nuts"), QStringLiteral("org.nxos.nuts"),
                                            QStringLiteral("OperationCompleted"), this,
                                            SLOT(onOperationCompleted(bool, QString)));

        QDBusConnection::systemBus().connect(QStringLiteral("org.nxos.nuts"), QStringLiteral("/org/nxos/nuts"), QStringLiteral("org.nxos.nuts"),
                                            QStringLiteral("OperationFailed"), this,
                                            SLOT(onOperationFailed(QString)));

        QDBusConnection::systemBus().connect(QStringLiteral("org.nxos.nuts"), QStringLiteral("/org/nxos/nuts"), QStringLiteral("org.nxos.nuts"),
                                            QStringLiteral("LogMessage"), this,
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
    m_isRescueOperation = false;
    Q_EMIT busyChanged();
    Q_EMIT operationTypeChanged();

    m_statusMessage = QStringLiteral("Starting update...");
    Q_EMIT statusMessageChanged();

    showNotification(QStringLiteral("NUTS Update"), QStringLiteral("System update starting..."), KNotification::Persistent);

    // Call the D-Bus method directly - D-Bus will auto-start the helper as root
    // and PolKit will handle the authentication dialog automatically
    QDBusReply<bool> reply = m_helperInterface->call(QStringLiteral("PerformUpdate"));
    if (!reply.isValid()) {
        qWarning() << "Failed to call PerformUpdate:" << reply.error().message();
        m_busy = false;
        Q_EMIT busyChanged();

        // Check if it was an authentication failure
        if (reply.error().type() == QDBusError::AccessDenied) {
            onOperationFailed(QStringLiteral("Authentication cancelled or failed"));
        } else {
            onOperationFailed(QStringLiteral("Failed to start update: ") + reply.error().message());
        }
    } else {
        // D-Bus call succeeded
        bool started = reply.value();
        if (!started) {
            qWarning() << "PerformUpdate returned false";
            m_busy = false;
            Q_EMIT busyChanged();
            onOperationFailed(QStringLiteral("Failed to start update operation"));
        } else {
            qDebug() << "Update operation started successfully";
            // Operation is running asynchronously - progress updates will come via signals
        }
    }
}

void NutsClient::performRescue() {
    if (!m_connected || m_busy) {
        return;
    }

    m_busy = true;
    m_isRescueOperation = true;
    Q_EMIT busyChanged();
    Q_EMIT operationTypeChanged();

    m_statusMessage = QStringLiteral("Starting rescue...");
    Q_EMIT statusMessageChanged();

    showNotification(QStringLiteral("NUTS Rescue"), QStringLiteral("System rescue starting..."), KNotification::Persistent);

    // Call the D-Bus method directly - D-Bus will auto-start the helper as root
    // and PolKit will handle the authentication dialog automatically
    QDBusReply<bool> reply = m_helperInterface->call(QStringLiteral("PerformRescue"));
    if (!reply.isValid()) {
        qWarning() << "Failed to call PerformRescue:" << reply.error().message();
        m_busy = false;
        Q_EMIT busyChanged();

        // Check if it was an authentication failure
        if (reply.error().type() == QDBusError::AccessDenied) {
            onOperationFailed(QStringLiteral("Authentication cancelled or failed"));
        } else {
            onOperationFailed(QStringLiteral("Failed to start rescue: ") + reply.error().message());
        }
    } else {
        // D-Bus call succeeded
        bool started = reply.value();
        if (!started) {
            qWarning() << "PerformRescue returned false";
            m_busy = false;
            Q_EMIT busyChanged();
            onOperationFailed(QStringLiteral("Failed to start rescue operation"));
        } else {
            qDebug() << "Rescue operation started successfully";
            // Operation is running asynchronously - progress updates will come via signals
        }
    }
}


void NutsClient::checkForUpdates() {
    if (!m_connected || m_busy) {
        return;
    }

    m_statusMessage = QStringLiteral("Checking for updates...");
    Q_EMIT statusMessageChanged();

    // Call the helper's CheckForUpdates method (single source of truth)
    QDBusReply<QVariantMap> reply = m_helperInterface->call(QStringLiteral("CheckForUpdates"));

    if (!reply.isValid()) {
        qWarning() << "Failed to check for updates:" << reply.error().message();
        m_statusMessage = QStringLiteral("Failed to check for updates: ") + reply.error().message();
        Q_EMIT statusMessageChanged();
        return;
    }

    QVariantMap result = reply.value();

    bool available = result[QStringLiteral("available")].toBool();
    m_updateAvailable = available;

    if (!available) {
        QString error = result[QStringLiteral("error")].toString();
        if (!error.isEmpty()) {
            m_statusMessage = error;
            Q_EMIT operationFailed(error);
        } else {
            m_statusMessage = QStringLiteral("No updates available");
            Q_EMIT noUpdatesAvailable(QStringLiteral("Your system is up to date."));
        }
        Q_EMIT updateAvailableChanged();
        Q_EMIT statusMessageChanged();
        return;
    }

    // Update is available - Helper provides all metadata
    m_updateVersion = result[QStringLiteral("targetVersion")].toString();

    // Get file size from Helper (already cached)
    qint64 bytes = result[QStringLiteral("updateSize")].toLongLong();
    if (bytes <= 0) {
        bytes = result[QStringLiteral("otaSize")].toLongLong();
    }
    m_updateSize = QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QStringLiteral(" GB");

    // Get release notes URL from Helper
    QString releaseNotesUrl = result[QStringLiteral("releaseNotesUrl")].toString();

    // Fetch release notes
    QNetworkAccessManager* notesManager = new QNetworkAccessManager(this);
    QNetworkReply* notesReply = notesManager->get(QNetworkRequest(QUrl(releaseNotesUrl)));

    connect(notesReply, &QNetworkReply::finished, this, [this, notesReply, notesManager]() {
        notesReply->deleteLater();
        notesManager->deleteLater();

        if (notesReply->error() == QNetworkReply::NoError) {
            m_updateNotes = QString::fromUtf8(notesReply->readAll());
        } else {
            m_updateNotes = QStringLiteral("Release notes not available.");
        }

        Q_EMIT updateAvailableChanged();
        Q_EMIT updateInfoChanged();
        m_statusMessage = QStringLiteral("Update available: %1").arg(m_updateVersion);
        Q_EMIT statusMessageChanged();
    });
}

void NutsClient::refreshSystemInfo() {
    if (!m_connected) {
        return;
    }

    QDBusReply<QVariantMap> reply = m_helperInterface->call(QStringLiteral("GetSystemInfo"));
    if (reply.isValid()) {
        QVariantMap info = reply.value();
        m_distributionInfo = QStringLiteral("%1 %2")
                                .arg(info[QStringLiteral("distribution")].toString())
                                .arg(info[QStringLiteral("version")].toString());
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
        m_statusMessage += QStringLiteral("\n") + details;
    }

    Q_EMIT progressChanged();
    Q_EMIT statusMessageChanged();
}

void NutsClient::onOperationCompleted(bool success, const QString& message) {
    m_busy = false;
    m_isRescueOperation = false;
    Q_EMIT busyChanged();
    Q_EMIT operationTypeChanged();

    m_statusMessage = message;
    Q_EMIT statusMessageChanged();

    showNotification(QStringLiteral("NUTS"), message, KNotification::CloseOnTimeout);

    Q_EMIT operationCompleted(success, message);
}

void NutsClient::onOperationFailed(const QString& error) {
    m_busy = false;
    m_isRescueOperation = false;
    Q_EMIT busyChanged();
    Q_EMIT operationTypeChanged();

    m_statusMessage = QStringLiteral("Error: ") + error;
    Q_EMIT statusMessageChanged();

    showNotification(QStringLiteral("NUTS Error"), error, KNotification::CloseOnTimeout | KNotification::Persistent);

    Q_EMIT operationFailed(error);
}

void NutsClient::onOperationFailed(const char* error) {
    onOperationFailed(QString::fromUtf8(error));
}

void NutsClient::onLogMessage(int level, const QString& message) {
    qDebug() << "NUTS Log [" << level << "]:" << message;
}

void NutsClient::showNotification(const QString& title, const QString& message,
                                  KNotification::NotificationFlags flags) {
    KNotification* notification = new KNotification(QStringLiteral("nutsEvent"), flags);
    notification->setTitle(title);
    notification->setText(message);
    notification->setIconName(QStringLiteral("system-software-update"));
    notification->sendEvent();
}

void NutsClient::showNotification(const char* title, const QString& message,
                                  KNotification::NotificationFlags flags) {
    showNotification(QString::fromUtf8(title), message, flags);
}

void NutsClient::showNotification(const QString& title, const char* message,
                                  KNotification::NotificationFlags flags) {
    showNotification(title, QString::fromUtf8(message), flags);
}

void NutsClient::showNotification(const char* title, const char* message,
                                  KNotification::NotificationFlags flags) {
    showNotification(QString::fromUtf8(title), QString::fromUtf8(message), flags);
}

QString NutsClient::statusToString(int status) const {
    switch (status) {
        case 0: return QStringLiteral("Idle");
        case 1: return QStringLiteral("Checking connectivity");
        case 2: return QStringLiteral("Creating backup");
        case 3: return QStringLiteral("Compressing backup");
        case 4: return QStringLiteral("Downloading update");
        case 5: return QStringLiteral("Verifying update");
        case 6: return QStringLiteral("Applying update");
        case 7: return QStringLiteral("Restoring backup");
        case 8: return QStringLiteral("Decompressing backup");
        case 9: return QStringLiteral("Completed");
        case 10: return QStringLiteral("Failed");
        default: return QStringLiteral("Unknown");
    }
}

} // namespace Nuts
