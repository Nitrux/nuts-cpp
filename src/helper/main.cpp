#include "NutsHelper.h"
#include "nuts/Logger.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <iostream>
#include <unistd.h>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Nitrux"));
    app.setOrganizationDomain(QStringLiteral("nxos.org"));
    app.setApplicationName(QStringLiteral("nuts-helper"));
    app.setApplicationVersion(QStringLiteral("3.0.0"));

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

    if (!bus.registerService(QStringLiteral("org.nxos.nuts"))) {
        Nuts::Logger::instance().error(QStringLiteral("Failed to register D-Bus service: ") +
                                       bus.lastError().message());
        return 1;
    }

    if (!bus.registerObject(QStringLiteral("/org/nxos/nuts"), &helper,
                           QDBusConnection::ExportAllSlots |
                           QDBusConnection::ExportAllSignals)) {
        Nuts::Logger::instance().error(QStringLiteral("Failed to register D-Bus object: ") +
                                       bus.lastError().message());
        return 1;
    }

    Nuts::Logger::instance().success("NUTS Helper registered on D-Bus");

    return app.exec();
}
