#pragma once

#include "Project.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace censorcut {

/// One contiguous range of source video that should appear in the output.
/// Times are inclusive of startMs and exclusive of endMs.
struct KeepSegment {
    qint64 startMs = 0;
    qint64 endMs   = 0;
    [[nodiscard]] qint64 durationMs() const noexcept { return endMs - startMs; }
};

/// Result of computing what to keep from a project.
struct ExportPlan {
    QList<KeepSegment> keepSegments;
    QString            errorMessage;  // non-empty -> plan is invalid

    [[nodiscard]] bool ok() const { return errorMessage.isEmpty() && !keepSegments.isEmpty(); }
};

/// Confirmed marker ranges, clamped to [0, durationMs), sorted by start, with
/// overlapping and adjacent ranges merged. This is the exact complement of
/// planExport()'s keep segments — exposed so the edit-list writer applies the
/// same merge rules as the encoder instead of reimplementing them.
QList<QPair<qint64, qint64>> mergedConfirmedCuts(const QList<Marker>& markers,
                                                 qint64 durationMs);

/// Compute keep-segments from a project.
/// - Confirmed markers are merged (overlapping or adjacent ranges collapse).
/// - Pending and rejected markers are ignored.
/// - durationMs <= 0 -> error.
/// - Markers covering the whole [0, durationMs) -> error (nothing to keep).
ExportPlan planExport(const Project& project);

/// Quality preset for the per-segment encode.
enum class ExportQuality {
    /// Re-encode each kept segment so cuts are frame-accurate.
    Accurate,
    /// Stream-copy each kept segment, snapping to source keyframes.
    /// Near-instant but cuts can be off by up to a GOP.
    Fast,
};

/// Inputs to the per-segment ffmpeg arg builder.
struct ExportArgsOptions {
    QString       sourcePath;
    QString       outputPath;
    KeepSegment   segment;
    ExportQuality quality           = ExportQuality::Accurate;
    int           fps               = 0;   // 0 -> don't pass -r (keep source fps)
    QString       videoCodec        = QStringLiteral("libx264");
    int           crf               = 18;
    QString       preset            = QStringLiteral("medium");
    QString       audioCodec        = QStringLiteral("aac");
    int           audioBitrateKbps  = 192;
};

/// Build the ffmpeg argv (without the ffmpeg executable itself) for encoding
/// a single keep-segment into outputPath. Suitable for QProcess::start.
QStringList buildSegmentEncodeArgs(const ExportArgsOptions& opts);

/// Build the ffmpeg argv for the lossless concat step that stitches segment
/// files back together (per the concat demuxer + -c copy).
QStringList buildConcatArgs(const QString& listFilePath, const QString& outputPath);

} // namespace censorcut
