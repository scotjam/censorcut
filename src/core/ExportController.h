#pragma once

#include "ExportPlan.h"
#include "Project.h"

#include <QObject>
#include <QString>

namespace censorcut {

class FfmpegRunner;

/// Drives the full export pipeline:
///   1. Compute plan (planExport)
///   2. Encode each keep-segment to a temp dir as segNNNN.mp4
///   3. Write list.txt and run ffmpeg concat -c copy to a temp output
///   4. Atomically move the temp output to the requested destination
///
/// Async — emits progressChanged / phaseChanged / completed / failed.
/// Single-shot: call start() once per export. The temp dir is cleaned up on
/// completion or on any failure path.
class ExportController : public QObject {
    Q_OBJECT
public:
    explicit ExportController(QObject* parent = nullptr);
    ~ExportController() override;

    /// Begin an export. Returns false (and emits failed) if the plan is
    /// invalid, the temp dir can't be created, or another export is already
    /// running. Caller is responsible for confirming any destination overwrite
    /// prior to calling start() — the controller will overwrite without
    /// asking.
    bool start(const Project& project,
               const QString& destinationPath,
               ExportQuality quality);

    /// Kill the current ffmpeg invocation and clean up the temp dir.
    /// Emits failed("Cancelled.") if running.
    void cancel();

    bool isRunning() const;

signals:
    /// Overall progress in [0.0, 1.0] across both encode and concat phases.
    void progressChanged(double fraction);
    /// Human-readable phase label, e.g. "Encoding segment 3 of 7".
    void phaseChanged(const QString& label);
    /// Emitted once on success. The output is already at the destination.
    void completed(const QString& outputPath);
    /// Emitted once on any failure path (cancel, ffmpeg error, I/O error).
    void failed(const QString& reason);

private slots:
    void onSegmentProgress(double fraction);
    void onSegmentFinished(bool ok, const QString& stderrTail);
    void onConcatProgress(double fraction);
    void onConcatFinished(bool ok, const QString& stderrTail);
    void onSubtitleMuxProgress(double fraction);
    void onSubtitleMuxFinished(bool ok, const QString& stderrTail);
    void onRunnerFailed(const QString& reason);

private:
    enum class Phase { Idle, Encoding, Concat, SubtitleMux, Done };

    void encodeNextSegment();
    void runConcat();
    void runSubtitleMux();
    void finalizeOutput();
    void abortWith(const QString& reason);
    void cleanupTempDir();
    QString segmentPath(int index) const;

    /// Run ffprobe synchronously; populate m_textSubtitleStreams with the
    /// indices of any text-format subtitle streams (subrip/ass/ssa/
    /// mov_text/webvtt) in the source. Image-based subs (PGS/VobSub) are
    /// skipped and a stderr note is emitted.
    void probeTextSubtitles();

    /// For each text subtitle stream, run ffmpeg synchronously to extract
    /// SRT, parse it, apply the cut plan, and write a re-timed SRT into
    /// the temp dir. Populates m_cutSubtitleSrts with the resulting paths.
    void extractAndCutSubtitles();

    /// Discover sidecar subtitle files next to the source video
    /// (<movie>.srt, <movie>.en.srt, .ass, .ssa, .vtt) and, for each one,
    /// convert to SRT, apply the cut plan, and append to m_cutSubtitleSrts.
    void pickUpSidecarSubtitles();

    /// Convert any text subtitle file to SRT in m_tempDir, apply the cut
    /// plan, and append the re-timed SRT to m_cutSubtitleSrts. Returns
    /// false on any failure (caller can decide to log/continue).
    bool processOneSubtitleSource(const QString& ffmpegPath,
                                  const QStringList& inputArgs,
                                  int ordinal,
                                  const QString& label);

    Phase         m_phase   = Phase::Idle;
    Project       m_project;
    ExportPlan    m_plan;
    ExportQuality m_quality = ExportQuality::Accurate;
    QString       m_destination;
    QString       m_tempDir;
    QString       m_tempOutput;
    QString       m_tempMuxedOutput;            // post-subtitle-mux output
    int           m_currentSegmentIdx = 0;
    qint64        m_totalDurationMs   = 0;
    qint64        m_doneDurationMs    = 0;
    bool          m_cancelRequested   = false;

    QList<int>    m_textSubtitleStreams;        // indices in the source
    QStringList   m_cutSubtitleSrts;            // SRT files in the temp dir

    FfmpegRunner* m_runner = nullptr;
};

} // namespace censorcut
