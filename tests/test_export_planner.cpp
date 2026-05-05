#include "core/ExportPlan.h"
#include "core/Marker.h"
#include "core/Project.h"

#include <QtTest/QtTest>

using namespace censorcut;

namespace {

Marker confirmedCut(qint64 startMs, qint64 endMs)
{
    Marker m;
    m.startMs = startMs;
    m.endMs   = endMs;
    m.status  = Status::Confirmed;
    return m;
}

Project makeProject(qint64 durationMs, const QList<Marker>& markers = {})
{
    Project p;
    p.sourceFile = QStringLiteral("/films/Movie.mp4");
    p.durationMs = durationMs;
    p.markers    = markers;
    return p;
}

} // namespace

class TestExportPlanner : public QObject {
    Q_OBJECT
private slots:
    void planSimple();
    void planLeadingCut();
    void planTrailingCut();
    void planFullCut();
    void planOverlapping();
    void planAdjacent();
    void planRejectedIgnored();
    void planZeroDuration();
    void buildFfmpegArgsAccurate();
    void buildFfmpegArgsFast();
    void buildConcatCommand();
};

void TestExportPlanner::planSimple()
{
    // 10s movie, cut [3s, 5s] -> keep [0, 3s] and [5s, 10s]
    auto plan = planExport(makeProject(10'000, {confirmedCut(3'000, 5'000)}));
    QVERIFY2(plan.ok(), qPrintable(plan.errorMessage));
    QCOMPARE(plan.keepSegments.size(), 2);
    QCOMPARE(plan.keepSegments[0].startMs, qint64{0});
    QCOMPARE(plan.keepSegments[0].endMs,   qint64{3'000});
    QCOMPARE(plan.keepSegments[1].startMs, qint64{5'000});
    QCOMPARE(plan.keepSegments[1].endMs,   qint64{10'000});
}

void TestExportPlanner::planLeadingCut()
{
    // Cut starts at 0 -> no leading keep-segment
    auto plan = planExport(makeProject(10'000, {confirmedCut(0, 4'000)}));
    QVERIFY(plan.ok());
    QCOMPARE(plan.keepSegments.size(), 1);
    QCOMPARE(plan.keepSegments[0].startMs, qint64{4'000});
    QCOMPARE(plan.keepSegments[0].endMs,   qint64{10'000});
}

void TestExportPlanner::planTrailingCut()
{
    // Cut ends at duration -> no trailing keep-segment
    auto plan = planExport(makeProject(10'000, {confirmedCut(7'000, 10'000)}));
    QVERIFY(plan.ok());
    QCOMPARE(plan.keepSegments.size(), 1);
    QCOMPARE(plan.keepSegments[0].startMs, qint64{0});
    QCOMPARE(plan.keepSegments[0].endMs,   qint64{7'000});
}

void TestExportPlanner::planFullCut()
{
    // Confirmed markers cover the whole movie -> error, no plan
    auto plan = planExport(makeProject(10'000, {confirmedCut(0, 10'000)}));
    QVERIFY(!plan.ok());
    QVERIFY(plan.keepSegments.isEmpty());
    QVERIFY(!plan.errorMessage.isEmpty());
}

void TestExportPlanner::planOverlapping()
{
    // [2,5] overlaps [4,7] -> single merged cut [2,7], keep [0,2] and [7,10]
    auto plan = planExport(makeProject(10'000, {
        confirmedCut(2'000, 5'000),
        confirmedCut(4'000, 7'000),
    }));
    QVERIFY(plan.ok());
    QCOMPARE(plan.keepSegments.size(), 2);
    QCOMPARE(plan.keepSegments[0].endMs, qint64{2'000});
    QCOMPARE(plan.keepSegments[1].startMs, qint64{7'000});
}

void TestExportPlanner::planAdjacent()
{
    // [2,5] and [5,7] touch with no gap -> single merged cut, keep [0,2] and [7,10]
    auto plan = planExport(makeProject(10'000, {
        confirmedCut(2'000, 5'000),
        confirmedCut(5'000, 7'000),
    }));
    QVERIFY(plan.ok());
    QCOMPARE(plan.keepSegments.size(), 2);
    QCOMPARE(plan.keepSegments[0].endMs,   qint64{2'000});
    QCOMPARE(plan.keepSegments[1].startMs, qint64{7'000});
}

void TestExportPlanner::planRejectedIgnored()
{
    // Pending and rejected markers must not affect the plan.
    Marker pending  = confirmedCut(2'000, 4'000); pending.status  = Status::Pending;
    Marker rejected = confirmedCut(6'000, 8'000); rejected.status = Status::Rejected;
    Marker confirmed = confirmedCut(5'000, 5'500);

    auto plan = planExport(makeProject(10'000, {pending, rejected, confirmed}));
    QVERIFY(plan.ok());
    QCOMPARE(plan.keepSegments.size(), 2);
    QCOMPARE(plan.keepSegments[0].startMs, qint64{0});
    QCOMPARE(plan.keepSegments[0].endMs,   qint64{5'000});
    QCOMPARE(plan.keepSegments[1].startMs, qint64{5'500});
    QCOMPARE(plan.keepSegments[1].endMs,   qint64{10'000});
}

void TestExportPlanner::planZeroDuration()
{
    auto plan = planExport(makeProject(0, {}));
    QVERIFY(!plan.ok());
    QVERIFY(!plan.errorMessage.isEmpty());
}

void TestExportPlanner::buildFfmpegArgsAccurate()
{
    ExportArgsOptions opts;
    opts.sourcePath = QStringLiteral("/films/Movie.mp4");
    opts.outputPath = QStringLiteral("/tmp/seg0.mp4");
    opts.segment    = {3'000, 7'500};
    opts.quality    = ExportQuality::Accurate;
    opts.fps        = 24;

    const QStringList args = buildSegmentEncodeArgs(opts);

    // Spot-check critical pieces.
    QVERIFY(args.contains(QStringLiteral("-ss")));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-ss")) + 1), QStringLiteral("3.000"));

    QCOMPARE(args.at(args.indexOf(QStringLiteral("-i")) + 1), QStringLiteral("/films/Movie.mp4"));
    // Duration: 7500ms - 3000ms = 4500ms = 4.500s
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-t")) + 1), QStringLiteral("4.500"));

    QVERIFY(args.contains(QStringLiteral("libx264")));
    QVERIFY(args.contains(QStringLiteral("yuv420p")));
    QVERIFY(args.contains(QStringLiteral("cfr")));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-r")) + 1), QStringLiteral("24"));
    QVERIFY(args.contains(QStringLiteral("aac")));
    QCOMPARE(args.last(), QStringLiteral("/tmp/seg0.mp4"));
    // -progress pipe:1 must be present so the runner can stream output.
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-progress")) + 1), QStringLiteral("pipe:1"));
}

void TestExportPlanner::buildFfmpegArgsFast()
{
    ExportArgsOptions opts;
    opts.sourcePath = QStringLiteral("/films/Movie.mp4");
    opts.outputPath = QStringLiteral("/tmp/seg0.mp4");
    opts.segment    = {3'000, 7'500};
    opts.quality    = ExportQuality::Fast;

    const QStringList args = buildSegmentEncodeArgs(opts);

    QCOMPARE(args.at(args.indexOf(QStringLiteral("-c")) + 1), QStringLiteral("copy"));
    // No re-encode flags in fast mode.
    QVERIFY(!args.contains(QStringLiteral("-c:v")));
    QVERIFY(!args.contains(QStringLiteral("libx264")));
    QVERIFY(!args.contains(QStringLiteral("-crf")));
    QVERIFY(!args.contains(QStringLiteral("yuv420p")));
    QCOMPARE(args.last(), QStringLiteral("/tmp/seg0.mp4"));
}

void TestExportPlanner::buildConcatCommand()
{
    const QStringList args = buildConcatArgs(QStringLiteral("/tmp/list.txt"),
                                              QStringLiteral("/tmp/out.mp4"));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-f")) + 1),    QStringLiteral("concat"));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-safe")) + 1), QStringLiteral("0"));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-i")) + 1),    QStringLiteral("/tmp/list.txt"));
    QCOMPARE(args.at(args.indexOf(QStringLiteral("-c")) + 1),    QStringLiteral("copy"));
    QCOMPARE(args.last(),                                         QStringLiteral("/tmp/out.mp4"));
}

QTEST_MAIN(TestExportPlanner)
#include "test_export_planner.moc"
