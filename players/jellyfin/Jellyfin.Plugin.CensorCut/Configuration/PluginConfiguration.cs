using Jellyfin.Database.Implementations.Enums;
using MediaBrowser.Model.Plugins;

namespace Jellyfin.Plugin.CensorCut.Configuration;

/// <summary>
/// Plugin configuration.
/// </summary>
public class PluginConfiguration : BasePluginConfiguration
{
    /// <summary>
    /// Gets or sets which segment type the cuts are reported as.
    /// </summary>
    /// <remarks>
    /// Jellyfin's segment types are a fixed set (Intro, Outro, Recap, Preview,
    /// Commercial) with nothing meaning "censored", so the cuts have to borrow
    /// one. Commercial is the default because it is the type clients most
    /// consistently offer to skip automatically rather than only via a button.
    /// </remarks>
    public MediaSegmentType SegmentType { get; set; } = MediaSegmentType.Commercial;

    /// <summary>
    /// Gets or sets the profile id to apply, for example "age-7". Empty means
    /// use the profile named by the edit list itself.
    /// </summary>
    public string? ProfileId { get; set; }

    /// <summary>
    /// Gets or sets an override for how far ahead of a cut a segment starts,
    /// in milliseconds. Zero means use the value in the edit list. Raise it if
    /// a client is slow to react and the first moments of a cut slip through.
    /// </summary>
    public long LeadInOverrideMs { get; set; }
}
