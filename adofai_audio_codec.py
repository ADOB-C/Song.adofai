#!/usr/bin/env python3
"""adofai-audio-codec: lossless audio <-> .adofai "audio-as-chart" codec.

Maps a PCM audio file onto an ADOFAI chart the way community auto-chart tools do:
every audio sample becomes one tile, the track's BPM is chosen so that
BPM / 60 == the audio sample rate, and each sample value is stored in a
per-tile SetHitsound event's `hitsoundVolume` field as  sample / 655.36.

  sample rate (Hz)  |  BPM            |  samples / second
  ------------------|-----------------|------------------
  44100             |  2646000        |  44100   (default, matches Unity.wav_rate.adofai)

Format facts (verified bit-exact against 1.5 GB community charts):
  * hitsoundVolume = int16_sample / 655.36  is an EXACT binary64 (dyadic), so
    decode(encode(x)) == x for every sample; roundtrip diff is zero.
  * angleData is all zeros (direction is meaningless for audio); one
    SetHitsound ("Kick", gameSound "Hitsound") per sample, floor 1..N.

Input handling: WAV files are read with the stdlib `wave` module; any other
audio container (m4a/webm/opus/mp3/flac/...) is decoded with ffmpeg
(downmixed to mono, original sample rate preserved). Title/artist metadata
is read with ffprobe when available and embedded into the chart's settings,
and used to auto-name the output "Artist - Title.adofai".

Only the Python standard library is required; ffmpeg/ffprobe are optional
(needed only for non-WAV input or for auto-naming from container tags).
"""
from __future__ import annotations

import argparse
import array
import math
import mmap
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import io
import wave

__version__ = "1.1.0"

AUTHOR = "Music.adofai (https://github.com/CHT-1192/Music.adofai)"

_VOL_RE = re.compile(
    rb'"floor"\s*:\s*(\d+)\s*,\s*"eventType"\s*:\s*"SetHitsound"[^{}]*?'
    rb'"hitsoundVolume"\s*:\s*(-?[\d.eE+]+)'
)


def _str_field(haystack: bytes, key: str) -> str | None:
    m = re.search(rb'"' + key.encode() + rb'"\s*:\s*"((?:[^"\\]|\\.)*)"', haystack)
    if not m:
        return None
    return m.group(1).decode("utf-8", "replace")


def _num_field(haystack: bytes, key: str) -> int | None:
    m = re.search(rb'"' + key.encode() + rb'"\s*:\s*(\d+)', haystack)
    return int(m.group(1)) if m else None


# --------------------------------------------------------------------------
# audio input / metadata
# --------------------------------------------------------------------------

