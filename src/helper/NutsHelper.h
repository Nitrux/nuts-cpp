// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include "nuts/Types.h"
#include "nuts/SystemInterface.h"
#include "nuts/BackupManager.h"
#include "nuts/UpdateManager.h"
#include <QObject>
#include <QDBusContext>
#include <QDBusVariant>

namespace Nuts {

class NutsHelper : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.nxos.nuts")

public:
    explicit NutsHelper(QObject* parent = nullptr);
    ~NutsHelper();

public Q_SLOTS:
    // D-Bus methods
    bool PerformUpdate();
    bool PerformRescue();
    QVariantMap GetSystemInfo();
    bool CheckConnectivity();
    void Cancel();

Q_SIGNALS:
    // D-Bus signals
    void ProgressChanged(int status, int percentage, const QString& message, const QString& details);
    void OperationCompleted(bool success, const QString& message);
    void OperationFailed(const QString& error);
    void LogMessage(int level, const QString& message);

private:
    void initialize();
    void cleanup();

    void handleUpdateOperation();
    void handleRescueOperation();

    void emitProgress(OperationStatus status, int percentage,
                     const QString& message, const QString& details = QString());

    void connectSignals();

    SystemInterface* m_sysInterface{nullptr};
    BackupManager* m_backupManager{nullptr};
    UpdateManager* m_updateManager{nullptr};

    bool m_cancelled{false};
    OperationType m_currentOperation;
};

} // namespace Nuts
