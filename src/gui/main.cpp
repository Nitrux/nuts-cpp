// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "NutsClient.h"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <KLocalizedContext>
#include <KLocalizedString>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    KLocalizedString::setApplicationDomain("nuts");

    app.setOrganizationName(QStringLiteral("Nitrux"));
    app.setOrganizationDomain(QStringLiteral("nxos.org"));
    app.setApplicationName(QStringLiteral("NUTS"));
    app.setApplicationDisplayName(QStringLiteral("Nitrux Update Tool System"));
    app.setApplicationVersion(QStringLiteral("3.0.0"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("system-software-update")));

    // Create NUTS client
    Nuts::NutsClient nutsClient;

    // Create QML engine
    QQmlApplicationEngine engine;

    // Add KDE i18n support
    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));

    // Expose NutsClient to QML
    engine.rootContext()->setContextProperty(QStringLiteral("nutsClient"), &nutsClient);

    // Load QML
    const QUrl url(QStringLiteral("qrc:/main.qml"));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject* obj, const QUrl& objUrl) {
        if (!obj && url == objUrl) {
            QCoreApplication::exit(-1);
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
