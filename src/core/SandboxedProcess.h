#pragma once

#include <QProcess>

namespace censorcut {

/// QProcess subclass that places the child process (and any descendants
/// it spawns) in a Windows Job Object configured for:
///
///   - JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: child dies if the editor
///     dies, even if the editor crashes.
///   - JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION: child crashes
///     cleanly instead of triggering Windows Error Reporting dialogs.
///   - JOB_OBJECT_LIMIT_JOB_MEMORY: per-job memory cap. Default 4 GiB.
///   - No JOB_OBJECT_LIMIT_BREAKAWAY_OK: child processes that the child
///     spawns (e.g. analyze.py spawning ffmpeg) inherit the Job and
///     can't escape the cage.
///
/// Hardware acceleration (CUDA, D3D11VA, DXVA2, NVDEC) is NOT affected
/// by Job Objects — the GPU stays accessible at full speed.
///
/// On non-Windows platforms this is a no-op pass-through to QProcess.
/// Linux/macOS sandboxing (seccomp / sandbox-exec) is tracked
/// separately as a follow-up.
///
/// Usage: drop-in replacement for QProcess. The Job is created lazily
/// in the constructor and the child is assigned to it as soon as the
/// `started()` signal fires (DirectConnection, microseconds after
/// CreateProcess returns — well before ffmpeg starts decoding any
/// untrusted bytes).
class SandboxedProcess : public QProcess {
    Q_OBJECT
public:
    explicit SandboxedProcess(QObject* parent = nullptr);
    ~SandboxedProcess() override;

    /// Total memory cap for the job (process + descendants), in bytes.
    /// 0 means no memory cap. Default 4 GiB. Must be set BEFORE the
    /// first start() to take effect.
    void setJobMemoryLimitBytes(qint64 bytes);

private slots:
    void onStarted();

private:
#ifdef Q_OS_WIN
    void* m_jobHandle = nullptr;  // HANDLE
    bool ensureJob();
    void destroyJob();
#endif
    qint64 m_jobMemoryLimitBytes = 4LL * 1024 * 1024 * 1024;
};

} // namespace censorcut
