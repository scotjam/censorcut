// Tests for the shared edit-list format read by the VLC and Jellyfin
// integrations. The property that matters most: a shortcut must remove
// exactly the ranges an encoded copy would remove — if the two disagree, the
// same movie is censored differently depending on how it's watched.

#include "core/EditList.h"
#include "core/ExportPlan.h"
#include "core/Project.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace censorcut;

namespace {

Marker cut(qint64 startMs, qint64 endMs,
           const QString& category = QStringLiteral("Manual"),
           Status status = Status::Confirmed)
{
    Marker m;
    m.startMs  = startMs;
    m.endMs    = endMs;
    m.category = category;
    m.status   = status;
    return m;
}

Project projectWith(QList<Marker> markers, qint64 durationMs = 600000, int age = 7)
{
    Project p;
    p.sourceFile    = QStringLiteral("/films/Example.mkv");
    p.sourceHash    = QStringLiteral("deadbeef");
    p.durationMs    = durationMs;
    p.markers       = std::move(markers);
    p.activeProfile = AgeProfile::forAge(age);
    p.activeProfile.minAge = age;
    return p;
}

} // namespace

class TestEditList : public QObject {
    Q_OBJECT

private slots:
    void pathSitsBesideTheMovie()
    {
        QCOMPARE(EditList::pathFor(QStringLiteral("/films/Example.mkv")),
                 QStringLiteral("/films/Example.mkv.censorcut-edl.json"));
    }

    void cutsAreTheComplementOfKeepSegments()
    {
        const Project p = projectWith({ cut(10000, 20000), cut(50000, 60000) });

        const EditList list = EditList::fromProject(p);
        const ExportPlan plan = planExport(p);
        QVERIFY(plan.ok());

        // Walk the keep segments and the cuts together: concatenated they must
        // tile [0, duration) exactly, with no gap and no overlap.
        QList<QPair<qint64, qint64>> tiles;
        for (const auto& k : plan.keepSegments) tiles.append({k.startMs, k.endMs});
        for (const auto& c : list.profiles.first().cuts) tiles.append({c.startMs, c.endMs});
        std::sort(tiles.begin(), tiles.end());

        qint64 cursor = 0;
        for (const auto& t : tiles) {
            QCOMPARE(t.first, cursor);
            cursor = t.second;
        }
        QCOMPARE(cursor, p.durationMs);
    }

    void overlappingAndAdjacentMarkersMerge()
    {
        const Project p = projectWith({
            cut(10000, 20000),
            cut(15000, 25000),   // overlaps the previous
            cut(25000, 30000),   // exactly adjacent to the merged range
            cut(90000, 95000),
        });

        const EditList list = EditList::fromProject(p);
        const auto& cuts = list.profiles.first().cuts;
        QCOMPARE(cuts.size(), 2);
        QCOMPARE(cuts[0].startMs, 10000);
        QCOMPARE(cuts[0].endMs,   30000);
        QCOMPARE(cuts[1].startMs, 90000);
    }

    void pendingAndRejectedMarkersAreNotExported()
    {
        const Project p = projectWith({
            cut(10000, 20000, QStringLiteral("Violence"), Status::Pending),
            cut(30000, 40000, QStringLiteral("Violence"), Status::Rejected),
            cut(50000, 60000, QStringLiteral("Violence"), Status::Confirmed),
        });

        const auto& cuts = EditList::fromProject(p).profiles.first().cuts;
        QCOMPARE(cuts.size(), 1);
        QCOMPARE(cuts[0].startMs, 50000);
    }

    void categoryIsOnlyClaimedWhenUnambiguous()
    {
        const Project p = projectWith({
            cut(10000, 20000, QStringLiteral("Violence")),
            cut(50000, 60000, QStringLiteral("Language")),
            cut(55000, 65000, QStringLiteral("Violence")),  // merges across categories
        });

        const auto& cuts = EditList::fromProject(p).profiles.first().cuts;
        QCOMPARE(cuts.size(), 2);
        QCOMPARE(cuts[0].category, QStringLiteral("Violence"));
        QVERIFY2(cuts[1].category.isEmpty(),
                 "a range merged from two categories must not claim either");
    }

    void cutsTouchingTheEndsSurvive()
    {
        const Project p = projectWith({ cut(0, 5000), cut(595000, 600000) });
        const auto& cuts = EditList::fromProject(p).profiles.first().cuts;
        QCOMPARE(cuts.size(), 2);
        QCOMPARE(cuts[0].startMs, 0);
        QCOMPARE(cuts[1].endMs,   600000);
    }

