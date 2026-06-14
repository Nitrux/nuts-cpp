#include "NutsClient.h"
#include <QGuiApplication>
#include <QSurfaceFormat>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <KLocalizedContext>
#include <KLocalizedString>
#include <MauiKit4/Core/mauiapp.h>
#include <KAboutData>
#include <QDate>

int main(int argc, char* argv[]) {

    // 1. ENABLE WINDOW TRANSPARENCY
    // This MUST be set before the QGuiApplication is created.
    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    // 2. INITIALIZE APPLICATION
    QGuiApplication app(argc, argv);

    // 3. SETUP ORGANIZATION
    app.setOrganizationName(QStringLiteral("Nitrux"));
    app.setApplicationName(QStringLiteral("Nitrux Update Tool System"));

    // 4. SETUP WINDOW ICON
    QIcon appIcon(QStringLiteral(":/assets/nuts-gui.svg")); 
    app.setWindowIcon(appIcon);

    KLocalizedString::setApplicationDomain("nuts");

    // 6. SETUP ABOUT DATA
    KAboutData about(QStringLiteral("nuts"),
                     i18n("Nitrux Update Tool System"),
                     QStringLiteral("3.0.0"),
                     i18n("A simple utility to update Nitrux."),
                     KAboutLicense::BSD_3_Clause,
                     i18n("© %1 Made by Nitrux | Built with MauiKit", QString::number(QDate::currentDate().year())));

    about.addAuthor(QStringLiteral("Uri Herrera"), i18n("Developer"), QStringLiteral("uri_herrera@nxos.org"));
    about.setHomepage(QStringLiteral("https://nxos.org"));
    about.setProductName(QByteArrayLiteral("nitrux/nuts"));
    about.setOrganizationDomain(QByteArrayLiteral("nxos.org"));
    about.setDesktopFileName(QStringLiteral("org.nxos.nuts"));
    
    about.setProgramLogo(app.windowIcon());
    KAboutData::setApplicationData(about);

    // Configure MauiKit after KAboutData is set, so MauiApp can append
    // framework build metadata to the existing About data.
    MauiApp::instance()->setIconName(QStringLiteral("qrc:/assets/nuts-gui.svg"));

    // 7. INITIALIZE LOGIC BEFORE ENGINE
    Nuts::NutsClient nutsClient;

    // 8. SETUP ENGINE
    QQmlApplicationEngine engine;

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
