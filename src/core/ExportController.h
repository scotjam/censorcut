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
    void onRunnerFailed(const QString& reason);

private:
    enum class Phase { Idle, Encoding, Concat, Done };

    void encodeNextSegment();
    void runConcat();
    void abortWith(const QString& reason);
    void cleanupTempDir();
    QString segmentPath(int index) const;

    Phase         m_phase   = Phase::Idle;
    Project       m_project;
    ExportPlan    m_plan;
    ExportQuality m_quality = ExportQuality::Accurate;
    QString       m_destination;
    QString       m_tempDir;
    QString       m_tempOutput;
    int           m_currentSegmentIdx = 0;
    qint64        m_totalDurationMs   = 0;
    qint64        m_doneDurationMs    = 0;
    bool          m_cancelRequested   = false;

    FfmpegRunner* m_runner = nullptr;
};

} // namespace censorcut
