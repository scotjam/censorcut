#include "FingerprintMatcher.h"

#include <QStringLiteral>

#include <algorithm>
#include <cmath>

namespace censorcut {

namespace {

// ---------------------------------------------------------------------
// Hex Hamming distance — used by the v9 path's pHash agreement check.
// ---------------------------------------------------------------------

int hexNibble(QChar c)
{
    if (c >= QLatin1Char('0') && c <= QLatin1Char('9')) return int(c.unicode()) - '0';
    if (c >= QLatin1Char('a') && c <= QLatin1Char('f')) return int(c.unicode()) - 'a' + 10;
    if (c >= QLatin1Char('A') && c <= QLatin1Char('F')) return int(c.unicode()) - 'A' + 10;
    return -1;
}

int popcount(int x)
{
    int n = 0;
    while (x) { n += (x & 1); x >>= 1; }
    return n;
}

// ---------------------------------------------------------------------
// F path: subset alignment with cursor advance + MAD discriminator
// ---------------------------------------------------------------------

struct PairedDiff { qint64 shortT; qint64 longT; };

/// Order-preserving 1-to-1 subset matcher. Walks `longTimes` once with a
/// cursor; for each `shortTimes[i]` advances past entries < target-tol,
/// then accepts the next entry within +-tol_ms (consuming it) or skips.
QList<PairedDiff> orderedPairs(const QList<qint64>& shortTimes,
                                 const QList<qint64>& longTimes,
                                 qint64 offsetMs, qint64 tolMs)
{
    QList<PairedDiff> pairs;
    int j = 0;
    const int nLong = longTimes.size();
    for (qint64 t : shortTimes) {
        const qint64 target = t + offsetMs;
        while (j < nLong && longTimes[j] < target - tolMs) ++j;
        if (j >= nLong) break;
        if (std::abs(longTimes[j] - target) <= tolMs) {
            pairs.append({t, longTimes[j]});
            ++j;
        }
    }
    return pairs;
}

/// Try a handful of plausible offsets (aligning short[0] / short[-1]
/// against each of the first / last probeK entries in long), keep the
/// one that pairs the most short entries. Returns the best matched
/// pair list.
QList<PairedDiff> bestOffsetPairs(const QList<qint64>& shortTimes,
                                    const QList<qint64>& longTimes,
                                    qint64 tolMs,
                                    int probeK = 50,
                                    qint64 probeRangeMs = 10LL * 60 * 1000)
{
    QList<PairedDiff> best;
    if (shortTimes.isEmpty() || longTimes.isEmpty()) return best;

    QList<qint64> candidates;
    const qint64 s0 = shortTimes.first();
    const qint64 sLast = shortTimes.last();
    const int nLong = static_cast<int>(longTimes.size());
    const int limitFront = std::min(probeK, nLong);
    for (int k = 0; k < limitFront; ++k) {
        const qint64 cand = longTimes[k] - s0;
        if (std::abs(cand) <= probeRangeMs && !candidates.contains(cand)) {
            candidates.append(cand);
        }
    }
    const int limitBack = std::min(probeK, nLong);
    for (int k = 0; k < limitBack; ++k) {
        const int idx = nLong - 1 - k;
        if (idx < 0) break;
        const qint64 cand = longTimes[idx] - sLast;
        if (std::abs(cand) <= probeRangeMs && !candidates.contains(cand)) {
            candidates.append(cand);
        }
    }
    if (candidates.isEmpty()) return best;
    for (qint64 cand : candidates) {
        const auto pairs = orderedPairs(shortTimes, longTimes, cand, tolMs);
        if (pairs.size() > best.size()) best = pairs;
    }
    return best;
}

double medianOf(QList<double> xs)
{
    if (xs.isEmpty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    const int n = xs.size();
    if (n & 1) return xs[n / 2];
    return 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

double madMs(const QList<qint64>& diffs)
{
    if (diffs.isEmpty()) return 0.0;
    QList<double> as_double;
    as_double.reserve(diffs.size());
    for (qint64 d : diffs) as_double.append(double(d));
    const double med = medianOf(as_double);
    QList<double> abs_dev;
    abs_dev.reserve(diffs.size());
    for (double d : as_double) abs_dev.append(std::abs(d - med));
    return medianOf(abs_dev);
}

MatchVerdict matchF(const FilmFingerprint& a, const FilmFingerprint& b)
{
    MatchVerdict v;
    QList<qint64> timesA = a.keyframeTimesMs;
    QList<qint64> timesB = b.keyframeTimesMs;
    std::sort(timesA.begin(), timesA.end());
    std::sort(timesB.begin(), timesB.end());

    if (timesA.size() < fp_match_f::kMinKeyframes
        || timesB.size() < fp_match_f::kMinKeyframes) {
        v.totalShorter = std::min(timesA.size(), timesB.size());
        v.reason = QStringLiteral("insufficient keyframes (a=%1, b=%2)")
                       .arg(timesA.size()).arg(timesB.size());
        return v;
    }

    const bool aIsShort = (timesA.size() <= timesB.size());
    const QList<qint64>& shortT = aIsShort ? timesA : timesB;
    const QList<qint64>& longT  = aIsShort ? timesB : timesA;

    const auto pairs = bestOffsetPairs(shortT, longT,
                                          fp_match_f::kGapToleranceMs);
    v.matched      = pairs.size();
    v.totalShorter = shortT.size();
    v.matchFraction = double(v.matched) / std::max(1, v.totalShorter);

    QList<qint64> diffs;
    diffs.reserve(pairs.size());
    for (const auto& p : pairs) diffs.append(p.longT - p.shortT);
    v.madMs = madMs(diffs);

    // estimatedTrimMs is the offset to ADD to remote (= b) times to
    // land in local (= a) times:  local_t = remote_t + estimatedTrimMs.
    // diffs[i] = pairs[i].longT - pairs[i].shortT. When aIsShort=true,
    // shortT comes from a (local) and longT from b (remote), so
    // diffs[i] = b_t - a_t = remote - local — we negate to express as
    // local - remote. When aIsShort=false, shortT comes from b and
    // longT from a, so diffs[i] = a_t - b_t = local - remote already.
    qint64 medianDiff = 0;
    if (!diffs.isEmpty()) {
        QList<qint64> sorted = diffs;
        std::sort(sorted.begin(), sorted.end());
        medianDiff = sorted[sorted.size() / 2];
    }
    v.estimatedTrimMs = aIsShort ? -medianDiff : medianDiff;

    v.isSameFilm = (v.matchFraction >= fp_match_f::kMinMatchFraction)
                    && (v.madMs <= fp_match_f::kMaxMadMs);
    v.reason = QStringLiteral("timing %1/%2 (%3%), MAD=%4 ms, trim~=%5 ms")
                   .arg(v.matched).arg(v.totalShorter)
                   .arg(int(100.0 * v.matchFraction))
                   .arg(int(v.madMs))
                   .arg(v.estimatedTrimMs);
    return v;
}

// ---------------------------------------------------------------------
// v9 path: 1-to-1 paired gap comparison + pHash agreement
// ---------------------------------------------------------------------

MatchVerdict matchV9(const FilmFingerprint& a, const FilmFingerprint& b)
{
    MatchVerdict v;
    if (a.peaks.size() < fp_match_v9::kMinPeaks
        || b.peaks.size() < fp_match_v9::kMinPeaks) {
        v.reason = QStringLiteral("insufficient peaks (a=%1, b=%2)")
                       .arg(a.peaks.size()).arg(b.peaks.size());
        return v;
    }
    if (std::abs(a.peaks.size() - b.peaks.size())
        > fp_match_v9::kPeakCountTolerance) {
        v.reason = QStringLiteral("peak counts too different (a=%1, b=%2)")
                       .arg(a.peaks.size()).arg(b.peaks.size());
        return v;
    }

    const int n = static_cast<int>(std::min(a.peaks.size(), b.peaks.size()));
    // estimatedTrimMs is the offset to add to remote (b) times to land
    // in local (a) times — i.e., local - remote. Per-pair we compute
    // a - b (= local - remote) and take the median across paired peaks.
    QList<qint64> peakDiffs;
    peakDiffs.reserve(n);
    for (int i = 0; i < n; ++i) {
        peakDiffs.append(a.peaks[i].tMs - b.peaks[i].tMs);
    }
    std::sort(peakDiffs.begin(), peakDiffs.end());
    v.estimatedTrimMs = peakDiffs[n / 2];

    // pHash agreement.
    int matchedPh = 0, totalPh = 0;
    for (int i = 0; i < n; ++i) {
        const QString& ha = a.peaks[i].phash;
        const QString& hb = b.peaks[i].phash;
        if (ha.isEmpty() || hb.isEmpty()) continue;
        ++totalPh;
        if (hexHammingDistance(ha, hb) <= fp_match_v9::kPhashHammingMax)
            ++matchedPh;
    }
    v.phashMatched  = matchedPh;
    v.phashCompared = totalPh;

    // Gap agreement: |gap_a[i] - gap_b[i]| <= tol for the first n-1 entries.
    int matchedGaps = 0;
    const int gapN = n - 1;
    for (int i = 0; i < gapN; ++i) {
        const qint64 ga = a.peaks[i + 1].tMs - a.peaks[i].tMs;
        const qint64 gb = b.peaks[i + 1].tMs - b.peaks[i].tMs;
        if (std::abs(ga - gb) <= fp_match_v9::kGapToleranceMs)
            ++matchedGaps;
    }
    v.matched      = matchedGaps;
    v.totalShorter = gapN;
    v.matchFraction = (gapN > 0) ? (double(matchedGaps) / gapN) : 0.0;
    const double phashFrac = (totalPh > 0) ? (double(matchedPh) / totalPh) : 0.0;
    v.isSameFilm = (v.matchFraction >= fp_match_v9::kMinGapFraction)
                    && (phashFrac >= fp_match_v9::kMinPhashFraction);
    v.reason = QStringLiteral(
                   "gaps %1/%2 (%3%), pHash %4/%5 (%6%), trim~=%7 ms")
                   .arg(matchedGaps).arg(gapN)
                   .arg(int(100.0 * v.matchFraction))
                   .arg(matchedPh).arg(totalPh)
                   .arg(int(100.0 * phashFrac))
                   .arg(v.estimatedTrimMs);
    return v;
}

} // namespace

int hexHammingDistance(const QString& a, const QString& b)
{
    if (a.size() != 16 || b.size() != 16) return 64;
    int total = 0;
    for (int i = 0; i < 16; ++i) {
        const int na = hexNibble(a.at(i));
        const int nb = hexNibble(b.at(i));
        if (na < 0 || nb < 0) return 64;
        total += popcount(na ^ nb);
    }
    return total;
}

MatchVerdict matchFingerprints(const FilmFingerprint& a,
                                const FilmFingerprint& b)
{
    using namespace fp_type;
    if (a.type == QLatin1String(Keyframes) && b.type == QLatin1String(Keyframes))
        return matchF(a, b);
    if (a.type == QLatin1String(AudioPeakGaps)
        && b.type == QLatin1String(AudioPeakGaps))
        return matchV9(a, b);
    MatchVerdict v;
    v.reason = QStringLiteral(
                   "incompatible fingerprint types: '%1' vs '%2'")
                   .arg(a.type).arg(b.type);
    return v;
}

} // namespace censorcut
