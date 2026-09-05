#!/usr/bin/env python3
"""adofai-audio-codec: lossless WAV <-> .adofai "audio-as-chart" codec.

Maps a mono PCM WAV onto an ADOFAI chart the way community auto-chart tools do:
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

Only the Python standard library is used.
"""
from __future__ import annotations

import argparse
import array
import math
import mmap
import os
import re
import struct
import sys
import time
import wave

__version__ = "1.0.0"

# Defaults chosen to be drop-in compatible with the original
# "Unity.wav_rate.adofai" (44.1 kHz) chart family.
DEFAULT_SAMPLE_RATE = 44100
DEFAULT_BPM = DEFAULT_SAMPLE_RATE * 60

_VOL_RE = re.compile(
    rb'"floor"\s*:\s*(\d+)\s*,\s*"eventType"\s*:\s*"SetHitsound"[^{}]*?'
    rb'"hitsoundVolume"\s*:\s*(-?[\d.eE+]+)'
)


# --------------------------------------------------------------------------
# encoding
# --------------------------------------------------------------------------

def encode_wav(wav_path: str, out_path: str, bpm: int) -> int:
    """Encode a mono 16-bit WAV into the audio-as-chart format.

    Returns the number of samples written.
    """
    with wave.open(wav_path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            sys.exit(f"error: {wav_path}: need mono 16-bit PCM (got {w.getnchannels()}ch, {w.getsampwidth()*8}-bit)")
        rate = w.getframerate()
        if bpm != rate * 60:
            print(f"note: bpm set to {rate*60} to match source rate {rate} Hz", file=sys.stderr)
            bpm = rate * 60
        n = w.getnframes()
        raw = w.readframes(n)
    s = array.array("h")
    s.frombytes(raw)
    n = len(s)

    def vol(x: int) -> str:
        return repr(x / 655.36)  # dyadic -> exact float64 roundtrip

    head = (
        '{\r\n\t"angleData": [\r\n'
        + ",\r\n".join("0" for _ in range(n + 1))
        + '\r\n\t],\r\n'
        + '\t"settings": {\r\n'
        + '\t\t"version": 13,\r\n'
        + f'\t\t"bpm": {bpm},\r\n'
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
    print(f"encoded {n} samples -> {out_path} ({os.path.getsize(out_path)/1e9:.2f} GB) in {time.time()-t0:.0f}s")
    return n


# --------------------------------------------------------------------------
# decoding
# --------------------------------------------------------------------------

def _read_samples(adofai_path: str) -> tuple[list[int], list[int]]:
    """Parse all SetHitsound volumes in floor order. Returns (floors, samples)."""
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
    """Decode an audio-as-chart back to mono 16-bit 44.1 kHz WAV.

    gain < 1.0 scales the output for comfortable listening (bit-exact only
    at gain == 1.0). Returns the number of samples decoded.
    """
    _, samples = _read_samples(adofai_path)
    if gain != 1.0:
        samples = [max(-32768, min(32767, int(round(x * gain)))) for x in samples]
    out = array.array("h", samples)
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(DEFAULT_SAMPLE_RATE)
        w.writeframes(out.tobytes())
    print(f"decoded {len(out)} samples -> {out_path} ({os.path.getsize(out_path)/1e6:.1f} MB)")
    return len(out)


# --------------------------------------------------------------------------
# verify / info / selftest
# --------------------------------------------------------------------------

def verify_chart(adofai_path: str, ref_wav: str) -> int:
    """Diff a decoded chart against a reference WAV (bit-exactness check)."""
    _, samples = _read_samples(adofai_path)
    with wave.open(ref_wav, "rb") as w:
        raw = w.readframes(w.getnframes())
    ref = array.array("h")
    ref.frombytes(raw)
    if len(samples) != len(ref):
        print(f"length mismatch: chart {len(samples)} vs ref {len(ref)}")
        return 1
    mism = 0
    first = None
    for i, (a, b) in enumerate(zip(samples, ref)):
        if a != b:
            mism += 1
            if first is None:
                first = i
    if mism == 0:
        print(f"VERIFY OK: {len(samples)}/{len(ref)} samples identical")
        return 0
    print(f"VERIFY FAILED: {mism}/{len(ref)} mismatches, first at sample {first}")
    return 1


def chart_info(adofai_path: str) -> None:
    floors, samples = _read_samples(adofai_path)
    first, last = floors[0], floors[-1]
    dur = len(samples) / DEFAULT_SAMPLE_RATE
    bpm = None
    with open(adofai_path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        m = re.search(rb'"bpm"\s*:\s*(\d+)', mm[: (1 << 28)])
        if m:
            bpm = int(m.group(1))
        mm.close()
    print(f"floors: {first}..{last}  samples: {len(samples)}")
    print(f"sample rate (bpm/60): {bpm/60 if bpm else '?'} Hz  duration: {dur:.3f}s")
    print(f"size: {os.path.getsize(adofai_path)/1e9:.3f} GB")


def self_test() -> int:
    """Roundtrip a synthetic 1-second WAV through encode+decode in temp files."""
    import tempfile

    n = DEFAULT_SAMPLE_RATE
    s = array.array("h")
    for i in range(n):
        v = int(32767 * 0.6 * math.sin(2 * math.pi * 440.0 * i / n))
        if i % 997 == 0:
            v = -v  # a few polarity flips
        s.append(v)
    with tempfile.TemporaryDirectory() as td:
        wav = os.path.join(td, "tone.wav")
        chart = os.path.join(td, "tone.adofai")
        back = os.path.join(td, "tone_back.wav")
        with wave.open(wav, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(DEFAULT_SAMPLE_RATE)
            w.writeframes(s.tobytes())
        encode_wav(wav, chart, DEFAULT_BPM)
        decode_chart(chart, back)
        rc = verify_chart(chart, wav)
        if rc == 0 and os.path.getsize(back) == os.path.getsize(wav):
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

    p = sub.add_parser("encode", help="WAV -> .adofai")
    p.add_argument("wav")
    p.add_argument("out")
    p.add_argument("--bpm", type=int, default=DEFAULT_BPM,
                   help=f"default {DEFAULT_BPM} (= {DEFAULT_SAMPLE_RATE} Hz); auto-corrected to wav_rate*60")
    p.set_defaults(fn=lambda a: encode_wav(a.wav, a.out, a.bpm))

    p = sub.add_parser("decode", help=".adofai -> WAV (44.1 kHz mono s16)")
    p.add_argument("chart")
    p.add_argument("out")
    p.add_argument("--gain", type=float, default=1.0,
                   help="output gain (default 1.0 = bit-exact; 0.5 = comfortable listening)")
    p.set_defaults(fn=lambda a: decode_chart(a.chart, a.out, a.gain))

    p = sub.add_parser("verify", help="diff decoded chart against a reference WAV")
    p.add_argument("chart")
    p.add_argument("ref")
    p.set_defaults(fn=lambda a: verify_chart(a.chart, a.ref))

    p = sub.add_parser("info", help="print chart audio metadata")
    p.add_argument("chart")
    p.set_defaults(fn=lambda a: chart_info(a.chart))

    p = sub.add_parser("self-test", help="roundtrip a synthetic tone")
    p.set_defaults(fn=lambda a: self_test())

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
