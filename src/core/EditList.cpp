#include "EditList.h"

#include "ExportPlan.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace censorcut {

namespace {

std::string toStd(const QString& s) { return s.toStdString(); }

QString fromStd(const nlohmann::json& j, const char* key, const QString& fallback = {})
{
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    return QString::fromStdString(j[key].get<std::string>());
}

qint64 intOr(const nlohmann::json& j, const char* key, qint64 fallback)
{
    if (!j.contains(key) || !j[key].is_number_integer()) return fallback;
    return j[key].get<qint64>();
}

} // namespace

QString EditProfile::idForAge(int age)
{
    return age > 0 ? QStringLiteral("age-%1").arg(age) : QStringLiteral("all");
}

QString EditList::pathFor(const QString& moviePath)
{
    return moviePath + QStringLiteral(".censorcut-edl.json");
}

EditList EditList::fromProject(const Project& project)
{
    EditList list;
    list.sourceFileName = QFileInfo(project.sourceFile).fileName();
    list.sourceHash     = project.sourceHash;
    list.durationMs     = project.durationMs;

    EditProfile profile;
    profile.minAge = project.activeProfile.minAge;
    profile.id     = EditProfile::idForAge(profile.minAge);
    profile.label  = project.activeProfile.label.isEmpty()
                         ? QStringLiteral("Age %1").arg(profile.minAge)
                         : project.activeProfile.label;

    // Reuse the encoder's merge rules so a shortcut and an encoded copy remove
    // exactly the same ranges.
    const auto ranges = mergedConfirmedCuts(project.markers, project.durationMs);
    for (const auto& r : ranges) {
        EditCut cut;
        cut.startMs = r.first;
        cut.endMs   = r.second;
        // Attribute the cut to a category when exactly one confirmed marker
        // produced it; merged ranges spanning several categories stay blank
        // rather than claiming a misleading single label.
        QString only;
        bool ambiguous = false;
        for (const auto& m : project.markers) {
            if (m.status != Status::Confirmed || !m.isValid()) continue;
            if (m.startMs >= cut.endMs || m.endMs <= cut.startMs) continue;
            if (only.isEmpty()) only = m.category;
            else if (only != m.category) { ambiguous = true; break; }
        }
        if (!ambiguous) cut.category = only;
        profile.cuts.append(cut);
    }

    list.profiles.append(profile);
    list.defaultProfileId = profile.id;
    return list;
}

void EditList::upsertProfile(const EditProfile& profile)
{
    for (auto& p : profiles) {
        if (p.id == profile.id) { p = profile; return; }
    }
    profiles.append(profile);
    std::sort(profiles.begin(), profiles.end(),
              [](const EditProfile& a, const EditProfile& b) {
                  if (a.minAge != b.minAge) return a.minAge < b.minAge;
                  return a.id < b.id;
              });
}

const EditProfile* EditList::findProfile(const QString& id) const
{
    for (const auto& p : profiles) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool EditList::saveTo(const QString& path, QString* errorOut) const
{
    nlohmann::json j;
    j["schemaVersion"]    = schemaVersion;
    j["generator"]        = "censorcut";
    j["sourceFileName"]   = toStd(sourceFileName);
    j["sourceHash"]       = toStd(sourceHash);
    j["durationMs"]       = durationMs;
    j["defaultProfileId"] = toStd(defaultProfileId);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : profiles) {
        nlohmann::json pj;
        pj["id"]       = toStd(p.id);
        pj["label"]    = toStd(p.label);
        pj["minAge"]   = p.minAge;
        pj["leadInMs"] = p.leadInMs;

        nlohmann::json cuts = nlohmann::json::array();
        for (const auto& c : p.cuts) {
            nlohmann::json cj;
            cj["startMs"] = c.startMs;
            cj["endMs"]   = c.endMs;
            if (!c.category.isEmpty()) cj["category"] = toStd(c.category);
            cuts.push_back(cj);
        }
        pj["cuts"] = cuts;
        arr.push_back(pj);
    }
    j["profiles"] = arr;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open edit list for write: %1").arg(path);
        return false;
    }
    const std::string out = j.dump(2);
    if (f.write(out.data(), qint64(out.size())) != qint64(out.size())) {
        if (errorOut) *errorOut = QStringLiteral("Short write to edit list: %1").arg(path);
        return false;
    }
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("Cannot commit edit list: %1").arg(path);
        return false;
    }
    return true;
}

std::optional<EditList> EditList::loadFrom(const QString& path, QString* errorOut)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open edit list: %1").arg(path);
        return std::nullopt;
    }
    const QByteArray raw = f.readAll();

    nlohmann::json j = nlohmann::json::parse(raw.constData(), raw.constData() + raw.size(),
                                             nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        if (errorOut) *errorOut = QStringLiteral("Malformed edit list: %1").arg(path);
        return std::nullopt;
    }

    EditList list;
    list.schemaVersion    = int(intOr(j, "schemaVersion", kEditListSchemaVersion));
    list.sourceFileName   = fromStd(j, "sourceFileName");
    list.sourceHash       = fromStd(j, "sourceHash");
    list.durationMs       = intOr(j, "durationMs", 0);
    list.defaultProfileId = fromStd(j, "defaultProfileId");

    if (j.contains("profiles") && j["profiles"].is_array()) {
        for (const auto& pj : j["profiles"]) {
            if (!pj.is_object()) continue;
            EditProfile p;
            p.id       = fromStd(pj, "id");
            p.label    = fromStd(pj, "label");
            p.minAge   = int(intOr(pj, "minAge", 0));
            p.leadInMs = intOr(pj, "leadInMs", kDefaultLeadInMs);
            if (pj.contains("cuts") && pj["cuts"].is_array()) {
                for (const auto& cj : pj["cuts"]) {
                    if (!cj.is_object()) continue;
                    EditCut c;
                    c.startMs  = intOr(cj, "startMs", 0);
                    c.endMs    = intOr(cj, "endMs", 0);
                    c.category = fromStd(cj, "category");
                    if (c.endMs > c.startMs) p.cuts.append(c);
                }
            }
            if (!p.id.isEmpty()) list.profiles.append(p);
        }
    }
    return list;
}

QString EditList::writeShortcutFor(const Project& project,
                                   const QString& moviePath,
                                   QString* errorOut)
{
    const QString path = pathFor(moviePath);

    EditList fresh = fromProject(project);
    // fromProject always emits a profile for the active age, so an empty cut
    // list — not an empty profile list — is what "nothing to do" looks like.
    // Writing it would leave a shortcut that silently plays the film uncut.
    if (fresh.profiles.isEmpty() || fresh.profiles.first().cuts.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Nothing to cut — no confirmed markers.");
        return {};
    }

    // Merge into whatever is already there so generating a second age doesn't
    // discard the first. A source hash mismatch means the movie was replaced
    // under us, in which case the old profiles describe different footage and
    // must not be carried forward.
    EditList out = fresh;
    if (std::optional<EditList> existing = loadFrom(path)) {
        const bool sameSource = existing->sourceHash.isEmpty()
                                || project.sourceHash.isEmpty()
                                || existing->sourceHash == project.sourceHash;
        if (sameSource) {
            out = *existing;
            out.schemaVersion  = kEditListSchemaVersion;
            out.sourceFileName = fresh.sourceFileName;
            out.sourceHash     = fresh.sourceHash;
            out.durationMs     = fresh.durationMs;
            out.upsertProfile(fresh.profiles.first());
            out.defaultProfileId = fresh.profiles.first().id;
        }
    }

    if (!out.saveTo(path, errorOut)) return {};
    return path;
}

} // namespace censorcut
