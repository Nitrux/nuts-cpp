// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

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

void Agent::progress(int pct, const QString& msg)
{
    // Parsed by UpdateManager::parseAndRelayAgentOutput() to emit updateProgress().
    QTextStream out(stdout);
    out << "[NUTS-AGENT] PROGRESS: " << pct << " " << msg << "\n";
    out.flush();
}

bool Agent::exec(const QString& program, const QStringList& args, QString* outPtr)
{
    // Abort immediately if a signal was received between commands.
    if (s_interrupted) {
        log("WARNING", "Interrupted before: " + program);
        return false;
    }

    // Log the full command line.
    QString cmdLine = program;
    if (!args.isEmpty())
        cmdLine += " " + args.join(' ');
    log("DEBUG", "EXEC: " + cmdLine);

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start(program, args);

    // 10-minute per-command timeout (matches the old executeInOverlay timeout).
    if (!proc.waitForFinished(600000)) {
        proc.kill();
        log("ERROR", "Command timed out: " + cmdLine);
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
        for (const QString& line : err.split('\n', Qt::SkipEmptyParts))
            log("ERROR", line);
    }

    int code = proc.exitCode();
    log("DEBUG", QString("EXIT(%1): %2").arg(code).arg(program));
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
    exec("/usr/bin/mount", {"-t", "devtmpfs", "dev", "/dev"});
    exec("/usr/bin/mkdir", {"-p", "/tmp"});
    exec("/usr/bin/chmod", {"1777", "/tmp"});

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
    log("INFO", "NX_HOME device: " + homeDev);

    bool mountOk = exec("/usr/bin/mount", {"-t", "auto", homeDev, "/home"});
    log("DEBUG", QString("mount %1 /home: %2")
        .arg(homeDev, mountOk ? "ok" : "failed (may already be mounted)"));

    // --- Mount NX_VAR_LIB → /var/lib ---
    log("DEBUG", "Resolving LABEL=NX_VAR_LIB...");
    if (!exec("/usr/sbin/findfs", {"LABEL=NX_VAR_LIB"}, &output)) {
        log("ERROR", "Could not find partition labeled NX_VAR_LIB");
        return false;
    }
    QString varLibDev = output.trimmed();
    log("INFO", "NX_VAR_LIB device: " + varLibDev);

    mountOk = exec("/usr/bin/mount", {"-t", "auto", varLibDev, "/var/lib"});
    log("DEBUG", QString("mount %1 /var/lib: %2")
        .arg(varLibDev, mountOk ? "ok" : "failed (may already be mounted)"));

    // --- Create working directories ---
    exec("/usr/bin/mkdir", {"-p", m_params.downloadDir});
    exec("/usr/bin/mkdir", {"-p", m_params.squashfsDir});
    log("DEBUG", "Working dirs: " + m_params.downloadDir + ", " + m_params.squashfsDir);

    return true;
}

