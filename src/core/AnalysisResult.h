#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <nlohmann/json_fwd.hpp>

namespace censorcut {

/// A time-series of detector scores at a fixed sampling period.
struct ScoreSeries {
    int             samplePeriodMs = 100;
    QVector<double> values;
};

/// One suggested cut from the analyzer.
struct Suggestion {
    QString     category;
    qint64      startMs    = 0;
    qint64      endMs      = 0;
    double      score      = 0.0;
    QStringList reasons;
    /// Pubkey hexes of peer authors whose accept-decision feedback rows
    /// near-matched frames in [startMs, endMs). The C++ side propagates
    /// these onto the resulting Marker; on confirm/reject of the marker
    /// MainWindow drives TrustLedger reward/penalty for each author.
    QStringList contributingAuthors;
};

/// Per-category fusion summary. Helps the user see *why* no suggestions
/// fired (e.g. "Cruelty peaked at 0.42, threshold 0.7 — try lower
/// Sensitivity").
struct CategoryDiagnostic {
    QString category;
    double  peak                 = 0.0;
    double  threshold            = 0.0;
    int     aboveCount           = 0;
    int     suggestionsEmitted   = 0;
};

/// One frame's L2-normalized CLIP embedding, kept only for frames inside
/// emitted suggestions so the feedback loop can persist it on accept/
/// reject without ballooning the JSON.
struct FrameEmbedding {
    qint64          timeMs = 0;
    QVector<float>  vec;  // typically 768 or 1024 floats
};

/// One audio peak of the v9 audio-peak-gap fingerprint. Per-peak time
/// is in absolute milliseconds; pHash is the 16-hex averaged-5-frame
/// pHash at the peak. v9 is the fallback path when keyframe extraction
/// can't get a usable fingerprint (fixed-GOP encodes, missing container
/// index, exotic muxes).
struct FingerprintPeak {
    qint64  tMs = 0;
    QString phash;        // 16-char hex (64 bits), may be empty if pHash decode failed
};

/// Type tag values. Stored as QString in FilmFingerprint::type so the
/// JSON round-trip is trivial and unknown tags fall through to isValid()==false.
namespace fp_type {
    constexpr const char* Keyframes     = "keyframes";
    constexpr const char* AudioPeakGaps = "audio_peak_gaps";
    constexpr const char* Unknown       = "unknown";
}

/// Video fingerprint, two-variant. The Python detector picks the right
/// one per file based on what its container index supports; see
/// python/censorcut/detectors/video_fingerprint.py.
///
///   Keyframes (F): keyframeTimesMs[] from MKV Cues / MP4 stss / AVI
///       idx1. Match via subset alignment + offset-residual MAD.
///       ~1000x faster fingerprint compute than the old v1 path because
///       we read only container metadata, never the bitstream.
///
///   AudioPeakGaps (v9): peaks[]+gapsMs[] from the audio-peak detector.
///       Match via 1-to-1 paired-by-index gap comparison with pHash
///       agreement. Slower (audio demux walks the file) but works on
///       containers that don't expose keyframe times.
///
/// Different cuts of the same film (theatrical vs director's vs
/// censored derivative) produce different fingerprints by design —
/// matching is INTENDED to fail across cuts, so applying one cut's
/// edits to another's timeline won't silently corrupt them.
struct FilmFingerprint {
    int     version           = 1;
    QString type;             // "keyframes" / "audio_peak_gaps" / "unknown"
    qint64  durationMs        = 0;
    int     approxDurationMin = 0;

    // Keyframes variant
    QList<qint64>            keyframeTimesMs;

    // AudioPeakGaps variant
    QList<FingerprintPeak>   peaks;
    QList<qint64>            gapsMs;
    qint64                   innerSpanMs = 0;

    [[nodiscard]] bool isValid() const {
        if (type == QLatin1String(fp_type::Keyframes))
            return !keyframeTimesMs.isEmpty();
        if (type == QLatin1String(fp_type::AudioPeakGaps))
            return !peaks.isEmpty();
        return false;
    }

    /// Short bucket key for indexing by the edits server (and for the
    /// status bar). Films of substantially different durations can't
    /// match, so a single integer (rounded minutes) is enough.
    [[nodiscard]] QString bucketKey() const {
        return QString::number(approxDurationMin);
    }
};

/// Result of running the analyzer over a source video. Per-second raw
/// scores are kept so the host UI can re-threshold (or apply a different
/// age profile) without re-running ML.
struct AnalysisResult {
    int                             schemaVersion = 1;
    qint64                          durationMs    = 0;
    QHash<QString, ScoreSeries>     rawScores;     // detector key -> series
    QList<Suggestion>               suggestions;
    QList<CategoryDiagnostic>       diagnostics;
    QList<FrameEmbedding>           frameEmbeddings;
    FilmFingerprint                 fingerprint;
    bool                            yamnetUsed  = false;
    bool                            clipUsed    = false;
    bool                            whisperUsed = false;
    double                          thresholdMul = 1.0;

    [[nodiscard]] bool isEmpty() const {
        return suggestions.isEmpty() && rawScores.isEmpty();
    }
};

/// Parse the result JSON written by python -m censorcut.analyze. Returns
/// the populated result on success; sets *errorOut and returns an empty
/// result on parse failure.
AnalysisResult parseAnalysisResultJson(const QByteArray& json, QString* errorOut = nullptr);

} // namespace censorcut
