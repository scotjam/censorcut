--[[
CensorCut — VLC interface script.

Watches playback and skips past every cut in the movie's edit list, so a
censored viewing needs no re-encoded copy.

Install:
  Linux/macOS  ~/.local/share/vlc/lua/intf/censorcut.lua
  Windows      %APPDATA%\vlc\lua\intf\censorcut.lua

Run (either):
  vlc --extraintf=luaintf --lua-intf=censorcut
  or Tools > Preferences > All > Interface > Main interfaces > Lua
     -> Lua interface = censorcut, and tick "Lua interface" under
        Main interfaces > Extra interface modules.

It reads "<movie>.censorcut-edl.json" beside the file being played, picks the
profile named by CENSORCUT_PROFILE (or the file's defaultProfileId), and jumps
over each cut.

Accuracy note: this polls, so it always notices entry into a cut late. It
therefore jumps leadInMs BEFORE each cut starts, giving up a fraction of a
second of clean footage rather than showing a frame of a cut. That direction
is deliberate — see leadInMs in the edit list.
]]

local POLL_MS       = 50    -- how often to check position
local REARM_GUARD_MS = 400  -- ignore a cut we just jumped out of, to avoid loops

--=========================================================================
-- Small JSON reader.
-- VLC's Lua sandbox has no JSON library on all platforms (vlc.json exists in
-- some builds only), so parse the subset we emit ourselves.
--=========================================================================
local function skip_ws(s, i)
    local _, j = s:find("^[ \t\r\n]*", i)
    return (j or i - 1) + 1
end

local parse_value

