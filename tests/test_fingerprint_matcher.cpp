#include "core/AnalysisResult.h"
#include "core/FingerprintMatcher.h"

#include <QtTest/QtTest>

using namespace censorcut;

namespace {

FingerprintAnchor anchor(qint64 tMs, const QString& sig)
{
    FingerprintAnchor a;
    a.tMs      = tMs;
    a.peakLufs = -10.0;
    a.sig      = sig;
    return a;
}
FingerprintAnchor anchor(qint64 tMs, const char* sig)
{
    return anchor(tMs, QString::fromLatin1(sig));
}

FilmFingerprint fp(qint64 durationMs,
                   std::initializer_list<FingerprintAnchor> anchors)
{
    FilmFingerprint f;
    f.durationMs = durationMs;
    for (const auto& a : anchors) f.anchors.append(a);
    return f;
}

/// Toggle one bit in a hex sig so we can simulate a small acoustic
/// drift (a different mastering pass of the same film).
QString flipOneBit(const QString& sig, int bitIndex)
{
    bool ok = false;
    quint64 v = sig.toULongLong(&ok, 16);
    Q_ASSERT(ok);
    v ^= (1ULL << (63 - bitIndex));
    return QStringLiteral("%1").arg(v, 16, 16, QLatin1Char('0'));
}

} // namespace

class TestFingerprintMatcher : public QObject {
    Q_OBJECT
private slots:
    void hammingDistanceBasics();
    void matchesIdenticalFingerprint();
    void matchesAcrossDifferentIntroLengths();
    void matchesAcrossPalNtscSpeedup();
    void rejectsCompletelyDifferentFilm();
    void rejectsPartialOverlap();
    void mapTimeAppliesAffine();
};

void TestFingerprintMatcher::hammingDistanceBasics()
{
    QCOMPARE(hexHammingDistance("0000000000000000", "0000000000000000"), 0);
    QCOMPARE(hexHammingDistance("0000000000000000", "0000000000000001"), 1);
    QCOMPARE(hexHammingDistance("ffffffffffffffff", "0000000000000000"), 64);
    // Bad inputs return sentinel 64.
    QCOMPARE(hexHammingDistance("not hex",          "0000000000000000"), 64);
    QCOMPARE(hexHammingDistance("0000000000000000", "tooshort"),         64);
}

