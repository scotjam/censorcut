#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace censorcut {

/// Runs a single ffmpeg invocation, parses its -progress pipe:1 output, and
/// reports progress via signals. Supports cancellation.
///
/// Caller pattern:
///   auto* runner = new FfmpegRunner(this);
///   runner->setExpectedOutputDurationMs(4500);
///   connect(runner, &FfmpegRunner::progressChanged, ...);
///   connect(runner, &FfmpegRunner::finished,        ...);
///   runner->start(buildSegmentEncodeArgs(opts));
class FfmpegRunner : public QObject {
    Q_OBJECT
public:
    explicit FfmpegRunner(QObject* parent = nullptr);
    ~FfmpegRunner() override;

    /// Locate ffmpeg by checking, in order:
    ///   1. An override path set via setFfmpegPath()
    ///   2. <repo or app dir>/third_party/ffmpeg/bin/ffmpeg(.exe)
    ///   3. The system PATH
    /// Returns an empty string if no ffmpeg is found.
    static QString locateFfmpeg();

    /// Locate ffprobe alongside ffmpeg using the same heuristics. Empty
    /// string if not found.
    static QString locateFfprobe();

    void setFfmpegPath(const QString& path);
    QString ffmpegPath() const;

    /// Total expected output duration, used to compute the progress fraction
    /// from ffmpeg's out_time_ms (which is reported in microseconds).
    void setExpectedOutputDurationMs(qint64 ms);

    /// Start ffmpeg. Returns false if the executable can't be found or the
    /// process fails to start; in that case `failed` is also emitted.
    bool start(const QStringList& args);

    /// Kill the running process. Safe to call when not running.
    void cancel();

    bool isRunning() const;

signals:
    /// fraction in [0.0, 1.0]
    void progressChanged(double fraction);
    /// ok=true on exit code 0 and not cancelled. stderrTail is the last few
    /// KB of ffmpeg's stderr (useful for diagnostics on failure).
    void finished(bool ok, const QString& stderrTail);
    /// Emitted on launch failure (couldn't find ffmpeg, or QProcess::start
    /// returned an error before the process ran).
    void failed(const QString& reason);

private slots:
    void onStdoutReady();
    void onStderrReady();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    void parseProgressLine(const QByteArray& line);

    QProcess   m_proc;
    QString    m_ffmpegPath;
    qint64     m_expectedDurationMs = 0;
    QByteArray m_stdoutBuf;
    QByteArray m_stderrTail;
    qint64     m_lastOutTimeUs   = 0;
    double     m_lastReportedFrac = -1.0;
    bool       m_cancelled = false;
};

} // namespace censorcut