bool Agent::downloadOTAPayload()
{
    QString otaPath = m_params.downloadDir + "/nuts-ota.squashfs";

    log("INFO", "OTA target path: " + otaPath);
    log("INFO", "OTA expected checksum: " + m_params.otaChecksum);
    log("INFO", "Mirrors available: " + QString::number(m_params.mirrors.size()));

    // Check if a valid file already exists.
    QString output;
    if (exec("/usr/bin/test", {"-f", otaPath})) {
        log("INFO", "Existing OTA file found, verifying checksum...");
        QString checksumExpr = QString(
            "[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]")
            .arg(otaPath, m_params.otaChecksum);
        if (exec("/bin/sh", {"-c", checksumExpr})) {
            log("SUCCESS", "Existing OTA payload verified, skipping download.");
            return true;
        }
        log("WARNING", "Existing OTA payload checksum mismatch. Re-downloading.");
        exec("/usr/bin/rm", {"-f", otaPath});
    } else {
        log("INFO", "No existing OTA file found at " + otaPath);
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
        exec("/usr/bin/rm", {"-f", otaPath});

        log("INFO", "Trying mirror: " + url);
        bool ok = exec("/usr/bin/axel", {"-n", "10", "-o", otaPath, url});

        if (!ok) {
            log("WARNING", "Download from " + url + " failed.");
            continue;
        }
        log("DEBUG", "axel download complete, verifying checksum...");

        QString checksumExpr = QString(
            "[ \"$(sha256sum '%1' | awk '{print $1}')\" = \"%2\" ]")
            .arg(otaPath, m_params.otaChecksum);
        if (exec("/bin/sh", {"-c", checksumExpr})) {
            log("SUCCESS", "OTA payload downloaded and verified from " + url);
            return true;
        }

        // Log actual checksum for diagnosis.
        QString actualSum;
        exec("/bin/sh",
             {"-c", QString("sha256sum '%1' | awk '{print $1}'").arg(otaPath)},
             &actualSum);
        log("WARNING", "Checksum mismatch from " + url);
        log("WARNING", "  Expected: " + m_params.otaChecksum);
        log("WARNING", "  Got:      " + actualSum.trimmed());
        exec("/usr/bin/rm", {"-f", otaPath});
    }

    log("ERROR", "Failed to download OTA payload from all mirrors.");
    return false;
}

bool Agent::mountOTAPayload()
{
    QString otaPath    = m_params.downloadDir + "/nuts-ota.squashfs";
    QString mountPoint = m_params.squashfsDir;

    log("INFO", "Mounting: " + otaPath + " -> " + mountPoint);
    if (!exec("/usr/bin/mount", {otaPath, mountPoint})) {
        log("ERROR", "Failed to mount OTA squashfs");
        return false;
    }
    QString listing;
    exec("/usr/bin/ls", {mountPoint}, &listing);
    log("DEBUG", "OTA squashfs root contents: " + listing.trimmed());
    return true;
}

bool Agent::prepareUpdateTools()
{
    const QString appImagePath = "/tmp/dpkg-1.22.21-x86_64.AppImage";
    const QString extractDir   = "/tmp/pkgman-extracted";
    const QString appRunPath   = extractDir + "/squashfs-root/AppRun";

    log("INFO", "dpkg AppImage URL:  " + m_params.dpkgAppImageUrl);
    log("INFO", "dpkg AppImage path: " + appImagePath);
    log("INFO", "dpkg AppRun path:   " + appRunPath);
    log("INFO", "Expected DPKG_AI_SUM: " + m_params.dpkgAppImageSum);

    // If already extracted from a previous (interrupted) run, skip download.
    if (exec("/usr/bin/test", {"-f", appRunPath})) {
        log("INFO", "Extracted OTA tooling already present, skipping download.");
    } else {
        log("INFO", "OTA tooling not found, downloading...");

        if (!exec("/usr/bin/axel",
                  {"-n", "10", "-o", appImagePath, m_params.dpkgAppImageUrl})) {
            log("ERROR", "Failed to download OTA tooling");
            return false;
        }
        log("SUCCESS", "dpkg AppImage downloaded.");

        // Verify checksum.
        log("INFO", "Verifying dpkg AppImage checksum...");
        QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                                  .arg(m_params.dpkgAppImageSum, appImagePath);
        if (!exec("/bin/sh", {"-c", checksumCmd})) {
            log("ERROR", "CRITICAL: dpkg AppImage checksum mismatch!");
            QString actualSum;
            exec("/bin/sh",
                 {"-c", QString("sha256sum '%1' | awk '{print $1}'").arg(appImagePath)},
                 &actualSum);
            log("ERROR", "  Expected: " + m_params.dpkgAppImageSum);
            log("ERROR", "  Got:      " + actualSum.trimmed());
            exec("/usr/bin/rm", {"-f", appImagePath});
            return false;
        }
        log("SUCCESS", "dpkg AppImage checksum OK.");

        exec("/usr/bin/chmod", {"+x", appImagePath});

        log("INFO", "Extracting dpkg AppImage...");
        exec("/usr/bin/rm", {"-rf", extractDir});
        exec("/usr/bin/mkdir", {"-p", extractDir});
        if (!exec("/bin/sh",
                  {"-c", "cd " + extractDir + " && " + appImagePath + " --appimage-extract"})) {
            log("ERROR", "Failed to extract dpkg AppImage");
            return false;
        }

        if (!exec("/usr/bin/test", {"-f", appRunPath})) {
            log("ERROR", "Extraction completed but AppRun not found at: " + appRunPath);
            QString listing;
            exec("/usr/bin/ls", {"-la", extractDir + "/squashfs-root/"}, &listing);
            log("DEBUG", "squashfs-root contents: " + listing.trimmed());
            return false;
        }
        log("SUCCESS", "dpkg AppImage extracted. AppRun found.");
    }

    m_pkgManagerPath = appRunPath;
    log("INFO", "Using dpkg tooling at: " + m_pkgManagerPath);

    // Symlink dpkg → AppRun (mirrors: ln -svf "$AIPKG_MANAGER" /usr/bin/dpkg).
    exec("/usr/bin/ln", {"-svf", appRunPath, "/usr/bin/dpkg"});

    const QString binDir = extractDir + "/squashfs-root/usr/bin";
    const QStringList tools = {
        "dpkg-deb", "dpkg-divert", "dpkg-query", "dpkg-realpath",
        "dpkg-split", "dpkg-statoverride", "dpkg-trigger",
        "dpkg-maintscript-helper", "update-alternatives"
    };
    for (const QString& tool : tools) {
        QString target = binDir + "/" + tool;
        if (exec("/usr/bin/test", {"-f", target}))
            exec("/usr/bin/ln", {"-svf", target, "/usr/bin/" + tool});
        else
            log("WARNING", "Tool binary not found in AppImage, skipping: " + target);
    }

    exec("/usr/bin/mkdir", {"-p", "/usr/share"});
    exec("/usr/bin/ln",
         {"-svf", extractDir + "/squashfs-root/usr/share/dpkg", "/usr/share/dpkg"});

    return true;
}

