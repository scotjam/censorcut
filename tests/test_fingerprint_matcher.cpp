#include "core/AnalysisResult.h"
#include "core/FingerprintMatcher.h"

#include <QtTest/QtTest>

using namespace censorcut;

namespace {

FingerprintAnchor anchor(double tau, const QString& phash)
{
    FingerprintAnchor a;
    a.tau   = tau;
    a.phash = phash;
    return a;
}
FingerprintAnchor anchor(double tau, const char* phash)
{
    return anchor(tau, QString::fromLatin1(phash));
}

FilmFingerprint fp(qint64 durationMs,
                   const QString& digest,
                   std::initializer_list<FingerprintAnchor> anchors)
{
    FilmFingerprint f;
    f.durationMs = durationMs;
    f.digest     = digest;
    for (const auto& a : anchors) f.anchors.append(a);
    return f;
}

} // namespace

class TestFingerprintMatcher : public QObject {
    Q_OBJECT
private slots:

    // -------- hexHammingDistance --------

    void hammingZero()
    {
        QCOMPARE(hexHammingDistance(QStringLiteral("0123456789abcdef"),
                                    QStringLiteral("0123456789abcdef")), 0);
    }

    void hammingAllBits()
    {
        QCOMPARE(hexHammingDistance(QStringLiteral("ffffffffffffffff"),
                                    QStringLiteral("0000000000000000")), 64);
    }

    void hammingShapeFailure()
    {
        // Different length → max distance.
        QCOMPARE(hexHammingDistance(QStringLiteral("abc"),
                                    QStringLiteral("0000000000000000")), 64);
    }

    void hammingNonHex()
    {
        // Non-hex char → max distance.
        QCOMPARE(hexHammingDistance(QStringLiteral("zzzzzzzzzzzzzzzz"),
                                    QStringLiteral("0000000000000000")), 64);
    }

    // -------- bodyWindowForDuration --------

    void bodyWindowFullLengthFilm()
    {
        const auto bw = bodyWindowForDuration(90LL * 60 * 1000);
        QCOMPARE(bw.first,  10LL * 60 * 1000);
        QCOMPARE(bw.second, 80LL * 60 * 1000);
    }

    void bodyWindowShortFilm()
    {
        // 12 min / 3 = 4 min cushion.
        const auto bw = bodyWindowForDuration(12LL * 60 * 1000);
        QCOMPARE(bw.first,   4LL * 60 * 1000);
        QCOMPARE(bw.second,  8LL * 60 * 1000);
    }

    // -------- alignmentFromDurations --------

    void alignmentEqualDurations()
    {
        const auto m = alignmentFromDurations(90LL * 60 * 1000,
                                              90LL * 60 * 1000);
        QCOMPARE(m.scale, 1.0);
        QCOMPARE(m.offsetMs, qint64(0));
    }

    void alignmentPalSpeedup()
    {
        // 24p original at 90 min vs PAL-sped-up at ~86.4 min.
        // Both >= 30 min → cushions are 10 min on each side.
        // Local body span = 70 min. Remote body span = 66.4 min.
        // Mapping remote_t to local_t: scale = 70/66.4 ≈ 1.054
        // offset = 10*60_000 - 10*60_000 * 1.054
        //        = 10*60_000 * (1 - 1.054) ≈ -32530 ms
        const qint64 local24p = 90LL * 60 * 1000;        // 5,400,000
        const qint64 remotePAL = qint64(local24p * 24.0 / 25.0); // 5,184,000
        const auto m = alignmentFromDurations(local24p, remotePAL);
        const double expectedScale = double(70 * 60 * 1000) / double(remotePAL - 20*60*1000);
        QVERIFY(qAbs(m.scale - expectedScale) < 1e-6);
        // map remote midpoint (PAL: 43.2 min) onto local timeline.
        const qint64 mid_remote = remotePAL / 2;
        const qint64 mapped = mapTime(m, mid_remote);
        // Should land somewhere near the local midpoint (45 min).
        QVERIFY2(qAbs(mapped - local24p / 2) < 60'000,
                 qPrintable(QStringLiteral("expected ~45min, got %1").arg(mapped)));
    }

    // -------- matchAnchors --------

    void matchExactTauAndPhash()
    {
        FilmFingerprint a = fp(60'000, "digest-a", {
            anchor(0.10, "0000000000000000"),
            anchor(0.50, "ffffffffffffffff"),
            anchor(0.90, "1234567890abcdef"),
        });
        FilmFingerprint b = a;  // identical
        const auto m = matchAnchors(a, b);
        QCOMPARE(m.size(), 3);
    }

    void matchToleratesPhashJitter()
    {
        // pHash differs by a few bits — within the maxHamming default (8).
        FilmFingerprint a = fp(60'000, "digest-a", {
            anchor(0.10, "0000000000000000"),
        });
        FilmFingerprint b = fp(60'000, "digest-b", {
            anchor(0.10, "0000000000000003"),  // 2 bits different
        });
        const auto m = matchAnchors(a, b);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m.first().hammingDist, 2);
    }

    void matchRejectsDistantTau()
    {
        FilmFingerprint a = fp(60'000, "a", {
            anchor(0.10, "0000000000000000"),
        });
        FilmFingerprint b = fp(60'000, "b", {
            anchor(0.50, "0000000000000000"),  // tau too different
        });
        const auto m = matchAnchors(a, b);
        QCOMPARE(m.size(), 0);
    }

    // -------- matchFingerprints --------

    void matchByDigestEquality()
    {
        FilmFingerprint a = fp(60'000, "shared-digest", {
            anchor(0.10, "0000000000000000"),
            anchor(0.90, "ffffffffffffffff"),
        });
        FilmFingerprint b = fp(57'600, "shared-digest", { /* anchors empty
            on remote side — server may strip them; digest is enough */
            anchor(0.10, "0000000000000000"),
            anchor(0.90, "ffffffffffffffff"),
        });
        const auto v = matchFingerprints(a, b);
        QVERIFY(v.isSameFilm);
        QVERIFY(v.alignment.has_value());
    }

    void matchByAnchorsWhenDigestsDiffer()
    {
        // Build a 25-anchor fingerprint pair — different digests but
        // anchors agree on tau + pHash.
        FilmFingerprint a, b;
        a.durationMs = 60'000;
        b.durationMs = 60'000;
        a.digest = QStringLiteral("aaaa");
        b.digest = QStringLiteral("bbbb");
        for (int i = 0; i < 25; ++i) {
            const double tau = 0.02 + 0.04 * i;
            const QString hash = QString::fromLatin1("0000000000000000");
            a.anchors.append(anchor(tau, hash));
            b.anchors.append(anchor(tau, hash));
        }
        const auto v = matchFingerprints(a, b);
        QVERIFY(v.isSameFilm);
        QCOMPARE(v.matchedAnchors, 25);
    }

    void rejectsDifferentFilms()
    {
        // Different tau patterns, different digests.
        FilmFingerprint a = fp(60'000, "x", {
            anchor(0.10, "0000000000000000"),
            anchor(0.50, "ffffffffffffffff"),
            anchor(0.90, "1234567890abcdef"),
        });
        FilmFingerprint b = fp(60'000, "y", {
            anchor(0.20, "0000000000000000"),
            anchor(0.60, "ffffffffffffffff"),
            anchor(0.95, "1234567890abcdef"),
        });
        const auto v = matchFingerprints(a, b);
        QVERIFY(!v.isSameFilm);
    }
};

QTEST_APPLESS_MAIN(TestFingerprintMatcher)
#include "test_fingerprint_matcher.moc"
