--[[
Tests for the pure logic in censorcut.lua — JSON parsing, URI handling,
profile selection, and the skip decision. No VLC required.

  lua players/vlc/test_censorcut.lua

VLC embeds Lua but ships no standalone interpreter, so this needs lua 5.1+
installed separately. It is written to run anywhere, including CI.
]]

local failures, checks = 0, 0

local function check(cond, label)
    checks = checks + 1
    if not cond then
        failures = failures + 1
        io.stderr:write("FAIL: " .. label .. "\n")
    end
end

local function eq(got, want, label)
    checks = checks + 1
    if got ~= want then
        failures = failures + 1
        io.stderr:write(string.format("FAIL: %s (got %s, want %s)\n",
                                      label, tostring(got), tostring(want)))
    end
end

-- Minimal vlc stub: the module only touches vlc.msg at load time.
vlc = {
    msg = { info = function() end, warn = function() end, dbg = function() end },
    misc = { mdate = function() return 0 end, mwait = function() end },
    var = { get = function() return 0 end, set = function() end },
    object = { input = function() return nil end },
    input = { item = function() return nil end },
    volume = { get = function() return 100 end },
}

local here = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
_CENSORCUT_TEST = true
local cc = dofile(here .. "/censorcut.lua")
check(cc ~= nil, "module returns its internals under _CENSORCUT_TEST")

--=========================================================================
-- JSON
--=========================================================================
local sample = [[
{
  "schemaVersion": 1,
  "sourceFileName": "Example.mkv",
  "durationMs": 600000,
  "defaultProfileId": "age-7",
  "profiles": [
    { "id": "age-5", "label": "Age 5", "minAge": 5, "leadInMs": 150,
      "cuts": [ {"startMs": 10000, "endMs": 20000, "category": "Violence"},
                {"startMs": 50000, "endMs": 60000} ] },
    { "id": "age-7", "label": "Age 7", "minAge": 7, "leadInMs": 200,
      "cuts": [ {"startMs": 10000, "endMs": 20000} ] }
  ]
}
]]

local doc = cc.json_decode(sample)
check(doc ~= nil, "parses the edit list")
eq(doc.schemaVersion, 1, "schemaVersion")
eq(doc.sourceFileName, "Example.mkv", "sourceFileName")
eq(doc.defaultProfileId, "age-7", "defaultProfileId")
eq(#doc.profiles, 2, "profile count")
eq(doc.profiles[1].cuts[1].category, "Violence", "nested category")
eq(doc.profiles[1].cuts[2].endMs, 60000, "cut without a category still parses")

local bad = cc.json_decode("{ not json")
check(bad == nil or bad.profiles == nil,
      "malformed input yields no usable profiles rather than throwing")
eq(cc.json_decode('{"a":"x\\ny"}').a, "x\ny", "escape sequences")
eq(cc.json_decode('{"a":"\\u00e9"}').a, "\195\169", "unicode escape to utf-8")

--=========================================================================
-- URI handling
--=========================================================================
eq(cc.uri_to_path("file:///D:/films/A%20Film.mkv"), "D:/films/A Film.mkv",
   "windows uri with a percent-encoded space")
eq(cc.uri_to_path("file:///home/u/films/Film.mkv"), "/home/u/films/Film.mkv",
   "unix uri keeps its leading slash")
eq(cc.uri_to_path("http://example.com/stream.m3u8"), nil,
   "non-file uris are ignored")
eq(cc.uri_to_path(nil), nil, "nil uri is safe")

--=========================================================================
-- Profile selection
--=========================================================================
eq(cc.pick_profile(doc).id, "age-7", "defaults to defaultProfileId")

local no_default = cc.json_decode(sample)
no_default.defaultProfileId = "age-99"
eq(cc.pick_profile(no_default).id, "age-5",
   "an unknown profile id falls back to the strictest, never to no censoring")

eq(cc.pick_profile(nil), nil, "missing edit list yields no profile")
eq(cc.pick_profile({ profiles = {} }), nil, "empty profile list yields none")

--=========================================================================
-- Time-unit calibration
--=========================================================================
eq(cc.detect_scale(30 * 1000 * 1000, 600), 1000, "microseconds detected")
eq(cc.detect_scale(30 * 1000 * 1000, 5), 1, "value too large for us -> already ms")
eq(cc.detect_scale(0, 600), nil, "no reading yet -> retry later")

--=========================================================================
-- Skip decision
--=========================================================================
cc.state.lead = 150
cc.state.cuts = { { s = 10000, e = 20000 }, { s = 50000, e = 60000 } }

eq(cc.cut_at(5000), nil, "well before a cut")
eq(cc.cut_at(9000), nil, "before the lead-in window")
check(cc.cut_at(9900) ~= nil, "inside the lead-in window jumps early")
check(cc.cut_at(15000) ~= nil, "inside a cut")
eq(cc.cut_at(20000), nil, "exactly at the end is already clear (half-open)")
eq(cc.cut_at(30000), nil, "between cuts")
check(cc.cut_at(55000) ~= nil, "inside the second cut")
eq(cc.cut_at(9900).e, 20000, "jump target is the cut end")

-- The lead-in must never be so eager that it swallows the gap between two
-- cuts placed back to back with a short gap.
cc.state.cuts = { { s = 10000, e = 20000 }, { s = 20050, e = 30000 } }
eq(cc.cut_at(20000).e, 30000, "a short gap inside the lead-in jumps to the far end")

print(string.format("%d checks, %d failures", checks, failures))
os.exit(failures == 0 and 0 or 1)