bool Agent::syncPackageData()
{
    log("INFO", "Package DB archive URL:  " + m_params.varLibUrl);
    log("INFO", "Package DB archive path: " + m_params.varLibUrl);
    log("INFO", "Expected VAR_LIB_SUM: " + m_params.varLibSum);

    QString tarPath = QString("/tmp/var-lib-dpkg-%1.tar.xz").arg(m_params.minTarget);

    // Remove any stale file first.
    exec("/usr/bin/rm", {"-f", tarPath});

    log("INFO", "Downloading package database archive...");
    if (!exec("/usr/bin/axel", {"-n", "10", "-o", tarPath, m_params.varLibUrl})) {
        log("ERROR", "Failed to download package database archive");
        return false;
    }
    log("SUCCESS", "Package database archive downloaded.");

    // Verify checksum before extraction — unverified tar extraction to / is
    // extremely dangerous (zip-slip / overwrite attacks).
    log("INFO", "Verifying package database archive checksum...");
    QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(m_params.varLibSum, tarPath);
    if (!exec("/bin/sh", {"-c", checksumCmd})) {
        log("ERROR", "CRITICAL: Package database checksum mismatch!");
        QString actualSum;
        exec("/bin/sh",
             {"-c", QString("sha256sum '%1' | awk '{print $1}'").arg(tarPath)},
             &actualSum);
        log("ERROR", "  Expected: " + m_params.varLibSum);
        log("ERROR", "  Got:      " + actualSum.trimmed());
        exec("/usr/bin/rm", {"-f", tarPath});
        return false;
    }
    log("SUCCESS", "Package database archive checksum OK.");

    // Extract into the lower layer.
    log("INFO", "Extracting package database archive to /...");
    if (!exec("/bin/sh",
              {"-c", "mkdir -p /var/lib/dpkg && cd / && /usr/bin/tar -xf " + tarPath})) {
        log("ERROR", "Failed to extract package database archive");
        return false;
    }

    if (!exec("/usr/bin/test", {"-f", "/var/lib/dpkg/status"})) {
        log("ERROR", "Package database extraction failed: /var/lib/dpkg/status not found");
        QString listing;
        exec("/usr/bin/ls", {"-la", "/var/lib/dpkg/"}, &listing);
        log("DEBUG", "/var/lib/dpkg/ contents: " + listing.trimmed());
        return false;
    }
    log("SUCCESS", "Package database extracted. /var/lib/dpkg/status confirmed.");
    return true;
}