void TestFingerprintMatcher::matchesIdenticalFingerprint()
{
    const auto a = fp(90 * 60'000, {
        anchor(10 * 60'000, "1111aaaa00001111"),
        anchor(20 * 60'000, "2222bbbb00002222"),
        anchor(70 * 60'000, "3333cccc00003333"),
        anchor(85 * 60'000, "4444dddd00004444"),
    });
    const auto v = matchFingerprints(a, a);
    QVERIFY(v.isSameFilm);
    QCOMPARE(v.matchedAnchors, 4);
    QVERIFY(v.alignment.has_value());
    QCOMPARE(v.alignment->offsetMs, qint64{0});
    QVERIFY(qFuzzyCompare(v.alignment->scale, 1.0));
}

void TestFingerprintMatcher::matchesAcrossDifferentIntroLengths()
{
    // File b is the same film with a 30 s longer intro: every anchor
    // shifts by +30000 ms, sigs identical apart from a 1-bit jitter.
    const auto a = fp(90 * 60'000, {
        anchor(10 * 60'000, "1111aaaa00001111"),
        anchor(20 * 60'000, "2222bbbb00002222"),
        anchor(70 * 60'000, "3333cccc00003333"),
        anchor(85 * 60'000, "4444dddd00004444"),
    });
    const auto b = fp(90 * 60'000 + 30'000, {
        anchor(10 * 60'000 + 30'000, flipOneBit("1111aaaa00001111", 5)),
        anchor(20 * 60'000 + 30'000, flipOneBit("2222bbbb00002222", 17)),
        anchor(70 * 60'000 + 30'000, flipOneBit("3333cccc00003333", 33)),
        anchor(85 * 60'000 + 30'000, flipOneBit("4444dddd00004444", 50)),
    });
    const auto v = matchFingerprints(a, b);
    QVERIFY(v.isSameFilm);
    QCOMPARE(v.matchedAnchors, 4);
    QVERIFY(v.alignment.has_value());
    // Mapping a remote time at 10:30 should land us at our 10:00 mark.
    const qint64 mapped = mapTime(*v.alignment, 10 * 60'000 + 30'000);
    QVERIFY(std::llabs(mapped - 10 * 60'000) < 100);
}

void TestFingerprintMatcher::matchesAcrossPalNtscSpeedup()
{
    // Same film but the remote copy runs ~4% faster (PAL speedup vs.
    // NTSC). Anchor times scale by 0.96; sigs unchanged.
    const auto a = fp(90 * 60'000, {
        anchor(10 * 60'000, "1111aaaa00001111"),
        anchor(20 * 60'000, "2222bbbb00002222"),
        anchor(70 * 60'000, "3333cccc00003333"),
        anchor(85 * 60'000, "4444dddd00004444"),
    });
    auto scaledB = a;
    for (auto& an : scaledB.anchors) an.tMs = qint64(an.tMs * 0.96);
    scaledB.durationMs = qint64(scaledB.durationMs * 0.96);

    const auto v = matchFingerprints(a, scaledB);
    QVERIFY(v.isSameFilm);
    QCOMPARE(v.matchedAnchors, 4);
    QVERIFY(v.alignment.has_value());
    // Scale should land in the PAL/NTSC band.
    QVERIFY(v.alignment->scale > 1.03 && v.alignment->scale < 1.05);
    // Mapping the remote 10:00 anchor (times 0.96) back gets us to 10:00.
    const qint64 mapped = mapTime(*v.alignment, qint64(10 * 60'000 * 0.96));
    QVERIFY(std::llabs(mapped - 10 * 60'000) < 800);
}

void TestFingerprintMatcher::rejectsCompletelyDifferentFilm()
{
    const auto a = fp(90 * 60'000, {
        anchor(10 * 60'000, "1111aaaa00001111"),
        anchor(20 * 60'000, "2222bbbb00002222"),
        anchor(70 * 60'000, "3333cccc00003333"),
        anchor(85 * 60'000, "4444dddd00004444"),
    });
    // All sigs unrelated.
    const auto b = fp(110 * 60'000, {
        anchor(8  * 60'000, "ffffffff77777777"),
        anchor(40 * 60'000, "ee0000ee5555aaaa"),
        anchor(80 * 60'000, "1234567890abcdef"),
        anchor(105 * 60'000, "abcdef0123456789"),
    });
    const auto v = matchFingerprints(a, b);
    QVERIFY(!v.isSameFilm);
    QVERIFY(v.matchedAnchors < 3);
    QVERIFY(!v.alignment.has_value());
}

void TestFingerprintMatcher::rejectsPartialOverlap()
{
    // Two anchors match (a single mastering tweak) but the other two
    // don't and the surviving pairs aren't enough for a fit.
    const auto a = fp(90 * 60'000, {
        anchor(10 * 60'000, "1111aaaa00001111"),
        anchor(20 * 60'000, "2222bbbb00002222"),
        anchor(70 * 60'000, "3333cccc00003333"),
        anchor(85 * 60'000, "4444dddd00004444"),
    });
    const auto b = fp(90 * 60'000, {
        anchor(10 * 60'000, "1111aaaa00001111"),  // matches
        anchor(20 * 60'000, "2222bbbb00002222"),  // matches
        anchor(70 * 60'000, "fedcba9876543210"),  // unrelated
        anchor(85 * 60'000, "0123456789abcdef"),  // unrelated
    });
    const auto v = matchFingerprints(a, b, /*maxHamming=*/8,
                                     /*minMatches=*/3);
    QVERIFY(!v.isSameFilm);
}

void TestFingerprintMatcher::mapTimeAppliesAffine()
{
    AffineTimeMap m{ /*scale=*/1.0, /*offsetMs=*/-2500 };
    QCOMPARE(mapTime(m, 10000), qint64{7500});
    AffineTimeMap m2{ 0.96, 1000 };
    QCOMPARE(mapTime(m2, 1000), qint64{1960});
}

QTEST_MAIN(TestFingerprintMatcher)
#include "test_fingerprint_matcher.moc"
