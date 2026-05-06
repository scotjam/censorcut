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

/// Result of running the analyzer over a source video. Per-second raw
/// scores are kept so the host UI can re-threshold (or apply a different
/// age profile) without re-running ML.
struct AnalysisResult {
    int                             schemaVersion = 1;
    qint64                          durationMs    = 0;
    QHash<QString, ScoreSeries>     rawScores;     // detector key -> series
    QList<Suggestion>               suggestions;
    QList<CategoryDiagnostic>       diagnostics;
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