bool Agent::performPackageUpdates()
{
    QString otaDir     = m_params.squashfsDir + "/ota";
    QString updatesDir = otaDir + "/updates";
    QString nvidiaDir  = otaDir + "/nvidia";

    log("INFO", "OTA updates dir: " + updatesDir);
    log("INFO", "dpkg tooling path: " + m_pkgManagerPath);
    log("INFO", QString("NVIDIA hardware: %1").arg(m_params.hasNvidia ? "yes" : "no"));

    // Verify the dpkg tooling is present.
    if (!exec("/usr/bin/test", {"-f", m_pkgManagerPath})) {
        log("ERROR", "dpkg tooling not found: " + m_pkgManagerPath);
        return false;
    }

    // --- Phase 1: Collect .deb paths ---
    log("INFO", "--- Phase 1: Collecting .deb packages ---");

    // Build the argument list for find: [path...] -name *.deb -print0
    QStringList findArgs = {updatesDir};
    if (m_params.hasNvidia)
        findArgs << nvidiaDir;
    findArgs << "-name" << "*.deb" << "-print0";

    QString findOutput;
    bool findOk = exec("/usr/bin/find", findArgs, &findOutput);

    if (!findOk && findOutput.trimmed().isEmpty()) {
        log("ERROR", "find failed to enumerate .deb packages");
        return false;
    }

    QStringList debs = findOutput.split('\0', Qt::SkipEmptyParts);
    log("INFO", QString("Found %1 .deb package(s).").arg(debs.size()));
    for (const QString& deb : debs)
        log("DEBUG", "  deb: " + deb);

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
        log("INFO", QString("Unpacking batch %1/%2 (%3 package(s))...")
            .arg(batchNum).arg(totalBatches).arg(batch.size()));

        QStringList unpackArgs = {m_pkgManagerPath, "--force-all", "--unpack"};
        unpackArgs << batch;

        if (!exec(m_pkgManagerPath,
                  QStringList{"--force-all", "--unpack"} + batch)) {
            log("ERROR", QString("Unpack failed on batch %1/%2").arg(batchNum).arg(totalBatches));
            return false;
        }
        log("SUCCESS", QString("Batch %1/%2 unpacked.").arg(batchNum).arg(totalBatches));
    }
    log("SUCCESS", "All packages unpacked.");

    // --- Phase 3: Configure + audit loop ---
    log("INFO", "--- Phase 3: Configuring packages ---");

    const int maxPasses = 15;
    QString   lastAudit;

    for (int pass = 1; pass <= maxPasses; ++pass) {
        log("INFO", QString("Configuration pass %1/%2...").arg(pass).arg(maxPasses));

        // --configure -a: ignore non-zero (may be partial).
        exec(m_pkgManagerPath, {"--force-all", "--configure", "-a"});

        // --audit: what's still broken?
        QString auditOutput;
        exec(m_pkgManagerPath, {"--audit"}, &auditOutput);
        QString currentAudit = auditOutput.trimmed();

        if (currentAudit.isEmpty()) {
            log("SUCCESS", QString("Package configuration converged after %1 pass(es).").arg(pass));
            break;
        }

        log("INFO", "dpkg --audit output:\n" + currentAudit);

        if (pass > 1 && currentAudit == lastAudit) {
            log("ERROR", "Package configuration stuck — no progress between passes.");
            return false;
        }

        if (pass == maxPasses) {
            log("ERROR", QString("Package configuration failed to converge after %1 passes.").arg(maxPasses));
            return false;
        }

        lastAudit = currentAudit;
        ::sleep(1);
    }

    exec("/usr/bin/rm", {"-rf", "/tmp/pkgman-extracted"});
    log("SUCCESS", "Package updates applied successfully.");
    return true;
}

