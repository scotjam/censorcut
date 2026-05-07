#include "AnalysisResult.h"

#include <nlohmann/json.hpp>

namespace censorcut {

AnalysisResult parseAnalysisResultJson(const QByteArray& jsonBytes, QString* errorOut)
{
    AnalysisResult result;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonBytes.constData(),
                                  jsonBytes.constData() + jsonBytes.size());
    } catch (const std::exception& e) {
        if (errorOut)
            *errorOut = QStringLiteral("Analyzer JSON parse error: %1")
                            .arg(QString::fromUtf8(e.what()));
        return result;
    }

    result.schemaVersion = j.value("schemaVersion", 1);
    result.durationMs    = j.value("durationMs", qint64{0});
    result.yamnetUsed    = j.value("yamnetUsed",  false);
    result.clipUsed      = j.value("clipUsed",    false);
    result.whisperUsed   = j.value("whisperUsed", false);
    result.thresholdMul  = j.value("thresholdMul", 1.0);

    if (j.contains("categoryDiagnostics") && j.at("categoryDiagnostics").is_array()) {
        for (const auto& dj : j.at("categoryDiagnostics")) {
            if (!dj.is_object()) continue;
            CategoryDiagnostic d;
            d.category           = QString::fromStdString(
                dj.value("category", std::string{}));
            d.peak               = dj.value("peak",      0.0);
            d.threshold          = dj.value("threshold", 0.0);
            d.aboveCount         = dj.value("aboveCount", 0);
            d.suggestionsEmitted = dj.value("suggestionsEmitted", 0);
            result.diagnostics.append(d);
        }
    }

    if (j.contains("rawScores") && j.at("rawScores").is_object()) {
        for (auto it = j.at("rawScores").begin(); it != j.at("rawScores").end(); ++it) {
            const auto& seriesObj = it.value();
            if (!seriesObj.is_object()) continue;
            ScoreSeries s;
            s.samplePeriodMs = seriesObj.value("samplePeriodMs", 100);
            if (seriesObj.contains("values") && seriesObj.at("values").is_array()) {
                const auto& arr = seriesObj.at("values");
                s.values.reserve(static_cast<int>(arr.size()));
                for (const auto& v : arr) {
                    s.values.append(v.is_number() ? v.get<double>() : 0.0);
                }
            }
            result.rawScores.insert(QString::fromStdString(it.key()), s);
        }
    }

    if (j.contains("frameEmbeddings") && j.at("frameEmbeddings").is_array()) {
        for (const auto& fj : j.at("frameEmbeddings")) {
            if (!fj.is_object()) continue;
            FrameEmbedding fe;
            fe.timeMs = fj.value("tMs", qint64{0});
            if (fj.contains("vec") && fj.at("vec").is_array()) {
                const auto& arr = fj.at("vec");
                fe.vec.reserve(static_cast<int>(arr.size()));
                for (const auto& v : arr) {
                    fe.vec.append(v.is_number() ? float(v.get<double>()) : 0.0f);
                }
            }
            if (!fe.vec.isEmpty()) result.frameEmbeddings.append(fe);
        }
    }

    if (j.contains("fingerprint") && j.at("fingerprint").is_object()) {
        const auto& fj = j.at("fingerprint");
        FilmFingerprint fp;
        fp.durationMs = fj.value("durationMs", qint64{0});
        fp.digest     = QString::fromStdString(fj.value("fingerprint", std::string{}));
        if (fj.contains("anchors") && fj.at("anchors").is_array()) {
            for (const auto& aj : fj.at("anchors")) {
                if (!aj.is_object()) continue;
                FingerprintAnchor a;
                a.tMs      = aj.value("tMs",      qint64{0});
                a.peakLufs = aj.value("peakLufs", 0.0);
                a.sig      = QString::fromStdString(aj.value("sig", std::string{}));
                if (a.tMs >= 0 && !a.sig.isEmpty()) fp.anchors.append(a);
            }
        }
        result.fingerprint = fp;
    }

    if (j.contains("suggestions") && j.at("suggestions").is_array()) {
        for (const auto& sj : j.at("suggestions")) {
            if (!sj.is_object()) continue;
            Suggestion s;
            s.category = QString::fromStdString(sj.value("category", std::string{}));
            s.startMs  = sj.value("startMs", qint64{0});
            s.endMs    = sj.value("endMs",   qint64{0});
            s.score    = sj.value("score",   0.0);
            if (sj.contains("reasons") && sj.at("reasons").is_array()) {
                for (const auto& rj : sj.at("reasons")) {
                    if (rj.is_string())
                        s.reasons.append(QString::fromStdString(rj.get<std::string>()));
                }
            }
            if (sj.contains("contributingAuthors") &&
                sj.at("contributingAuthors").is_array()) {
                for (const auto& aj : sj.at("contributingAuthors")) {
                    if (aj.is_string())
                        s.contributingAuthors.append(
                            QString::fromStdString(aj.get<std::string>()));
                }
            }
            if (s.endMs > s.startMs && !s.category.isEmpty())
                result.suggestions.append(s);
        }
    }
    return result;
}

} // namespace censorcut
