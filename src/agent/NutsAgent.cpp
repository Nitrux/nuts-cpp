// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#include "NutsAgent.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <csignal>
#include <unistd.h>   // ::sleep()

namespace NutsAgent {

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

volatile sig_atomic_t Agent::s_interrupted = 0;

static void signalHandler(int /*sig*/) noexcept
{
    Agent::s_interrupted = 1;
}

void installSignalHandlers()
{
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Agent::Agent(const AgentParams& params)
    : m_params(params)
{
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

void Agent::log(const QString& level, const QString& msg)
{
    // Write structured log lines to stdout.  The parent (UpdateManager) captures
    // the entire agent stdout and forwards these lines to its Logger.
    QTextStream out(stdout);
    out << "[NUTS-AGENT] " << level << ": " << msg << "\n";
    out.flush();
}

void Agent::log(const char* level, const QString& msg)
{
    log(QString::fromUtf8(level), msg);
}

void Agent::log(const char* level, const char* msg)
{
    log(QString::fromUtf8(level), QString::fromUtf8(msg));
}

void Agent::progress(int pct, const QString& msg)
{
    // Parsed by UpdateManager::parseAndRelayAgentOutput() to emit updateProgress().
    QTextStream out(stdout);
    out << "[NUTS-AGENT] PROGRESS: " << pct << " " << msg << "\n";
    out.flush();
}

void Agent::progress(int pct, const char* msg)
{
    progress(pct, QString::fromUtf8(msg));
}

static QStringList toQStringList(std::initializer_list<const char*> args)
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(args.size()));
    for (const char* arg : args)
        list.append(QString::fromUtf8(arg));
    return list;
}

bool Agent::exec(const char* program, std::initializer_list<const char*> args, QString* outPtr)
{
    return exec(QString::fromUtf8(program), toQStringList(args), outPtr);
}

bool Agent::exec(const char* program, const QStringList& args, QString* outPtr)
{
    return exec(QString::fromUtf8(program), args, outPtr);
}

bool Agent::exec(const QString& program, const QStringList& args, QString* outPtr)
{
    // Abort immediately if a signal was received between commands.
    if (s_interrupted) {
        log("WARNING", QStringLiteral("Interrupted before: ") + program);
        return false;
    }

    // Log the full command line.
    QString cmdLine = program;
    if (!args.isEmpty())
        cmdLine += QStringLiteral(" ") + args.join(QStringLiteral(" "));
    log("DEBUG", QStringLiteral("EXEC: ") + cmdLine);

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(program, args);

    // 10-minute per-command timeout (matches the old executeInOverlay timeout).
    if (!proc.waitForFinished(600000)) {
        proc.kill();
        log("ERROR", QStringLiteral("Command timed out: ") + cmdLine);
        return false;
    }

    QString out  = QString::fromUtf8(proc.readAllStandardOutput());
    QString err  = QString::fromUtf8(proc.readAllStandardError());

    if (!out.trimmed().isEmpty()) {
        // Forward child stdout to our stdout so the parent captures it.
        QTextStream(stdout) << out;
        if (outPtr)
            *outPtr = out;
    }
    if (!err.trimmed().isEmpty()) {
        // Route child stderr through the structured log format on stdout so
        // UpdateManager::parseAndRelayAgentOutput() captures and forwards it to
        // the Logger.  This avoids dpkg/axel errors being silently discarded
        // when the parent reads only agentOutput (stdout).
        for (const QString& line : err.split(QStringLiteral("\n"), Qt::SkipEmptyParts))
            log("ERROR", line);
    }

    int code = proc.exitCode();
    log("DEBUG", QStringLiteral("EXIT(%1): %2").arg(code).arg(program));
    return code == 0;
}

// ---------------------------------------------------------------------------
// Top-level orchestration
// ---------------------------------------------------------------------------

// Convenience macro: check the interrupt flag between steps and bail out cleanly.
#define CHECK_INTERRUPTED() \
    do { \
        if (s_interrupted) { \
            log("WARNING", "Interrupted — cleaning up and exiting."); \
            cleanup(); \
            return 8; \
        } \
    } while (0)

int Agent::run()
{
    log("INFO", "=== nuts-agent starting ===");

    // /dev and /tmp may not be set up in the lower layer.
    exec("/usr/bin/mount", QStringList{QStringLiteral("-t"), QStringLiteral("devtmpfs"), QStringLiteral("dev"), QStringLiteral("/dev")});
    exec("/usr/bin/mkdir", QStringList{QStringLiteral("-p"), QStringLiteral("/tmp")});
    exec("/usr/bin/chmod", QStringList{QStringLiteral("1777"), QStringLiteral("/tmp")});

    CHECK_INTERRUPTED();
    progress(10, "Mounting system partitions");
    log("INFO", "--- Step: Mounting system partitions ---");
    if (!prepareSystemPartitions()) {
        log("ERROR", "prepareSystemPartitions() FAILED");
        cleanup();
        return 1;
    }
    log("SUCCESS", "System partitions ready.");

    CHECK_INTERRUPTED();
    progress(15, "Downloading OTA payload");
    log("INFO", "--- Step: Downloading OTA payload ---");
    if (!downloadOTAPayload()) {
        log("ERROR", "downloadOTAPayload() FAILED");
        cleanup();
        return 2;
    }
    log("SUCCESS", "OTA payload ready.");

    CHECK_INTERRUPTED();
    progress(25, "Mounting OTA squashfs");
    log("INFO", "--- Step: Mounting OTA squashfs ---");
    if (!mountOTAPayload()) {
        log("ERROR", "mountOTAPayload() FAILED");
        cleanup();
        return 3;
    }
    log("SUCCESS", "OTA squashfs mounted.");

    CHECK_INTERRUPTED();
    progress(30, "Preparing dpkg tooling");
    log("INFO", "--- Step: Preparing dpkg tooling ---");
    if (!prepareUpdateTools()) {
        log("ERROR", "prepareUpdateTools() FAILED");
        cleanup();
        return 4;
    }
    log("SUCCESS", "dpkg tooling ready.");

    CHECK_INTERRUPTED();
    progress(40, "Syncing package database");
    log("INFO", "--- Step: Syncing package database ---");
    if (!syncPackageData()) {
        log("ERROR", "syncPackageData() FAILED");
        cleanup();
        return 5;
    }
    log("SUCCESS", "Package database synced.");

    CHECK_INTERRUPTED();
    progress(50, "Applying packages (this may take time)");
    log("INFO", "--- Step: Applying package updates ---");
    if (!performPackageUpdates()) {
        log("ERROR", "performPackageUpdates() FAILED");
        cleanup();
        return 6;
    }
    log("SUCCESS", "Packages applied.");

    CHECK_INTERRUPTED();
    progress(90, "Running final cleanup");
    log("INFO", "--- Step: Running cleanup crew ---");
    if (!runCleanupCrew()) {
        log("ERROR", "runCleanupCrew() FAILED");
        cleanup();
        return 7;
    }
    log("SUCCESS", "Cleanup crew finished.");

    log("INFO", "--- Step: Installing policy symlinks ---");
    installPolicySymlinks();

    log("INFO", "Flushing writes to disk...");
    exec("/usr/bin/sync", {});

    cleanup();

    progress(100, "Update complete");
    log("SUCCESS", "=== nuts-agent finished successfully ===");
    return 0;
}

#undef CHECK_INTERRUPTED

// ---------------------------------------------------------------------------
// Step implementations
// ---------------------------------------------------------------------------

bool Agent::prepareSystemPartitions()
{
    QString output;

    // --- Mount NX_HOME → /home ---
    log("DEBUG", "Resolving LABEL=NX_HOME...");
    if (!exec("/usr/sbin/findfs", {"LABEL=NX_HOME"}, &output)) {
        log("ERROR", "Could not find partition labeled NX_HOME");
        return false;
    }
    QString homeDev = output.trimmed();
    log("INFO", QStringLiteral("NX_HOME device: ") + homeDev);

    bool mountOk = exec(QStringLiteral("/usr/bin/mount"), QStringList{QStringLiteral("-t"), QStringLiteral("auto"), homeDev, QStringLiteral("/home")});
    log("DEBUG", QStringLiteral("mount %1 /home: %2")
        .arg(homeDev, mountOk ? QStringLiteral("ok") : QStringLiteral("failed (may already be mounted)")));

    // --- Mount NX_VAR_LIB → /var/lib ---
    log("DEBUG", "Resolving LABEL=NX_VAR_LIB...");
    if (!exec("/usr/sbin/findfs", {"LABEL=NX_VAR_LIB"}, &output)) {
        log("ERROR", "Could not find partition labeled NX_VAR_LIB");
        return false;
    }
    QString varLibDev = output.trimmed();
    log("INFO", QStringLiteral("NX_VAR_LIB device: ") + varLibDev);

    mountOk = exec(QStringLiteral("/usr/bin/mount"), QStringList{QStringLiteral("-t"), QStringLiteral("auto"), varLibDev, QStringLiteral("/var/lib")});
    log("DEBUG", QStringLiteral("mount %1 /var/lib: %2")
        .arg(varLibDev, mountOk ? QStringLiteral("ok") : QStringLiteral("failed (may already be mounted)")));

    // --- Create working directories ---
    exec("/usr/bin/mkdir", QStringList{QStringLiteral("-p"), m_params.downloadDir});
    exec("/usr/bin/mkdir", QStringList{QStringLiteral("-p"), m_params.squashfsDir});
    log("DEBUG", QStringLiteral("Working dirs: ") + m_params.downloadDir + QStringLiteral(", ") + m_params.squashfsDir);

    return true;
}

bool Agent::downloadOTAPayload()
{
    QString otaPath = m_params.downloadDir  + QStringLiteral("/nuts-ota.squashfs");

    log("INFO", QStringLiteral("OTA target path: ") + otaPath);
    log("INFO", QStringLiteral("OTA expected checksum: ") + m_params.otaChecksum);
    log("INFO", QStringLiteral("Mirrors available: ") + QString::number(m_params.mirrors.size()));

    // Check if a valid file already exists.
    QString output;
    if (exec(QStringLiteral("/usr/bin/test"), QStringList{QStringLiteral("-f"), otaPath})) {
        log("INFO", "Existing OTA file found, verifying checksum...");
        QString checksumExpr = QStringLiteral(
            "[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]")
            .arg(otaPath, m_params.otaChecksum);
        if (exec(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), checksumExpr})) {
            log("SUCCESS", "Existing OTA payload verified, skipping download.");
            return true;
        }
        log("WARNING", "Existing OTA payload checksum mismatch. Re-downloading.");
        exec(QStringLiteral("/usr/bin/rm"), QStringList{QStringLiteral("-f"), otaPath});
    } else {
        log("INFO", QStringLiteral("No existing OTA file found at ") + otaPath);
    }

    if (m_params.mirrors.isEmpty()) {
        log("ERROR", "No mirrors available.");
        return false;
    }

    for (const QString& mirror : m_params.mirrors) {
        QString url = mirror.trimmed();
        if (url.isEmpty())
            continue;

        // Always start clean — never resume across mirrors.
        exec(QStringLiteral("/usr/bin/rm"), QStringList{QStringLiteral("-f"), otaPath});

        log("INFO", QStringLiteral("Trying mirror: ") + url);
        bool ok = exec("/usr/bin/axel", QStringList{QStringLiteral("-n"), QStringLiteral("10"), QStringLiteral("-o"), otaPath, url});

        if (!ok) {
            log("WARNING", QStringLiteral("Download from ") + url  + QStringLiteral(" failed."));
            continue;
        }
        log("DEBUG", "axel download complete, verifying checksum...");

        QString checksumExpr = QStringLiteral(
            "[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]")
            .arg(otaPath, m_params.otaChecksum);
        if (exec(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), checksumExpr})) {
            log("SUCCESS", QStringLiteral("OTA payload downloaded and verified from ") + url);
            return true;
        }

        // Log actual checksum for diagnosis.
        QString actualSum;
        exec(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), QStringLiteral("sha256sum '%1' | awk '{print $1}'").arg(otaPath)}, &actualSum);
        log("WARNING", QStringLiteral("Checksum mismatch from ") + url);
        log("WARNING", QStringLiteral("  Expected: ") + m_params.otaChecksum);
        log("WARNING", QStringLiteral("  Got:      ") + actualSum.trimmed());
        exec(QStringLiteral("/usr/bin/rm"), QStringList{QStringLiteral("-f"), otaPath});
    }

    log("ERROR", "Failed to download OTA payload from all mirrors.");
    return false;
}

