using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Jellyfin.Plugin.CensorCut;

/// <summary>
/// One range the player must skip. Half-open: [StartMs, EndMs).
/// </summary>
public sealed class EditCut
{
    /// <summary>Gets or sets the start of the cut, in milliseconds.</summary>
    [JsonPropertyName("startMs")]
    public long StartMs { get; set; }

    /// <summary>Gets or sets the end of the cut, in milliseconds.</summary>
    [JsonPropertyName("endMs")]
    public long EndMs { get; set; }

    /// <summary>Gets or sets the originating category, when unambiguous.</summary>
    [JsonPropertyName("category")]
    public string? Category { get; set; }
}

/// <summary>
/// The cut list for one age, already resolved by the CensorCut app — marker
/// status and category thresholds have been applied, so nothing here needs
/// reinterpreting.
/// </summary>
public sealed class EditProfile
{
    /// <summary>Gets or sets the stable profile id, e.g. "age-7".</summary>
    [JsonPropertyName("id")]
    public string? Id { get; set; }

    /// <summary>Gets or sets the human-readable label.</summary>
    [JsonPropertyName("label")]
    public string? Label { get; set; }

    /// <summary>Gets or sets the minimum viewer age this profile targets.</summary>
    [JsonPropertyName("minAge")]
    public int MinAge { get; set; }

    /// <summary>
    /// Gets or sets how far ahead of a cut a player should jump. Clients notice
    /// entry into a segment late, so segments are widened at the front by this
    /// amount rather than risking a visible frame of the cut.
    /// </summary>
    [JsonPropertyName("leadInMs")]
    public long LeadInMs { get; set; } = 150;

    /// <summary>Gets the cuts in this profile.</summary>
    [JsonPropertyName("cuts")]
    public List<EditCut> Cuts { get; init; } = new List<EditCut>();
}

/// <summary>
/// The "&lt;movie&gt;.censorcut-edl.json" file written beside a movie, holding
/// every age profile generated for it.
/// </summary>
public sealed class EditList
{
    private static readonly JsonSerializerOptions _jsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    /// <summary>Gets or sets the format version.</summary>
    [JsonPropertyName("schemaVersion")]
    public int SchemaVersion { get; set; }

    /// <summary>Gets or sets the movie's file name, without its directory.</summary>
    [JsonPropertyName("sourceFileName")]
    public string? SourceFileName { get; set; }

    /// <summary>Gets or sets the hash identifying the source the cuts were made against.</summary>
    [JsonPropertyName("sourceHash")]
    public string? SourceHash { get; set; }

    /// <summary>Gets or sets the source duration in milliseconds.</summary>
    [JsonPropertyName("durationMs")]
    public long DurationMs { get; set; }

    /// <summary>Gets or sets the profile to use when nothing else is configured.</summary>
    [JsonPropertyName("defaultProfileId")]
    public string? DefaultProfileId { get; set; }

    /// <summary>Gets the profiles held in this file.</summary>
    [JsonPropertyName("profiles")]
    public List<EditProfile> Profiles { get; init; } = new List<EditProfile>();

    /// <summary>
    /// Builds the edit list path for a media file.
    /// </summary>
    /// <param name="mediaPath">Full path to the movie.</param>
    /// <returns>The sidecar path.</returns>
    public static string PathFor(string mediaPath) => mediaPath + ".censorcut-edl.json";

    /// <summary>
    /// Loads the edit list beside a media file, or null when there is none or
    /// it cannot be read.
    /// </summary>
    /// <param name="mediaPath">Full path to the movie.</param>
    /// <returns>The parsed edit list, or null.</returns>
    public static EditList? LoadFor(string? mediaPath)
    {
        if (string.IsNullOrEmpty(mediaPath))
        {
            return null;
        }

        var path = PathFor(mediaPath);
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            using var stream = File.OpenRead(path);
            return JsonSerializer.Deserialize<EditList>(stream, _jsonOptions);
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>
    /// Picks the profile to apply: an explicitly configured id if given,
    /// otherwise the file's own default.
    /// </summary>
    /// <remarks>
    /// When the requested id is not in the file, this falls back to the
    /// strictest (youngest) profile rather than to the file default or to
    /// nothing. Both alternatives are laxer than what was asked for, and the
    /// failure they produce — a film quietly playing with fewer cuts than
    /// intended, or none — is the one that matters here. The VLC script
    /// applies the same rule; the two players must not disagree about what a
    /// given edit list means.
    /// </remarks>
    /// <param name="preferredProfileId">Configured profile id, or null to use the file default.</param>
    /// <returns>The chosen profile, or null when the file holds none.</returns>
    public EditProfile? PickProfile(string? preferredProfileId)
    {
        if (Profiles.Count == 0)
        {
            return null;
        }

        var wanted = string.IsNullOrWhiteSpace(preferredProfileId)
            ? DefaultProfileId
            : preferredProfileId;

        if (!string.IsNullOrWhiteSpace(wanted))
        {
            foreach (var profile in Profiles)
            {
                if (string.Equals(profile.Id, wanted, StringComparison.OrdinalIgnoreCase))
                {
                    return profile;
                }
            }
        }

        EditProfile? strictest = null;
        foreach (var profile in Profiles)
        {
            if (strictest is null || profile.MinAge < strictest.MinAge)
            {
                strictest = profile;
            }
        }

        return strictest;
    }
}