    void roundTripsThroughJson()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("out.censorcut-edl.json"));

        const Project p = projectWith({ cut(10000, 20000, QStringLiteral("Violence")) });
        const EditList written = EditList::fromProject(p);
        QString err;
        QVERIFY2(written.saveTo(path, &err), qPrintable(err));

        const auto read = EditList::loadFrom(path, &err);
        QVERIFY2(read.has_value(), qPrintable(err));
        QCOMPARE(read->durationMs, written.durationMs);
        QCOMPARE(read->sourceHash, written.sourceHash);
        QCOMPARE(read->defaultProfileId, written.defaultProfileId);
        QCOMPARE(read->profiles.size(), 1);
        QCOMPARE(read->profiles[0].id, QStringLiteral("age-7"));
        QCOMPARE(read->profiles[0].leadInMs, kDefaultLeadInMs);
        QCOMPARE(read->profiles[0].cuts.size(), 1);
        QCOMPARE(read->profiles[0].cuts[0].startMs, 10000);
        QCOMPARE(read->profiles[0].cuts[0].category, QStringLiteral("Violence"));
    }

    void sourceFileNameIsStoredWithoutItsDirectory()
    {
        // The list travels with the movie; an absolute path would rot the
        // moment the library is moved or mounted elsewhere.
        const EditList list = EditList::fromProject(projectWith({ cut(1000, 2000) }));
        QCOMPARE(list.sourceFileName, QStringLiteral("Example.mkv"));
    }

    void secondAgeMergesInsteadOfClobbering()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString movie = dir.filePath(QStringLiteral("Example.mkv"));

        Project young = projectWith({ cut(10000, 20000), cut(50000, 60000) }, 600000, 5);
        young.sourceFile = movie;
        QString err;
        const QString path = EditList::writeShortcutFor(young, movie, &err);
        QVERIFY2(!path.isEmpty(), qPrintable(err));

        Project older = projectWith({ cut(10000, 20000) }, 600000, 11);
        older.sourceFile = movie;
        QVERIFY2(!EditList::writeShortcutFor(older, movie, &err).isEmpty(), qPrintable(err));

        const auto read = EditList::loadFrom(path, &err);
        QVERIFY2(read.has_value(), qPrintable(err));
        QCOMPARE(read->profiles.size(), 2);
        // Sorted by age, and the younger profile's two cuts are intact.
        QCOMPARE(read->profiles[0].id, QStringLiteral("age-5"));
        QCOMPARE(read->profiles[0].cuts.size(), 2);
        QCOMPARE(read->profiles[1].id, QStringLiteral("age-11"));
        QCOMPARE(read->profiles[1].cuts.size(), 1);
        // The most recently written profile is the one a player defaults to.
        QCOMPARE(read->defaultProfileId, QStringLiteral("age-11"));
    }

    void rewritingTheSameAgeReplacesIt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString movie = dir.filePath(QStringLiteral("Example.mkv"));

        Project p = projectWith({ cut(10000, 20000) }, 600000, 7);
        p.sourceFile = movie;
        QVERIFY(!EditList::writeShortcutFor(p, movie).isEmpty());

        p.markers = { cut(80000, 90000), cut(100000, 110000) };
        const QString path = EditList::writeShortcutFor(p, movie);
        QVERIFY(!path.isEmpty());

        const auto read = EditList::loadFrom(path);
        QVERIFY(read.has_value());
        QCOMPARE(read->profiles.size(), 1);
        QCOMPARE(read->profiles[0].cuts.size(), 2);
        QCOMPARE(read->profiles[0].cuts[0].startMs, 80000);
    }

    void aReplacedSourceDiscardsStaleProfiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString movie = dir.filePath(QStringLiteral("Example.mkv"));

        Project first = projectWith({ cut(10000, 20000) }, 600000, 5);
        first.sourceFile = movie;
        QVERIFY(!EditList::writeShortcutFor(first, movie).isEmpty());

        // Same path, different content: the old cuts describe different
        // footage and would land in the wrong places.
        Project swapped = projectWith({ cut(30000, 40000) }, 600000, 11);
        swapped.sourceFile = movie;
        swapped.sourceHash = QStringLiteral("cafebabe");
        const QString path = EditList::writeShortcutFor(swapped, movie);
        QVERIFY(!path.isEmpty());

        const auto read = EditList::loadFrom(path);
        QVERIFY(read.has_value());
        QCOMPARE(read->profiles.size(), 1);
        QCOMPARE(read->profiles[0].id, QStringLiteral("age-11"));
        QCOMPARE(read->sourceHash, QStringLiteral("cafebabe"));
    }

    void noConfirmedMarkersIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString movie = dir.filePath(QStringLiteral("Example.mkv"));

        Project p = projectWith({ cut(10000, 20000, QStringLiteral("V"), Status::Pending) });
        p.sourceFile = movie;
        QString err;
        QVERIFY(EditList::writeShortcutFor(p, movie, &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(!QFile::exists(EditList::pathFor(movie)));
    }

    void malformedFileLoadsAsFailureNotCrash()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("bad.censorcut-edl.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ this is not json");
        f.close();

        QString err;
        QVERIFY(!EditList::loadFrom(path, &err).has_value());
        QVERIFY(!err.isEmpty());
    }
};

QTEST_MAIN(TestEditList)
#include "test_edit_list.moc"
