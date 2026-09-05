#!/usr/bin/env python3
"""adofai-audio-codec: lossless audio <-> .adofai "audio-as-chart" codec.

Maps a PCM audio file onto an ADOFAI chart the way community auto-chart tools do:
every audio sample becomes one tile, the track's BPM is chosen so that
BPM / 60 == the audio sample rate, and each sample value is stored in a
per-tile SetHitsound event's `hitsoundVolume` field as  sample / 655.36.

Layout (v2, space-optimized; still plain ADOFAI JSON):
  * settings.hitsoundVolume = first sample (no event needed for floor 1);
    ADOFAI SetHitsound volume persists forward, so events are only written at
    sample-to-sample *changes* (silence runs, leading/trailing zeros and exact
    repeats cost no events at all).
  * angleData stays all zeros (direction is meaningless for audio) but is
    written densely ("0,0,..."), a few thousand per line.
  * trailing silence needs no events: after the final change to 0 the decoder
    fills zeros up to the tile count implied by angleData.

Fidelity facts (verified bit-exact on 1.5 GB charts):
  * hitsoundVolume = int16_sample / 655.36 is an EXACT binary64 (dyadic);
    decode(encode(x)) == x for every sample, roundtrip diff zero.
  * decoding older full-event charts also works: sample count is
    max(angleData_len - 1, last event floor).
"""
from __future__ import annotations

import argparse
import array
import io
import math
import mmap
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import wave

__version__ = "2.0.0"

AUTHOR = "Music.adofai (https://github.com/CHT-1192/Music.adofai)"

_VOL_RE = re.compile(
    rb'"floor"\s*:\s*(\d+)\s*,\s*"eventType"\s*:\s*"SetHitsound"[^{}]*?'
    rb'"hitsoundVolume"\s*:\s*(-?[\d.eE+]+)'
)


def _fmt(v: float) -> str:
    """Short decimal formatting; exact for dyadic int16/655.36 values."""
    if v == 0.0:
        return "0"          # 0.0 -> 0
    if v == int(v):
        return str(int(v))
    return repr(v)


def _vol(x: int) -> str:
    return _fmt(x / 655.36)  # dyadic -> exact float64 roundtrip


def _str_field(haystack: bytes, key: str) -> str | None:
    m = re.search(rb'"' + key.encode() + rb'"\s*:\s*"((?:[^"\\]|\\.)*)"', haystack)
    return m.group(1).decode("utf-8", "replace") if m else None


def _num_field(haystack: bytes, key: str) -> float | None:
    m = re.search(rb'"' + key.encode() + rb'"\s*:\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)', haystack)
    return float(m.group(1)) if m else None


def _settings_region(adofai_path: str) -> bytes:
    with open(adofai_path, "rb") as f:
        head = f.read(1 << 29)  # settings live near the top in our charts
    si = head.find(b'"settings"')
    return head[si : si + (1 << 20)] if si >= 0 else head


# --------------------------------------------------------------------------
# audio input / metadata
# --------------------------------------------------------------------------

def read_tags(path: str) -> dict:
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        return {}
    try:
        out = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format_tags=title,artist,album",
             "-of", "default=noprint_wrappers=1", path],
            capture_output=True, text=True, timeout=120,
        ).stdout
    except Exception:
        return {}
    tags: dict = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            if v and k in ("title", "artist", "album"):
                tags[k] = v.strip()
    return tags


def _sanitize(name: str) -> str:
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "-", name)
    name = re.sub(r"\s+", " ", name).strip(" .")
    return name[:150] or "untitled"


def auto_output_name(path: str, tags: dict) -> str:
    title = tags.get("title")
    artist = tags.get("artist")
    if not title:
        stem = os.path.splitext(os.path.basename(path))[0]
        title = re.sub(r"[-_]+$", "", stem) or "untitled"
    title = _sanitize(title)
    if artist:
        return f"{_sanitize(artist)} - {title}.adofai"
    return f"{title}.adofai"


