// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include <QObject>
#include <QString>
#include <QDBusInterface>
#include <QDBusConnection>
#include <KNotification>

namespace Nuts {

class NutsClient : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(int progressPercentage READ progressPercentage NOTIFY progressChanged)
    Q_PROPERTY(QString progressMessage READ progressMessage NOTIFY progressChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString distributionInfo READ distributionInfo NOTIFY systemInfoChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateAvailableChanged)
    Q_PROPERTY(QString updateVersion READ updateVersion NOTIFY updateInfoChanged)
    Q_PROPERTY(QString updateSize READ updateSize NOTIFY updateInfoChanged)
    Q_PROPERTY(QString updateNotes READ updateNotes NOTIFY updateInfoChanged)
    Q_PROPERTY(bool isLiveSession READ isLiveSession NOTIFY isLiveSessionChanged)

public:
    explicit NutsClient(QObject* parent = nullptr);
    ~NutsClient();

    bool isConnected() const { return m_connected; }
    bool isBusy() const { return m_busy; }
    int progressPercentage() const { return m_progressPercentage; }
    QString progressMessage() const { return m_progressMessage; }
    QString statusMessage() const { return m_statusMessage; }
    QString distributionInfo() const { return m_distributionInfo; }
    bool updateAvailable() const { return m_updateAvailable; }
    QString updateVersion() const { return m_updateVersion; }
    QString updateSize() const { return m_updateSize; }
    QString updateNotes() const { return m_updateNotes; }
    bool isLiveSession() const { return m_isLiveSession; }

public Q_SLOTS:
    void performUpdate();
    void performRescue();
    void cancel();
    void refreshSystemInfo();
    void checkForUpdates();

Q_SIGNALS:
    void connectedChanged();
    void busyChanged();
    void progressChanged();
    void statusMessageChanged();
    void systemInfoChanged();
    void operationCompleted(bool success, const QString& message);
    void operationFailed(const QString& error);
    void updateAvailableChanged();
    void updateInfoChanged();
    void isLiveSessionChanged();

private Q_SLOTS:
    void onProgressChanged(int status, int percentage, const QString& message, const QString& details);
    void onOperationCompleted(bool success, const QString& message);
    void onOperationFailed(const QString& error);
    void onLogMessage(int level, const QString& message);

private:
    void connectToHelper();
    void showNotification(const QString& title, const QString& message,
                         KNotification::NotificationFlags flags = KNotification::DefaultEvent);
    QString statusToString(int status) const;

    QDBusInterface* m_helperInterface{nullptr};
    bool m_connected{false};
    bool m_busy{false};
    int m_progressPercentage{0};
    QString m_progressMessage;
    QString m_statusMessage;
    QString m_distributionInfo;
    bool m_updateAvailable{false};
    QString m_updateVersion;
    QString m_updateSize;
    QString m_updateNotes;
    bool m_isLiveSession{false};
};

} // namespace Nuts
