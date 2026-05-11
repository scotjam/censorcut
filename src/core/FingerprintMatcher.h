#pragma once

#include "AnalysisResult.h"

#include <QList>
#include <QString>

namespace censorcut {

/// Hamming distance (in bits) between two 16-char hex pHash strings.
/// Returns 64 (max) on shape failure so unparseable pHashes can never
/// accidentally count as "close". Reused by the v9 peak-gap matcher.
int hexHammingDistance(const QString& a, const QString& b);

/// Outcome of comparing two fingerprints. Fields populated depend on
/// the fingerprint type — keyframes uses madMs; audio_peak_gaps uses
/// phashMatched/Compared. estimatedTrimMs is defined as
///     local_t - remote_t
/// across paired timestamps, so the consumer maps a remote (pack)
/// time to the local (editor) timeline via
///     local_t = remote_t + estimatedTrimMs
struct MatchVerdict {
    bool    isSameFilm    = false;
    int     matched       = 0;       // matched short-side entries
    int     totalShorter  = 0;       // total in the shorter sequence
    double  matchFraction = 0.0;     // matched / totalShorter
    double  madMs         = 0.0;     // F path: residual MAD over matched pairs
    int     phashMatched  = 0;       // v9 path
    int     phashCompared = 0;       // v9 path
    qint64  estimatedTrimMs = 0;
    QString reason;
};

/// F (Keyframes) match tuning — exposed so callers can probe / tweak.
namespace fp_match_f {
    constexpr qint64 kGapToleranceMs   = 2000;
    constexpr double kMinMatchFraction = 0.55;
    constexpr double kMaxMadMs         = 250.0;
    constexpr int    kMinKeyframes     = 30;
}

/// v9 (AudioPeakGaps) match tuning — mirrors video_fingerprint_v9_peakgaps.py.
namespace fp_match_v9 {
    constexpr qint64 kGapToleranceMs       = 5000;
    constexpr int    kPhashHammingMax      = 20;
    constexpr double kMinGapFraction       = 0.55;
    constexpr double kMinPhashFraction     = 0.40;
    constexpr int    kPeakCountTolerance   = 4;
    constexpr int    kMinPeaks             = 4;
}

/// One-shot match dispatcher. Dispatches by fingerprint type:
///   - both 'keyframes'        → F path (subset-align + MAD)
///   - both 'audio_peak_gaps'  → v9 path (paired-gap + pHash)
///   - mismatched/unknown      → isSameFilm=false with explanation
MatchVerdict matchFingerprints(const FilmFingerprint& localA,
                               const FilmFingerprint& remoteB);

} // namespace censorcut
