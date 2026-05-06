#pragma once

#include "AnalysisResult.h"

#include <QList>
#include <QString>

#include <optional>

namespace censorcut {

/// Pair of indices: a.anchors[aIdx] matches b.anchors[bIdx].
struct AnchorMatch {
    int  aIdx        = -1;
    int  bIdx        = -1;
    int  hammingDist = 64;
};

/// Affine map from a remote-file timeline to a local-file timeline:
///     local_t = scale * remote_t + offsetMs
struct AffineTimeMap {
    double scale     = 1.0;
    qint64 offsetMs  = 0;
};

/// Hamming distance (in bits) between two 16-char hex signatures. Both
/// must be exactly 16 hex characters; returns 64 (max) on shape failure
/// so unparseable signatures can never accidentally count as "close".
int hexHammingDistance(const QString& sigA, const QString& sigB);

/// Pair anchors of `a` with anchors of `b` whose signatures are within
/// `maxHamming` bits. Each anchor on either side is matched at most once
/// (greedy by ascending distance).
QList<AnchorMatch> matchAnchors(const FilmFingerprint& a,
                                const FilmFingerprint& b,
                                int maxHamming = 8);

/// Given a list of anchor matches, fit an affine `t_local = scale *
/// t_remote + offset` and return it if at least `minMatches` of those
/// pairs agree with the fit within `tolerance_ms`. Returns nullopt
/// otherwise — i.e. anchors look superficially similar but the timing
/// pattern doesn't make sense as the same film.
std::optional<AffineTimeMap> fitAffine(const QList<AnchorMatch>& matches,
                                       const FilmFingerprint& a,
                                       const FilmFingerprint& b,
                                       int     minMatches  = 3,
                                       qint64  toleranceMs = 800);

/// Apply an affine map.
inline qint64 mapTime(const AffineTimeMap& m, qint64 remoteTime) {
    return static_cast<qint64>(m.scale * static_cast<double>(remoteTime)) + m.offsetMs;
}

/// One-shot helper: given two fingerprints, decide whether they match
/// and, if so, return the alignment from `b` (remote) into `a` (local).
struct MatchVerdict {
    bool                          isSameFilm = false;
    int                           matchedAnchors = 0;
    int                           totalAnchors   = 0;
    std::optional<AffineTimeMap>  alignment;
};
MatchVerdict matchFingerprints(const FilmFingerprint& localA,
                               const FilmFingerprint& remoteB,
                               int maxHamming  = 8,
                               int minMatches  = 3,
                               qint64 toleranceMs = 800);

} // namespace censorcut