bool Agent::mountOTAPayload()
{
    QString otaPath    = m_params.downloadDir  + QStringLiteral("/nuts-ota.squashfs");
    QString mountPoint = m_params.squashfsDir;

    log("INFO", QStringLiteral("Mounting: ") + otaPath + QStringLiteral(" -> ") + mountPoint);
    if (!exec("/usr/bin/mount", QStringList{otaPath, mountPoint})) {
        log("ERROR", "Failed to mount OTA squashfs");
        return false;
    }
    QString listing;
    exec("/usr/bin/ls", QStringList{mountPoint}, &listing);
    log("DEBUG", QStringLiteral("OTA squashfs root contents: ") + listing.trimmed());
    return true;
}

bool Agent::prepareUpdateTools()
{
    const QString appImagePath = QStringLiteral("/tmp/dpkg-1.22.21-x86_64.AppImage");
    const QString extractDir   = QStringLiteral("/tmp/pkgman-extracted");
    const QString appRunPath   = extractDir  + QStringLiteral("/squashfs-root/AppRun");

    log("INFO", QStringLiteral("dpkg AppImage URL:  ") + m_params.dpkgAppImageUrl);
    log("INFO", QStringLiteral("dpkg AppImage path: ") + appImagePath);
    log("INFO", QStringLiteral("dpkg AppRun path:   ") + appRunPath);
    log("INFO", QStringLiteral("Expected DPKG_AI_SUM: ") + m_params.dpkgAppImageSum);

    // If already extracted from a previous (interrupted) run, skip download.
    if (exec(QStringLiteral("/usr/bin/test"), QStringList{QStringLiteral("-f"), appRunPath})) {
        log("INFO", "Extracted OTA tooling already present, skipping download.");
    } else {
        log("INFO", "OTA tooling not found, downloading...");

        if (!exec(QStringLiteral("/usr/bin/axel"), QStringList{QStringLiteral("-n"), QStringLiteral("10"), QStringLiteral("-o"), appImagePath, m_params.dpkgAppImageUrl})) {
            log("ERROR", "Failed to download OTA tooling");
            return false;
        }
        log("SUCCESS", "dpkg AppImage downloaded.");

        // Verify checksum.
        log("INFO", "Verifying dpkg AppImage checksum...");
        QString checksumCmd = QStringLiteral("echo '%1  %2' | /usr/bin/sha256sum -c -")
                                  .arg(m_params.dpkgAppImageSum, appImagePath);
        if (!exec(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), checksumCmd})) {
            log("ERROR", "CRITICAL: dpkg AppImage checksum mismatch!");
            QString actualSum;
            exec("/bin/sh", QStringList{QStringLiteral("-c"), QStringLiteral("sha256sum '%1' | awk '{print $1}'").arg(appImagePath)}, &actualSum);
            log("ERROR", QStringLiteral("  Expected: ") + m_params.dpkgAppImageSum);
            log("ERROR", QStringLiteral("  Got:      ") + actualSum.trimmed());
            exec("/usr/bin/rm", QStringList{QStringLiteral("-f"), appImagePath});
            return false;
        }
        log("SUCCESS", "dpkg AppImage checksum OK.");

        exec(QStringLiteral("/usr/bin/chmod"), QStringList{QStringLiteral("+x"), appImagePath});

        log("INFO", "Extracting dpkg AppImage...");
        exec(QStringLiteral("/usr/bin/rm"), QStringList{QStringLiteral("-rf"), extractDir});
        exec(QStringLiteral("/usr/bin/mkdir"), QStringList{QStringLiteral("-p"), extractDir});
        if (!exec("/bin/sh", QStringList{QStringLiteral("-c"), QStringLiteral("cd ") + extractDir + QStringLiteral(" && ") + appImagePath + QStringLiteral(" --appimage-extract")})) {
            log("ERROR", "Failed to extract dpkg AppImage");
            return false;
        }

        if (!exec(QStringLiteral("/usr/bin/test"), QStringList{QStringLiteral("-f"), appRunPath})) {
            log("ERROR", QStringLiteral("Extraction completed but AppRun not found at: ") + appRunPath);
            QString listing;
            exec("/usr/bin/ls", QStringList{QStringLiteral("-la"), extractDir + QStringLiteral("/squashfs-root/")}, &listing);
            log("DEBUG", QStringLiteral("squashfs-root contents: ") + listing.trimmed());
            return false;
        }
        log("SUCCESS", "dpkg AppImage extracted. AppRun found.");
    }

    m_pkgManagerPath = appRunPath;
    log("INFO", QStringLiteral("Using dpkg tooling at: ") + m_pkgManagerPath);

    // Symlink dpkg → AppRun (mirrors: ln -svf "$AIPKG_MANAGER" /usr/bin/dpkg).
    exec(QStringLiteral("/usr/bin/ln"), QStringList{QStringLiteral("-svf"), appRunPath, QStringLiteral("/usr/bin/dpkg")});

    const QString binDir = extractDir  + QStringLiteral("/squashfs-root/usr/bin");
    const QStringList tools = {
        QStringLiteral("dpkg-deb"), QStringLiteral("dpkg-divert"), QStringLiteral("dpkg-query"), QStringLiteral("dpkg-realpath"),
        QStringLiteral("dpkg-split"), QStringLiteral("dpkg-statoverride"), QStringLiteral("dpkg-trigger"),
        QStringLiteral("dpkg-maintscript-helper"), QStringLiteral("update-alternatives")
    };
    for (const QString& tool : tools) {
        QString target = binDir + QStringLiteral("/") + tool;
        if (exec("/usr/bin/test", QStringList{QStringLiteral("-f"), target}))
            exec(QStringLiteral("/usr/bin/ln"), QStringList{QStringLiteral("-svf"), target, QStringLiteral("/usr/bin/") + tool});
        else
            log("WARNING", QStringLiteral("Tool binary not found in AppImage, skipping: ") + target);
    }

    exec(QStringLiteral("/usr/bin/mkdir"), QStringList{QStringLiteral("-p"), QStringLiteral("/usr/share")});
    exec(QStringLiteral("/usr/bin/ln"), QStringList{QStringLiteral("-svf"), extractDir + QStringLiteral("/squashfs-root/usr/share/dpkg"), QStringLiteral("/usr/share/dpkg")});

    return true;
}

