#include "Project.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>

#include <nlohmann/json.hpp>

namespace censorcut {

namespace {

constexpr qint64 kHashWindowBytes = 1 * 1024 * 1024;  // 1 MB head + 1 MB tail

void to_json_export(nlohmann::json& j, const ExportSettings& e)
{
    j = nlohmann::json{
        {"videoCodec",          e.videoCodec.toStdString()},
        {"crf",                 e.crf},
        {"preset",              e.preset.toStdString()},
        {"audioCodec",          e.audioCodec.toStdString()},
        {"audioBitrateKbps",    e.audioBitrateKbps},
        {"copyAllAudioTracks",  e.copyAllAudioTracks},
        {"keyframeAlignedFast", e.keyframeAlignedFast},
    };
}

void from_json_export(const nlohmann::json& j, ExportSettings& e)
{
    e.videoCodec   = QString::fromStdString(j.value("videoCodec",  std::string{"libx264"}));
    e.crf          = j.value("crf", 18);
    e.preset       = QString::fromStdString(j.value("preset",      std::string{"medium"}));
    e.audioCodec   = QString::fromStdString(j.value("audioCodec",  std::string{"aac"}));
    e.audioBitrateKbps   = j.value("audioBitrateKbps", 192);
    e.copyAllAudioTracks = j.value("copyAllAudioTracks", false);
    e.keyframeAlignedFast = j.value("keyframeAlignedFast", false);
}

} // namespace

QString Project::sidecarPathFor(const QString& moviePath)
{
    return moviePath + QStringLiteral(".censorcut.json");
}

QString Project::computeSourceHash(const QString& moviePath)
{
    QFile f(moviePath);
    if (!f.open(QIODevice::ReadOnly)) return {};

    const qint64 size = f.size();
    QCryptographicHash hash(QCryptographicHash::Sha1);

    // filesize as a fixed-width little-endian field
    const auto sizeBytes = QByteArray(reinterpret_cast<const char*>(&size), sizeof(size));
    hash.addData(sizeBytes);

    // first window
    {
        const QByteArray head = f.read(qMin<qint64>(kHashWindowBytes, size));
        hash.addData(head);
    }
    // last window (skip if file already fully covered)
    if (size > 2 * kHashWindowBytes) {
        if (!f.seek(size - kHashWindowBytes)) return {};
        const QByteArray tail = f.read(kHashWindowBytes);
        hash.addData(tail);
    }
    return QString::fromLatin1(hash.result().toHex());
}

std::optional<Project> Project::loadFromSidecar(const QString& sidecarPath,
                                                QString* errorOut)
{
    QFile f(sidecarPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Could not open sidecar: %1").arg(sidecarPath);
        return std::nullopt;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f.readAll().toStdString());
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QStringLiteral("Sidecar JSON parse error: %1")
                                      .arg(QString::fromUtf8(e.what()));
        return std::nullopt;
    }

    Project p;
    p.schemaVersion = j.value("schemaVersion", kSchemaVersion);
    p.sourceFile    = QString::fromStdString(j.value("sourceFile", std::string{}));
    p.sourceHash    = QString::fromStdString(j.value("sourceHash", std::string{}));
    p.durationMs    = j.value("durationMs", qint64{0});

    if (j.contains("markers") && j.at("markers").is_array()) {
        for (const auto& mj : j.at("markers")) {
            Marker m;
            from_json(mj, m);
            p.markers.append(m);
        }
    }
    if (j.contains("exportSettings")) {
        from_json_export(j.at("exportSettings"), p.exportSettings);
    }
    if (j.contains("activeProfile")) {
        from_json(j.at("activeProfile"), p.activeProfile);
    }
    return p;
}

bool Project::saveToSidecar(const QString& sidecarPath, QString* errorOut) const
{
    nlohmann::json j;
    j["schemaVersion"] = schemaVersion;
    j["sourceFile"]    = sourceFile.toStdString();
    j["sourceHash"]    = sourceHash.toStdString();
    j["durationMs"]    = durationMs;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : markers) {
        nlohmann::json mj;
        to_json(mj, m);
        arr.push_back(mj);
    }
    j["markers"] = arr;

    nlohmann::json ej;
    to_json_export(ej, exportSettings);
    j["exportSettings"] = ej;

    nlohmann::json pj;
    to_json(pj, activeProfile);
    j["activeProfile"] = pj;

    QSaveFile f(sidecarPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = QStringLiteral("Cannot open sidecar for write: %1")
                                      .arg(sidecarPath);
        return false;
    }
    const std::string out = j.dump(2);
    if (f.write(out.data(), static_cast<qint64>(out.size())) != static_cast<qint64>(out.size())) {
        if (errorOut) *errorOut = QStringLiteral("Short write to sidecar");
        return false;
    }
    if (!f.commit()) {
        if (errorOut) *errorOut = QStringLiteral("Sidecar commit failed");
        return false;
    }
    return true;
}

} // namespace censorcut
