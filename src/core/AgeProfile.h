#pragma once

#include <QHash>
#include <QString>
#include <nlohmann/json_fwd.hpp>

namespace censorcut {

/// Per-category override inside an age profile.
struct CategoryOverride {
    bool   enabled   = true;
    double threshold = 0.5;
    int    padBeforeMs = 1000;
    int    padAfterMs  = 500;
};

/// An age profile maps a viewer age to a default analyzer configuration.
/// In M1 this is just data — the analyzer that consumes it lands in M3.
struct AgeProfile {
    int     minAge       = 8;
    QString label        = QStringLiteral("Default");
    double  thresholdMul = 1.0;   // global multiplier on category thresholds
    double  padMul       = 1.0;   // global multiplier on padding
    QHash<QString, CategoryOverride> overrides;

    /// Built-in default profiles, keyed by minimum age (4, 5, 7, 9, 11, 13, 15).
    /// Returns the profile with the largest minAge that is <= age.
    static AgeProfile forAge(int age);

    /// Full set of built-in profiles.
    static QList<AgeProfile> builtIns();
};

void to_json(nlohmann::json& j, const AgeProfile& p);
void from_json(const nlohmann::json& j, AgeProfile& p);

} // namespace censorcut