bool Agent::syncPackageData()
{
    log("INFO", QStringLiteral("Package DB archive URL:  ") + m_params.varLibUrl);
    log("INFO", QStringLiteral("Package DB archive path: ") + m_params.varLibUrl);
    log("INFO", QStringLiteral("Expected VAR_LIB_SUM: ") + m_params.varLibSum);

    QString tarPath = QStringLiteral("/tmp/var-lib-dpkg-%1.tar.xz").arg(m_params.minTarget);

    // Remove any stale file first.
    exec("/usr/bin/rm", QStringList{QStringLiteral("-f"), tarPath});

    log("INFO", "Downloading package database archive...");
    if (!exec(QStringLiteral("/usr/bin/axel"), QStringList{QStringLiteral("-n"), QStringLiteral("10"), QStringLiteral("-o"), tarPath, m_params.varLibUrl})) {
        log("ERROR", "Failed to download package database archive");
        return false;
    }
    log("SUCCESS", "Package database archive downloaded.");

    // Verify checksum before extraction — unverified tar extraction to / is
    // extremely dangerous (zip-slip / overwrite attacks).
    log("INFO", "Verifying package database archive checksum...");
    QString checksumCmd = QStringLiteral("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(m_params.varLibSum, tarPath);
    if (!exec(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), checksumCmd})) {
        log("ERROR", "CRITICAL: Package database checksum mismatch!");
        QString actualSum;
        exec("/bin/sh", QStringList{QStringLiteral("-c"), QStringLiteral("sha256sum '%1' | awk '{print $1}'").arg(tarPath)}, &actualSum);
        log("ERROR", QStringLiteral("  Expected: ") + m_params.varLibSum);
        log("ERROR", QStringLiteral("  Got:      ") + actualSum.trimmed());
        exec("/usr/bin/rm", QStringList{QStringLiteral("-f"), tarPath});
        return false;
    }
    log("SUCCESS", "Package database archive checksum OK.");

    // Extract into the lower layer.
    log("INFO", "Extracting package database archive to /...");
    if (!exec("/bin/sh", QStringList{QStringLiteral("-c"), QStringLiteral("mkdir -p /var/lib/dpkg && cd / && /usr/bin/tar -xf ") + tarPath})) {
        log("ERROR", "Failed to extract package database archive");
        return false;
    }

    if (!exec("/usr/bin/test", QStringList{QStringLiteral("-f"), QStringLiteral("/var/lib/dpkg/status")})) {
        log("ERROR", "Package database extraction failed: /var/lib/dpkg/status not found");
        QString listing;
        exec("/usr/bin/ls", QStringList{QStringLiteral("-la"), QStringLiteral("/var/lib/dpkg/")}, &listing);
        log("DEBUG", QStringLiteral("/var/lib/dpkg/ contents: ") + listing.trimmed());
        return false;
    }
    log("SUCCESS", "Package database extracted. /var/lib/dpkg/status confirmed.");
    return true;
}

