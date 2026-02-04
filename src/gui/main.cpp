// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#include "NutsClient.h"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <KLocalizedContext>
#include <KLocalizedString>
#include <MauiKit4/Core/mauiapp.h>
#include <KAboutData>
#include <QDate>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 1. Setup Organization
    app.setOrganizationName(QStringLiteral("Nitrux"));
    app.setOrganizationDomain(QStringLiteral("nxos.org"));
    app.setApplicationName(QStringLiteral("NUTS"));
    app.setApplicationDisplayName(QStringLiteral("Nitrux Update Tool System"));
    app.setApplicationVersion(QStringLiteral("3.0.0"));

    // 2. Setup Window Icon (Required for the About Dialog logo)
    // Note: Ensure you have moved the assets folder to src/gui/assets/ so this path works
    QIcon appIcon(QStringLiteral(":/assets/nuts-gui.svg")); 
    app.setWindowIcon(appIcon);

    // 3. Configure MauiKit
    MauiApp::instance()->setIconName("qrc:/assets/nuts-gui.svg");

    KLocalizedString::setApplicationDomain("nuts");

    // 4. Setup About Data
    KAboutData about(QStringLiteral("nuts"),
                     i18n("Nitrux Update Tool System"),
                     QStringLiteral("3.0.0"),
                     i18n("System update and backup utility"),
                     KAboutLicense::BSD_3_Clause,
                     i18n("© %1 Made by Nitrux | Built with MauiKit", QString::number(QDate::currentDate().year())));

    about.addAuthor(QStringLiteral("Uri Herrera"), i18n("Developer"), QStringLiteral("uri_herrera@nxos.org"));
    about.addAuthor(QStringLiteral("Luis Lavaire"), i18n("Developer"), QStringLiteral("luis_lavaire@nxos.org"));

    about.setHomepage("https://nxos.org");
    about.setOrganizationDomain("nxos.org");
    about.setDesktopFileName("org.nxos.nuts");
    
    // Set the logo using the icon we just loaded
    about.setProgramLogo(app.windowIcon());

    // CRITICAL: You must register the data!
    KAboutData::setApplicationData(about);

    // 5. Setup Engine
    QQmlApplicationEngine engine;
    Nuts::NutsClient nutsClient;

    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("nutsClient"), &nutsClient);

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
