#include "ExportController.h"

#include "FfmpegRunner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

namespace censorcut {

namespace {
constexpr double kEncodeShare = 0.9;   // first 90% of progress is per-segment encoding
constexpr double kConcatShare = 0.1;   // last 10% is concat
} // namespace

ExportController::ExportController(QObject* parent)
    : QObject(parent)
{
    m_runner = new FfmpegRunner(this);
}

ExportController::~ExportController()
{
    cleanupTempDir();
}

bool ExportController::isRunning() const
{
    return m_phase == Phase::Encoding || m_phase == Phase::Concat;
}

QString ExportController::segmentPath(int index) const
{
    return QStringLiteral("%1/seg%2.mp4").arg(m_tempDir).arg(index, 4, 10, QChar('0'));
}

bool ExportController::start(const Project& project,
                             const QString& destinationPath,
                             ExportQuality quality)
{
    if (isRunning()) return false;

    m_project     = project;
    m_destination = destinationPath;
    m_quality     = quality;
    m_plan        = planExport(project);
    if (!m_plan.ok()) {
        emit failed(m_plan.errorMessage);
        return false;
    }
    m_totalDurationMs = 0;
    for (const auto& s : m_plan.keepSegments) m_totalDurationMs += s.durationMs();
    m_doneDurationMs    = 0;
    m_currentSegmentIdx = 0;
    m_cancelRequested   = false;

    const QString uniq = QStringLiteral("censorcut_export_%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());
    m_tempDir = QDir::tempPath() + QLatin1Char('/') + uniq;
    if (!QDir().mkpath(m_tempDir)) {
        emit failed(QStringLiteral("Could not create temp directory: %1").arg(m_tempDir));
        return false;
    }

    disconnect(m_runner, nullptr, this, nullptr);
    connect(m_runner, &FfmpegRunner::progressChanged,
            this, &ExportController::onSegmentProgress);
    connect(m_runner, &FfmpegRunner::finished,
            this, &ExportController::onSegmentFinished);
    connect(m_runner, &FfmpegRunner::failed,
            this, &ExportController::onRunnerFailed);

    m_phase = Phase::Encoding;
    encodeNextSegment();
    return true;
}

void ExportController::encodeNextSegment()
{
    const auto& seg = m_plan.keepSegments.at(m_currentSegmentIdx);

    ExportArgsOptions opts;
    opts.sourcePath = m_project.sourceFile;
    opts.outputPath = segmentPath(m_currentSegmentIdx);
    opts.segment    = seg;
    opts.quality    = m_quality;
    // For the v1 export we don't probe ffprobe yet — leave fps unset so
    // ffmpeg keeps the source rate. -vsync cfr in the args still requests
    // a constant frame rate so concat boundaries don't drift.

    emit phaseChanged(QStringLiteral("Encoding segment %1 of %2 — %3 quality")
                          .arg(m_currentSegmentIdx + 1)
                          .arg(m_plan.keepSegments.size())
                          .arg(m_quality == ExportQuality::Accurate
                                   ? QStringLiteral("frame-accurate")
                                   : QStringLiteral("fast")));

    m_runner->setExpectedOutputDurationMs(seg.durationMs());
    if (!m_runner->start(buildSegmentEncodeArgs(opts))) {
        // failed() will fire via onRunnerFailed
    }
}

void ExportController::onSegmentProgress(double fraction)
{
    if (m_phase != Phase::Encoding) return;
    const auto& seg = m_plan.keepSegments.at(m_currentSegmentIdx);
    const qint64 segDoneMs   = qint64(fraction * double(seg.durationMs()));
    const qint64 totalDoneMs = m_doneDurationMs + segDoneMs;
    const double frac = m_totalDurationMs > 0
        ? std::clamp(double(totalDoneMs) / double(m_totalDurationMs), 0.0, 1.0)
        : 0.0;
    emit progressChanged(frac * kEncodeShare);
}

void ExportController::onSegmentFinished(bool ok, const QString& stderrTail)
{
    if (m_phase != Phase::Encoding) return;
    if (m_cancelRequested) { abortWith(QStringLiteral("Cancelled.")); return; }
    if (!ok) {
        abortWith(QStringLiteral("ffmpeg failed while encoding segment %1.\n\n%2")
                      .arg(m_currentSegmentIdx + 1).arg(stderrTail.right(2000)));
        return;
    }
    m_doneDurationMs += m_plan.keepSegments.at(m_currentSegmentIdx).durationMs();
    ++m_currentSegmentIdx;
    if (m_currentSegmentIdx < m_plan.keepSegments.size()) {
        encodeNextSegment();
    } else {
        runConcat();
    }
}

void ExportController::runConcat()
{
    const QString listPath = m_tempDir + QStringLiteral("/list.txt");
    {
        QFile lf(listPath);
        if (!lf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            abortWith(QStringLiteral("Could not write concat list: %1").arg(listPath));
            return;
        }
        QTextStream out(&lf);
        for (int i = 0; i < m_plan.keepSegments.size(); ++i) {
            // Relative names; concat demuxer resolves them against the list
            // file's directory.
            out << "file '" << QStringLiteral("seg%1.mp4").arg(i, 4, 10, QChar('0')) << "'\n";
        }
    }

    const QString suffix = QFileInfo(m_destination).suffix();
    m_tempOutput = m_tempDir + QStringLiteral("/output.")
                 + (suffix.isEmpty() ? QStringLiteral("mp4") : suffix);

    disconnect(m_runner, nullptr, this, nullptr);
    connect(m_runner, &FfmpegRunner::progressChanged,
            this, &ExportController::onConcatProgress);
    connect(m_runner, &FfmpegRunner::finished,
            this, &ExportController::onConcatFinished);
    connect(m_runner, &FfmpegRunner::failed,
            this, &ExportController::onRunnerFailed);

    emit phaseChanged(QStringLiteral("Combining %1 segments…").arg(m_plan.keepSegments.size()));

    m_phase = Phase::Concat;
    m_runner->setExpectedOutputDurationMs(m_totalDurationMs);
    if (!m_runner->start(buildConcatArgs(listPath, m_tempOutput))) {
        // failed() fires via onRunnerFailed
    }
}

void ExportController::onConcatProgress(double fraction)
{
    if (m_phase != Phase::Concat) return;
    emit progressChanged(std::clamp(kEncodeShare + fraction * kConcatShare, 0.0, 1.0));
}

void ExportController::onConcatFinished(bool ok, const QString& stderrTail)
{
    if (m_phase != Phase::Concat) return;
    if (m_cancelRequested) { abortWith(QStringLiteral("Cancelled.")); return; }
    if (!ok) {
        abortWith(QStringLiteral("ffmpeg failed while combining segments.\n\n%1")
                      .arg(stderrTail.right(2000)));
        return;
    }

    // Move the temp output into place. On Windows QFile::rename can't
    // overwrite, so explicitly remove first if the destination exists.
    if (QFile::exists(m_destination)) {
        if (!QFile::remove(m_destination)) {
            abortWith(QStringLiteral("Destination exists and could not be removed: %1")
                          .arg(m_destination));
            return;
        }
    }
    if (!QFile::rename(m_tempOutput, m_destination)) {
        // Fallback for cross-volume moves (rename only works within the same volume).
        if (!QFile::copy(m_tempOutput, m_destination)) {
            abortWith(QStringLiteral("Could not move output to %1").arg(m_destination));
            return;
        }
        QFile::remove(m_tempOutput);
    }

    cleanupTempDir();
    m_phase = Phase::Done;
    emit progressChanged(1.0);
    emit completed(m_destination);
}

void ExportController::onRunnerFailed(const QString& reason)
{
    abortWith(reason);
}

void ExportController::cancel()
{
    if (!isRunning()) return;
    m_cancelRequested = true;
    if (m_runner->isRunning()) m_runner->cancel();
    // The runner's finished() will fire via onSegmentFinished/onConcatFinished
    // which will see m_cancelRequested and call abortWith("Cancelled.").
}

void ExportController::abortWith(const QString& reason)
{
    cleanupTempDir();
    m_phase = Phase::Idle;
    emit failed(reason);
}

void ExportController::cleanupTempDir()
{
    if (m_tempDir.isEmpty()) return;
    QDir(m_tempDir).removeRecursively();
    m_tempDir.clear();
    m_tempOutput.clear();
}

} // namespace censorcut
