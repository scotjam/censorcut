#include "FingerprintMatcher.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace censorcut {

namespace {

/// Popcount on a 64-bit integer using the textbook divide-and-conquer
/// approach. We don't depend on a builtin so this stays portable across
/// MSVC and GCC without an extra include.
int popcount64(quint64 x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return int((x * 0x0101010101010101ULL) >> 56);
}

bool tryParseHex64(const QString& s, quint64& out)
{
    if (s.size() != 16) return false;
    bool ok = false;
    out = s.toULongLong(&ok, 16);
    return ok;
}

} // namespace

int hexHammingDistance(const QString& sigA, const QString& sigB)
{
    quint64 a = 0, b = 0;
    if (!tryParseHex64(sigA, a)) return 64;
    if (!tryParseHex64(sigB, b)) return 64;
    return popcount64(a ^ b);
}

QList<AnchorMatch> matchAnchors(const FilmFingerprint& a,
                                const FilmFingerprint& b,
                                int maxHamming)
{
    QList<AnchorMatch> out;
    if (a.anchors.isEmpty() || b.anchors.isEmpty()) return out;

    // Build all (aIdx, bIdx, dist) triples within maxHamming, then
    // greedily pick the closest pairs, never reusing an index on either
    // side. This is fine for our 4×4 problem; for larger sets we'd want
    // the Hungarian algorithm, but 16 candidates is trivial.
    QList<AnchorMatch> candidates;
    candidates.reserve(a.anchors.size() * b.anchors.size());
    for (int i = 0; i < a.anchors.size(); ++i) {
        for (int j = 0; j < b.anchors.size(); ++j) {
            const int d = hexHammingDistance(a.anchors.at(i).sig,
                                              b.anchors.at(j).sig);
            if (d <= maxHamming) {
                candidates.append({i, j, d});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const AnchorMatch& x, const AnchorMatch& y) {
                  return x.hammingDist < y.hammingDist;
              });
    QSet<int> usedA, usedB;
    for (const auto& m : candidates) {
        if (usedA.contains(m.aIdx) || usedB.contains(m.bIdx)) continue;
        out.append(m);
        usedA.insert(m.aIdx);
        usedB.insert(m.bIdx);
    }
    return out;
}

std::optional<AffineTimeMap> fitAffine(const QList<AnchorMatch>& matches,
                                       const FilmFingerprint& a,
                                       const FilmFingerprint& b,
                                       int     minMatches,
                                       qint64  toleranceMs)
{
    if (matches.size() < minMatches) return std::nullopt;

    // Closed-form linear least-squares for t_local = scale*t_remote + b.
    const int n = matches.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& m : matches) {
        const double x = double(b.anchors.at(m.bIdx).tMs);
        const double y = double(a.anchors.at(m.aIdx).tMs);
        sx  += x;     sy  += y;
        sxx += x * x; sxy += x * y;
    }
    const double meanX = sx / n;
    const double meanY = sy / n;
    const double denom = sxx - sx * meanX;

    AffineTimeMap fit;
    if (std::abs(denom) < 1e-3) {
        // All matched anchors at (effectively) the same remote time.
        // Fall back to scale=1, offset = meanY - meanX so the median pair
        // lines up.
        fit.scale    = 1.0;
        fit.offsetMs = static_cast<qint64>(meanY - meanX);
    } else {
        fit.scale    = (sxy - sx * meanY) / denom;
        fit.offsetMs = static_cast<qint64>(meanY - fit.scale * meanX);
    }

    // Sanity: a sane fit has scale within a few percent of 1. (PAL/NTSC
    // would be 24/25 = 0.96 or 25/24 = 1.04, so we allow [0.9, 1.1].)
    if (fit.scale < 0.9 || fit.scale > 1.1) return std::nullopt;

    // Verify each matched pair lands within tolerance.
    int agree = 0;
    for (const auto& m : matches) {
        const qint64 predicted = mapTime(fit, b.anchors.at(m.bIdx).tMs);
        const qint64 actual    = a.anchors.at(m.aIdx).tMs;
        if (std::llabs(predicted - actual) <= toleranceMs) ++agree;
    }
    if (agree < minMatches) return std::nullopt;
    return fit;
}

MatchVerdict matchFingerprints(const FilmFingerprint& a,
                               const FilmFingerprint& b,
                               int maxHamming, int minMatches,
                               qint64 toleranceMs)
{
    MatchVerdict v;
    v.totalAnchors = std::min(a.anchors.size(), b.anchors.size());
    if (!a.isValid() || !b.isValid()) return v;

    const auto pairs = matchAnchors(a, b, maxHamming);
    v.matchedAnchors = pairs.size();
    if (pairs.size() < minMatches) return v;

    auto fit = fitAffine(pairs, a, b, minMatches, toleranceMs);
    if (!fit) return v;

    v.isSameFilm = true;
    v.alignment  = fit;
    return v;
}

} // namespace censorcut
