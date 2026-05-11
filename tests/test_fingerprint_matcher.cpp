#include "core/AnalysisResult.h"
#include "core/FingerprintMatcher.h"

#include <QtTest/QtTest>

#include <algorithm>

using namespace censorcut;

namespace {

FilmFingerprint kfFingerprint(qint64 durationMs,
                                std::initializer_list<qint64> times)
{
    FilmFingerprint f;
    f.version           = 1;
    f.type              = QStringLiteral("keyframes");
    f.durationMs        = durationMs;
    f.approxDurationMin = int(durationMs / 60'000);
    for (qint64 t : times) f.keyframeTimesMs.append(t);
    std::sort(f.keyframeTimesMs.begin(), f.keyframeTimesMs.end());
    return f;
}

FilmFingerprint v9Fingerprint(qint64 durationMs,
                                std::initializer_list<qint64> peakTimes,
                                const QString& phash)
{
    FilmFingerprint f;
    f.version           = 1;
    f.type              = QStringLiteral("audio_peak_gaps");
    f.durationMs        = durationMs;
    f.approxDurationMin = int(durationMs / 60'000);
    for (qint64 t : peakTimes) f.peaks.append({t, phash});
    return f;
}

/// Synthesise a non-uniformly-spaced keyframe sequence so the matcher
/// has real timing signal to align on. Returns ~150 timestamps that
/// follow a deterministic-but-irregular cadence — irregular enough that
/// two different sequences won't trivially align.
QList<qint64> makeIrregularSequence(qint64 startMs,
                                       int count,
                                       int seed)
{
    QList<qint64> out;
    qint64 t = startMs;
    // Linear-congruential pseudo-random, fully deterministic per seed.
    quint32 s = quint32(seed);
    for (int i = 0; i < count; ++i) {
        out.append(t);
        s = s * 1103515245u + 12345u;
        const int delta = 1000 + int((s >> 16) % 8000);  // 1..9 sec
        t += delta;
    }
    return out;
}

} // namespace


class TestFingerprintMatcher : public QObject {
    Q_OBJECT
private slots:

    // --------------------------------------------------------------
    // hexHammingDistance
    // --------------------------------------------------------------

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
        QCOMPARE(hexHammingDistance(QStringLiteral("abc"),
                                    QStringLiteral("0000000000000000")), 64);
    }

    void hammingNonHex()
    {
        QCOMPARE(hexHammingDistance(QStringLiteral("zzzzzzzzzzzzzzzz"),
                                    QStringLiteral("0000000000000000")), 64);
    }

    // --------------------------------------------------------------
    // F path
    // --------------------------------------------------------------

    void kf_identicalSequencesMatch()
    {
        const auto times = makeIrregularSequence(0, 150, 42);
        FilmFingerprint a, b;
        a.version = b.version = 1;
        a.type    = b.type    = QStringLiteral("keyframes");
        a.durationMs = b.durationMs = 600'000;
        a.keyframeTimesMs = b.keyframeTimesMs = times;
        const auto v = matchFingerprints(a, b);
        QVERIFY2(v.isSameFilm, qPrintable(v.reason));
        QVERIFY(v.matchFraction > 0.9);
        QCOMPARE(int(v.madMs), 0);
        QCOMPARE(v.estimatedTrimMs, qint64(0));
    }

    void kf_introTrimMatches()
    {
        // a is the original; b has 22 sec sliced off the front.
        const auto orig = makeIrregularSequence(0, 150, 42);
        QList<qint64> trimmed;
        const qint64 trimMs = 22'000;
        for (qint64 t : orig) {
            if (t >= trimMs) trimmed.append(t - trimMs);
        }
        FilmFingerprint a, b;
        a.version = b.version = 1;
        a.type    = b.type    = QStringLiteral("keyframes");
        a.durationMs = 600'000;
        b.durationMs = 600'000 - trimMs;
        a.keyframeTimesMs = orig;
        b.keyframeTimesMs = trimmed;
        // matchFingerprints(localA, remoteB): trim = (b - a). b's
        // timeline is shifted EARLIER by 22 sec relative to a's, so
        // for any aligned pair the remote_t - local_t = -22000 → estimatedTrim
        // applied as local = remote + trim should map b's 0 onto a's 22000.
        const auto v = matchFingerprints(a, b);
        QVERIFY2(v.isSameFilm, qPrintable(v.reason));
        QCOMPARE(v.estimatedTrimMs, qint64(22'000));
        QVERIFY(v.madMs < fp_match_f::kMaxMadMs);
    }

    void kf_differentSequencesDiffer()
    {
        FilmFingerprint a, b;
        a.version = b.version = 1;
        a.type    = b.type    = QStringLiteral("keyframes");
        a.durationMs = b.durationMs = 600'000;
        a.keyframeTimesMs = makeIrregularSequence(0, 150, 1234);
        b.keyframeTimesMs = makeIrregularSequence(0, 150, 5678);
        const auto v = matchFingerprints(a, b);
        QVERIFY2(!v.isSameFilm, qPrintable(v.reason));
    }

    void kf_tooFewKeyframesRejects()
    {
        FilmFingerprint a, b;
        a.type = b.type = QStringLiteral("keyframes");
        a.keyframeTimesMs = {0, 1000, 2000};
        b.keyframeTimesMs = {0, 1000, 2000};
        const auto v = matchFingerprints(a, b);
        QVERIFY(!v.isSameFilm);
        QVERIFY(v.reason.contains(QStringLiteral("insufficient")));
    }

    // --------------------------------------------------------------
    // v9 path
    // --------------------------------------------------------------

    void v9_samePeaksSameHashMatch()
    {
        const QString h = QStringLiteral("00000000ffffffff");
        FilmFingerprint a = v9Fingerprint(
            600'000,
            {30'000, 60'000, 95'000, 140'000, 180'000, 230'000, 280'000},
            h);
        FilmFingerprint b = a;
        const auto v = matchFingerprints(a, b);
        QVERIFY2(v.isSameFilm, qPrintable(v.reason));
        QCOMPARE(v.estimatedTrimMs, qint64(0));
    }

    void v9_introTrimMatches()
    {
        const QString h = QStringLiteral("00000000ffffffff");
        FilmFingerprint a = v9Fingerprint(
            600'000,
            {30'000, 60'000, 95'000, 140'000, 180'000, 230'000, 280'000},
            h);
        FilmFingerprint b = v9Fingerprint(
            578'000,
            {8'000, 38'000, 73'000, 118'000, 158'000, 208'000, 258'000},
            h);
        const auto v = matchFingerprints(a, b);
        QVERIFY2(v.isSameFilm, qPrintable(v.reason));
        // estimatedTrimMs = local - remote = a - b. b's peaks are
        // 22 sec earlier than a's, so the offset is +22 sec.
        QCOMPARE(v.estimatedTrimMs, qint64(22'000));
    }

    void v9_differentPhashRejects()
    {
        FilmFingerprint a = v9Fingerprint(
            600'000,
            {30'000, 60'000, 95'000, 140'000, 180'000, 230'000, 280'000},
            QStringLiteral("00000000ffffffff"));
        FilmFingerprint b = a;
        // Make every pHash totally different (max Hamming distance).
        for (auto& p : b.peaks)
            p.phash = QStringLiteral("ffffffff00000000");
        const auto v = matchFingerprints(a, b);
        QVERIFY(!v.isSameFilm);
    }

    // --------------------------------------------------------------
    // Cross-type dispatch
    // --------------------------------------------------------------

    void crossType_returnsIncompatible()
    {
        FilmFingerprint kf = kfFingerprint(60'000, {0, 1000, 2000, 3000});
        FilmFingerprint v9 = v9Fingerprint(60'000, {0, 30'000},
                                              QStringLiteral("0000000000000000"));
        const auto v = matchFingerprints(kf, v9);
        QVERIFY(!v.isSameFilm);
        QVERIFY(v.reason.contains(QStringLiteral("incompatible")));
    }

    void unknownType_doesNotMatch()
    {
        FilmFingerprint a;
        a.type = QStringLiteral("unknown");
        FilmFingerprint b = a;
        const auto v = matchFingerprints(a, b);
        QVERIFY(!v.isSameFilm);
    }
};

QTEST_APPLESS_MAIN(TestFingerprintMatcher)
#include "test_fingerprint_matcher.moc"
