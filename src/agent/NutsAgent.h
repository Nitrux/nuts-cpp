// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include <QString>
#include <QStringList>
#include <csignal>   // sig_atomic_t

namespace NutsAgent {

// All parameters read from the INI file written by UpdateManager before the agent is launched.
struct AgentParams {
    QString minTarget;
    QString otaChecksum;
    QString dpkgAppImageUrl;
    QString dpkgAppImageSum;
    QString varLibUrl;
    QString varLibSum;
    QString ccuUrl;
    QString ccuChecksum;
    QString downloadDir;
    QString squashfsDir;
    QStringList mirrors;
    bool hasNvidia{false};
};

// Self-contained update agent that runs as a single persistent process inside
// overlayroot-chroot.  All mounts created by the agent survive for the
// lifetime of this process, fixing the "session amnesia" problem of the old
// step-by-step executeInOverlay() approach.
//
// No Q_OBJECT / signals / event loop — everything is synchronous.
class Agent {
public:
    explicit Agent(const AgentParams& params);

    // Execute the full update pipeline.
    // Returns 0 on success; 1-7 on the step that failed (for parent diagnostics);
    // negative values for configuration errors.
    int run();

private:
    // --- Update steps (mirror the original UpdateManager private helpers) ---

    // Mount /home (NX_HOME) and /var/lib (NX_VAR_LIB) + create working dirs.
    bool prepareSystemPartitions();

    // Download nuts-ota.squashfs from mirrors, verify SHA-256.
    bool downloadOTAPayload();

    // Mount the OTA squashfs at squashfsDir.
    bool mountOTAPayload();

    // Download the dpkg AppImage, verify, extract, create symlinks.
    bool prepareUpdateTools();

    // Download var-lib-dpkg tar, verify, extract to /.
    bool syncPackageData();

    // Phase 1 (batch unpack) + Phase 2 (configure/audit loop).
    bool performPackageUpdates();

    // Download nuts-cpp-ccu cleanup script, verify, execute.
    bool runCleanupCrew();

    // Create nx-pkgmgr-policy symlinks for apt* and dpkg*.
    void installPolicySymlinks();

    // Unmount squashfsDir, /home, /var/lib, /dev.  Called on all exit paths.
    void cleanup();

    // --- Utilities ---

    // Synchronous command runner.  Returns true iff exit code == 0.
    // Child stdout is forwarded to the agent's stdout (captured by the parent).
    // Child stderr is routed through log("ERROR", ...) on stdout so that
    // UpdateManager::parseAndRelayAgentOutput() captures and forwards it to
    // the Logger — dpkg/axel errors are never silently discarded.
    // If outPtr is non-null, the captured stdout string is written there too.
    // Returns false immediately (without launching) if s_interrupted is set.
    bool exec(const QString& program, const QStringList& args,
              QString* outPtr = nullptr);

    // Structured logging.  All output goes to stdout so the parent process
    // captures it in agentOutput.
    void log(const QString& level, const QString& msg);

    // Emit a structured progress line that UpdateManager::parseAndRelayAgentOutput()
    // parses to forward updateProgress() signals to the GUI.
    // Format: "[NUTS-AGENT] PROGRESS: <pct> <msg>\n"
    void progress(int pct, const QString& msg);

    AgentParams m_params;

    // Set by prepareUpdateTools(), used by performPackageUpdates().
    QString m_pkgManagerPath;

    // Set to non-zero by the POSIX signal handler (see installSignalHandlers()).
    // Agent::exec() checks this after each command and aborts if set.
    static volatile sig_atomic_t s_interrupted;
};

// Register SIGINT / SIGTERM handlers that set Agent::s_interrupted.
// Called once from main() before Agent::run().
void installSignalHandlers();

} // namespace NutsAgent
