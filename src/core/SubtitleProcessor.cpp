#include "SubtitleProcessor.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <algorithm>

namespace censorcut {

namespace {

// 00:01:23,456 --> 00:01:25,789
const QRegularExpression kTimeLineRe(
    QStringLiteral(R"(^\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3}))"));

qint64 parseMs(int h, int m, int s, int frac, int fracDigits)
{
    qint64 ms = qint64(h) * 3600000 + qint64(m) * 60000 + qint64(s) * 1000;
    // SRT permits 3 digits but be tolerant of 1–3 digit fractional milliseconds.
    int scale = 1;
    for (int i = fracDigits; i < 3; ++i) scale *= 10;
    return ms + qint64(frac) * scale;
}

QString formatMs(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 totalSec = ms / 1000;
    const int h    = int(totalSec / 3600);
    const int min  = int((totalSec % 3600) / 60);
    const int sec  = int(totalSec % 60);
    const int milli = int(ms % 1000);
    return QStringLiteral("%1:%2:%3,%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(min, 2, 10, QLatin1Char('0'))
        .arg(sec, 2, 10, QLatin1Char('0'))
        .arg(milli, 3, 10, QLatin1Char('0'));
}

} // namespace

QList<SubtitleEntry> parseSrt(const QString& srt)
{
    // Normalize line endings, then split into "blocks" by blank lines.
    QString normalized = srt;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // Strip a leading UTF-8 BOM if present.
    if (!normalized.isEmpty() && normalized.at(0) == QChar(0xFEFF))
        normalized.remove(0, 1);

    const QStringList lines = normalized.split(QLatin1Char('\n'));
    QList<SubtitleEntry> out;
    QStringList textLines;
    SubtitleEntry pending;
    bool haveTimes = false;

    auto flush = [&]() {
        if (haveTimes) {
            pending.text = textLines.join(QLatin1Char('\n')).trimmed();
            if (pending.endMs > pending.startMs) out.append(pending);
        }
        pending = SubtitleEntry{};
        textLines.clear();
        haveTimes = false;
    };

    for (const QString& raw : lines) {
        const QString line = raw;
        if (line.trimmed().isEmpty()) {
            flush();
            continue;
        }
        const auto match = kTimeLineRe.match(line);
        if (match.hasMatch() && !haveTimes) {
            // Discard any stray text we collected before the time line — that
            // was the SRT index, which we don't preserve (we re-number on
            // write).
            textLines.clear();
            pending.startMs = parseMs(match.captured(1).toInt(),
                                      match.captured(2).toInt(),
                                      match.captured(3).toInt(),
                                      match.captured(4).toInt(),
                                      match.captured(4).size());
            pending.endMs   = parseMs(match.captured(5).toInt(),
                                      match.captured(6).toInt(),
                                      match.captured(7).toInt(),
                                      match.captured(8).toInt(),
                                      match.captured(8).size());
            haveTimes = true;
            continue;
        }
        if (!haveTimes) {
            // Could be the index — ignore until we see the time line.
            continue;
        }
        textLines.append(line);
    }
    flush();
    return out;
}

QList<SubtitleEntry> applyKeepSegments(const QList<SubtitleEntry>& entries,
                                       const QList<KeepSegment>& keepSegments)
{
    QList<SubtitleEntry> out;
    if (keepSegments.isEmpty()) return out;

    // Cumulative time the keep-segments add up to before each segment.
    // The output time of source position p (within keep segment k_i) is:
    //     output_t = (p - k_i.startMs) + cumulative[i]
    QVector<qint64> cumulative;
    cumulative.reserve(keepSegments.size());
    qint64 acc = 0;
    for (const auto& s : keepSegments) {
        cumulative.append(acc);
        acc += s.durationMs();
    }

    for (const SubtitleEntry& e : entries) {
        if (e.endMs <= e.startMs || e.text.trimmed().isEmpty()) continue;

        for (int i = 0; i < keepSegments.size(); ++i) {
            const auto& seg = keepSegments.at(i);
            if (e.endMs <= seg.startMs) break;          // entries are usually
            if (e.startMs >= seg.endMs) continue;       // sorted, but tolerate

            const qint64 srcStart = std::max(e.startMs, seg.startMs);
            const qint64 srcEnd   = std::min(e.endMs,   seg.endMs);
            if (srcEnd <= srcStart) continue;

            SubtitleEntry piece;
            piece.startMs = (srcStart - seg.startMs) + cumulative.at(i);
            piece.endMs   = (srcEnd   - seg.startMs) + cumulative.at(i);
            piece.text    = e.text;
            if (piece.endMs > piece.startMs) out.append(piece);
        }
    }

    // Sort just in case the source had out-of-order entries.
    std::sort(out.begin(), out.end(),
              [](const SubtitleEntry& a, const SubtitleEntry& b) {
                  return a.startMs < b.startMs;
              });
    return out;
}

QString writeSrt(const QList<SubtitleEntry>& entries)
{
    QString out;
    out.reserve(entries.size() * 64);
    int idx = 1;
    for (const SubtitleEntry& e : entries) {
        out += QString::number(idx++);
        out += QStringLiteral("\r\n");
        out += formatMs(e.startMs);
        out += QStringLiteral(" --> ");
        out += formatMs(e.endMs);
        out += QStringLiteral("\r\n");
        out += e.text;
        if (!e.text.endsWith(QLatin1Char('\n'))) out += QStringLiteral("\r\n");
        out += QStringLiteral("\r\n");
    }
    return out;
}

QList<SubtitleEntry> readSrtFile(const QString& path, QString* errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open SRT: %1").arg(path);
        return {};
    }
    return parseSrt(QString::fromUtf8(f.readAll()));
}

bool writeSrtFile(const QString& path,
                  const QList<SubtitleEntry>& entries,
                  QString* errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open SRT for write: %1").arg(path);
        return false;
    }
    const QByteArray bytes = writeSrt(entries).toUtf8();
    return f.write(bytes) == bytes.size();
}

} // namespace censorcut
