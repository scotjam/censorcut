"""Fast keyframe-time extraction via direct container-metadata parse.

ffprobe's '-skip_frame nokey -show_frames=pts_time' walks the entire
bitstream through the demuxer + codec, which on a network share is
I/O bound at the network's effective throughput - measured at ~4 MB/s
on the project's home NAS, so ~7 min for a 1.8 GB file.

Every container we care about already records keyframe positions in
its index:

  MKV  -> Cues element (referenced from SeekHead at the start of the
          Segment).
  MP4  -> stss table inside moov.trak.mdia.minf.stbl (with stts giving
          per-sample durations).
  AVI  -> idx1 chunk listing all chunks plus AVIIF_KEYFRAME flags.

Reading just those structures takes a couple of small seeks and
~50 KB of network reads -- ~1000x less per MB than the bitstream walk.

This module exposes a single public entry point, extract_keyframe_times_ms,
that picks the right parser by sniffing the file's first bytes and
returns the list of keyframe timestamps in milliseconds. Unparseable
or unindexed files return None so the caller can fall back to ffprobe.
"""

from __future__ import annotations

import struct
import subprocess
from pathlib import Path
from typing import List, Optional, Tuple


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def extract_keyframe_times_ms(path: str) -> Optional[List[int]]:
    """Return sorted keyframe timestamps in ms, or None if the
    container's index can't be parsed by any of the direct parsers.
    The caller should fall back to ffprobe-based extraction in that
    case (see video_fingerprint_v11_keyframe._extract_keyframe_times_ms)."""
    try:
        with open(path, "rb") as f:
            head = f.read(16)
    except OSError:
        return None
    if len(head) < 12:
        return None

    # MKV / WebM: starts with EBML header 0x1A 0x45 0xDF 0xA3.
    if head.startswith(b"\x1A\x45\xDF\xA3"):
        try:
            return _mkv_cues_keyframe_times(path)
        except Exception:
            return None

    # MP4 / ISO BMFF: first 8 bytes are a box header; the type at
    # offset 4 is one of {ftyp, free, mdat, moov, ...} but a well
    # formed file starts with ftyp.
    if head[4:8] in (b"ftyp", b"moov", b"free", b"skip", b"mdat"):
        try:
            return _mp4_stss_keyframe_times(path)
        except Exception:
            return None

    # AVI: 'RIFF....AVI '
    if head[:4] == b"RIFF" and head[8:12] == b"AVI ":
        try:
            return _avi_idx1_keyframe_times(path)
        except Exception:
            return None

    return None


# ---------------------------------------------------------------------
# Matroska / WebM
# ---------------------------------------------------------------------

_MKV_EBML_HEADER       = 0x1A45DFA3
_MKV_SEGMENT           = 0x18538067
_MKV_SEEKHEAD          = 0x114D9B74
_MKV_SEEK              = 0x4DBB
_MKV_SEEKID            = 0x53AB
_MKV_SEEKPOSITION      = 0x53AC
_MKV_INFO              = 0x1549A966
_MKV_TIMESCALE         = 0x2AD7B1
_MKV_TRACKS            = 0x1654AE6B
_MKV_TRACKENTRY        = 0xAE
_MKV_TRACKNUMBER       = 0xD7
_MKV_TRACKTYPE         = 0x83
_MKV_CUES              = 0x1C53BB6B
_MKV_CUEPOINT          = 0xBB
_MKV_CUETIME           = 0xB3
_MKV_CUETRACKPOSITIONS = 0xB7
_MKV_CUETRACK          = 0xF7

_MKV_TRACKTYPE_VIDEO   = 1


def _ebml_read_id(buf: bytes, pos: int) -> Tuple[int, int]:
    """Read an EBML element ID. Element IDs keep the length-marker bit
    in the value (so the four registered Matroska IDs above stay as
    written constants).
    Returns (id_with_marker, bytes_consumed)."""
    first = buf[pos]
    if first == 0:
        raise ValueError("Invalid EBML id (leading zero)")
    length = 1
    mask = 0x80
    while not (first & mask):
        length += 1
        mask >>= 1
        if mask == 0 or length > 8:
            raise ValueError("Invalid EBML id length")
    value = 0
    for i in range(length):
        value = (value << 8) | buf[pos + i]
    return value, length


