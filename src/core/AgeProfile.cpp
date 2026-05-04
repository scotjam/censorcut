#include "AgeProfile.h"

#include <nlohmann/json.hpp>

namespace censorcut {

namespace {

CategoryOverride co(bool en, double th, int padB = 1000, int padA = 500)
{
    CategoryOverride c;
    c.enabled = en;
    c.threshold = th;
    c.padBeforeMs = padB;
    c.padAfterMs = padA;
    return c;
}

AgeProfile makeUnder5()
{
    AgeProfile p;
    p.minAge = 4;
    p.label = QStringLiteral("Under 5 — maximum protection");
    p.thresholdMul = 0.6;
    p.padMul = 1.5;
    p.overrides = {
        {"Jump scare",      co(true, 0.35, 2000, 1000)},
        {"Scary music",     co(true, 0.35, 2000, 1000)},
        {"Screaming",       co(true, 0.40)},
        {"Crying / sad",    co(true, 0.50)},
        {"Yelling",         co(true, 0.50)},
        {"Gunfire",         co(true, 0.30)},
        {"Hitting",         co(true, 0.45)},
        {"Sword fight",     co(true, 0.45)},
        {"Chase",           co(true, 0.45)},
        {"Violence",        co(true, 0.40)},
        {"Cruelty",         co(true, 0.35)},
        {"Pushing",         co(true, 0.45)},
        {"Kill / threat",   co(true, 0.40)},
    };
    return p;
}

AgeProfile makeAge5()
{
    AgeProfile p;
    p.minAge = 5;
    p.label = QStringLiteral("5–6 — strong protection");
    p.thresholdMul = 0.75;
    p.padMul = 1.25;
    p.overrides = {
        {"Jump scare",      co(true, 0.40)},
        {"Scary music",     co(true, 0.45)},
        {"Screaming",       co(true, 0.45)},
        {"Crying / sad",    co(false, 0.55)},
        {"Yelling",         co(true, 0.55)},
        {"Gunfire",         co(true, 0.40)},
        {"Hitting",         co(true, 0.50)},
        {"Sword fight",     co(true, 0.50)},
        {"Chase",           co(true, 0.50)},
        {"Violence",        co(true, 0.45)},
        {"Cruelty",         co(true, 0.40)},
        {"Pushing",         co(true, 0.55)},
        {"Kill / threat",   co(true, 0.45)},
    };
    return p;
}

AgeProfile makeAge7()
{
    AgeProfile p;
    p.minAge = 7;
    p.label = QStringLiteral("7–8 — moderate protection");
    p.thresholdMul = 0.9;
    p.padMul = 1.0;
    p.overrides = {
        {"Jump scare",      co(true, 0.55)},
        {"Scary music",     co(false, 0.60)},
        {"Screaming",       co(true, 0.60)},
        {"Crying / sad",    co(false, 0.70)},
        {"Yelling",         co(false, 0.70)},
        {"Gunfire",         co(true, 0.50)},
        {"Hitting",         co(true, 0.60)},
        {"Sword fight",     co(false, 0.65)},
        {"Chase",           co(true, 0.65)},
        {"Violence",        co(true, 0.55)},
        {"Cruelty",         co(true, 0.50)},
        {"Pushing",         co(false, 0.65)},
        {"Kill / threat",   co(true, 0.55)},
    };
    return p;
}

AgeProfile makeAge9()
{
    AgeProfile p;
    p.minAge = 9;
    p.label = QStringLiteral("9–10 — light protection");
    p.thresholdMul = 1.0;
    p.padMul = 1.0;
    p.overrides = {
        {"Jump scare",      co(true,  0.65)},
        {"Scary music",     co(false, 0.70)},
        {"Screaming",       co(true,  0.70)},
        {"Crying / sad",    co(false, 0.80)},
        {"Yelling",         co(false, 0.80)},
        {"Gunfire",         co(true,  0.60)},
        {"Hitting",         co(false, 0.70)},
        {"Sword fight",     co(false, 0.75)},
        {"Chase",           co(false, 0.75)},
        {"Violence",        co(true,  0.70)},
        {"Cruelty",         co(true,  0.60)},
        {"Pushing",         co(false, 0.75)},
        {"Kill / threat",   co(true,  0.65)},
    };
    return p;
}

AgeProfile makeAge11()
{
    AgeProfile p;
    p.minAge = 11;
    p.label = QStringLiteral("11–12 — minimal");
    p.thresholdMul = 1.1;
    p.padMul = 0.85;
    p.overrides = {
        {"Jump scare",      co(true,  0.75)},
        {"Cruelty",         co(true,  0.70)},
        {"Kill / threat",   co(true,  0.75)},
        // everything else off by default
    };
    return p;
}

AgeProfile makeAge13()
{
    AgeProfile p;
    p.minAge = 13;
    p.label = QStringLiteral("13–14 — very minimal");
    p.thresholdMul = 1.2;
    p.padMul = 0.75;
    p.overrides = {
        {"Jump scare",      co(true,  0.85)},
    };
    return p;
}

AgeProfile makeAge15()
{
    AgeProfile p;
    p.minAge = 15;
    p.label = QStringLiteral("15+ — off");
    p.thresholdMul = 1.0;
    p.padMul = 1.0;
    // No overrides; everything disabled by default at the analyzer level.
    return p;
}

} // namespace

QList<AgeProfile> AgeProfile::builtIns()
{
    return { makeUnder5(), makeAge5(), makeAge7(), makeAge9(),
             makeAge11(),  makeAge13(), makeAge15() };
}

AgeProfile AgeProfile::forAge(int age)
{
    AgeProfile chosen = makeUnder5();
    for (const auto& p : builtIns()) {
        if (p.minAge <= age && p.minAge >= chosen.minAge) {
            chosen = p;
        }
    }
    return chosen;
}

void to_json(nlohmann::json& j, const AgeProfile& p)
{
    nlohmann::json overrides = nlohmann::json::object();
    for (auto it = p.overrides.constBegin(); it != p.overrides.constEnd(); ++it) {
        overrides[it.key().toStdString()] = {
            {"enabled",     it.value().enabled},
            {"threshold",   it.value().threshold},
            {"padBeforeMs", it.value().padBeforeMs},
            {"padAfterMs",  it.value().padAfterMs},
        };
    }
    j = nlohmann::json{
        {"minAge",       p.minAge},
        {"label",        p.label.toStdString()},
        {"thresholdMul", p.thresholdMul},
        {"padMul",       p.padMul},
        {"overrides",    overrides},
    };
}

void from_json(const nlohmann::json& j, AgeProfile& p)
{
    p.minAge       = j.value("minAge", 8);
    p.label        = QString::fromStdString(j.value("label", std::string{}));
    p.thresholdMul = j.value("thresholdMul", 1.0);
    p.padMul       = j.value("padMul", 1.0);
    p.overrides.clear();
    if (j.contains("overrides") && j.at("overrides").is_object()) {
        for (auto it = j.at("overrides").begin(); it != j.at("overrides").end(); ++it) {
            CategoryOverride c;
            c.enabled     = it.value().value("enabled",     true);
            c.threshold   = it.value().value("threshold",   0.5);
            c.padBeforeMs = it.value().value("padBeforeMs", 1000);
            c.padAfterMs  = it.value().value("padAfterMs",  500);
            p.overrides.insert(QString::fromStdString(it.key()), c);
        }
    }
}

} // namespace censorcut
