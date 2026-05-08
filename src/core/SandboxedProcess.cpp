#include "SandboxedProcess.h"

#include <QDebug>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace censorcut {

SandboxedProcess::SandboxedProcess(QObject* parent)
    : QProcess(parent)
{
    // DirectConnection so the slot runs synchronously when QProcess
    // emits started() — child has only just been launched, hasn't yet
    // reached untrusted input.
    connect(this, &QProcess::started, this, &SandboxedProcess::onStarted,
            Qt::DirectConnection);
}

SandboxedProcess::~SandboxedProcess()
{
#ifdef Q_OS_WIN
    destroyJob();
#endif
}

void SandboxedProcess::setJobMemoryLimitBytes(qint64 bytes)
{
    m_jobMemoryLimitBytes = bytes;
}

#ifdef Q_OS_WIN
bool SandboxedProcess::ensureJob()
{
    if (m_jobHandle) return true;
    HANDLE h = CreateJobObjectW(nullptr, nullptr);
    if (!h) {
        qWarning() << "SandboxedProcess: CreateJobObjectW failed,"
                   << "GetLastError =" << GetLastError();
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    DWORD flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (m_jobMemoryLimitBytes > 0) {
        flags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        info.JobMemoryLimit = SIZE_T(m_jobMemoryLimitBytes);
    }
    info.BasicLimitInformation.LimitFlags = flags;
    if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation,
                                  &info, sizeof(info))) {
        qWarning() << "SandboxedProcess: SetInformationJobObject failed,"
                   << "GetLastError =" << GetLastError();
        CloseHandle(h);
        return false;
    }
    m_jobHandle = h;
    return true;
}

void SandboxedProcess::destroyJob()
{
    if (m_jobHandle) {
        // Closing the last handle to the Job triggers KILL_ON_JOB_CLOSE
        // for any still-running child. That's the desired behaviour:
        // the editor's exit kills the analyzer/ffmpeg subprocess too.
        CloseHandle(static_cast<HANDLE>(m_jobHandle));
        m_jobHandle = nullptr;
    }
}
#endif

void SandboxedProcess::onStarted()
{
#ifdef Q_OS_WIN
    if (!ensureJob()) return;
    const qint64 pid = processId();
    if (pid <= 0) {
        qWarning() << "SandboxedProcess: started() with no PID";
        return;
    }
    HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                              FALSE, DWORD(pid));
    if (!proc) {
        qWarning() << "SandboxedProcess: OpenProcess failed,"
                   << "GetLastError =" << GetLastError();
        return;
    }
    if (!AssignProcessToJobObject(static_cast<HANDLE>(m_jobHandle), proc)) {
        qWarning() << "SandboxedProcess: AssignProcessToJobObject failed,"
                   << "GetLastError =" << GetLastError();
    }
    CloseHandle(proc);
#endif
}

} // namespace censorcut