bool Agent::performPackageUpdates()
{
    QString otaDir     = m_params.squashfsDir  + QStringLiteral("/ota");
    QString updatesDir = otaDir  + QStringLiteral("/updates");
    QString nvidiaDir  = otaDir  + QStringLiteral("/nvidia");

    log("INFO", QStringLiteral("OTA updates dir: ") + updatesDir);
    log("INFO", QStringLiteral("dpkg tooling path: ") + m_pkgManagerPath);
    log("INFO", QStringLiteral("NVIDIA hardware: %1").arg(m_params.hasNvidia ? QStringLiteral("yes") : QStringLiteral("no")));

    // Verify the dpkg tooling is present.
    if (!exec(QStringLiteral("/usr/bin/test"), QStringList{QStringLiteral("-f"), m_pkgManagerPath})) {
        log("ERROR", QStringLiteral("dpkg tooling not found: ") + m_pkgManagerPath);
        return false;
    }

    // --- Phase 1: Collect .deb paths ---
    log("INFO", "--- Phase 1: Collecting .deb packages ---");

    // Build the argument list for find: [path...] -name *.deb -print0
    QStringList findArgs = {updatesDir};
    if (m_params.hasNvidia)
        findArgs << nvidiaDir;
    findArgs << QStringLiteral("-name") << QStringLiteral("*.deb") << QStringLiteral("-print0");

    QString findOutput;
    bool findOk = exec("/usr/bin/find", findArgs, &findOutput);

    if (!findOk && findOutput.trimmed().isEmpty()) {
        log("ERROR", "find failed to enumerate .deb packages");
        return false;
    }

    QStringList debs = findOutput.split(QChar(u'\0'), Qt::SkipEmptyParts);
    log("INFO", QStringLiteral("Found %1 .deb package(s).").arg(debs.size()));
    for (const QString& deb : debs)
        log("DEBUG", QStringLiteral("  deb: ") + deb);

    if (debs.isEmpty()) {
        log("WARNING", "No .deb packages found. Nothing to install.");
        return true;
    }

    // --- Phase 2: Unpack in batches of 120 ---
    log("INFO", "--- Phase 2: Unpacking packages ---");

    const int batchSize   = 120;
    int       totalBatches = (debs.size() + batchSize - 1) / batchSize;

    for (int i = 0; i < debs.size(); i += batchSize) {
        QStringList batch    = debs.mid(i, batchSize);
        int         batchNum = (i / batchSize) + 1;
        log("INFO", QStringLiteral("Unpacking batch %1/%2 (%3 package(s))...")
            .arg(batchNum).arg(totalBatches).arg(batch.size()));

        if (!exec(m_pkgManagerPath,
                  QStringList{QStringLiteral("--force-all"), QStringLiteral("--unpack")} + batch)) {
            log("ERROR", QStringLiteral("Unpack failed on batch %1/%2").arg(batchNum).arg(totalBatches));
            return false;
        }
        log("SUCCESS", QStringLiteral("Batch %1/%2 unpacked.").arg(batchNum).arg(totalBatches));
    }
    log("SUCCESS", "All packages unpacked.");

    // --- Phase 3: Configure + audit loop ---
    log("INFO", "--- Phase 3: Configuring packages ---");

    const int maxPasses = 15;
    QString   lastAudit;

    for (int pass = 1; pass <= maxPasses; ++pass) {
        log("INFO", QStringLiteral("Configuration pass %1/%2...").arg(pass).arg(maxPasses));

        // --configure -a: ignore non-zero (may be partial).
        exec(m_pkgManagerPath, QStringList{QStringLiteral("--force-all"), QStringLiteral("--configure"), QStringLiteral("-a")});

        // --audit: what's still broken?
        QString auditOutput;
        exec(m_pkgManagerPath, QStringList{QStringLiteral("--audit")}, &auditOutput);
        QString currentAudit = auditOutput.trimmed();

        if (currentAudit.isEmpty()) {
            log("SUCCESS", QStringLiteral("Package configuration converged after %1 pass(es).").arg(pass));
            break;
        }

        log("INFO", QStringLiteral("dpkg --audit output:\n") + currentAudit);

        if (pass > 1 && currentAudit == lastAudit) {
            log("ERROR", "Package configuration stuck — no progress between passes.");
            return false;
        }

        if (pass == maxPasses) {
            log("ERROR", QStringLiteral("Package configuration failed to converge after %1 passes.").arg(maxPasses));
            return false;
        }

        lastAudit = currentAudit;
        ::sleep(1);
    }

    exec(QStringLiteral("/usr/bin/rm"), QStringList{QStringLiteral("-rf"), QStringLiteral("/tmp/pkgman-extracted")});
    log("SUCCESS", "Package updates applied successfully.");
    return true;
}

