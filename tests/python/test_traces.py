# SPDX-License-Identifier: MIT
"""REQ-TEST-003: the committed golden Tick traces are fresh and pinned.

The C fake Tick replays these traces (ctest test_fake_tick_*); this checks
they are exactly what gen_traces.py produces from the vendored hdl_model at
the pins.json Tick revision, so a stale or hand-edited trace fails.
"""

import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRACES = ROOT / "tests" / "traces"
SCENARIOS = {"ordered_queue", "late_on_submit_drop", "late_execute_now", "flush",
             "overflow", "completion_slot_hold", "repeat", "pps_align",
             "late_drop_slot_full"}


def _gen():
    spec = importlib.util.spec_from_file_location("gen_traces", TRACES / "gen_traces.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def test_traces_regenerate_identically():
    rendered = _gen().render_all()
    committed = {p.name for p in TRACES.glob("*.json")}
    assert committed == set(rendered)
    for name, text in rendered.items():
        assert (TRACES / name).read_text() == text, f"{name} is stale: rerun gen_traces.py"


def test_every_scenario_present():
    assert {p.stem for p in TRACES.glob("*.json")} == SCENARIOS


def test_provenance_matches_pins_and_vendored_model():
    pins = json.loads((ROOT / "pins.json").read_text())
    for p in TRACES.glob("*.json"):
        t = json.loads(p.read_text())
        prov = t["provenance"]
        assert prov["tick_revision"] == pins["tick"]["revision"]
        assert prov["model_sha256"] == _sha(ROOT / prov["model"])
        assert prov["scheduler_sha256"] == _sha(ROOT / "tests/vendor/tick/scheduler.py")
        assert prov["generator_sha256"] == _sha(ROOT / prov["generator"])
        assert t["config"] == {"tick_hz": 100_000_000, "queue_depth": 16}


def test_rtl_sourced_steps_are_marked():
    for p in TRACES.glob("*.json"):
        t = json.loads(p.read_text())
        rtl_steps = [s for s in t["steps"] if s.get("source") == "rtl"]
        assert (t["source"] == "rtl") == bool(rtl_steps), p.name
        for s in rtl_steps:
            assert s["rtl_ref"].startswith("hdl/library/axi_timed_command_scheduler/")
    assert json.loads((TRACES / "late_drop_slot_full.json").read_text())["source"] == "rtl"


def test_completion_slot_hold_records_executed_equals_requested():
    """tfcollins/tick#38, as the trace pins it: seq 62 fires at the ack (5000)
    but records executed = requested = 2000."""
    t = json.loads((TRACES / "completion_slot_hold.json").read_text())
    comps = [c for s in t["steps"] for c in s["completions"] if c["sequence_id"] == 62]
    assert len(comps) == 1
    assert comps[0]["executed_timestamp_ticks"] == comps[0]["requested_timestamp_ticks"] == 2000
    fired = next(s for s in t["steps"] if any(c["sequence_id"] == 62 for c in s["completions"]))
    assert fired["op"] == "write" and fired["attr"] == "completion_ack"
    assert fired["expect"]["current_time_ticks"] == 5000
