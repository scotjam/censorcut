#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace censorcut {

/// Wraps the `censorcut-sync` Rust binary as a child process. Started
/// when the user has opted in to P2P feedback sharing (default), killed
/// when they opt out. Reads stdin/stderr lines through to qDebug for
/// dev visibility — censorcut-sync is the single source of truth for
/// what got sent / received.
class SyncProcess : public QObject {
    Q_OBJECT
public:
    explicit SyncProcess(QObject* parent = nullptr);
    ~SyncProcess() override;

    /// Try to find the sidecar binary in (in order):
    ///   1. The path passed via setBinaryPath()
    ///   2. <app dir>/censorcut-sync(.exe)
    ///   3. <app dir>/../sync/target/release/censorcut-sync(.exe)  (dev)
    ///   4. <app dir>/../sync/target/debug/censorcut-sync(.exe)    (dev)
    ///   5. PATH lookup
    /// Empty string if not found.
    static QString locateBinary();

    void setBinaryPath(const QString& path);

    /// Start the sidecar with the standard feedback/peers/proposed paths
    /// and an optional bootstrap-peer list. Idempotent: if already running,
    /// does nothing. Returns false if the binary couldn't be located or
    /// QProcess refused to start.
    bool start(const QStringList& bootstrap = {});

    /// Send SIGTERM-equivalent to the sidecar and wait briefly for it
    /// to exit. Idempotent.
    void stop();

    bool isRunning() const;

signals:
    void started();
    void stopped(int exitCode);
    void failed(const QString& reason);

private slots:
    void onStdoutReady();
    void onStderrReady();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    QProcess m_proc;
    QString  m_binaryPath;
};

} // namespace censorcut