bool Agent::runCleanupCrew()
{
    const QString ccuPath = QStringLiteral("/tmp/nuts-cpp-ccu");

    log("INFO", QStringLiteral("CCU URL:  ") + m_params.ccuUrl);
    log("INFO", QStringLiteral("CCU path: ") + ccuPath);
    log("INFO", QStringLiteral("Expected NUTS_CCU_CHECKSUM: ") + m_params.ccuChecksum);

    log("INFO", "Downloading cleanup crew...");
    if (!exec(QStringLiteral("/usr/bin/axel"), QStringList{QStringLiteral("-n"), QStringLiteral("10"), QStringLiteral("-o"), ccuPath, m_params.ccuUrl})) {
        log("ERROR", "Failed to download cleanup crew");
        return false;
    }
    log("SUCCESS", "Cleanup crew downloaded.");

    log("INFO", "Verifying cleanup crew checksum...");
    QString checksumCmd = QStringLiteral("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(m_params.ccuChecksum, ccuPath);
    if (!exec(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), checksumCmd})) {
        log("ERROR", "CRITICAL: Cleanup crew checksum mismatch!");
        QString actualSum;
        exec("/bin/sh", QStringList{QStringLiteral("-c"), QStringLiteral("sha256sum '%1' | awk '{print $1}'").arg(ccuPath)}, &actualSum);
        log("ERROR", QStringLiteral("  Expected: ") + m_params.ccuChecksum);
        log("ERROR", QStringLiteral("  Got:      ") + actualSum.trimmed());
        exec(QStringLiteral("/usr/bin/rm"), QStringList{QStringLiteral("-f"), ccuPath});
        return false;
    }
    log("SUCCESS", "Cleanup crew checksum OK.");

    exec(QStringLiteral("/usr/bin/chmod"), QStringList{QStringLiteral("+x"), ccuPath});

    log("INFO", "Running cleanup crew...");
    if (!exec(ccuPath, QStringList{})) {
        log("ERROR", "Cleanup crew exited with non-zero status");
        return false;
    }
    log("SUCCESS", "Cleanup crew completed successfully.");
    return true;
}