def _ebml_read_size(buf: bytes, pos: int) -> Tuple[int, int]:
    """Read an EBML data size. Length marker is stripped from the
    returned value.
    Returns (size, bytes_consumed)."""
    first = buf[pos]
    if first == 0:
        raise ValueError("Invalid EBML size (leading zero)")
    length = 1
    mask = 0x80
    while not (first & mask):
        length += 1
        mask >>= 1
        if mask == 0 or length > 8:
            raise ValueError("Invalid EBML size length")
    value = first & (mask - 1)
    for i in range(1, length):
        value = (value << 8) | buf[pos + i]
    return value, length


def _ebml_read_uint(buf: bytes, pos: int, size: int) -> int:
    out = 0
    for i in range(size):
        out = (out << 8) | buf[pos + i]
    return out


def _mkv_cues_keyframe_times(path: str) -> Optional[List[int]]:
    """Direct Matroska Cues parse. Reads the EBML header, then the
    Segment's SeekHead to find the Cues and Tracks offsets, identifies
    the video track number from Tracks, then walks each CuePoint and
    keeps only those that index the video track. Without this filter,
    Cues entries that index audio cluster boundaries would inflate the
    keyframe count and wreck cross-encode invariance (the same content
    re-encoded with different audio cadence has different audio
    CuePoints)."""
    with open(path, "rb") as f:
        head = f.read(64 * 1024)
        if not head.startswith(b"\x1A\x45\xDF\xA3"):
            return None

        # EBML header element.
        pos = 4
        ebml_size, sz_len = _ebml_read_size(head, pos)
        pos += sz_len + ebml_size

        # Segment element.
        seg_id, id_len = _ebml_read_id(head, pos)
        if seg_id != _MKV_SEGMENT:
            return None
        pos += id_len
        _, sz_len = _ebml_read_size(head, pos)
        pos += sz_len
        segment_data_start = pos

        # Walk Segment children to find SeekHead, Info, and Tracks
        # offsets. Tracks may also appear inline (rare), so handle both.
        timescale_ns = 1_000_000
        cues_offset_in_segment: Optional[int] = None
        tracks_offset_in_segment: Optional[int] = None
        inline_tracks_data: Optional[bytes] = None
        while pos + 16 < len(head):
            child_id, id_len = _ebml_read_id(head, pos)
            pos += id_len
            child_size, sz_len = _ebml_read_size(head, pos)
            pos += sz_len
            if child_id == _MKV_SEEKHEAD:
                cues_off, tracks_off = _mkv_walk_seekhead(
                    head, pos, child_size)
                if cues_off is not None:
                    cues_offset_in_segment = cues_off
                if tracks_off is not None:
                    tracks_offset_in_segment = tracks_off
            elif child_id == _MKV_INFO:
                timescale_ns = _mkv_extract_timescale(
                    head, pos, child_size, timescale_ns)
            elif child_id == _MKV_TRACKS:
                inline_tracks_data = head[pos:pos + child_size]
            pos += child_size
            if cues_offset_in_segment is not None and (
                    tracks_offset_in_segment is not None
                    or inline_tracks_data is not None):
                break

        if cues_offset_in_segment is None:
            return None

        # Resolve video track number. If we already have Tracks data
        # inline, use it; otherwise seek to the offset.
        video_track: Optional[int] = None
        if inline_tracks_data is not None:
            video_track = _mkv_find_video_track(inline_tracks_data)
        elif tracks_offset_in_segment is not None:
            f.seek(segment_data_start + tracks_offset_in_segment)
            tk_head = f.read(16)
            if len(tk_head) >= 8:
                tk_id, id_len = _ebml_read_id(tk_head, 0)
                if tk_id == _MKV_TRACKS:
                    tk_size, sz_len = _ebml_read_size(tk_head, id_len)
                    f.seek(segment_data_start + tracks_offset_in_segment
                           + id_len + sz_len)
                    tk_data = f.read(tk_size)
                    video_track = _mkv_find_video_track(tk_data)

        # Read Cues element.
        cues_abs = segment_data_start + cues_offset_in_segment
        f.seek(cues_abs)
        head2 = f.read(16)
        if len(head2) < 8:
            return None
        cues_id, id_len = _ebml_read_id(head2, 0)
        if cues_id != _MKV_CUES:
            return None
        cues_size, sz_len = _ebml_read_size(head2, id_len)
        f.seek(cues_abs + id_len + sz_len)
        cues_data = f.read(cues_size)

    return _mkv_walk_cues_for_times(cues_data, timescale_ns, video_track)


