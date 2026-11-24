# SPDX-License-Identifier: MIT
"""The golden capture-tag vectors are fresh, pinned and self-consistent.

test_fake_dma (C) compares the fake DMA's tag bytes with every positive case;
this checks the file is exactly what gen_tag_vectors.py produces from the
vendored codec at the pins.json Tick revision (the generator's ``--check``).
"""

import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VEC = ROOT / "tests" / "vectors"
FILE = VEC / "capture_tags.json"


def _sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _gen():
    spec = importlib.util.spec_from_file_location("gen_tag_vectors", VEC / "gen_tag_vectors.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_check_passes():
    r = subprocess.run([sys.executable, str(VEC / "gen_tag_vectors.py"), "--check"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_regenerates_identically():
    assert FILE.read_text() == _gen().render()


def test_provenance_matches_pins_and_codec():
    t = json.loads(FILE.read_text())
    pins = json.loads((ROOT / "pins.json").read_text())
    prov = t["provenance"]
    assert prov["tick_revision"] == pins["tick"]["revision"]
    assert prov["codec_sha256"] == _sha(ROOT / prov["codec"])
    assert prov["generator_sha256"] == _sha(ROOT / prov["generator"])


def test_not_registered_as_a_trace():
    assert FILE.parent.name != "traces"
    assert not list((ROOT / "tests" / "traces").glob("capture_tags*"))


def test_required_cases_present():
    t = json.loads(FILE.read_text())
    names = {c["name"] for c in t["cases"]}
    assert {"on_time_gate0", "late_executed", "stale", "gate2_late", "tick_max",
            "hz_and_burst", "bad_magic", "bad_version"} <= names
    assert any(c["positive"] and c["tick_hz"] and c["burst"] for c in t["cases"])


def test_bytes_match_the_wire_layout():
    """An independent decode of the 32 bytes (little-endian, <IHBBQIHHII)."""
    for c in json.loads(FILE.read_text())["cases"]:
        raw = bytes.fromhex(c["bytes"])
        assert len(raw) == 32
        magic, ver, words, flags, tick, seq, target, cmd, hz, burst = struct.unpack("<IHBBQIHHII", raw)
        if not c["positive"]:
            assert (magic != 0x4B434954) or (ver != 1), c["name"]
            continue
        assert (magic, ver, words) == (0x4B434954, 1, 8)
        assert flags == c["flags"] and flags & 1
        assert (flags >> 4) == c["gate"]
        assert bool(flags & 2) == c["stale"] and bool(flags & 4) == c["late"]
        assert (tick, seq, target, cmd, hz, burst) == (
            c["capture_tick"], c["seq"], c["target"], c["cmd"], c["tick_hz"], c["burst"])
