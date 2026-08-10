#include "ExportPlan.h"

#include <algorithm>

namespace censorcut {

QList<QPair<qint64, qint64>> mergedConfirmedCuts(const QList<Marker>& markers,
                                                 qint64 durationMs)
{
    QList<QPair<qint64, qint64>> ranges;
    ranges.reserve(markers.size());
    for (const auto& m : markers) {
        if (m.status != Status::Confirmed) continue;
        if (!m.isValid()) continue;
        const qint64 start = std::clamp<qint64>(m.startMs, 0, durationMs);
        const qint64 end   = std::clamp<qint64>(m.endMs,   0, durationMs);
        if (end <= start) continue;
        ranges.append(qMakePair(start, end));
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const QPair<qint64, qint64>& a, const QPair<qint64, qint64>& b) {
                  return a.first < b.first;
              });

    QList<QPair<qint64, qint64>> merged;
    for (const auto& r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second) {
            merged.last().second = std::max(merged.last().second, r.second);
        } else {
            merged.append(r);
        }
    }
    return merged;
}

namespace {

QString msToSeconds(qint64 ms)
{
    return QString::number(ms / 1000.0, 'f', 3);
}

} // namespace

ExportPlan planExport(const Project& project)
{
    ExportPlan plan;
    if (project.durationMs <= 0) {
        plan.errorMessage = QStringLiteral("Source duration is zero or unknown.");
        return plan;
    }

    const auto cuts = mergedConfirmedCuts(project.markers, project.durationMs);

    qint64 cursor = 0;
    for (const auto& cut : cuts) {
        if (cut.first > cursor) {
            plan.keepSegments.append({cursor, cut.first});
        }
        cursor = std::max(cursor, cut.second);
    }
    if (cursor < project.durationMs) {
        plan.keepSegments.append({cursor, project.durationMs});
    }

    if (plan.keepSegments.isEmpty()) {
        plan.errorMessage = QStringLiteral(
            "All confirmed markers cover the entire video — nothing left to keep.");
    }
    return plan;
}

QStringList buildSegmentEncodeArgs(const ExportArgsOptions& opts)
{
    QStringList args;
    args << QStringLiteral("-y");
    // -ss before -i = fast input seek (used in both modes; for the accurate
    // re-encode the seek point is exactly honoured because we re-encode from
    // scratch).
    args << QStringLiteral("-ss") << msToSeconds(opts.segment.startMs);
    args << QStringLiteral("-i")  << opts.sourcePath;
    // -t after -i is unambiguously a duration limit on the output; safer
    // than -to which has different semantics depending on -ss placement.
    args << QStringLiteral("-t")  << msToSeconds(opts.segment.durationMs());

    if (opts.quality == ExportQuality::Fast) {
        args << QStringLiteral("-c") << QStringLiteral("copy")
             << QStringLiteral("-avoid_negative_ts") << QStringLiteral("make_zero");
    } else {
        args << QStringLiteral("-c:v")     << opts.videoCodec
             << QStringLiteral("-crf")     << QString::number(opts.crf)
             << QStringLiteral("-preset")  << opts.preset
             << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
             << QStringLiteral("-vsync")   << QStringLiteral("cfr");
        if (opts.fps > 0) {
            args << QStringLiteral("-r") << QString::number(opts.fps);
        }
        args << QStringLiteral("-c:a") << opts.audioCodec
             << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(opts.audioBitrateKbps);
    }

    args << QStringLiteral("-progress") << QStringLiteral("pipe:1");
    args << opts.outputPath;
    return args;
}

QStringList buildConcatArgs(const QString& listFilePath, const QString& outputPath)
{
    return QStringList{
        QStringLiteral("-y"),
        QStringLiteral("-f"),    QStringLiteral("concat"),
        QStringLiteral("-safe"), QStringLiteral("0"),
        QStringLiteral("-i"),    listFilePath,
        QStringLiteral("-c"),    QStringLiteral("copy"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        outputPath,
    };
}

} // namespace censorcut
