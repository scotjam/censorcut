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

    /// Compute the primary path of the sidecar JSON for a given movie.
    /// Returns "<movie>.censorcut.json" alongside the movie.
    static QString sidecarPathFor(const QString& moviePath);

    /// Compute the fallback sidecar path under QStandardPaths::AppDataLocation
    /// when the primary location is read-only — typical for movies on
    /// network shares, optical media, or mounted ISOs. Two movies with the
    /// same filename in different folders don't collide because the
    /// fallback name is keyed on a hash of the absolute movie path.
    static QString sidecarFallbackPathFor(const QString& moviePath);

    /// Both candidate sidecar paths in priority order: [primary, fallback].
    /// Loaders should try them in order; savers should try primary first
    /// and fall back on permission failure.
    static QStringList sidecarLoadCandidatesFor(const QString& moviePath);

    /// Compute the output path for the censored copy.
    /// Inserts " CENSORED-<age>" before the extension. For example:
    ///   "/films/Title.mp4" with age 7 -> "/films/Title CENSORED-7.mp4"
    /// If age <= 0 the suffix becomes just " CENSORED" (no number).
    /// The original file is never touched; this only computes a path.
    static QString censoredOutputPathFor(const QString& moviePath, int age);

    /// Compute the source hash (sha1 over first+last 1MB plus filesize).
    /// Returns empty string on I/O error.
    static QString computeSourceHash(const QString& moviePath);

    /// Canonical JSON serialization of the user-edited marker list, suitable
    /// for equality comparison ("does the interface differ from the file?").
    /// Excludes derived fields like sourceHash so it's stable across saves.
    static QString markersFingerprint(const QList<Marker>& markers);

    /// Load a project from a sidecar JSON. Returns std::nullopt on failure.
    static std::optional<Project> loadFromSidecar(const QString& sidecarPath,
                                                  QString* errorOut = nullptr);

    /// Save this project to a sidecar JSON. Returns true on success.
    bool saveToSidecar(const QString& sidecarPath, QString* errorOut = nullptr) const;

    /// Save this project trying the primary sidecar path first, falling
    /// back to the AppData path if the primary's directory is read-only
    /// (network share, optical media, etc.). Returns the path the
    /// project was actually saved to, or empty string on total failure.
    /// `usedFallbackOut` is set to true when the fallback path was used
    /// — UI can surface this so the user knows where their cuts went.
    QString saveToBestSidecarPathFor(const QString& moviePath,
                                     bool* usedFallbackOut = nullptr,
                                     QString* errorOut = nullptr) const;
};

} // namespace censorcut
