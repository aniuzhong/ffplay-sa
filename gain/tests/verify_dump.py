#!/usr/bin/env python3
"""Integration verifier for the ffplay gain experiment (stdlib only).

make:  generate a stereo S16 WAV (sine + tail silence) as test input.
check: recompute the expected post-gain S16 stream with the same math the
       port implements (s16/32768 -> multiply -> clamp -> round(*32768)),
       align it against the FFPLAY_GAIN_DUMP capture by fingerprint search
       (ffplay emits startup silence before the first decoded frame), and
       require a bit-exact match.  With --events the expected stream gets
       OBS-style volume steps at round(sec * freq) device frames, so the
       step placement in the dump is verified too.

The sample-rate assumption: the dump is interpreted at the WAV's rate.
ffplay asks SDL for the stream rate and SDL may renegotiate it; if that
happens the frame counts won't line up and `check` says so (the C layer
logs the negotiated rate via note_device).  On typical Windows/macOS
setups 48000 passes through untouched.
"""

import argparse
import math
import struct
import sys
import wave

SCALE = 32768.0


def db_to_mul(db):
    return 0.0 if not math.isfinite(db) else 10.0 ** (db / 20.0)


def float_to_s16(v):
    scaled = round(v * SCALE)
    if scaled > 32767:
        scaled = 32767
    elif scaled < -32768:
        scaled = -32768
    return scaled


def parse_events(spec):
    """'sec:db,sec:db' -> sorted [(sec, mul)], like ffplay_gain_obs.c."""
    events = []
    for part in filter(None, spec.split(",")):
        sec, db = part.split(":")
        events.append((float(sec), db_to_mul(float(db))))
    return sorted(events)


def mul_at_frame(frame, freq, static_mul, events):
    """Volume envelope: steps land ON their target frame (new value from it)."""
    mul = static_mul
    for sec, e_mul in events:
        if frame >= round(sec * freq):
            mul = e_mul
    return mul


def make_wav(path, freq=48000, seconds=8.0, amp=0.5, tone_s=0.75):
    n = int(freq * seconds)
    tone = int(freq * tone_s)  # sine duration, then silence tail
    frames = bytearray()
    for i in range(n):
        s = 0.0 if i >= tone else amp * math.sin(2.0 * math.pi * 440.0 * i / freq)
        v = float_to_s16(s)
        frames += struct.pack("<hh", v, v)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(freq)
        w.writeframes(bytes(frames))
    print(f"wrote {path}: {n} frames @ {freq} Hz (tone {tone} frames)")


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 2, "expect stereo S16"
        freq = w.getframerate()
        raw = w.readframes(w.getnframes())
    return freq, list(struct.unpack(f"<{len(raw)//2}h", raw))


def expected_stream(source, freq, db, events):
    static_mul = db_to_mul(db)
    out = []
    for frame in range(len(source) // 2):
        mul = mul_at_frame(frame, freq, static_mul, events)
        for ch in range(2):
            v = source[frame * 2 + ch] / SCALE * mul
            if v != v:            # NaN guard, mirrors the C pipeline order
                v = 0.0
            v = min(1.0, max(-1.0, v))
            out.append(float_to_s16(v))
    return out


def check(wav_path, dump_path, db, events):
    freq, source = read_wav(wav_path)
    with open(dump_path, "rb") as f:
        raw = f.read()
    dump = list(struct.unpack(f"<{len(raw)//2}h", raw))

    events = parse_events(events) if events else []
    expect = expected_stream(source, freq, db, events)

    print(f"dump: {len(dump)//2} frames, wav: {len(source)//2} frames, "
          f"expect: {len(expect)//2} frames @ {freq} Hz")
    if len(dump) < len(expect) // 2:
        sys.exit(f"FAIL: dump far shorter than expected - SDL likely renegotiated "
                 f"the device rate; check the [obs-gain] log line")
    # ffplay -autoexit exits before the device drains its buffer; a small
    # tail gap (~one SDL callback) is normal.  Compare the overlap.

    # Align: find expect[:256] inside dump (skip ffplay's startup silence).
    fp = expect[:256]
    off = None
    for start in range(0, min(len(dump) - len(fp), freq * 2) + 1):
        if dump[start:start + len(fp)] == fp:
            off = start
            break
    if off is None:
        sys.exit("FAIL: fingerprint not found in dump - content mismatch")

    n = min(len(dump) - off, len(expect))
    mism = 0
    first = None
    for i in range(n):
        if dump[off + i] != expect[i]:
            mism += 1
            if first is None:
                first = (i, dump[off + i], expect[i])
    print(f"aligned at dump frame {off // 2}; compared {n} samples "
          f"({100.0 * n / len(expect):.2f}% of expected)")
    if mism:
        sys.exit(f"FAIL: {mism} mismatched samples, first at frame "
                 f"{first[0] // 2} ch{first[0] % 2}: dump {first[1]} vs expect {first[2]}")
    print(f"PASS: bit-exact ({n} samples, db={db}, events={events})")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("wav")
    m.add_argument("--freq", type=int, default=48000)
    m.add_argument("--seconds", type=float, default=8.0)
    m.add_argument("--amp", type=float, default=0.5)
    m.add_argument("--tone", type=float, default=0.75, help="sine duration (s); rest is silence tail")
    c = sub.add_parser("check")
    c.add_argument("wav")
    c.add_argument("dump")
    c.add_argument("--db", type=float, default=0.0)
    c.add_argument("--events", default=None)
    args = p.parse_args()

    if args.cmd == "make":
        make_wav(args.wav, args.freq, args.seconds, args.amp, args.tone)
    else:
        check(args.wav, args.dump, args.db, args.events)


if __name__ == "__main__":
    main()
