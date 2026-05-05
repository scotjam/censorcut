#include "core/ExportPlan.h"
#include "core/SubtitleProcessor.h"

#include <QtTest/QtTest>

using namespace censorcut;

namespace {

QString srtSample()
{
    // Three entries: 0..2s (kept), 4..6s (in cut), 8..10s (kept after cut).
    return QStringLiteral(
        "1\r\n"
        "00:00:00,000 --> 00:00:02,000\r\n"
        "Hello\r\n"
        "\r\n"
        "2\r\n"
        "00:00:04,000 --> 00:00:06,000\r\n"
        "Inside the cut\r\n"
        "\r\n"
        "3\r\n"
        "00:00:08,000 --> 00:00:10,000\r\n"
        "After the cut\r\n");
}

SubtitleEntry sub(qint64 startMs, qint64 endMs, const char* text)
{
    SubtitleEntry e;
    e.startMs = startMs;
    e.endMs   = endMs;
    e.text    = QString::fromUtf8(text);
    return e;
}

KeepSegment seg(qint64 a, qint64 b) { return {a, b}; }

} // namespace

class TestSubtitleProcessor : public QObject {
    Q_OBJECT
private slots:
    void parsesBasicSrt();
    void roundTripWriteParse();
    void dropsEntriesEntirelyInsideCut();
    void shiftsEntriesAfterCut();
    void clampsEntryStraddlingCutEnd();
    void splitsEntrySpanningACut();
    void emptyKeepSegmentsProducesEmpty();
    void parserToleratesDotInsteadOfComma();
};

void TestSubtitleProcessor::parsesBasicSrt()
{
    const auto entries = parseSrt(srtSample());
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries[0].startMs, qint64{0});
    QCOMPARE(entries[0].endMs,   qint64{2000});
    QCOMPARE(entries[0].text,    QStringLiteral("Hello"));
    QCOMPARE(entries[2].startMs, qint64{8000});
    QCOMPARE(entries[2].text,    QStringLiteral("After the cut"));
}

void TestSubtitleProcessor::roundTripWriteParse()
{
    QList<SubtitleEntry> entries = {
        sub(0,    2000, "Hello"),
        sub(4000, 6000, "World\nMulti-line"),
    };
    const QString rendered = writeSrt(entries);
    const auto reparsed = parseSrt(rendered);
    QCOMPARE(reparsed.size(), entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        QCOMPARE(reparsed[i].startMs, entries[i].startMs);
        QCOMPARE(reparsed[i].endMs,   entries[i].endMs);
        QCOMPARE(reparsed[i].text,    entries[i].text);
    }
}

void TestSubtitleProcessor::dropsEntriesEntirelyInsideCut()
{
    // Cuts: [3000, 7000]. Keeps: [0, 3000) and [7000, 10000).
    QList<KeepSegment> keep = { seg(0, 3000), seg(7000, 10000) };
    QList<SubtitleEntry> in = { sub(4000, 6000, "Inside cut") };
    const auto out = applyKeepSegments(in, keep);
    QVERIFY(out.isEmpty());
}

void TestSubtitleProcessor::shiftsEntriesAfterCut()
{
    // 4-second cut between two keeps. An entry at source-time 8..10
    // should land at output-time 4..6 in the cut video.
    QList<KeepSegment> keep = { seg(0, 3000), seg(7000, 10000) };
    QList<SubtitleEntry> in = { sub(8000, 9500, "After cut") };
    const auto out = applyKeepSegments(in, keep);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().startMs, qint64{4000});
    QCOMPARE(out.first().endMs,   qint64{5500});
    QCOMPARE(out.first().text,    QStringLiteral("After cut"));
}

void TestSubtitleProcessor::clampsEntryStraddlingCutEnd()
{
    // Source: keep [0,3000), cut [3000,7000), keep [7000,10000).
    // Entry 2500..4500 — first 500ms is in keep1, rest in the cut.
    QList<KeepSegment> keep = { seg(0, 3000), seg(7000, 10000) };
    QList<SubtitleEntry> in = { sub(2500, 4500, "Half in") };
    const auto out = applyKeepSegments(in, keep);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out.first().startMs, qint64{2500});
    QCOMPARE(out.first().endMs,   qint64{3000});
}

void TestSubtitleProcessor::splitsEntrySpanningACut()
{
    // Entry 2500..7500 covers the end of keep1, the cut, and the start of keep2.
    // It should produce two output entries with the same text.
    QList<KeepSegment> keep = { seg(0, 3000), seg(7000, 10000) };
    QList<SubtitleEntry> in = { sub(2500, 7500, "Through cut") };
    const auto out = applyKeepSegments(in, keep);
    QCOMPARE(out.size(), 2);
    // Piece in keep1: source 2500..3000 -> output 2500..3000 (cumulative 0).
    QCOMPARE(out[0].startMs, qint64{2500});
    QCOMPARE(out[0].endMs,   qint64{3000});
    // Piece in keep2: source 7000..7500 -> output 3000..3500 (cumulative 3000).
    QCOMPARE(out[1].startMs, qint64{3000});
    QCOMPARE(out[1].endMs,   qint64{3500});
    QCOMPARE(out[0].text, QStringLiteral("Through cut"));
    QCOMPARE(out[1].text, QStringLiteral("Through cut"));
}

void TestSubtitleProcessor::emptyKeepSegmentsProducesEmpty()
{
    const auto out = applyKeepSegments({sub(0, 1000, "x")}, {});
    QVERIFY(out.isEmpty());
}

void TestSubtitleProcessor::parserToleratesDotInsteadOfComma()
{
    // Some buggy SRT writers use '.' instead of ',' as the millisecond
    // separator. Be tolerant.
    const QString srt = QStringLiteral(
        "1\n00:00:01.000 --> 00:00:02.500\nHi\n");
    const auto entries = parseSrt(srt);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().startMs, qint64{1000});
    QCOMPARE(entries.first().endMs,   qint64{2500});
}

QTEST_MAIN(TestSubtitleProcessor)
#include "test_subtitle_processor.moc"