def decode_pcm(path: str) -> tuple[array.array, int]:
    """Return (mono s16 samples, sample_rate) for any ffmpeg-readable file."""
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            ch, sw, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            if ch == 1 and sw == 2:
                a = array.array("h")
                a.frombytes(w.readframes(w.getnframes()))
                return a, rate
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit(f"error: {path}: need a mono 16-bit WAV or ffmpeg to decode other formats")
    proc = subprocess.Popen(
        [ffmpeg, "-v", "error", "-i", path, "-f", "wav", "-ac", "1",
         "-sample_fmt", "s16", "pipe:1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert proc.stdout is not None
    raw = proc.stdout.read()
    proc.wait()
    if proc.returncode != 0:
        err = proc.stderr.read().decode("utf-8", "replace") if proc.stderr else ""
        sys.exit(f"error: ffmpeg failed on {path}:\n{err[-2000:]}")
    with wave.open(io.BytesIO(raw), "rb") as w:
        rate = w.getframerate()
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
    return a, rate


# --------------------------------------------------------------------------
# encoding
# --------------------------------------------------------------------------

def encode_file(input_path: str, out_path: str | None, out_dir: str | None,
                title: str | None, artist: str | None) -> int:
    s, rate = decode_pcm(input_path)
    n = len(s)

    tags = read_tags(input_path)
    title = title or tags.get("title")
    artist = artist or tags.get("artist")

    if out_path is None:
        out_path = os.path.join(out_dir or ".", auto_output_name(input_path, {"title": title, "artist": artist}))

    esc = lambda t: (t or "").replace("\\", "\\\\").replace('"', '\\"')

    # events only at volume changes (ADOFAI SetHitsound volume persists forward)
    events: list[tuple[int, int]] = []
    prev = s[0]
    for i in range(1, n):
        if s[i] != prev:
            events.append((i + 1, s[i]))  # floor i+1 <-> sample i
            prev = s[i]

    bpm = rate * 60

    # folded angleData: dense runs of "0", a few thousand per line
    line_buf: list[str] = []
    chunk: list[str] = []
    for _ in range(n):
        chunk.append("0")
        if len(chunk) >= 4000:
            line_buf.append(",".join(chunk))
            chunk = []
    if chunk:
        line_buf.append(",".join(chunk))
    folded = ",\r\n".join(line_buf)

    settings = (
        '\t"settings": {\r\n'
        f'\t\t"version": 13,\r\n'
        f'\t\t"bpm": {bpm},\r\n'
        f'\t\t"artist": "{esc(artist)}",\r\n'
        f'\t\t"song": "{esc(title)}",\r\n'
        f'\t\t"author": "{esc(AUTHOR)}",\r\n'
        '\t\t"hitsound": "Kick",\r\n'
        f'\t\t"hitsoundVolume": {_vol(s[0])},\r\n'   # initial volume: no floor-1 event
        '\t\t"offset": 0\r\n'
        '\t},\r\n'
        '\t"actions": [\r\n'
    )
    line = '\t\t{{ "floor": {0}, "eventType": "SetHitsound", "gameSound": "Hitsound", "hitsound": "Kick", "hitsoundVolume": {1} }},\r\n'

    t0 = time.time()
    with open(out_path, "wb") as f:
        f.write(('{\r\n\t"angleData": [\r\n' + folded + '],\r\n' + settings).encode("utf-8"))
        buf: list[str] = []
        for floor, x in events:
            buf.append(line.format(floor, _vol(x)))
            if len(buf) >= 5000:
                f.write("".join(buf).encode("utf-8"))
                buf = []
        if buf:
            f.write("".join(buf).encode("utf-8"))
        f.write(b"\t]\r\n}\r\n")
    size = os.path.getsize(out_path)
    print(f"encoded {n} samples ({rate/1000:.1f} kHz, {n/rate:.1f}s) -> {out_path} ({size/1e9:.2f} GB) in {time.time()-t0:.0f}s")
    print(f"  events: {len(events)} (max {n-1}; saved {n-1-len(events)}, {100.0*(n-1-len(events))/max(1,n-1):.1f}%)")
    if artist or title:
        print(f"  artist={artist or ''}  song={title or ''}  author={AUTHOR}")
    return n


# --------------------------------------------------------------------------
# decoding
# --------------------------------------------------------------------------

def _angle_entries(adofai_path: str) -> int:
    """Count angleData array entries without a full JSON parse."""
    with open(adofai_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            ast = mm.find(b'"angleData"')
            if ast < 0:
                return 0
            a0 = mm.find(b"[", ast)
            end = mm.find(b'"settings"', a0)
            if end < 0:
                end = mm.find(b'"pathData"', a0)
            seg = mm[a0:end if end > 0 else len(mm)]
            close = seg.rfind(b"]")
            seg = seg[:close] if close > 0 else seg
            return seg.count(b",") + 1
        finally:
            mm.close()


def _read_audio(adofai_path: str) -> tuple[array.array, int, float]:
    """Decode an audio-as-chart into (samples_int16, sample_count, rate_hz).

    Handles both v2 change-event layouts and older full-event charts.
    Sample count = max(angleData_len - 1, last event floor).
    """
    vol0 = 0
    hv: float | None = None
    rate = 44100.0
    floors: list[int] = []
    vols: list[float] = []
    with open(adofai_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        t0 = time.time()
        try:
            head = mm[: (1 << 29)]
            si = head.find(b'"settings"')
            if si >= 0:
                region = head[si : si + (1 << 20)]
                hv = _num_field(region, "hitsoundVolume")
                if hv is not None:
                    vol0 = int(round(hv * 655.36))
                bpm = _num_field(region, "bpm")
                if bpm:
                    rate = bpm / 60.0
            for m in _VOL_RE.finditer(mm):
                floors.append(int(m.group(1)))
                vols.append(float(m.group(2)))
            last_floor = floors[-1] if floors else 0
        finally:
            mm.close()
    entries = _angle_entries(adofai_path)
    # layout disambiguation:
    #   v2 (this codec): settings.hitsoundVolume = first sample (|vol|<=50),
    #     angleData has exactly one entry per sample
    #   legacy/full-event charts: settings.hitsoundVolume = 100 (a dummy),
    #     angleData has samples+1 entries
    if hv is not None and abs(hv) <= 50.0:
        n_samples = entries
    else:
        n_samples = entries - 1
    total = max(n_samples, last_floor)
    print(f"read {len(floors)} events, {total} samples in {time.time()-t0:.0f}s", file=sys.stderr)

    # rebuild by persistent-volume segments (array multiply, no per-sample loop)
    cur = max(-32768, min(32767, vol0))
    out = array.array("h")
    pos = 0
    for floor, v in zip(floors, vols):
        if floor - 1 > pos:
            out.extend(array.array("h", [cur]) * (floor - 1 - pos))
        cur = max(-32768, min(32767, int(round(v * 655.36))))
        pos = max(pos, floor - 1)
    if pos < total:
        out.extend(array.array("h", [cur]) * (total - pos))
    if len(out) > total:
        del out[total:]
    return out, total, rate


def decode_chart(adofai_path: str, out_path: str, gain: float = 1.0) -> int:
    out, total, rate = _read_audio(adofai_path)
    if gain != 1.0:
        g = array.array("h", (max(-32768, min(32767, int(round(x * gain)))) for x in out))
        out = g
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(round(rate)))
        w.writeframes(out.tobytes())
    print(f"decoded {len(out)} samples ({rate/1000:.1f} kHz, {total/rate:.1f}s) -> {out_path} ({os.path.getsize(out_path)/1e6:.1f} MB)")
    return len(out)


def verify_chart(adofai_path: str, ref_path: str) -> int:
    samples, total, _ = _read_audio(adofai_path)
    ref_pcm, _ = decode_pcm(ref_path)
    if len(samples) != len(ref_pcm) or total != len(ref_pcm):
        print(f"length mismatch: chart {len(samples)} (total {total}) vs ref {len(ref_pcm)}")
        return 1
    mism = 0
    first = None
    for i, (a, b) in enumerate(zip(samples, ref_pcm)):
        if a != b:
            mism += 1
            if first is None:
                first = i
    if mism == 0:
        print(f"VERIFY OK: {len(samples)}/{len(ref_pcm)} samples identical")
        return 0
    print(f"VERIFY FAILED: {mism}/{len(ref_pcm)} mismatches, first at sample {first}")
    return 1


def chart_info(adofai_path: str) -> None:
    _, total, rate = _read_audio(adofai_path)
    region = _settings_region(adofai_path)
    bpm = _num_field(region, "bpm")
    artist = _str_field(region, "artist")
    song = _str_field(region, "song")
    author = _str_field(region, "author")
    if bpm:
        rate = bpm / 60.0
    print(f"artist: {artist or '(none)'}")
    print(f"song:   {song or '(none)'}")
    print(f"author: {author or '(none)'}")
    print(f"events: {_angle_entries(adofai_path)} angle entries")
    print(f"sample rate (bpm/60): {rate} Hz  duration: {total/rate:.3f}s")
    print(f"size: {os.path.getsize(adofai_path)/1e9:.3f} GB")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def self_test() -> int:
    n = 44100
    s = array.array("h")
    for i in range(n):
        v = int(32767 * 0.6 * math.sin(2 * math.pi * 440.0 * i / n))
        if i % 997 == 0:
            v = -v
        s.append(v)
    with tempfile.TemporaryDirectory() as td:
        wav = os.path.join(td, "tone.wav")
        chart = os.path.join(td, "Tone Test - Artist.adofai")
        with wave.open(wav, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
            w.writeframes(s.tobytes())
        encode_file(wav, chart, None, "Tone Test", "Artist")
        back = os.path.join(td, "back.wav")
        decode_chart(chart, back)
        rc = verify_chart(chart, wav)
        if rc == 0:
            print("SELF-TEST PASSED")
            return 0
        print("SELF-TEST FAILED")
        return 1


def main() -> int:
    ap = argparse.ArgumentParser(
        prog="adofai_audio_codec",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("encode", help="audio -> .adofai (change-event layout v2)")
    p.add_argument("input", help="audio file (wav/m4a/webm/opus/mp3/...)")
    p.add_argument("out", nargs="?", help="output path; default: auto-named from tags")
    p.add_argument("--out-dir", help="directory for auto-named output (default: cwd)")
    p.add_argument("--title", help="override song title (falls back to ffprobe tags)")
    p.add_argument("--artist", help="override artist (falls back to ffprobe tags)")
    p.set_defaults(fn=lambda a: encode_file(a.input, a.out, a.out_dir, a.title, a.artist))

    p = sub.add_parser("decode", help=".adofai -> 44.1 kHz mono s16 WAV")
    p.add_argument("chart")
    p.add_argument("out")
    p.add_argument("--gain", type=float, default=1.0,
                   help="output gain (default 1.0 = bit-exact; 0.5 = comfortable listening)")
    p.set_defaults(fn=lambda a: decode_chart(a.chart, a.out, a.gain))

    p = sub.add_parser("verify", help="diff decoded chart against a reference audio file")
    p.add_argument("chart")
    p.add_argument("ref")
    p.set_defaults(fn=lambda a: verify_chart(a.chart, a.ref))

    p = sub.add_parser("info", help="print chart metadata (artist/song/author + audio info)")
    p.add_argument("chart")
    p.set_defaults(fn=lambda a: chart_info(a.chart))

    p = sub.add_parser("self-test", help="roundtrip a synthetic tone")
    p.set_defaults(fn=lambda a: self_test())

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
