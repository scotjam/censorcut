#pragma once

#include "AgeProfile.h"
#include "Marker.h"

#include <QList>
#include <QString>
#include <optional>

namespace censorcut {

constexpr int kSchemaVersion = 1;

struct ExportSettings {
    QString videoCodec  = QStringLiteral("libx264");
    int     crf         = 18;
    QString preset      = QStringLiteral("medium");
    QString audioCodec  = QStringLiteral("aac");
    int     audioBitrateKbps  = 192;
    bool    copyAllAudioTracks = false;
    bool    keyframeAlignedFast = false;
};

struct Project {
    QString             sourceFile;
    QString             sourceHash;     // sha1 of first+last 1MB + filesize
    qint64              durationMs = 0;
    QList<Marker>       markers;
    ExportSettings      exportSettings;
    AgeProfile          activeProfile = AgeProfile::forAge(8);
    int                 schemaVersion = kSchemaVersion;

    /// Compute the path of the sidecar JSON for a given movie.
    /// Returns "<movie>.censorcut.json" alongside the movie.
    static QString sidecarPathFor(const QString& moviePath);

    /// Compute the source hash (sha1 over first+last 1MB plus filesize).
    /// Returns empty string on I/O error.
    static QString computeSourceHash(const QString& moviePath);

    /// Load a project from a sidecar JSON. Returns std::nullopt on failure.
    static std::optional<Project> loadFromSidecar(const QString& sidecarPath,
                                                  QString* errorOut = nullptr);

    /// Save this project to a sidecar JSON. Returns true on success.
    bool saveToSidecar(const QString& sidecarPath, QString* errorOut = nullptr) const;
};

} // namespace censorcut
