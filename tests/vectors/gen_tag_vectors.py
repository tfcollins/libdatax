# SPDX-License-Identifier: MIT
"""Generate the golden capture-tag vectors (P3b S3).

Drives the vendored ``tick.capture`` (tests/vendor/tick, pinned revision in
pins.json) -- ``build_capture_header`` and ``parse_capture_header`` -- and
writes ``tests/vectors/capture_tags.json``: one case per line, each with the
inputs and the 32 header bytes (hex). The C fake DMA encodes its RX tag the
same way and test_fake_dma compares bytes with every positive case.

The file is under tests/vectors, not tests/traces, because the traces glob
registers every ``*.json`` there as a fake-Tick trace.

Usage:
    python3 tests/vectors/gen_tag_vectors.py           # (re)write the file
    python3 tests/vectors/gen_tag_vectors.py --check   # fail if it is stale
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VENDOR = ROOT / "tests" / "vendor"
sys.path.insert(0, str(VENDOR))

import tick  # noqa: E402  (the vendored copy, asserted below)
from tick import capture  # noqa: E402

if Path(tick.__file__).resolve().parent != VENDOR / "tick":
    raise SystemExit(f"tick imported from {tick.__file__}, not the vendored copy")

OUT = HERE / "capture_tags.json"
STREAM_GATE = 0x0003          # command_id of STREAM_GATE
GATE_TARGET = 0x0200          # target_id = 0x0200 | gate bit
U64_MAX = 2**64 - 1

# name, inputs. The fake Tick runs at 100 MHz.
POSITIVE = [
    ("on_time_gate0", dict(capture_tick=1_000_000, gate_id=0, sequence_id=7,
                           target_id=GATE_TARGET | 0, command_id=STREAM_GATE,
                           tick_hz=100_000_000, burst_index=0)),
    ("late_executed", dict(capture_tick=2_500, gate_id=0, sequence_id=8,
                           target_id=GATE_TARGET | 0, command_id=STREAM_GATE,
                           tick_hz=100_000_000, burst_index=1, late_executed=True)),
    ("stale", dict(capture_tick=1_000_000, gate_id=0, sequence_id=7,
                   target_id=GATE_TARGET | 0, command_id=STREAM_GATE,
                   tick_hz=100_000_000, burst_index=2, stale=True)),
    ("gate2_late", dict(capture_tick=123_456_789, gate_id=2, sequence_id=0x1234,
                        target_id=GATE_TARGET | 2, command_id=STREAM_GATE,
                        tick_hz=100_000_000, burst_index=3, late_executed=True)),
    ("tick_max", dict(capture_tick=U64_MAX, gate_id=0, sequence_id=1,
                      target_id=GATE_TARGET | 0, command_id=STREAM_GATE,
                      tick_hz=100_000_000, burst_index=0)),
    ("hz_and_burst", dict(capture_tick=0x0123_4567_89AB_CDEF, gate_id=1,
                          sequence_id=0xFFFF_FFFF, target_id=GATE_TARGET | 1,
                          command_id=STREAM_GATE, tick_hz=245_760_000,
                          burst_index=0x00AB_CDEF)),
]


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _positive(name: str, kw: dict) -> dict:
    raw = capture.build_capture_header(**kw)
    h = capture.parse_capture_header(raw)
    # The vector is only as good as the round trip: fail here, not in C.
    assert h.valid and h.capture_tick == kw["capture_tick"]
    assert h.gate_id == kw["gate_id"] and h.sequence_id == kw["sequence_id"]
    assert h.target_id == kw["target_id"] and h.command_id == kw["command_id"]
    assert h.tick_hz == kw["tick_hz"] and h.burst_index == kw["burst_index"]
    assert h.stale == kw.get("stale", False)
    assert h.late_executed == kw.get("late_executed", False)
    return {
        "name": name,
        "positive": True,
        "capture_tick": kw["capture_tick"],
        "gate": kw["gate_id"],
        "seq": kw["sequence_id"],
        "target": kw["target_id"],
        "cmd": kw["command_id"],
        "tick_hz": kw["tick_hz"],
        "burst": kw["burst_index"],
        "late": kw.get("late_executed", False),
        "stale": kw.get("stale", False),
        "flags": h.flags,
        "bytes": raw.hex(),
    }


def _negative(name: str, mutate) -> dict:
    raw = bytearray(capture.build_capture_header(
        capture_tick=1, tick_hz=100_000_000, target_id=GATE_TARGET,
        command_id=STREAM_GATE))
    mutate(raw)
    try:
        capture.parse_capture_header(bytes(raw))
    except capture.CaptureHeaderError as exc:
        err = str(exc)
    else:
        raise AssertionError(f"{name}: parse accepted a bad header")
    return {"name": name, "positive": False, "error": err, "bytes": bytes(raw).hex()}


def cases() -> list[dict]:
    out = [_positive(n, kw) for n, kw in POSITIVE]
    out.append(_negative("bad_magic", lambda r: r.__setitem__(0, r[0] ^ 0xFF)))
    out.append(_negative("bad_version", lambda r: r.__setitem__(4, 2)))
    return out


def render() -> str:
    prov = {
        "tick_revision": json.loads((ROOT / "pins.json").read_text())["tick"]["revision"],
        "codec": "tests/vendor/tick/capture.py",
        "codec_sha256": _sha(VENDOR / "tick" / "capture.py"),
        "generator": "tests/vectors/gen_tag_vectors.py",
        "generator_sha256": _sha(Path(__file__).resolve()),
    }
    # One case per line: the C test scans the file by key, not with a JSON parser.
    lines = ",\n".join("  " + json.dumps(c, sort_keys=True) for c in cases())
    return ('{\n "provenance": ' + json.dumps(prov, sort_keys=True) +
            ',\n "cases": [\n' + lines + "\n ]\n}\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="compare, do not write")
    a = ap.parse_args()
    text = render()
    if a.check:
        if not OUT.exists() or OUT.read_text() != text:
            print("stale: tests/vectors/capture_tags.json", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
