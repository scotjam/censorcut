# Player integrations

These let a player skip the marked scenes directly, so a censored viewing needs
no re-encoded copy: the movie stays exactly as it is and a small text file
beside it says which ranges to skip.

Create that file from the app with **File → Create Censor Shortcut**. It writes
`<movie>.censorcut-edl.json` next to the movie, holding one entry per age
profile you have generated.

## Read this before relying on it

A shortcut is a convenience, not an enforcement mechanism. Two things it does
not do:

- **The original stays playable.** It sits right there, uncut, and opening it
  in anything without the plugin plays the whole film. If the point is handing
  a child a device unsupervised, export an encoded copy instead — that file is
  censored no matter what opens it.
- **Jellyfin skipping happens in the client.** The server hands clients a list
  of segments; a client that does not support automatic skipping plays the
  film uncut. This fails *open*. Check your clients before trusting it.

VLC is the stronger of the two: the script does the seeking itself, so it does
not depend on client behaviour.

## VLC

Copy `vlc/censorcut.lua` into VLC's interface-script directory:

| Platform | Path |
| --- | --- |
| Windows | `%APPDATA%\vlc\lua\intf\censorcut.lua` |
| Linux | `~/.local/share/vlc/lua/intf/censorcut.lua` |
| macOS | `~/Library/Application Support/org.videolan.vlc/lua/intf/censorcut.lua` |

Then run VLC with the interface enabled:

```
vlc --extraintf=luaintf --lua-intf=censorcut
```

To make it permanent: *Tools → Preferences → Show settings: All → Interface →
Main interfaces*, tick **Lua interpreter**, then under *Main interfaces → Lua*
set **Lua interface** to `censorcut`.

Pick a specific age with the `CENSORCUT_PROFILE` environment variable (for
example `age-7`); otherwise the file's own default is used. If the id does not
exist in the file, the strictest profile is used rather than none.

Run its tests with any Lua 5.1+ interpreter:

```
lua vlc/test_censorcut.lua
```

## Jellyfin

Requires Jellyfin 10.10 or newer (media segments).

```
cd jellyfin/Jellyfin.Plugin.CensorCut
dotnet build -c Release
```

Copy the resulting `bin/Release/net9.0/Jellyfin.Plugin.CensorCut.dll` into a
`plugins/CensorCut/` folder in your Jellyfin data directory and restart the
server. Configure it under *Dashboard → Plugins → CensorCut*:

- **Report cuts as** — Jellyfin has no "censored" segment type, so cuts borrow
  one of its five. `Commercial` is the default because clients auto-skip it
  most consistently.
- **Age profile** — which profile to apply, e.g. `age-7`. Empty uses each
  file's default.
- **Lead-in override** — how early each segment starts. Raise it if a client
  reacts slowly and the first moments of a cut slip through.

Tests:

```
cd jellyfin/Jellyfin.Plugin.CensorCut.Tests
dotnet test
```

## Why cuts start slightly early

Both integrations begin a skip `leadInMs` *before* the cut's real start. A
player only notices it has entered a cut by checking the clock periodically, so
it always notices late. Starting early gives up a fraction of a second of clean
footage; starting on time would show the opening frames of the scene being
removed. For this tool that trade is the obvious one, and the amount is
adjustable per edit list.
