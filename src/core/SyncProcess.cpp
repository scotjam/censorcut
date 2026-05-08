#include "SyncProcess.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace censorcut {

SyncProcess::SyncProcess(QObject* parent)
    : QObject(parent)
{
    // censorcut-sync is a small Rust binary doing P2P gossip — ~64 MiB
    // is more than enough; cap it tightly so a malformed envelope
    // can't pressure the editor's memory.
    m_proc.setJobMemoryLimitBytes(256LL * 1024 * 1024);

    connect(&m_proc, &QProcess::readyReadStandardOutput,
            this, &SyncProcess::onStdoutReady);
    connect(&m_proc, &QProcess::readyReadStandardError,
            this, &SyncProcess::onStderrReady);
    connect(&m_proc, &QProcess::finished,
            this, &SyncProcess::onProcessFinished);
    connect(&m_proc, &QProcess::errorOccurred,
            this, &SyncProcess::onErrorOccurred);
}

SyncProcess::~SyncProcess()
{
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
}

QString SyncProcess::locateBinary()
{
#if defined(Q_OS_WIN)
    static const QString kExe = QStringLiteral("censorcut-sync.exe");
#else
    static const QString kExe = QStringLiteral("censorcut-sync");
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QLatin1Char('/') + kExe,
        appDir + QStringLiteral("/../sync/target/release/") + kExe,
        appDir + QStringLiteral("/../sync/target/debug/")   + kExe,
        // From <repo>/build/ when running the editor in-tree.
        appDir + QStringLiteral("/../../sync/target/release/") + kExe,
        appDir + QStringLiteral("/../../sync/target/debug/")   + kExe,
    };
    for (const auto& p : candidates) {
        if (QFileInfo::exists(p)) return QDir::cleanPath(p);
    }
    const QString sys = QStandardPaths::findExecutable(QStringLiteral("censorcut-sync"));
    if (!sys.isEmpty()) return sys;
    return {};
}

void SyncProcess::setBinaryPath(const QString& path)
{
    m_binaryPath = path;
}

bool SyncProcess::isRunning() const
{
    return m_proc.state() != QProcess::NotRunning;
}

bool SyncProcess::start(const QStringList& bootstrap)
{
    if (isRunning()) return true;

    QString exe = m_binaryPath.isEmpty() ? locateBinary() : m_binaryPath;
    if (exe.isEmpty()) {
        emit failed(QStringLiteral(
            "censorcut-sync binary not found. Build with `cargo build --release` "
            "in the sync/ directory or copy it next to censorcut.exe."));
        return false;
    }
    m_binaryPath = exe;

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString feedback  = QDir::cleanPath(home + QStringLiteral("/.censorcut/feedback.jsonl"));
    const QString peers     = QDir::cleanPath(home + QStringLiteral("/.censorcut/peers.jsonl"));
    const QString proposed  = QDir::cleanPath(home + QStringLiteral("/.censorcut/proposed.jsonl"));
    const QString identity  = QDir::cleanPath(home + QStringLiteral("/.censorcut/identity.key"));
    const QString accepted  = QDir::cleanPath(home + QStringLiteral("/.censorcut/accepted_categories.txt"));

    QStringList args;
    args << QStringLiteral("--feedback") << feedback
         << QStringLiteral("--peers")    << peers
         << QStringLiteral("--proposed") << proposed
         << QStringLiteral("--identity") << identity;
    if (QFileInfo::exists(accepted)) {
        args << QStringLiteral("--accepted-categories-file") << accepted;
    }
    args << QStringLiteral("gossip");
    for (const auto& boot : bootstrap) {
        args << QStringLiteral("--bootstrap") << boot;
    }

    qInfo().noquote() << "censorcut-sync:" << exe << args.join(QLatin1Char(' '));
    m_proc.setProgram(exe);
    m_proc.setArguments(args);
    m_proc.start();
    if (!m_proc.waitForStarted(5000)) {
        emit failed(QStringLiteral("censorcut-sync failed to start: %1").arg(m_proc.errorString()));
        return false;
    }
    emit started();
    return true;
}

void SyncProcess::stop()
{
    if (!isRunning()) return;
    m_proc.terminate();
    if (!m_proc.waitForFinished(3000)) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
}

void SyncProcess::onStdoutReady()
{
    const QByteArray buf = m_proc.readAllStandardOutput();
    if (!buf.isEmpty()) qDebug().noquote() << "[sync.out]" << QString::fromUtf8(buf).trimmed();
}

void SyncProcess::onStderrReady()
{
    const QByteArray buf = m_proc.readAllStandardError();
    if (!buf.isEmpty()) qInfo().noquote() << "[sync.err]" << QString::fromUtf8(buf).trimmed();
}

void SyncProcess::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    emit stopped(exitCode);
}

void SyncProcess::onErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        emit failed(QStringLiteral("censorcut-sync failed to start: %1")
                        .arg(m_proc.errorString()));
    }
}

} // namespace censorcut
