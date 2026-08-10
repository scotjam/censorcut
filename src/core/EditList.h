#pragma once

#include "Project.h"

#include <QList>
#include <QString>
#include <optional>

namespace censorcut {

constexpr int kEditListSchemaVersion = 1;

/// How far ahead of a cut a player should jump. Players discover that
/// playback has entered a cut by polling, so detection is always late by up
/// to one poll interval. Jumping early trades a fraction of a second of clean
/// footage for the guarantee that no frame of a cut is ever displayed — the
/// right direction to err in for this tool.
constexpr qint64 kDefaultLeadInMs = 150;

/// One range a player must skip. Half-open [startMs, endMs), same convention
/// as Marker.
struct EditCut {
    qint64  startMs = 0;
    qint64  endMs   = 0;
    QString category;   // informational; players may show or log it

    [[nodiscard]] qint64 durationMs() const noexcept { return endMs - startMs; }
};

/// The resolved cut list for one age. "Resolved" is the important word: the
/// app has already applied marker status and the age profile, so a player
/// never sees pending or rejected markers and never has to reimplement
/// category thresholds.
struct EditProfile {
    QString        id;                        // e.g. "age-7"
    QString        label;                     // e.g. "Age 7"
    int            minAge   = 0;
    qint64         leadInMs = kDefaultLeadInMs;
    QList<EditCut> cuts;

    /// Stable id for an age, used to match profiles across saves.
    static QString idForAge(int age);
};

/// The file both player integrations read: one per movie, holding every age
/// profile that has been generated for it.
///
/// One file rather than one per age because a player only knows the movie's
/// path — it has no way to guess which of several age-suffixed files to load.
/// Keeping them together also means generating a second age never invalidates
/// the first.
struct EditList {
    int     schemaVersion = kEditListSchemaVersion;
    QString sourceFileName;   // file name only, so the list survives a move
    QString sourceHash;       // Project::computeSourceHash — detects a swap
    qint64  durationMs = 0;
    QString defaultProfileId; // which profile a player picks absent user config
    QList<EditProfile> profiles;

    /// "<movie>.censorcut-edl.json", alongside the movie — the same
    /// convention as the marker sidecar so both are easy to spot and to
    /// exclude from a media scanner.
    static QString pathFor(const QString& moviePath);

    /// Build a single-profile edit list from a project's confirmed markers,
    /// using the project's active age profile. Cut ranges come from
    /// mergedConfirmedCuts(), so they match what the encoder would remove.
    static EditList fromProject(const Project& project);

    static std::optional<EditList> loadFrom(const QString& path,
                                            QString* errorOut = nullptr);
    bool saveTo(const QString& path, QString* errorOut = nullptr) const;

    /// Insert or replace a profile by id, keeping profiles sorted by minAge.
    /// Used to add an age to an existing file without disturbing the others.
    void upsertProfile(const EditProfile& profile);

    [[nodiscard]] const EditProfile* findProfile(const QString& id) const;

    /// Write this project's active profile into the movie's edit list,
    /// merging with any profiles already there. Returns the path written, or
    /// an empty string on failure.
    static QString writeShortcutFor(const Project& project,
                                    const QString& moviePath,
                                    QString* errorOut = nullptr);
};

} // namespace censorcut
