using Jellyfin.Database.Implementations.Enums;
using MediaBrowser.Model.Plugins;

namespace Jellyfin.Plugin.CensorCut.Configuration;

/// <summary>
/// Where the cutting happens.
/// </summary>
public enum CensorCutMode
{
    /// <summary>
    /// The server offers a censored version whose stream is already cut. Works
    /// on every client, because no client is involved in the decision.
    /// </summary>
    ServerSide = 0,

    /// <summary>
    /// The server reports the cuts as media segments and the client skips
    /// them. Cheaper — the original can still direct play — but a client
    /// without auto-skip support plays the film uncut.
    /// </summary>
    ClientSegments = 1,
}

/// <summary>
/// Plugin configuration.
/// </summary>
public class PluginConfiguration : BasePluginConfiguration
{
    /// <summary>
    /// Gets or sets where the cutting happens.
    /// </summary>
    /// <remarks>
    /// Exactly one mechanism runs at a time, deliberately. With both active a
    /// client would skip forward inside a stream the server had already cut,
    /// jumping past wanted footage.
    /// </remarks>
    public CensorCutMode Mode { get; set; } = CensorCutMode.ServerSide;

    /// <summary>
    /// Gets or sets how far past a cut to search for the keyframe that
    /// playback resumes on, in seconds.
    /// </summary>
    /// <remarks>
    /// Server-side mode only. Resuming has to land on a keyframe, so this
    /// bounds how much of the file is probed; a cut whose next keyframe is
    /// further away than this will not produce a censored version at all
    /// rather than an unsafe one.
    /// </remarks>
    public int KeyframeSearchWindowSeconds { get; set; } = 30;

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
