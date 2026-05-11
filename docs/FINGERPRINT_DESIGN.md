# Video fingerprinting — design narrative

How we arrived at the current v9 (audio-peak-gap) design, what we tried
and rejected, and why each pivot happened. Read this before changing
the fingerprinting algorithm — most of the cul-de-sacs are non-obvious
and we hit them in order.

## What the fingerprint has to do

The fingerprint is the lookup key for transferring user-authored cuts
across copies of a film. The hard requirements:

| Property | Why it matters |
|---|---|
| Same fingerprint when content is identical, different encode | Two encodes (codec, bitrate, resolution, container) of the same source must lookup the same edit pack |
| Different fingerprint when content differs (theatrical vs director's vs censored) | Edit packs target a specific cut; applying the wrong one would be silent corruption |
| Tolerant of intro/outro trim (head and/or tail) | Different distributions truncate logos/credits at different points; same body content should still match |
| No film titles or paths anywhere in the wire data | Privacy — the federated database can index by content fingerprint without ever knowing what film it is |
| Fast on network-drive sources | Users keep large libraries on NAS shares; a 5-minute fingerprint per file is not workable |
| Robust to audio remasters / different dubs (nice to have) | Same scenes at the same times → user's video-aligned cuts still apply |

## The cul-de-sacs

### Cul-de-sac 1: audio fingerprint as the primary identifier

Initial design used loud non-voice audio anchors with spectral
signatures and inter-anchor offsets. Deleted before launch when we
realised the fundamental mismatch: **edits are time ranges on a
video timeline**. If a user re-masters the audio (different mix,
different language dub, different bitrate), the audio fingerprint
changes — but the cuts they made are still applicable to the video.
We need the fingerprint to recognise *the cut of the film*, not the
audio mix. Switched to a video-content fingerprint.

### Cul-de-sac 2: dense-decode video fingerprint (v1)

The first video design decoded every source frame at 8 fps,
downscaled to 32×32 grayscale, computed pHash per frame, ran scene-
cut detection over the pHash sequence, took the top-100 cuts, and
hashed a tau-normalised position+pHash sequence into a single sha256
digest.

Looked great in tests. Then we tried it on a 2-hour 1080p film on a
network share. Wall time: **>1 minute**. The reason: even with
hardware decode running at 1000+ frames/sec, ffmpeg has to *read
every byte of the file* to traverse the bitstream and feed the
filter chain. For a 7 GB film at 50 MB/s SMB throughput, just the
read takes 140 sec — independent of how fast the GPU decodes.

Lesson: **decode work isn't the bottleneck on network sources, I/O
is.** The fix can't be "decode faster".

### Cul-de-sac 3: subsample at 2 fps (still v1)

`ffmpeg -vf fps=2` outputs 2 frames per second. We hoped that meant
decoding only 2 fps worth of frames, a 4× speedup over 8 fps.

It doesn't. The `fps` filter operates on already-decoded frames; it
discards the rest. ffmpeg still decodes every source frame (because
P/B frames depend on neighbours; you can't skip them without
breaking the codec state). I/O cost unchanged.

Lesson: **almost no ffmpeg sampling trick reduces I/O. They reduce
post-decode work, which isn't where the time goes.**

### Cul-de-sac 4: multi-sector via fractional seeks (v3)

Container-level seeks (`-ss BEFORE -i`) jump to a byte offset via
the file's index without traversing prior bytes. So: instead of
streaming the whole file, do N small seek-decode operations at
fixed body fractions (5%, 15%, ..., 95%), grab one frame at each,
hash it, build a sequence.

Real wall time: ~6 sec for 2hr 1080p over the network. Massive win
on speed.

But the digest depended on the body window (defined as
`cushion = 5% of duration`), which means when total duration changes
under intro trim, the body window shifts in source-time. A 22-sec
intro trim shifted per-anchor source content by ~30 sec at the
midpoint — completely different content sampled, completely
different digest.

Tested on a known intro-only trim pair: **digest didn't match**.

Lesson: **fixed-fraction body windows are not trim-tolerant.**

### Cul-de-sac 5: better per-anchor frame picking (v4, v5, v6)

Three variants of the multi-sector idea, each picking the
"most distinctive" frame within a small candidate window per anchor:

- **v4 — peer-relative outlier**: pick the candidate whose pHash
  bit-pattern is furthest from the bitwise majority of its window's
  candidates. Pseudo-scene-cut detection.
- **v5 — absolute pixel variance**: pick the candidate with highest
  pixel variance (sharpest content, least motion blur).
- **v6 — scene cut by pHash diff**: pick the candidate whose pHash
  has the largest Hamming distance from the immediately-previous
  candidate.

All three still used the fixed-fraction body window. All three
failed the same trim test in the same way as v3, for the same
reason. Per-anchor "distinctive frame" picking solves a different
problem than body-window-stability.

Lesson: **the body window matters more than per-anchor picking.**

### Cul-de-sac 6: audio-anchored body, fixed-fraction sampling (v7, v8)

Replace the time-fraction body endpoints with audio-content-derived
ones. Find the loudest peak in `[3 min, 15 min]` of the file →
`body_start`. Find the loudest peak in `[duration-15 min,
duration-5 min]` → `body_end`. Sample at fixed fractions inside.

Looked principled. Failed the trim test anyway. Why: **the search
windows themselves are file-time-relative.** When the file is
trimmed by 22 sec, the search range `[3 min, 15 min]` corresponds to
content-time `[3:22, 15:22]` in the trimmed copy. The loudest peak
in the trimmed search range can be a *different content event* than
the loudest peak in the original's search range, because the 22 sec
boundary shift can include or exclude one of the candidates.

Lesson: **any file-relative search window shifts under trim.**
Anchoring to "the loudest peak in this file region" is not the same
as anchoring to "the loudest peak in this content region."

## What actually works (v9)

The breakthrough: **don't use a body window at all.** Find audio
peaks throughout the file and use the *gaps between them* as the
fingerprint. Every peak is a content event (loud non-voice sound),
and every peak shifts by the same amount under any trim, so the
inter-peak gaps are content-stable.

### Algorithm

```
1. Decode audio
   ffmpeg -af "pan=mono|c0=0.5*c0-0.5*c1" -ar 8000 -ac 1 -f s16le -
   Side channel L−R for dub resilience (kills center-panned
   dialogue, preserves music + panned sound effects). Falls back
   to plain mono for mono sources.

2. RMS energy at 100 ms windows. 5-second rolling mean to smooth
   per-sample noise.

3. Greedy non-maximum-suppression peak picking inside the body
   region (5%-95% of the file): pick the loudest sample, mask
   ±30 sec, repeat until K=25 peaks selected or no candidates
   remain.

4. Sort peaks by time.

5. For each peak, decode 5 frames at 5 fps over a 1-second window
   centred on the peak. Average the 5 frames in the float-pixel
   domain before computing the pHash. Averaging smooths motion-
   blur and codec quantization noise — single-frame pHash at audio
   peaks is noisy because peaks coincide with action moments where
   per-frame quantization differs across encodes.

6. Build the fingerprint:
   - peaks: list of (tMs, phash) — 25 entries
   - gapsMs: list of consecutive gaps (24 entries)
   - innerSpanMs: gap between 2nd and 2nd-to-last peak — robust
                   index field even if one peak is dropped at
                   either edge from intro/outro trim
```

### Matching (fuzzy, not exact-digest)

There is no sha256 digest. Identity is the raw `peaks` list. Two
fingerprints match via `match_fingerprints(fp_a, fp_b)`:

- Pair peaks 1-to-1 by index, comparing only the overlapping prefix
  if K differs slightly.
- A gap is *matched* if the corresponding gaps differ by ≤ 5 sec.
- A pHash is *matched* if the Hamming distance is ≤ 20 / 64 bits.
- Verdict: same film if ≥ 55% of gaps AND ≥ 40% of pHashes match.
- Side product: `estimatedTrimMs` = median of paired peak time
  diffs. Tells the editor by how much to shift cut times when
  applying an edit pack from a differently-trimmed copy.

### Why fuzzy and not exact-digest

Tested an exact-digest version with 1-second-bucketed gaps: failed
on a known same-content pair because two of 14 gaps crossed the
1-second bucket boundary by < 100 ms. SHA-256 is a cliff edge: one
input bit flip → totally different output. Bucket-boundary fragility
is unavoidable when comparing values that vary by sub-bucket
amounts. Fuzzy matching with explicit tolerance bands is the right
primitive.

### Why these specific tolerances

Settled empirically on real test content. The same-content
discrimination margin is sharp:

| Comparison kind | Gap match % | pHash match % |
|---|---:|---:|
| Same cut, identical encode | 100 | 100 |
| Same cut, different encode | 100 | 100 |
| Same cut, intro trim | 70-95 | 80-95 |
| Different cuts of same film | 0-5 | 0-15 |
| Different films | 0-5 | 0 |

The gap between matched (≥70%) and not-matched (≤5%) is enormous —
no realistic risk of false positives at the current 55%/40%
thresholds, even on a 13,000-pair cross-comparison of a real film
library.

### Why these specific design choices

- **Side channel L−R audio**: in stereo film mixes, dialogue is
  typically center-panned. The Side channel kills 90%+ of dialogue
  while preserving music and panned sound effects. This makes the
  fingerprint partially dub-resilient — different dubs share their
  M&E (Music & Effects) stem, so they share their Side channel
  audio peaks too.

- **8 kHz mono**: 16 KB/s = ~115 MB for a 2-hour film. ~30× less
  I/O than the full audio, ~60× less than the video. On a network
  share at 50 MB/s, ~2.3 sec to read.

- **5-sec rolling mean before peak picking**: peak position is
  bit-identical across encodes only down to the smoothing
  resolution. Without smoothing, audio re-encoding can shift a
  peak's argmax by 100-500 ms. After smoothing, sub-100ms.

- **30-sec minimum peak spacing**: forces peaks to spread across
  the body. A 90-min film has 25 peaks spaced ≥30 sec apart →
  realistic inter-peak gaps in the 60-300 sec range. Two unrelated
  films would have to coincide on 14 of 24 gaps to ±5 sec each;
  the joint probability is astronomically small.

- **K = 25, top-K by smoothed amplitude**: smaller K (e.g., 15)
  failed when ranking-borderline peaks shuffled in/out across
  encodes. With K = 25, the deeper set has more redundancy, and
  the matcher tolerates ±4 peak-count differences.

- **5-frame averaging for pHash**: single-frame pHash at audio
  peaks is unreliable because peaks coincide with motion moments
  (gunshot, hit, music sting). Different encoders blur differently.
  Averaging across 1 sec / 5 frames cancels per-encoder quantization
  noise. This was the single biggest pHash-stability improvement.

- **innerSpanMs as the server index** (not approxDurationMin):
  duration is fragile when intro/outro lengths differ across
  releases. The gap between the 2nd and 2nd-to-last peaks is a
  content-anchored quantity that survives losing one peak at
  either edge. Server can index by `floor(innerSpanMs / 60_000)`
  for a coarse minute-bucket lookup.

## What this design does NOT solve

- **Re-shot scenes**: a director's cut that re-shoots one scene
  produces different audio peaks at the affected timestamps →
  different gap pattern → different fingerprint. This is correct
  behaviour: re-shot content *is* different content.

- **Aggressive audio remix**: if a re-master genuinely changes
  which moments are the loudest peaks (e.g., heavily compressed
  remix vs original dynamic mix), the top-K set can differ. Side
  channel partially mitigates by emphasising the preserved M&E
  stem, but a fully-remastered audio track is a real edge case.

- **Mono sources**: the Side channel falls back to plain mono,
  which doesn't suppress dialogue. Dub-resilience is lost for
  mono content. Mono is rare for modern films.

- **Films with very few loud non-voice events**: e.g., a quiet
  arthouse film. Peak picking might find fewer than K candidates
  above noise. The matcher tolerates K differences but at some
  point the fingerprint is too sparse to discriminate.

- **Fingerprint collisions across very-similar genre content**:
  not seen in real-library testing but theoretically possible.
  Mitigation: combine gap match with pHash match (already done) —
  joint coincidence is much rarer than either alone.

## Open follow-ups

See bd issues for the live list. As of this writing:

- *Tighten same-content match percentage* — incremental tuning
  for the matched-pair % to land closer to 100% rather than the
  current 70-95% on trim cases. Levers: raise K to 35-40, switch
  from top-K rank to threshold-based peak detection (mean+1.5σ),
  approximate-time pairing instead of 1-to-1 by index, stronger
  pHash variants. Don't pre-tighten — diminishing returns vs the
  clean separation we already have between MATCH and DIFFER.

- *Edits-server schema migration* — the M8.5 server currently
  stores `(tau, phash)` anchors per pack. Migrate to v9's
  `(tMs, phash)` peak shape + `gapsMs` array + `innerSpanMs` index.

- *Editor integration* — promote `video_fingerprint_v9_peakgaps`
  from prototype to the production fingerprint, replace the
  current `video_fingerprint.py` callsite, surface
  `estimatedTrimMs` in the edits-pull dialog so the user can see
  by how much imported cuts are being shifted.

## Summary diagram

```
   Source video file (anywhere on local disk or network share)
   ─┬───────────────────────────────────────────────────────────
    │
    │  ffmpeg pan=L-R, ar=8000, mono, s16le      (~120 MB for 2hr,
    │                                              ~2.5s on network)
    ▼
   8 kHz mono PCM
    │
    │  100ms RMS, 5s rolling mean
    ▼
   Smoothed energy series
    │
    │  greedy NMS top-K=25 in body region [5%-95%]
    │  with min spacing 30 sec
    ▼
   25 peak times, sorted ascending
    │
    │  per peak: ffmpeg -ss seek + decode 5 frames @ 5 fps
    │            → average in pixel space → 8x8 DCT block
    │            → drop DC → median threshold → 64-bit pHash
    ▼
   25 (tMs, phash) anchors + 24 gapsMs + innerSpanMs

                                ▲
                                │
   Server index: innerSpanMs bucketed to nearest 60 sec.
                                │
                                ▼
   Pairwise fuzzy match:
     - 1-to-1 peak pairing by index (tolerate ±4 missing)
     - per-gap tolerance ±5 sec
     - per-pHash tolerance Hamming ≤ 20 / 64
     - same film if ≥55% gaps AND ≥40% pHashes match
     - estimatedTrimMs = median(paired peak diffs)
```
