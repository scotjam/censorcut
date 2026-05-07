#include "FingerprintMatcher.h"

#include <QPair>

#include <algorithm>
#include <cmath>

namespace censorcut {

namespace {

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

QList<AnchorMatch> matchAnchors(const FilmFingerprint& a,
                                const FilmFingerprint& b,
                                double maxTauDelta,
                                int    maxHamming)
{
    QList<AnchorMatch> matches;
    if (a.anchors.isEmpty() || b.anchors.isEmpty()) return matches;

    QList<bool> bUsed(b.anchors.size(), false);
    int j = 0;
    for (int i = 0; i < a.anchors.size(); ++i) {
        const double tauA = a.anchors.at(i).tau;
        while (j < b.anchors.size() && b.anchors.at(j).tau < tauA - maxTauDelta) {
            ++j;
        }
        int bestIdx = -1;
        int bestHd  = maxHamming + 1;
        for (int k = j; k < b.anchors.size(); ++k) {
            const double tauB = b.anchors.at(k).tau;
            if (tauB > tauA + maxTauDelta) break;
            if (bUsed[k]) continue;
            const int hd = hexHammingDistance(a.anchors.at(i).phash,
                                              b.anchors.at(k).phash);
            if (hd <= maxHamming && hd < bestHd) {
                bestHd  = hd;
                bestIdx = k;
            }
        }
        if (bestIdx >= 0) {
            bUsed[bestIdx] = true;
            matches.append(AnchorMatch{ i, bestIdx, bestHd });
        }
    }
    return matches;
}

QPair<qint64, qint64> bodyWindowForDuration(qint64 durationMs)
{
    if (durationMs <= 0) return { 0, 0 };
    const qint64 cushion = std::min<qint64>(10LL * 60 * 1000, durationMs / 3);
    const qint64 lo = cushion;
    const qint64 hi = std::max<qint64>(lo + 1, durationMs - cushion);
    return { lo, hi };
}

AffineTimeMap alignmentFromDurations(qint64 localDurMs, qint64 remoteDurMs)
{
    const auto local  = bodyWindowForDuration(localDurMs);
    const auto remote = bodyWindowForDuration(remoteDurMs);
    const double aSpan = double(local.second  - local.first);
    const double bSpan = double(remote.second - remote.first);
    AffineTimeMap m;
    if (bSpan <= 0.0 || aSpan <= 0.0) {
        m.scale    = 1.0;
        m.offsetMs = 0;
        return m;
    }
    m.scale    = aSpan / bSpan;
    m.offsetMs = qint64(double(local.first) - double(remote.first) * m.scale);
    return m;
}

MatchVerdict matchFingerprints(const FilmFingerprint& localA,
                               const FilmFingerprint& remoteB,
                               double matchThreshold)
{
    MatchVerdict v;
    v.totalAnchors = std::min(localA.anchors.size(), remoteB.anchors.size());
    if (!localA.isValid() || !remoteB.isValid()) return v;

    if (!localA.digest.isEmpty() && localA.digest == remoteB.digest) {
        v.isSameFilm     = true;
        v.matchedAnchors = v.totalAnchors;
        v.alignment      = alignmentFromDurations(localA.durationMs, remoteB.durationMs);
        return v;
    }

    const auto matches = matchAnchors(localA, remoteB);
    v.matchedAnchors = matches.size();
    const int needed = std::max(20, int(matchThreshold * double(v.totalAnchors)));
    if (matches.size() < needed) return v;

    v.isSameFilm = true;
    v.alignment  = alignmentFromDurations(localA.durationMs, remoteB.durationMs);
    return v;
}

} // namespace censorcut
