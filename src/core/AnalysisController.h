#pragma once

#include "AnalysisResult.h"
#include "SandboxedProcess.h"

#include <QObject>
#include <QProcess>
#include <QString>

namespace censorcut {

/// Drives the Python analyzer subprocess: locates `python`, sets the
/// working directory so `python -m censorcut.analyze` resolves, parses
/// PROGRESS lines from stdout, then reads the result JSON on success.
class AnalysisController : public QObject {
    Q_OBJECT
public:
    explicit AnalysisController(QObject* parent = nullptr);
    ~AnalysisController() override;

    /// Locate the python interpreter (override > PATH > "python").
    static QString locatePython();

    /// Locate the censorcut python package directory (containing
    /// censorcut/analyze.py). Returns empty string if not found.
    static QString locatePythonPackageDir();

    void setPythonPath(const QString& path);

    /// Multiplier applied to every category's threshold before fusion.
    /// Values <1.0 make the analyzer more sensitive (more suggestions),
    /// >1.0 stricter. Default 1.0.
    void   setThresholdMultiplier(double mul);
    double thresholdMultiplier() const;

    bool isRunning() const;

    /// Start an analysis. inputPath is the source video. Returns false
    /// if a process is already running or python/the package can't be
    /// located.
    bool start(const QString& inputPath);

    /// Start a fast fingerprint-only run — skips loudness / YAMNet /
    /// CLIP / Whisper, computes only the scene-cut + pHash video
    /// fingerprint. Used to identify a movie immediately on open.
    bool startFingerprintOnly(const QString& inputPath);

    /// Kill the running subprocess. Safe when not running.
    void cancel();

signals:
    void progressChanged(double fraction);
    void phaseChanged(const QString& phase);
    /// Emitted on success. The result is also accessible by reference.
    void completed(const AnalysisResult& result);
    /// Emitted on cancel or any failure path.
    void failed(const QString& reason);

private slots:
    void onStdoutReady();
    void onStderrReady();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    void parseStdoutLine(const QByteArray& line);

    SandboxedProcess m_proc;
    QString    m_pythonPath;
    QString    m_packageDir;
    QString    m_outPath;     // where the analyzer writes its JSON
    QByteArray m_stdoutBuf;
    QByteArray m_stderrTail;
    bool       m_cancelled = false;
    double     m_thresholdMul = 1.0;
};

} // namespace censorcut
