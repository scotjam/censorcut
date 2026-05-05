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
            if (s.endMs > s.startMs && !s.category.isEmpty())
                result.suggestions.append(s);
        }
    }
    return result;
}

} // namespace censorcut
