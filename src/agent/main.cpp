#include "NutsAgent.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <iostream>
#include <unistd.h>  // ::getuid()

int main(int argc, char* argv[])
{
    // QCoreApplication is required by QProcess / QFile but we never call
    // app.exec() — all operations are synchronous.
    QCoreApplication app(argc, argv);
    app.setApplicationName("nuts-agent");
    app.setApplicationVersion("3.0.0");

    if (::getuid() != 0) {
        std::cerr << "[NUTS-AGENT] ERROR: Must run as root\n";
        return 127;
    }

    // Param file path: first CLI arg, or the default written by UpdateManager.
    const QString paramFile = (argc > 1)
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("/var/cache/nuts-cpp/agent-params.ini");

    if (!QFile::exists(paramFile)) {
        std::cerr << "[NUTS-AGENT] ERROR: Parameter file not found: "
                  << paramFile.toStdString() << "\n";
        return 126;
    }

    QSettings s(paramFile, QSettings::IniFormat);

    NutsAgent::AgentParams params;
    params.minTarget        = s.value("Agent/MINTARGET").toString();
    params.otaChecksum      = s.value("Agent/OTA_CHECKSUM").toString();
    params.dpkgAppImageUrl  = s.value("Agent/DPKG_AI_URL").toString();
    params.dpkgAppImageSum  = s.value("Agent/DPKG_AI_SUM").toString();
    params.varLibUrl        = s.value("Agent/VAR_LIB_URL").toString();
    params.varLibSum        = s.value("Agent/VAR_LIB_SUM").toString();
    params.ccuUrl           = s.value("Agent/NUTS_CCU_URL").toString();
    params.ccuChecksum      = s.value("Agent/NUTS_CCU_CHECKSUM").toString();
    params.downloadDir      = s.value("Agent/DOWNLOAD_DIR").toString();
    params.squashfsDir      = s.value("Agent/SQUASHFS_DIR").toString();
    params.hasNvidia        = s.value("Agent/HAS_NVIDIA", false).toBool();

    // Mirrors are stored pipe-delimited in a single INI value.
    const QString mirrorsStr = s.value("Agent/MIRRORS").toString();
    if (!mirrorsStr.isEmpty())
        params.mirrors = mirrorsStr.split('|', Qt::SkipEmptyParts);

    // Validate the fields that are strictly required to proceed.
    if (params.minTarget.isEmpty()
        || params.otaChecksum.isEmpty()
        || params.mirrors.isEmpty()
        || params.downloadDir.isEmpty()
        || params.squashfsDir.isEmpty())
    {
        std::cerr << "[NUTS-AGENT] ERROR: Required parameters missing in "
                  << paramFile.toStdString()
                  << " (need MINTARGET, OTA_CHECKSUM, MIRRORS, DOWNLOAD_DIR, SQUASHFS_DIR)\n";
        return 125;
    }

    // Install SIGINT / SIGTERM handlers so that a kill or Ctrl+C triggers
    // a clean cleanup() call rather than an abrupt exit that leaves /home and
    // /var/lib mounted inside the overlay.
    NutsAgent::installSignalHandlers();

    NutsAgent::Agent agent(params);
    return agent.run();
}