void Agent::installPolicySymlinks()
{
    const QStringList aptTools = {
        QStringLiteral("apt"), QStringLiteral("apt-cache"), QStringLiteral("apt-cdrom"), QStringLiteral("apt-config"), QStringLiteral("apt-get"), QStringLiteral("apt-mark")
    };
    for (const QString& tool : aptTools)
        exec("/usr/bin/ln", QStringList{QStringLiteral("-sf"), QStringLiteral("/usr/bin/nx-pkgmgr-policy"), QStringLiteral("/usr/bin/") + tool});

    const QStringList dpkgTools = {
        QStringLiteral("dpkg"), QStringLiteral("dpkg-deb"), QStringLiteral("dpkg-divert"), QStringLiteral("dpkg-maintscript-helper"), QStringLiteral("dpkg-query"),
        QStringLiteral("dpkg-realpath"), QStringLiteral("dpkg-split"), QStringLiteral("dpkg-statoverride"), QStringLiteral("dpkg-trigger"),
        QStringLiteral("dpkg-architecture"), QStringLiteral("dpkg-buildapi"), QStringLiteral("dpkg-buildflags"), QStringLiteral("dpkg-buildpackage"),
        QStringLiteral("dpkg-buildtree"), QStringLiteral("dpkg-checkbuilddeps"), QStringLiteral("dpkg-distaddfile"),
        QStringLiteral("dpkg-genbuildinfo"), QStringLiteral("dpkg-genchanges"), QStringLiteral("dpkg-gencontrol"), QStringLiteral("dpkg-gensymbols"),
        QStringLiteral("dpkg-mergechangelogs"), QStringLiteral("dpkg-name"), QStringLiteral("dpkg-parsechangelog"),
        QStringLiteral("dpkg-scanpackages"), QStringLiteral("dpkg-scansources"), QStringLiteral("dpkg-shlibdeps"), QStringLiteral("dpkg-source"),
        QStringLiteral("dpkg-vendor")
    };
    for (const QString& tool : dpkgTools)
        exec("/usr/bin/ln", QStringList{QStringLiteral("-sf"), QStringLiteral("/usr/bin/nx-pkgmgr-policy"), QStringLiteral("/usr/bin/") + tool});

    log("INFO", "Policy symlinks installed.");
}

void Agent::cleanup()
{
    log("INFO", "--- Cleanup: unmounting chroot mounts ---");

    auto umount = [this](const QString& mp) {
        bool ok = exec(QStringLiteral("/usr/bin/umount"), QStringList{mp});
        log("DEBUG", QStringLiteral("umount %1: %2").arg(mp, ok ? QStringLiteral("ok") : QStringLiteral("failed (may not be mounted)")));
    };

    umount(m_params.squashfsDir);
    umount(QStringLiteral("/home"));
    umount(QStringLiteral("/var/lib"));
    umount(QStringLiteral("/dev"));
}

} // namespace NutsAgent
