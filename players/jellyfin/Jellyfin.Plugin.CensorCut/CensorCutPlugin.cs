using System;
using System.Collections.Generic;
using Jellyfin.Plugin.CensorCut.Configuration;
using MediaBrowser.Common.Configuration;
using MediaBrowser.Common.Plugins;
using MediaBrowser.Model.Plugins;
using MediaBrowser.Model.Serialization;

namespace Jellyfin.Plugin.CensorCut;

/// <summary>
/// Plugin entrypoint.
/// </summary>
public class CensorCutPlugin : BasePlugin<PluginConfiguration>, IHasWebPages
{
    private readonly Guid _id = new("3f2b6c14-9a57-4e0b-8d21-6c5b0a7e4d92");

    /// <summary>
    /// Initializes a new instance of the <see cref="CensorCutPlugin"/> class.
    /// </summary>
    /// <param name="applicationPaths">Instance of the <see cref="IApplicationPaths"/> interface.</param>
    /// <param name="xmlSerializer">Instance of the <see cref="IXmlSerializer"/> interface.</param>
    public CensorCutPlugin(IApplicationPaths applicationPaths, IXmlSerializer xmlSerializer)
        : base(applicationPaths, xmlSerializer)
    {
        Instance = this;
    }

    /// <summary>
    /// Gets the current plugin instance.
    /// </summary>
    public static CensorCutPlugin? Instance { get; private set; }

    /// <inheritdoc />
    public override Guid Id => _id;

    /// <inheritdoc />
    public override string Name => "CensorCut";

    /// <inheritdoc />
    public override string Description =>
        "Skips the scenes marked in a movie's CensorCut edit list, without re-encoding the film.";

    /// <inheritdoc />
    public IEnumerable<PluginPageInfo> GetPages() =>
    [
        new()
        {
            Name = Name,
            EmbeddedResourcePath = $"{GetType().Namespace}.Configuration.config.html"
        }
    ];
}
