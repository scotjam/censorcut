#include "core/Marker.h"
#include "core/MarkerModel.h"
#include "core/Project.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace censorcut;

class TestMarkerModel : public QObject {
    Q_OBJECT
private slots:
    void addAndSort();
    void confirmedAndCutTotal();
    void overlapMerging();
    void roundTripSidecar();
    void censoredOutputPath();
};

void TestMarkerModel::addAndSort()
{
    MarkerModel model;
    Marker a; a.startMs = 5000; a.endMs = 8000;
    Marker b; b.startMs = 1000; b.endMs = 2000;
    Marker c; c.startMs = 3000; c.endMs = 4000;
    model.addMarker(a);
    model.addMarker(b);
    model.addMarker(c);

    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.markers().at(0).startMs, qint64{1000});
    QCOMPARE(model.markers().at(1).startMs, qint64{3000});
    QCOMPARE(model.markers().at(2).startMs, qint64{5000});
}

void TestMarkerModel::confirmedAndCutTotal()
{
    MarkerModel model;
    Marker a; a.startMs = 0;     a.endMs = 1000; a.status = Status::Confirmed;
    Marker b; b.startMs = 2000;  b.endMs = 3500; b.status = Status::Pending;
    Marker c; c.startMs = 5000;  c.endMs = 6000; c.status = Status::Confirmed;
    model.addMarker(a);
    model.addMarker(b);
    model.addMarker(c);

    QCOMPARE(model.confirmedMarkers().size(), 2);
    QCOMPARE(model.totalConfirmedCutMs(), qint64{2000});  // 1000 + 1000
}

void TestMarkerModel::overlapMerging()
{
    MarkerModel model;
    Marker a; a.startMs = 0;    a.endMs = 1000; a.status = Status::Confirmed;
    Marker b; b.startMs = 500;  b.endMs = 1500; b.status = Status::Confirmed;
    Marker c; c.startMs = 1500; c.endMs = 2000; c.status = Status::Confirmed;
    model.addMarker(a);
    model.addMarker(b);
    model.addMarker(c);

    // a and b overlap, b and c are adjacent -> all merge into [0, 2000]
    QCOMPARE(model.totalConfirmedCutMs(), qint64{2000});
}

void TestMarkerModel::roundTripSidecar()
{
    QTemporaryDir tmpDir;
    QVERIFY(tmpDir.isValid());
    const QString path = tmpDir.filePath(QStringLiteral("project.censorcut.json"));

    Project p;
    p.sourceFile = QStringLiteral("/tmp/example.mp4");
    p.sourceHash = QStringLiteral("deadbeef");
    p.durationMs = 90 * 60 * 1000;
    Marker m; m.startMs = 12000; m.endMs = 18500;
    m.category = QStringLiteral("Jump scare"); m.note = QStringLiteral("loud");
    p.markers.append(m);

    QString err;
    QVERIFY2(p.saveToSidecar(path, &err), qPrintable(err));

    auto loaded = Project::loadFromSidecar(path, &err);
    QVERIFY2(loaded.has_value(), qPrintable(err));
    QCOMPARE(loaded->sourceFile, p.sourceFile);
    QCOMPARE(loaded->durationMs, p.durationMs);
    QCOMPARE(loaded->markers.size(), 1);
    QCOMPARE(loaded->markers.first().startMs, qint64{12000});
    QCOMPARE(loaded->markers.first().endMs,   qint64{18500});
    QCOMPARE(loaded->markers.first().category, QStringLiteral("Jump scare"));
}

void TestMarkerModel::censoredOutputPath()
{
    // Standard case: age suffixed onto the stem, extension preserved.
    QCOMPARE(Project::censoredOutputPathFor(QStringLiteral("/films/Title.mp4"), 7),
             QStringLiteral("/films/Title CENSORED-7.mp4"));

    // Different age, different number.
    QCOMPARE(Project::censoredOutputPathFor(QStringLiteral("/films/Title.mp4"), 4),
             QStringLiteral("/films/Title CENSORED-4.mp4"));

    // age <= 0 falls back to no number — useful for test cuts or "general" version.
    QCOMPARE(Project::censoredOutputPathFor(QStringLiteral("/films/Title.mp4"), 0),
             QStringLiteral("/films/Title CENSORED.mp4"));

    // File with no extension still works.
    const QString noExt = Project::censoredOutputPathFor(
        QStringLiteral("/films/movie_no_ext"), 7);
    QVERIFY(noExt.endsWith(QStringLiteral("movie_no_ext CENSORED-7")));

    // Multi-dot filename: completeBaseName keeps the inner dots, only
    // the final extension is restored after the suffix.
    QCOMPARE(Project::censoredOutputPathFor(QStringLiteral("/films/Foo.Bar.2020.mkv"), 9),
             QStringLiteral("/films/Foo.Bar.2020 CENSORED-9.mkv"));
}

QTEST_MAIN(TestMarkerModel)
#include "test_marker_model.moc"
