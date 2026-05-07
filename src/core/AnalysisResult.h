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

/// One anchor inside an audio fingerprint: a loud non-voice peak time
/// plus its 64-bit spectral signature (16 hex chars).
struct FingerprintAnchor {
    qint64  tMs       = 0;
    double  peakLufs  = 0.0;
    QString sig;       // 16-char hex; "0000000000000000" if no signature
};

/// Content-derived film identifier built from 4 audio anchors. Two
/// fingerprints can be compared (FilmFingerprint::matches) to decide
/// whether two files contain the same film and at what time offset.
struct FilmFingerprint {
    qint64                       durationMs = 0;
    QString                      digest;   // sha256 of all 4 sig values
    QList<FingerprintAnchor>     anchors;
    [[nodiscard]] bool isValid() const { return !anchors.isEmpty(); }
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