bool Agent::runCleanupCrew()
{
    const QString ccuPath = "/tmp/nuts-cpp-ccu";

    log("INFO", "CCU URL:  " + m_params.ccuUrl);
    log("INFO", "CCU path: " + ccuPath);
    log("INFO", "Expected NUTS_CCU_CHECKSUM: " + m_params.ccuChecksum);

    log("INFO", "Downloading cleanup crew...");
    if (!exec("/usr/bin/axel", {"-n", "10", "-o", ccuPath, m_params.ccuUrl})) {
        log("ERROR", "Failed to download cleanup crew");
        return false;
    }
    log("SUCCESS", "Cleanup crew downloaded.");

    log("INFO", "Verifying cleanup crew checksum...");
    QString checksumCmd = QString("echo '%1  %2' | /usr/bin/sha256sum -c -")
                              .arg(m_params.ccuChecksum, ccuPath);
    if (!exec("/bin/sh", {"-c", checksumCmd})) {
        log("ERROR", "CRITICAL: Cleanup crew checksum mismatch!");
        QString actualSum;
        exec("/bin/sh",
             {"-c", QString("sha256sum '%1' | awk '{print $1}'").arg(ccuPath)},
             &actualSum);
        log("ERROR", "  Expected: " + m_params.ccuChecksum);
        log("ERROR", "  Got:      " + actualSum.trimmed());
        exec("/usr/bin/rm", {"-f", ccuPath});
        return false;
    }
    log("SUCCESS", "Cleanup crew checksum OK.");

    exec("/usr/bin/chmod", {"+x", ccuPath});

    log("INFO", "Running cleanup crew...");
    if (!exec(ccuPath, {})) {
        log("ERROR", "Cleanup crew exited with non-zero status");
        return false;
    }
    log("SUCCESS", "Cleanup crew completed successfully.");
    return true;
}

void Agent::installPolicySymlinks()
{
    const QStringList aptTools = {
        "apt", "apt-cache", "apt-cdrom", "apt-config", "apt-get", "apt-mark"
    };
    for (const QString& tool : aptTools)
        exec("/usr/bin/ln", {"-sf", "/usr/bin/nx-pkgmgr-policy", "/usr/bin/" + tool});

    const QStringList dpkgTools = {
        "dpkg", "dpkg-deb", "dpkg-divert", "dpkg-maintscript-helper", "dpkg-query",
        "dpkg-realpath", "dpkg-split", "dpkg-statoverride", "dpkg-trigger",
        "dpkg-architecture", "dpkg-buildapi", "dpkg-buildflags", "dpkg-buildpackage",
        "dpkg-buildtree", "dpkg-checkbuilddeps", "dpkg-distaddfile",
        "dpkg-genbuildinfo", "dpkg-genchanges", "dpkg-gencontrol", "dpkg-gensymbols",
        "dpkg-mergechangelogs", "dpkg-name", "dpkg-parsechangelog",
        "dpkg-scanpackages", "dpkg-scansources", "dpkg-shlibdeps", "dpkg-source",
        "dpkg-vendor"
    };
    for (const QString& tool : dpkgTools)
        exec("/usr/bin/ln", {"-sf", "/usr/bin/nx-pkgmgr-policy", "/usr/bin/" + tool});

    log("INFO", "Policy symlinks installed.");
}

void Agent::cleanup()
{
    log("INFO", "--- Cleanup: unmounting chroot mounts ---");

    auto umount = [this](const QString& mp) {
        bool ok = exec("/usr/bin/umount", {mp});
        log("DEBUG", QString("umount %1: %2").arg(mp, ok ? "ok" : "failed (may not be mounted)"));
    };

    umount(m_params.squashfsDir);
    umount("/home");
    umount("/var/lib");
    umount("/dev");
}

} // namespace NutsAgent