def _mkv_walk_seekhead(buf: bytes, start: int, size: int
                         ) -> Tuple[Optional[int], Optional[int]]:
    """Walk a SeekHead and return (cues_offset, tracks_offset) within
    the Segment data, or (None, None) for either if not referenced."""
    end = start + size
    p = start
    cues_off: Optional[int] = None
    tracks_off: Optional[int] = None
    while p + 4 < end:
        try:
            seek_id, id_len = _ebml_read_id(buf, p)
        except (IndexError, ValueError):
            break
        p += id_len
        seek_size, sz_len = _ebml_read_size(buf, p)
        p += sz_len
        if seek_id != _MKV_SEEK:
            p += seek_size
            continue
        target_id: Optional[int] = None
        target_pos: Optional[int] = None
        seek_end = p + seek_size
        q = p
        while q < seek_end:
            inner_id, iid_len = _ebml_read_id(buf, q)
            q += iid_len
            inner_size, isz_len = _ebml_read_size(buf, q)
            q += isz_len
            if inner_id == _MKV_SEEKID:
                target_id = _ebml_read_uint(buf, q, inner_size)
            elif inner_id == _MKV_SEEKPOSITION:
                target_pos = _ebml_read_uint(buf, q, inner_size)
            q += inner_size
        p += seek_size
        if target_id == _MKV_CUES and target_pos is not None:
            cues_off = target_pos
        elif target_id == _MKV_TRACKS and target_pos is not None:
            tracks_off = target_pos
    return cues_off, tracks_off


def _mkv_find_video_track(tracks_data: bytes) -> Optional[int]:
    """Walk Tracks for the first TrackEntry with TrackType == video,
    return its TrackNumber."""
    p = 0
    n = len(tracks_data)
    while p + 2 < n:
        try:
            elem_id, id_len = _ebml_read_id(tracks_data, p)
        except (IndexError, ValueError):
            break
        p += id_len
        elem_size, sz_len = _ebml_read_size(tracks_data, p)
        p += sz_len
        if elem_id != _MKV_TRACKENTRY:
            p += elem_size
            continue
        end = p + elem_size
        q = p
        track_number: Optional[int] = None
        track_type: Optional[int] = None
        while q < end:
            inner_id, iid_len = _ebml_read_id(tracks_data, q)
            q += iid_len
            inner_size, isz_len = _ebml_read_size(tracks_data, q)
            q += isz_len
            if inner_id == _MKV_TRACKNUMBER:
                track_number = _ebml_read_uint(tracks_data, q, inner_size)
            elif inner_id == _MKV_TRACKTYPE:
                track_type = _ebml_read_uint(tracks_data, q, inner_size)
            q += inner_size
        if (track_type == _MKV_TRACKTYPE_VIDEO
                and track_number is not None):
            return track_number
        p += elem_size
    return None


def _mkv_extract_timescale(buf: bytes, start: int, size: int,
                              default_ns: int) -> int:
    end = start + size
    p = start
    while p + 4 < end:
        inner_id, iid_len = _ebml_read_id(buf, p)
        p += iid_len
        inner_size, isz_len = _ebml_read_size(buf, p)
        p += isz_len
        if inner_id == _MKV_TIMESCALE:
            return _ebml_read_uint(buf, p, inner_size)
        p += inner_size
    return default_ns


