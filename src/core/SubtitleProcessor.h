#pragma once

#include "ExportPlan.h"

#include <QList>
#include <QString>

namespace censorcut {

/// One SRT block: a time range plus the multi-line dialogue text.
struct SubtitleEntry {
    qint64  startMs = 0;
    qint64  endMs   = 0;
    QString text;        // can contain '\n' between lines
};

/// Pure-logic helpers for taking a source SRT and producing a re-timed SRT
/// that matches the cuts an ExportController is about to apply. All times
/// are in milliseconds; no I/O happens through these functions except in
/// readSrtFile / writeSrtFile.

/// Parse SRT-format text. Tolerant of CR/LF differences and missing
/// trailing blank lines. Returns entries in source-file order.
QList<SubtitleEntry> parseSrt(const QString& srt);

/// Apply a list of keep-segments to a sorted list of subtitle entries.
/// - Entries entirely inside a cut are dropped.
/// - Entries that straddle a cut are split into one piece per kept segment
///   they touch (each piece carries the full text).
/// - All times are shifted left by the cumulative duration of cuts that
///   precede them, so the output is in the cut video's timeline.
QList<SubtitleEntry> applyKeepSegments(const QList<SubtitleEntry>& entries,
                                       const QList<KeepSegment>& keepSegments);

/// Render entries back to SRT text (CRLF line endings, blank line between
/// blocks, indices starting at 1).
QString writeSrt(const QList<SubtitleEntry>& entries);

/// Convenience helpers wrapping the above with file I/O.
QList<SubtitleEntry> readSrtFile(const QString& path, QString* errorOut = nullptr);
bool                 writeSrtFile(const QString& path,
                                  const QList<SubtitleEntry>& entries,
                                  QString* errorOut = nullptr);

} // namespace censorcut