def read_tags(path: str) -> dict:
    """Read container tags with ffprobe. Returns {} when unavailable."""
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
        # non-conforming WAV: fall through to ffmpeg below
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
    """Encode an audio file into the audio-as-chart format.

    out_path None -> auto-named from metadata ("Artist - Title.adofai"),
    placed in out_dir (default: current directory).
    """
    s, rate = decode_pcm(input_path)
    n = len(s)

    tags = read_tags(input_path)
    title = title or tags.get("title")
    artist = artist or tags.get("artist")

    if out_path is None:
        d = out_dir or "."
        name = auto_output_name(input_path, {"title": title, "artist": artist})
        out_path = os.path.join(d, name)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)

    bpm = rate * 60

    def vol(x: int) -> str:
        return repr(x / 655.36)  # dyadic -> exact float64 roundtrip

    esc = lambda s_: (s_ or "").replace("\\", "\\\\").replace('"', '\\"')
    head = (
        '{\r\n\t"angleData": [\r\n'
        + ",\r\n".join("0" for _ in range(n + 1))
        + '\r\n\t],\r\n'
        + '\t"settings": {\r\n'
        + '\t\t"version": 13,\r\n'
        + f'\t\t"bpm": {bpm},\r\n'
        + f'\t\t"artist": "{esc(artist)}",\r\n'
        + f'\t\t"song": "{esc(title)}",\r\n'
        + f'\t\t"author": "{esc(AUTHOR)}",\r\n'
        + '\t\t"hitsound": "Kick",\r\n'
        + '\t\t"hitsoundVolume": 100,\r\n'
        + '\t\t"offset": 0\r\n'
        + '\t},\r\n'
        + '\t"actions": [\r\n'
    ).encode("utf-8")

    line = '\t\t{{ "floor": {0}, "eventType": "SetHitsound", "gameSound": "Hitsound", "hitsound": "Kick", "hitsoundVolume": {1} }},\r\n'

    t0 = time.time()
    with open(out_path, "wb") as f:
        f.write(head)
        buf: list[str] = []
        for k in range(1, n + 1):  # floor k <-> sample k-1
            buf.append(line.format(k, vol(s[k - 1])))
            if len(buf) >= 5000:
                f.write("".join(buf).encode("utf-8"))
                buf = []
        if buf:
            f.write("".join(buf).encode("utf-8"))
        f.write(b"\t]\r\n}\r\n")
    print(f"encoded {n} samples ({rate/1000:.1f} kHz, {n/rate:.1f}s) -> {out_path} "
          f"({os.path.getsize(out_path)/1e9:.2f} GB) in {time.time()-t0:.0f}s")
    if artist or title:
        print(f"  artist={artist or ''}  song={title or ''}  author={AUTHOR}")
    return n


# --------------------------------------------------------------------------
# decoding / verify / info
# --------------------------------------------------------------------------

def _read_samples(adofai_path: str) -> tuple[list[int], list[int]]:
    floors: list[int] = []
    samples: list[int] = []
    with open(adofai_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        t0 = time.time()
        try:
            for m in _VOL_RE.finditer(mm):
                floors.append(int(m.group(1)))
                x = int(round(float(m.group(2)) * 655.36))
                samples.append(max(-32768, min(32767, x)))
        finally:
            mm.close()
    if not samples:
        sys.exit(f"error: no SetHitsound events found in {adofai_path}")
    print(f"read {len(samples)} samples in {time.time()-t0:.0f}s", file=sys.stderr)
    return floors, samples


def decode_chart(adofai_path: str, out_path: str, gain: float = 1.0) -> int:
    _, samples = _read_samples(adofai_path)
    if gain != 1.0:
        samples = [max(-32768, min(32767, int(round(x * gain)))) for x in samples]
    out = array.array("h", samples)
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(out.tobytes())
    print(f"decoded {len(out)} samples -> {out_path} ({os.path.getsize(out_path)/1e6:.1f} MB)")
    return len(out)


def verify_chart(adofai_path: str, ref_path: str) -> int:
    _, samples = _read_samples(adofai_path)
    ref_pcm, ref_rate = decode_pcm(ref_path)
    if ref_pcm.typecode != "h":
        ref_pcm = array.array("h", ref_pcm)
    if len(samples) != len(ref_pcm):
        print(f"length mismatch: chart {len(samples)} vs ref {len(ref_pcm)}")
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
    floors, samples = _read_samples(adofai_path)
    with open(adofai_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        head = mm[: (1 << 29)]
        si = head.find(b'"settings"')
        region = head[si : si + (1 << 20)] if si >= 0 else b""
        bpm = _num_field(region, "bpm")
        artist = _str_field(region, "artist")
        song = _str_field(region, "song")
        author = _str_field(region, "author")
        mm.close()
    rate = bpm / 60 if bpm else 44100.0
    print(f"artist: {artist or '(none)'}")
    print(f"song:   {song or '(none)'}")
    print(f"author: {author or '(none)'}")
    print(f"floors: {floors[0]}..{floors[-1]}  samples: {len(samples)}")
    print(f"sample rate (bpm/60): {rate} Hz  duration: {len(samples)/rate:.3f}s")
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
        info_rc = chart_info(chart) or 0
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

    p = sub.add_parser("encode", help="audio -> .adofai (WAV via stdlib; others via ffmpeg)")
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
