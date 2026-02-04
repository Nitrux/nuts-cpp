// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "NutsHelper.h"
#include "nuts/Logger.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("Nitrux");
    app.setOrganizationDomain("nxos.org");
    app.setApplicationName("nuts-helper");
    app.setApplicationVersion("3.0.0");

    // Check if running as root
    if (getuid() != 0) {
        std::cerr << "Error: NUTS helper must be run as root" << std::endl;
        return 1;
    }

    Nuts::Logger::instance().info("NUTS Helper starting...");

    // Create helper instance
    Nuts::NutsHelper helper;

    // Register on D-Bus
    QDBusConnection bus = QDBusConnection::systemBus();

    if (!bus.registerService("org.nxos.nuts")) {
        Nuts::Logger::instance().error("Failed to register D-Bus service: " +
                                       bus.lastError().message());
        return 1;
    }

    if (!bus.registerObject("/org/nxos/nuts", &helper,
                           QDBusConnection::ExportAllSlots |
                           QDBusConnection::ExportAllSignals)) {
        Nuts::Logger::instance().error("Failed to register D-Bus object: " +
                                       bus.lastError().message());
        return 1;
    }

    Nuts::Logger::instance().success("NUTS Helper registered on D-Bus");

    return app.exec();
}
