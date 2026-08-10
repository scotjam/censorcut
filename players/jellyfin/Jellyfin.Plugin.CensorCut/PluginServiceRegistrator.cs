using MediaBrowser.Controller;
using MediaBrowser.Controller.Library;
using MediaBrowser.Controller.MediaSegments;
using MediaBrowser.Controller.Plugins;
using Microsoft.Extensions.DependencyInjection;

namespace Jellyfin.Plugin.CensorCut;

/// <inheritdoc />
public class PluginServiceRegistrator : IPluginServiceRegistrator
{
    /// <inheritdoc />
    public void RegisterServices(IServiceCollection serviceCollection, IServerApplicationHost applicationHost)
    {
        // Both are registered; the configured mode decides which one actually
        // produces anything, so they can never both act on the same playback.
        serviceCollection.AddSingleton<IMediaSourceProvider, CensorCutMediaSourceProvider>();
        serviceCollection.AddSingleton<IMediaSegmentProvider, CensorCutSegmentProvider>();
    }
}
