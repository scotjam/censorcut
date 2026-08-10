# Player integrations

These let a player skip the marked scenes directly, so a censored viewing needs
no re-encoded copy: the movie stays exactly as it is and a small text file
beside it says which ranges to skip.

Create that file from the app with **File → Create Censor Shortcut**. It writes
`<movie>.censorcut-edl.json` next to the movie, holding one entry per age
profile you have generated.

## Read this before relying on it

**The original stays playable.** It sits right there, uncut, and opening it
directly — in a player without the script, or by browsing to the file — plays
the whole film. A shortcut changes what a *player* does; it does not change
what is on disk. If the point is handing a child a device unsupervised, export
an encoded copy instead: that file is censored no matter what opens it.

Within a configured player, though, the cutting is not optional. Jellyfin cuts
on the server by default, so no client can bypass it, and the VLC script does
its own seeking rather than trusting playlist options.

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

Requires Jellyfin 10.10 or newer.

```
cd jellyfin/Jellyfin.Plugin.CensorCut
dotnet build -c Release
```

Copy the resulting `bin/Release/net9.0/Jellyfin.Plugin.CensorCut.dll` into a
`plugins/CensorCut/` folder in your Jellyfin data directory and restart the
server. Configure it under *Dashboard → Plugins → CensorCut*.

### Server-side mode (default)

A movie with an edit list gains a second version, **Censored — Age N**, in the
player's version picker. Its stream is cut by the server's own ffmpeg, so it
plays cut on every client, including ones that have never heard of media
segments. Direct play and direct stream are refused for that version, which is
what makes it impossible to bypass — and also means it always transcodes, so
it costs CPU while playing.

The plugin writes a small `.censorcut.ffconcat` script beside each movie listing
the ranges to keep, plus one short chunk file per cut (see below). They have to
live there: ffmpeg rejects absolute paths inside a concat script as unsafe, and
`-safe 0` is not ours to add inside Jellyfin. **The media directory must be
writable.** If it is not, no censored version is offered and the reason is
logged.

**Cuts are frame-exact.** Getting there takes one trick. Playback can only be
referenced from a keyframe, and a concat `inpoint` that is not on one rewinds to
the *preceding* keyframe — which would replay the end of the scene being cut.
Snapping the resume forward to the next keyframe instead would be safe but would
throw away up to a whole GOP of wanted footage after every cut. So the partial
group of pictures between the true resume point and the next keyframe is
re-encoded into a small chunk, and everything from that keyframe onward is
referenced from the original untouched. Resume is exact, and the re-encoded part
is a few seconds per cut rather than the whole film.

Chunks are named after the exact range they cover, so they are reused across
restarts and regenerated only when the cuts change. They must match the source's
codec and container: a mismatched codec was measured to corrupt the following
entry's in/out points, and a mismatched container to drop the chunk from the
output entirely. A source whose codec has no matching encoder gets no censored
version rather than a wrong one.

### Client-side mode

Reports the cuts as media segments and lets the client skip them. The original
can still direct play, so it is cheaper — but **a client without automatic
skipping plays the film uncut**. It fails open. Only the segment type and
lead-in settings apply in this mode.

Exactly one mode is active at a time. Running both would have the client skip
forward inside a stream the server had already cut, jumping past wanted scenes.

### Settings

- **Where cutting happens** — server-side or client-side, as above.
- **Keyframe search window** — how far past a cut to look for the keyframe that
  ends the re-encoded chunk. Server mode only; a larger window means a longer
  probe, a smaller one risks re-encoding more than necessary.
- **Report cuts as** — which of Jellyfin's five segment types the cuts borrow;
  it has no "censored" type. Client mode only.
- **Age profile** — which profile to apply, e.g. `age-7`. Empty uses each
  file's default. An unknown id falls back to the strictest profile, never
  to none.
- **Lead-in override** — how early each cut is treated as starting. 0 uses the
  value stored in the edit list.

Tests:

```
cd jellyfin/Jellyfin.Plugin.CensorCut.Tests
dotnet test
```

## Why cuts start slightly early

Every integration begins a cut `leadInMs` *before* its real start, and the
reason differs by mode:

- **VLC and client-side Jellyfin** notice they have entered a cut only by
  checking the clock periodically, so they always notice late. Starting early
  gives up a fraction of a second of clean footage; starting on time would show
  the opening frames of the scene being removed.
- **Server-side Jellyfin** is frame-exact, but a concat `outpoint` includes the
  frame sitting exactly on it. With no lead-in, one or two frames of every cut
  survive — measured, not theorised. The lead-in absorbs that overshoot.

The amount is stored per edit list and can be overridden in the Jellyfin plugin.