local function parse_string(s, i)
    -- i points at the opening quote
    local out, j = {}, i + 1
    while j <= #s do
        local c = s:sub(j, j)
        if c == '"' then return table.concat(out), j + 1 end
        if c == '\\' then
            local e = s:sub(j + 1, j + 1)
            if     e == 'n' then out[#out + 1] = '\n'
            elseif e == 't' then out[#out + 1] = '\t'
            elseif e == 'r' then out[#out + 1] = '\r'
            elseif e == 'b' then out[#out + 1] = '\b'
            elseif e == 'f' then out[#out + 1] = '\f'
            elseif e == 'u' then
                local hex = s:sub(j + 2, j + 5)
                local cp = tonumber(hex, 16) or 63
                -- Only the BMP subset we could plausibly emit (labels,
                -- category names). Anything above 0xFFFF is not represented
                -- in our writer's output.
                if cp < 0x80 then
                    out[#out + 1] = string.char(cp)
                elseif cp < 0x800 then
                    out[#out + 1] = string.char(0xC0 + math.floor(cp / 0x40),
                                                0x80 + (cp % 0x40))
                else
                    out[#out + 1] = string.char(0xE0 + math.floor(cp / 0x1000),
                                                0x80 + (math.floor(cp / 0x40) % 0x40),
                                                0x80 + (cp % 0x40))
                end
                j = j + 4
            else out[#out + 1] = e end
            j = j + 2
        else
            out[#out + 1] = c
            j = j + 1
        end
    end
    return nil, j
end

local function parse_number(s, i)
    local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
    if not num then return nil, i end
    return tonumber(num), i + #num
end

local function parse_array(s, i)
    local arr = {}
    i = skip_ws(s, i + 1)
    if s:sub(i, i) == ']' then return arr, i + 1 end
    while true do
        local v
        v, i = parse_value(s, skip_ws(s, i))
        arr[#arr + 1] = v
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == ',' then i = i + 1
        elseif c == ']' then return arr, i + 1
        else return arr, i end
    end
end

local function parse_object(s, i)
    local obj = {}
    i = skip_ws(s, i + 1)
    if s:sub(i, i) == '}' then return obj, i + 1 end
    while true do
        i = skip_ws(s, i)
        if s:sub(i, i) ~= '"' then return obj, i end
        local key
        key, i = parse_string(s, i)
        i = skip_ws(s, i)
        if s:sub(i, i) ~= ':' then return obj, i end
        local v
        v, i = parse_value(s, skip_ws(s, i + 1))
        obj[key] = v
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == ',' then i = i + 1
        elseif c == '}' then return obj, i + 1
        else return obj, i end
    end
end

parse_value = function(s, i)
    local c = s:sub(i, i)
    if c == '{' then return parse_object(s, i) end
    if c == '[' then return parse_array(s, i) end
    if c == '"' then return parse_string(s, i) end
    if s:sub(i, i + 3) == 'true'  then return true,  i + 4 end
    if s:sub(i, i + 4) == 'false' then return false, i + 5 end
    if s:sub(i, i + 3) == 'null'  then return nil,   i + 4 end
    return parse_number(s, i)
end

local function json_decode(text)
    local ok, value = pcall(function()
        local v = parse_value(text, skip_ws(text, 1))
        return v
    end)
    if ok then return value end
    return nil
end

--=========================================================================
-- Locating the edit list for the item being played
--=========================================================================

--- VLC hands us a URI; turn "file:///D:/films/A%20Film.mkv" into a path.
local function uri_to_path(uri)
    if not uri then return nil end
    local path = uri
    if path:sub(1, 8) == "file:///" then
        path = path:sub(9)
        -- On Unix the leading slash is part of the path; on Windows the
        -- component already starts with a drive letter.
        if not path:match("^%a:") then path = "/" .. path end
    elseif path:sub(1, 7) == "file://" then
        path = path:sub(8)
    else
        return nil  -- not a local file; nothing to skip
    end
    path = path:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)
    return path
end

local function read_file(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local data = f:read("*all")
    f:close()
    return data
end

local function load_edit_list(media_path)
    if not media_path then return nil end
    local text = read_file(media_path .. ".censorcut-edl.json")
    if not text then return nil end
    return json_decode(text)
end

local function pick_profile(list)
    if not list or type(list.profiles) ~= "table" then return nil end
    local wanted = os.getenv("CENSORCUT_PROFILE") or list.defaultProfileId
    if wanted then
        for _, p in ipairs(list.profiles) do
            if p.id == wanted then return p end
        end
    end
    -- No match: fall back to the strictest (youngest) profile rather than
    -- none, so a stale profile id can't silently disable censoring.
    local best
    for _, p in ipairs(list.profiles) do
        if not best or (tonumber(p.minAge) or 0) < (tonumber(best.minAge) or 0) then
            best = p
        end
    end
    return best
end

--=========================================================================
-- Playback monitoring
--=========================================================================

local state = {
    uri      = nil,
    cuts     = {},          -- sorted array of {startMs, endMs}
    lead     = 150,
    scale    = nil,         -- divisor turning VLC's "time" into ms
    last_jump_to = nil,
    last_jump_at = nil,
}

--- VLC 3 reports input "time" in microseconds, but that has differed across
--- versions. Rather than assume, calibrate against the item's duration (which
--- vlc.input.item():duration() reports in seconds).
local function detect_scale(raw_time, duration_s)
    if not duration_s or duration_s <= 0 then return 1000 end  -- assume us->ms
    if not raw_time or raw_time <= 0 then return nil end       -- try again later
    local as_us = raw_time / 1000.0
    if as_us <= duration_s * 1000.0 * 1.5 then return 1000 end
    return 1  -- already milliseconds
end

local function load_for_current_item()
    state.uri, state.cuts, state.scale = nil, {}, nil
    local item = vlc.input.item()
    if not item then return end

    state.uri = item:uri()
    local path = uri_to_path(state.uri)
    local profile = pick_profile(load_edit_list(path))
    if not profile then
        vlc.msg.info("[censorcut] no edit list for " .. tostring(path))
        return
    end

    state.lead = tonumber(profile.leadInMs) or 150
    for _, c in ipairs(profile.cuts or {}) do
        local s, e = tonumber(c.startMs), tonumber(c.endMs)
        if s and e and e > s then state.cuts[#state.cuts + 1] = { s = s, e = e } end
    end
    table.sort(state.cuts, function(a, b) return a.s < b.s end)
    vlc.msg.info(string.format("[censorcut] %d cuts loaded (profile %s)",
                               #state.cuts, tostring(profile.id)))
end

--- The cut that position `ms` is inside of, or about to enter within `lead`.
local function cut_at(ms)
    for _, c in ipairs(state.cuts) do
        if ms < c.e and ms >= (c.s - state.lead) then return c end
        if c.s > ms + state.lead then break end  -- sorted; nothing closer
    end
    return nil
end

local function tick()
    local input = vlc.object.input()
    if not input then return end

    local raw = vlc.var.get(input, "time")
    if not raw then return end

    if not state.scale then
        local item = vlc.input.item()
        state.scale = detect_scale(raw, item and item:duration() or nil)
        if not state.scale then return end
    end

    local ms = raw / state.scale
    local c = cut_at(ms)
    if not c then return end

    -- Don't re-trigger on the cut we just left: after a jump VLC may report a
    -- stale time for a tick or two, which would otherwise jump repeatedly.
    local now = vlc.misc.mdate() / 1000
    if state.last_jump_to == c.e and state.last_jump_at
       and (now - state.last_jump_at) < REARM_GUARD_MS then
        return
    end

    vlc.msg.dbg(string.format("[censorcut] skipping %d -> %d ms", math.floor(ms), c.e))
    vlc.var.set(input, "time", math.floor(c.e * state.scale))
    state.last_jump_to = c.e
    state.last_jump_at = now
end

--=========================================================================
-- Test hook: when loaded by test_censorcut.lua the chunk returns its internals
-- and stops here, so the logic can be exercised without VLC or a real movie.
--=========================================================================
if _CENSORCUT_TEST then
    return {
        json_decode   = json_decode,
        uri_to_path   = uri_to_path,
        load_edit_list = load_edit_list,
        pick_profile  = pick_profile,
        detect_scale  = detect_scale,
        cut_at        = cut_at,
        state         = state,
    }
end

--=========================================================================
-- Main loop
--=========================================================================
vlc.msg.info("[censorcut] interface started")

while true do
    if vlc.volume.get() == nil then break end  -- VLC is shutting down

    local item = vlc.input.item()
    local uri = item and item:uri() or nil
    if uri ~= state.uri then
        load_for_current_item()
    end

    if #state.cuts > 0 then
        local ok, err = pcall(tick)
        if not ok then vlc.msg.warn("[censorcut] " .. tostring(err)) end
    end

    vlc.misc.mwait(vlc.misc.mdate() + POLL_MS * 1000)
end
