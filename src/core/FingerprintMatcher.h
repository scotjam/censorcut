#pragma once

#include "AnalysisResult.h"

#include <QList>
#include <QString>

#include <optional>

namespace censorcut {

/// Pair of indices into two fingerprints' anchor lists.
struct AnchorMatch {
    int aIdx        = -1;
    int bIdx        = -1;
    int hammingDist = 64;
};

/// Affine map from a remote-file timeline to a local-file timeline:
///     local_t = scale * remote_t + offsetMs
struct AffineTimeMap {
    double scale    = 1.0;
    qint64 offsetMs = 0;
};

/// Hamming distance (in bits) between two 16-char hex pHash strings.
/// Returns 64 (max) on shape failure so unparseable pHashes can never
/// accidentally count as "close".
int hexHammingDistance(const QString& a, const QString& b);

/// Greedy tau-based anchor matching: walk both lists (sorted by tau)
/// and pair anchors whose tau values agree within `maxTauDelta` and
/// whose pHash Hamming distance is within `maxHamming`.
QList<AnchorMatch> matchAnchors(const FilmFingerprint& a,
                                const FilmFingerprint& b,
                                double maxTauDelta = 0.005,
                                int    maxHamming  = 8);

/// The body window used by the analyzer (Python side mirror): the
/// adaptive cushion is `min(10 min, durationMs / 3)`. Returns
/// (body_start_ms, body_end_ms) for a given duration.
QPair<qint64, qint64> bodyWindowForDuration(qint64 durationMs);

/// Compute the affine time map between two fingerprints purely from
/// their durations, given that both used the same body-window formula.
/// This is exact — tau is scale-invariant by construction, so two
/// copies of the same film with different durations have anchors at
/// the same tau values that map to different physical times.
///
/// Concretely:
///     local_t  = local_body_start  + tau * local_body_span
///     remote_t = remote_body_start + tau * remote_body_span
/// solve for the affine map remote_t -> local_t:
///     scale  = local_body_span / remote_body_span
///     offset = local_body_start - remote_body_start * scale
AffineTimeMap alignmentFromDurations(qint64 localDurMs, qint64 remoteDurMs);

/// Apply an affine map.
inline qint64 mapTime(const AffineTimeMap& m, qint64 remoteTime) {
    return static_cast<qint64>(m.scale * static_cast<double>(remoteTime)) + m.offsetMs;
}

/// One-shot helper: given two fingerprints, decide whether they match
/// and, if so, return the alignment from `b` (remote) into `a` (local).
///
/// Decision logic:
///   1. If both digests are non-empty and equal → same film. Alignment
///      is computed from the duration pair (handles PAL/NTSC where
///      digests match but absolute times differ).
///   2. If digests differ but anchors line up: pair anchors by tau
///      proximity + pHash Hamming distance, count matches. If matches
///      >= matchThreshold * min(local.size, remote.size) → same film.
struct MatchVerdict {
    bool                          isSameFilm     = false;
    int                           matchedAnchors = 0;
    int                           totalAnchors   = 0;
    std::optional<AffineTimeMap>  alignment;
};
MatchVerdict matchFingerprints(const FilmFingerprint& localA,
                               const FilmFingerprint& remoteB,
                               double matchThreshold = 0.4);

} // namespace censorcut