def _mkv_walk_cues_for_times(cues_data: bytes, timescale_ns: int,
                                video_track: Optional[int]) -> List[int]:
    """Walk Cues. For each CuePoint, accept its CueTime only if at
    least one of its CueTrackPositions targets the video track. If
    video_track is None (we couldn't find Tracks), accept all
    CuePoints — better than failing entirely."""
    times_ms: List[int] = []
    p = 0
    n = len(cues_data)
    while p + 4 < n:
        try:
            elem_id, id_len = _ebml_read_id(cues_data, p)
        except (IndexError, ValueError):
            break
        p += id_len
        elem_size, sz_len = _ebml_read_size(cues_data, p)
        p += sz_len
        if elem_id != _MKV_CUEPOINT:
            p += elem_size
            continue
        end = p + elem_size
        q = p
        cue_time: Optional[int] = None
        targets_video = (video_track is None)
        while q < end:
            inner_id, iid_len = _ebml_read_id(cues_data, q)
            q += iid_len
            inner_size, isz_len = _ebml_read_size(cues_data, q)
            q += isz_len
            if inner_id == _MKV_CUETIME:
                cue_time = _ebml_read_uint(cues_data, q, inner_size)
            elif (inner_id == _MKV_CUETRACKPOSITIONS
                    and video_track is not None
                    and not targets_video):
                # Walk CueTrackPositions for CueTrack.
                ctp_end = q + inner_size
                r = q
                while r < ctp_end:
                    pos_id, pid_len = _ebml_read_id(cues_data, r)
                    r += pid_len
                    pos_size, psz_len = _ebml_read_size(cues_data, r)
                    r += psz_len
                    if pos_id == _MKV_CUETRACK:
                        ct = _ebml_read_uint(cues_data, r, pos_size)
                        if ct == video_track:
                            targets_video = True
                            break
                    r += pos_size
            q += inner_size
        if cue_time is not None and targets_video:
            times_ms.append((cue_time * timescale_ns) // 1_000_000)
        p += elem_size
    times_ms.sort()
    return times_ms


# ---------------------------------------------------------------------
# MP4 / ISO BMFF
# ---------------------------------------------------------------------

def _mp4_stss_keyframe_times(path: str) -> Optional[List[int]]:
    """Direct MP4 stss parse. Locates moov via top-level box scan
    (handles moov-at-end by seeking forward through mdat-sized boxes),
    then descends moov.trak.mdia.minf.stbl to read stss (sync sample
    list) and stts (sample-to-time deltas). Combines them into
    keyframe times in ms."""
    with open(path, "rb") as f:
        moov = _mp4_locate_moov(f)
        if moov is None:
            return None
        moov_data = moov

    # Descend moov -> trak (video) -> mdia -> minf -> stbl
    video_stbl = _mp4_find_video_stbl(moov_data)
    if video_stbl is None:
        return None
    stss_samples = _mp4_read_stss(video_stbl)
    if not stss_samples:
        return None
    timescale, durations = _mp4_read_stts(video_stbl,
                                            *_mp4_find_mdhd(moov_data))
    if timescale is None or not durations:
        return None

    # Build cumulative time table for each sample number (1-based in
    # MP4 spec; we'll use 0-based here and treat sample N's start as
    # cumulative duration of samples [0..N-1]).
    return _mp4_keyframe_times_from_stts(stss_samples, durations, timescale)


def _mp4_read_box_header(f) -> Optional[Tuple[bytes, int, int]]:
    """Read 8 (or 16, for size==1) bytes of MP4 box header at the
    current file offset. Returns (box_type, body_size_remaining,
    header_consumed)."""
    raw = f.read(8)
    if len(raw) < 8:
        return None
    size = struct.unpack(">I", raw[:4])[0]
    box_type = raw[4:8]
    consumed = 8
    if size == 1:
        # 64-bit size in next 8 bytes.
        ext = f.read(8)
        if len(ext) < 8:
            return None
        size = struct.unpack(">Q", ext)[0]
        consumed = 16
    elif size == 0:
        # Box extends to end of file. Treat as 0 for our walk -- caller
        # will give up.
        return (box_type, 0, consumed)
    body_size = size - consumed
    return (box_type, body_size, consumed)


def _mp4_locate_moov(f) -> Optional[bytes]:
    """Walk top-level MP4 boxes to find 'moov' and return its body."""
    f.seek(0)
    while True:
        pos_before = f.tell()
        hdr = _mp4_read_box_header(f)
        if hdr is None:
            return None
        box_type, body_size, consumed = hdr
        if box_type == b"moov":
            return f.read(body_size)
        if body_size < 0:
            return None
        # Empty boxes (body_size == 0) are valid — just skip the header.
        f.seek(pos_before + consumed + body_size)


def _mp4_iter_boxes(buf: bytes, start: int = 0, end: Optional[int] = None):
    """Iterate (box_type, body_offset, body_size) tuples within
    a region of an MP4 buffer."""
    if end is None:
        end = len(buf)
    p = start
    while p + 8 <= end:
        size = struct.unpack(">I", buf[p:p + 4])[0]
        box_type = buf[p + 4:p + 8]
        consumed = 8
        if size == 1:
            if p + 16 > end:
                break
            size = struct.unpack(">Q", buf[p + 8:p + 16])[0]
            consumed = 16
        if size < consumed:
            break
        body_off = p + consumed
        body_size = size - consumed
        if body_off + body_size > end:
            break
        yield box_type, body_off, body_size
        p = body_off + body_size


def _mp4_find_video_stbl(moov: bytes) -> Optional[bytes]:
    """Find the first track that's a video handler and return its
    stbl atom body."""
    for tk_type, tk_off, tk_size in _mp4_iter_boxes(moov):
        if tk_type != b"trak":
            continue
        trak = moov[tk_off:tk_off + tk_size]
        # Inside trak: mdia
        for md_type, md_off, md_size in _mp4_iter_boxes(trak):
            if md_type != b"mdia":
                continue
            mdia = trak[md_off:md_off + md_size]
            # Find hdlr to confirm it's video
            handler_type = None
            minf = None
            for ch_type, ch_off, ch_size in _mp4_iter_boxes(mdia):
                if ch_type == b"hdlr" and ch_size >= 12:
                    # FullBox (4 bytes), pre_defined (4), handler_type (4)
                    handler_type = mdia[ch_off + 8:ch_off + 12]
                elif ch_type == b"minf":
                    minf = mdia[ch_off:ch_off + ch_size]
            if handler_type != b"vide" or minf is None:
                continue
            # Inside minf: stbl
            for ch_type, ch_off, ch_size in _mp4_iter_boxes(minf):
                if ch_type == b"stbl":
                    return minf[ch_off:ch_off + ch_size]
    return None


def _mp4_find_mdhd(moov: bytes) -> Tuple[bytes, int]:
    """Return the mdhd box body + its absolute offset within moov,
    for the video track. mdhd carries the track timescale."""
    for tk_type, tk_off, tk_size in _mp4_iter_boxes(moov):
        if tk_type != b"trak":
            continue
        trak = moov[tk_off:tk_off + tk_size]
        for md_type, md_off, md_size in _mp4_iter_boxes(trak):
            if md_type != b"mdia":
                continue
            mdia = trak[md_off:md_off + md_size]
            handler_type = None
            mdhd_buf = b""
            for ch_type, ch_off, ch_size in _mp4_iter_boxes(mdia):
                if ch_type == b"hdlr" and ch_size >= 12:
                    handler_type = mdia[ch_off + 8:ch_off + 12]
                elif ch_type == b"mdhd":
                    mdhd_buf = mdia[ch_off:ch_off + ch_size]
            if handler_type == b"vide" and mdhd_buf:
                return (mdhd_buf, 0)
    return (b"", 0)


def _mp4_read_stss(stbl: bytes) -> List[int]:
    """Return list of 1-based sample numbers that are sync samples."""
    for box_type, body_off, body_size in _mp4_iter_boxes(stbl):
        if box_type != b"stss":
            continue
        # FullBox: version(1) + flags(3) + entry_count(4) + entries(4*N)
        if body_size < 8:
            return []
        entry_count = struct.unpack(
            ">I", stbl[body_off + 4:body_off + 8])[0]
        out = []
        ent_off = body_off + 8
        for i in range(entry_count):
            if ent_off + 4 > body_off + body_size:
                break
            out.append(struct.unpack(
                ">I", stbl[ent_off:ent_off + 4])[0])
            ent_off += 4
        return out
    return []


def _mp4_read_stts(stbl: bytes, mdhd: bytes, _unused: int
                    ) -> Tuple[Optional[int], List[int]]:
    """Return (timescale, per_sample_durations). Reads stts as a
    run-length-encoded sample-to-duration list, expands to a flat list
    indexed 1..N."""
    timescale: Optional[int] = None
    if len(mdhd) >= 24:
        # mdhd is the BOX BODY only (header already consumed by the
        # iterator). Body layout: version(1) + flags(3) + body data.
        version = mdhd[0]
        if version == 1:
            # creation_time(8) + modification_time(8) + timescale(4) + ...
            # offset = 4 (version+flags) + 8 + 8 = 20
            timescale = struct.unpack(">I", mdhd[20:24])[0]
        else:
            # creation_time(4) + modification_time(4) + timescale(4) + ...
            # offset = 4 (version+flags) + 4 + 4 = 12
            timescale = struct.unpack(">I", mdhd[12:16])[0]
    durations: List[int] = []
    for box_type, body_off, body_size in _mp4_iter_boxes(stbl):
        if box_type != b"stts":
            continue
        if body_size < 8:
            return (timescale, [])
        entry_count = struct.unpack(
            ">I", stbl[body_off + 4:body_off + 8])[0]
        ent_off = body_off + 8
        for _ in range(entry_count):
            if ent_off + 8 > body_off + body_size:
                break
            sample_count = struct.unpack(
                ">I", stbl[ent_off:ent_off + 4])[0]
            sample_delta = struct.unpack(
                ">I", stbl[ent_off + 4:ent_off + 8])[0]
            ent_off += 8
            durations.extend([sample_delta] * sample_count)
        return (timescale, durations)
    return (timescale, durations)


def _mp4_keyframe_times_from_stts(stss: List[int],
                                    durations: List[int],
                                    timescale: int) -> List[int]:
    """Given stss (1-based sample numbers) and durations (per-sample
    deltas in timescale units, indexed 0..N-1), return keyframe
    timestamps in milliseconds."""
    if not durations or timescale <= 0:
        return []
    # Cumulative start time per sample (1-based: sample i's start time
    # is cumulative[0] + ... + cumulative[i-1]).
    cumulative: List[int] = []
    total = 0
    for d in durations:
        cumulative.append(total)
        total += d
    out: List[int] = []
    for sn in stss:
        idx = sn - 1
        if 0 <= idx < len(cumulative):
            ticks = cumulative[idx]
            out.append((ticks * 1000) // timescale)
    out.sort()
    return out


# ---------------------------------------------------------------------
# AVI
# ---------------------------------------------------------------------

_AVIIF_KEYFRAME = 0x00000010


def _avi_idx1_keyframe_times(path: str) -> Optional[List[int]]:
    """Direct AVI idx1 parse. AVI is a RIFF tree:
       RIFF AVI -> LIST hdrl -> avih (header)
                                strl (per-stream)
                                  strh (stream header)
                -> LIST movi (chunks)
                -> idx1 (entries: chunk_id(4) flags(4) offset(4) size(4))

    The stream header gives us frame rate (dwScale / dwRate). The
    idx1 entries with AVIIF_KEYFRAME flag are keyframes; their order
    in the index matches frame order, so the i-th video keyframe in
    idx1 is at time i / fps."""
    with open(path, "rb") as f:
        riff = f.read(12)
        if len(riff) < 12 or riff[:4] != b"RIFF" or riff[8:12] != b"AVI ":
            return None
        # Parse top-level RIFF children to find 'hdrl' LIST and 'idx1'
        # chunk. We need stream-header dwScale / dwRate for fps.
        scale: Optional[int] = None
        rate: Optional[int] = None
        idx1_offset: Optional[int] = None
        idx1_size: Optional[int] = None

        # File size.
        file_end = Path(path).stat().st_size

        pos = 12
        while pos + 8 < file_end:
            f.seek(pos)
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            chunk_id = hdr[:4]
            chunk_size = struct.unpack("<I", hdr[4:8])[0]
            data_pos = pos + 8
            if chunk_id == b"LIST":
                list_type = f.read(4)
                if list_type == b"hdrl":
                    sc, rt = _avi_walk_hdrl(f, data_pos + 4,
                                              chunk_size - 4)
                    if sc is not None:
                        scale = sc
                        rate = rt
            elif chunk_id == b"idx1":
                idx1_offset = data_pos
                idx1_size = chunk_size
            # AVI chunks are 2-byte aligned.
            pos = data_pos + chunk_size + (chunk_size & 1)
            if idx1_offset is not None and scale is not None:
                break

        if idx1_offset is None or scale is None or rate is None or rate <= 0:
            return None
        fps = rate / scale
        if fps <= 0 or fps > 1000:
            return None

        f.seek(idx1_offset)
        idx1 = f.read(idx1_size)

    # Parse idx1 entries: 16 bytes each.
    n_entries = len(idx1) // 16
    times_ms: List[int] = []
    video_index = 0
    for i in range(n_entries):
        e = idx1[i * 16:(i + 1) * 16]
        chunk_id = e[:4]
        flags = struct.unpack("<I", e[4:8])[0]
        # Video chunks are '##dc' or '##db' where ## is the stream
        # number in ascii-hex (typically '00' for the first stream).
        if chunk_id[2:4] not in (b"dc", b"db", b"wb"):
            # Not a video chunk for any stream; skip.
            continue
        if chunk_id[2:4] == b"wb":
            # Audio chunk.
            continue
        if flags & _AVIIF_KEYFRAME:
            times_ms.append(int(video_index * 1000.0 / fps))
        video_index += 1
    times_ms.sort()
    return times_ms


def _avi_walk_hdrl(f, start: int, size: int
                    ) -> Tuple[Optional[int], Optional[int]]:
    """Inside the LIST hdrl of an AVI, find the first strl whose
    strh has fccType == 'vids', and return its (dwScale, dwRate)."""
    f.seek(start)
    data = f.read(size)
    p = 0
    while p + 8 < len(data):
        chunk_id = data[p:p + 4]
        chunk_size = struct.unpack("<I", data[p + 4:p + 8])[0]
        if chunk_id == b"LIST":
            list_type = data[p + 8:p + 12]
            if list_type == b"strl":
                # Walk inside strl for strh.
                inner_start = p + 12
                inner_end = p + 8 + chunk_size
                ip = inner_start
                while ip + 8 < inner_end:
                    sub_id = data[ip:ip + 4]
                    sub_size = struct.unpack(
                        "<I", data[ip + 4:ip + 8])[0]
                    if sub_id == b"strh" and sub_size >= 32:
                        fcc_type = data[ip + 8:ip + 12]
                        if fcc_type == b"vids":
                            scale = struct.unpack(
                                "<I", data[ip + 28:ip + 32])[0]
                            rate = struct.unpack(
                                "<I", data[ip + 32:ip + 36])[0]
                            return scale, rate
                    ip += 8 + sub_size + (sub_size & 1)
        p += 8 + chunk_size + (chunk_size & 1)
    return None, None
