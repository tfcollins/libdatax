# SPDX-License-Identifier: MIT
"""Generate the golden Tick traces the C fake Tick replays (REQ-TEST-003).

Drives the vendored ``tick.hdl_model`` (tests/vendor/tick, pinned revision in
pins.json) through named scenarios at tron's build parameters (100 MHz, queue
depth 16) and writes ``tests/traces/<name>.json``. Each trace is a list of
steps ``{t_ticks, op, attr, value}``; after every step it records the
attribute surface libdatax reads through the Tick driver (``expect``) and the
completions the model produced in that step.

Usage:
    python3 tests/traces/gen_traces.py           # (re)write the traces
    python3 tests/traces/gen_traces.py --check   # fail if any trace is stale
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VENDOR = ROOT / "tests" / "vendor"
sys.path.insert(0, str(VENDOR))

import tick  # noqa: E402  (the vendored copy, asserted below)
from tick.descriptor import CommandDescriptor  # noqa: E402
from tick.enums import CommandID, Flag, Status, TargetFamily  # noqa: E402
from tick.hdl_model import AxiTimedCommandSchedulerModel  # noqa: E402

if Path(tick.__file__).resolve().parent != VENDOR / "tick":
    raise SystemExit(f"tick imported from {tick.__file__}, not the vendored copy")

TICK_HZ = 100_000_000  # tron: Tick's TIME_TICK_HZ on hopcap's image
QUEUE_DEPTH = 16

# Register-visibility rules: how model state maps onto the driver's
# attributes. None of them changes the model's behaviour.
RULES = [
    "completion_* read 0 while the slot is empty (RTL REG_COMP_0..7 read "
    "completion_valid ? word : 0, axi_timed_command_scheduler.v:624-631); "
    "the model keeps its last completion after an ack",
    "a dropped completion's executed timestamp is 0 (RTL completion_words[4/5] "
    "<= 0 on every drop path); the model says None",
    "time advances event by event: the generator steps the model to every due "
    "queue timestamp, as the RTL dispatches on the cycle a command comes due",
    "every write is followed by a settle (model.advance_to(now)): the RTL's "
    "dispatch runs on the cycle after a push, ack or flush",
    "gpio_out is fake-only state (the driver exposes no GPIO attribute)",
]

EXPECT_KEYS = (
    "current_time_ticks",
    "queue_level",
    "late_count",
    "overflow_count",
    "pps_present",
    "time_aligned",
    "time_epoch_ticks",
    "completion_sequence_id",
    "completion_status",
    "completion_command_id",
    "completion_target_id",
    "completion_requested_timestamp_ticks",
    "completion_executed_timestamp_ticks",
    "completion_detail",
    "gpio_out",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def descriptor_csv(d: CommandDescriptor) -> str:
    """Twelve little-endian 32-bit words, as the driver's command_descriptor parses."""
    words = struct.unpack("<12I", d.pack())
    return ",".join(f"0x{w:08x}" for w in words)


def completion_dict(c) -> dict:
    return {
        "sequence_id": c.sequence_id,
        "command_id": int(c.command_id),
        "target_id": int(c.target_id),
        "status": int(Status[c.status_name]),
        "status_name": c.status_name,
        "requested_timestamp_ticks": c.requested_timestamp_ticks,
        "executed_timestamp_ticks": c.executed_timestamp_ticks or 0,
        "detail": c.detail,
    }


class Recorder:
    """Drives the model and records one trace."""

    def __init__(self, name: str, description: str):
        self.name = name
        self.description = description
        self.m = AxiTimedCommandSchedulerModel(queue_depth=QUEUE_DEPTH, tick_hz=TICK_HZ)
        self.steps: list[dict] = []
        self.source = "model"

    # -- model plumbing ---------------------------------------------------
    @property
    def now(self) -> int:
        return self.m.current_time_ticks

    def _settle(self) -> list:
        return list(self.m.advance_to(self.m.current_time_ticks))

    def _step_to(self, target: int) -> list:
        out = []
        while True:
            due = sorted(
                {e.timestamp_ticks for e in self.m._scheduler._queue
                 if self.now < e.timestamp_ticks <= target}
            )
            if not due:
                break
            out += self.m.advance_to(due[0])
        out += self.m.advance_to(target)
        return out

    def _snapshot(self) -> dict:
        m = self.m
        c = m.completion if m.completion_pending else None
        return {
            "current_time_ticks": m.current_time_ticks,
            "queue_level": m.queue_level,
            "late_count": m.late_count,
            "overflow_count": m.overflow_count,
            "pps_present": int(m.pps_present),
            "time_aligned": int(m.time_aligned),
            "time_epoch_ticks": m.time_epoch_ticks,
            "completion_sequence_id": c.sequence_id if c else 0,
            "completion_status": int(Status[c.status_name]) if c else 0,
            "completion_command_id": int(c.command_id) if c else 0,
            "completion_target_id": int(c.target_id) if c else 0,
            "completion_requested_timestamp_ticks": c.requested_timestamp_ticks if c else 0,
            "completion_executed_timestamp_ticks": (c.executed_timestamp_ticks or 0) if c else 0,
            "completion_detail": c.detail if c else 0,
            "gpio_out": m.gpio_out,
        }

    def _record(self, t, op, attr, value, completions, note=None, rtl=None):
        step = {"t_ticks": t, "op": op, "attr": attr, "value": value,
                "completions": [completion_dict(c) for c in completions],
                "expect": self._snapshot()}
        if note:
            step["note"] = note
        if rtl:
            step["source"] = "rtl"
            step["rtl_ref"] = rtl
            self.source = "rtl"
        self.steps.append(step)

    # -- scenario verbs -----------------------------------------------------
    def advance(self, t: int, note=None):
        assert t >= self.now, (self.name, t, self.now)
        t0 = self.now
        self._record(t0, "advance", None, t - t0, self._step_to(t), note)

    def push(self, cmd: str, seq: int, ts: int, *, report: bool, late: str = "drop",
             note=None, repeat_count=None, period_ticks=None, **kw):
        """Push one descriptor through the model's push_* API."""
        if cmd == "gpio":
            mask, value = kw["mask"], kw["value"]
            args = (mask, value, repeat_count or 0, period_ticks or 0)
            cid, tid = CommandID.GPIO_WRITE, TargetFamily.GPIO
        elif cmd == "marker":
            line, width = kw["line"], kw.get("width", 1)
            args = (line, max(width, 1), repeat_count or 0, period_ticks or 0)
            cid, tid = CommandID.MARKER_PULSE, TargetFamily.MARKER
        else:
            raise ValueError(cmd)
        flags = Flag.REPORT_COMPLETION if report else Flag.NONE
        if repeat_count is not None:
            flags |= Flag.REPEAT
        flags |= Flag.LATE_DROP if late == "drop" else Flag.LATE_EXECUTE_NOW
        d = CommandDescriptor(command_id=cid, target_id=tid, timestamp_ticks=ts,
                              flags=flags, sequence_id=seq, args=args)
        common = dict(sequence_id=seq, timestamp=ts,
                      late_policy="drop" if late == "drop" else "execute_now",
                      report_completion=report, repeat_count=repeat_count,
                      period_ticks=period_ticks)
        if cmd == "gpio":
            first = self.m.push_gpio(mask=mask, value=value, **common)
        else:
            first = self.m.push_marker(marker_id=line, width_ticks=width, **common)
        comps = ([first] if first else []) + self._settle()
        self._record(self.now, "write", "command_descriptor", descriptor_csv(d), comps, note)

    def push_rtl_late_lost(self, seq: int, ts: int, *, report: bool, note: str):
        """A late LATE_DROP descriptor pushed while the completion slot is full.

        The RTL counts it late and counts the lost report as an overflow, and
        neither queues nor executes it (axi_timed_command_scheduler.v:846-861,
        the final else of the late-on-submit branch; late_report_completion_blocked
        at line 356 only covers LATE_EXECUTE_NOW). The model instead queues it
        and reports LATE_DROPPED after the ack, so this step applies the RTL.
        """
        assert self.m.completion_pending and ts < self.now
        flags = (Flag.REPORT_COMPLETION if report else Flag.NONE) | Flag.LATE_DROP
        d = CommandDescriptor(command_id=CommandID.GPIO_WRITE, target_id=TargetFamily.GPIO,
                              timestamp_ticks=ts, flags=flags, sequence_id=seq,
                              args=(0x2, 0x2, 0, 0))
        self.m._scheduler._increment_late()
        self.m._scheduler._increment_overflow()
        self._record(self.now, "write", "command_descriptor", descriptor_csv(d), [], note,
                     rtl="hdl/library/axi_timed_command_scheduler/"
                         "axi_timed_command_scheduler.v:846-861")

    def ack(self, note=None):
        self.m.ack_completion()
        self._record(self.now, "write", "completion_ack", "1", self._settle(), note)

    def flush(self, note=None):
        self.m.flush_queue()
        self._record(self.now, "write", "flush_queue", "1", self._settle(), note)

    def epoch(self, ticks: int, note=None):
        self.m.write_time_epoch(ticks)
        self._record(self.now, "write", "time_epoch_ticks", str(ticks), self._settle(), note)

    def arm(self, note=None):
        self.m.arm_time_next_pps()
        self._record(self.now, "write", "arm_time_next_pps", "1", self._settle(), note)

    def pps(self, note=None):
        t0 = self.now
        self.m.pps_edge()
        self._record(t0, "pps_edge", None, None, self._settle(), note)

    def trace(self, provenance: dict) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "source": self.source,
            "config": {"tick_hz": TICK_HZ, "queue_depth": QUEUE_DEPTH},
            "provenance": provenance,
            "rules": RULES,
            "steps": self.steps,
        }


# ---- scenarios --------------------------------------------------------------

def ordered_queue() -> Recorder:
    r = Recorder("ordered_queue",
                 "Out-of-order submission executes in timestamp order; equal "
                 "timestamps keep submission order.")
    r.push("gpio", 1, 3000, report=True, mask=0x2, value=0x2)
    r.push("gpio", 2, 1000, report=True, mask=0x4, value=0x4)
    r.push("gpio", 3, 2000, report=True, mask=0x8, value=0x8)
    r.push("gpio", 4, 2000, report=False, mask=0x8, value=0x0,
           note="same timestamp as seq 3: runs after it (submission order)")
    r.advance(999)
    r.advance(1000, note="seq 2 executes first")
    r.ack()
    r.advance(2000, note="seq 3 then seq 4 at the same tick; only seq 3 reports")
    r.ack()
    r.advance(3000)
    r.ack()
    r.advance(4000)
    return r


def late_on_submit_drop() -> Recorder:
    r = Recorder("late_on_submit_drop",
                 "A descriptor already in the past on submit with LATE_DROP "
                 "completes LATE_DROPPED without executing; a drop reports even "
                 "without REPORT_COMPLETION.")
    r.advance(10_000)
    r.push("gpio", 10, 5000, report=True, mask=0x2, value=0x2)
    r.ack()
    r.push("gpio", 11, 6000, report=False, mask=0x2, value=0x2,
           note="unreported, but a drop is always reported")
    r.ack()
    r.advance(20_000)
    return r


def late_execute_now() -> Recorder:
    r = Recorder("late_execute_now",
                 "LATE_EXECUTE_NOW on a past timestamp executes at once and "
                 "reports LATE_EXECUTED at the submit tick; with the slot full a "
                 "reported one waits in the queue and executes at the ack.")
    r.advance(10_000)
    r.push("gpio", 20, 5000, report=True, late="execute_now", mask=0x2, value=0x2)
    r.ack()
    r.push("gpio", 21, 6000, report=False, late="execute_now", mask=0x4, value=0x4,
           note="unreported execute-now: executes, no completion")
    r.push("gpio", 22, 11_000, report=True, mask=0x8, value=0x8)
    r.advance(11_000, note="seq 22 fills the slot")
    r.push("gpio", 23, 7000, report=True, late="execute_now", mask=0x2, value=0x0,
           note="slot full: queued late-on-submit, not executed yet")
    r.advance(12_000)
    r.ack(note="slot freed: seq 23 executes now, LATE_EXECUTED at the ack tick")
    r.ack()
    return r


def flush() -> Recorder:
    r = Recorder("flush",
                 "flush_queue discards every queued descriptor; the completion "
                 "slot is untouched.")
    r.push("gpio", 30, 100, report=True, mask=0x2, value=0x2)
    r.advance(100)
    for i in range(5):
        r.push("gpio", 31 + i, 1000 + 100 * i, report=(i == 2), mask=0x4, value=0x4)
    r.flush(note="queue empties; seq 30's completion stays in the slot")
    r.advance(5000, note="nothing executes")
    r.ack()
    r.advance(6000)
    return r


def overflow() -> Recorder:
    r = Recorder("overflow",
                 "A 17th descriptor on a full 16-deep queue completes OVERFLOW "
                 "and counts; with the slot full the report is lost and only "
                 "overflow_count rises.")
    for i in range(16):
        r.push("gpio", 40 + i, 1000 + 10 * i, report=False, mask=0x2, value=(i & 1) << 1)
    r.push("gpio", 56, 5000, report=True, mask=0x4, value=0x4,
           note="17th entry: OVERFLOW")
    r.push("gpio", 57, 5000, report=True, mask=0x4, value=0x4,
           note="18th entry with the slot full: report lost, overflow_count 2")
    r.ack()
    r.advance(2000, note="the 16 queued entries all execute")
    return r


def completion_slot_hold() -> Recorder:
    r = Recorder("completion_slot_hold",
                 "A reported head command is held while the completion slot is "
                 "unacked (and holds everything behind it), then fires at the ack "
                 "and records executed = requested (tfcollins/tick#38). An "
                 "unreported head is not held.")
    r.push("gpio", 60, 1000, report=True, mask=0x2, value=0x2)
    r.push("gpio", 61, 1500, report=False, mask=0x4, value=0x4)
    r.push("gpio", 62, 2000, report=True, mask=0x8, value=0x8)
    r.push("gpio", 63, 3000, report=False, mask=0x2, value=0x0)
    r.advance(1000, note="seq 60 fills the slot")
    r.advance(1500, note="seq 61 is unreported: executes on time")
    r.advance(5000, note="seq 62 is held (slot full); seq 63 waits behind it")
    r.ack(note="seq 62 fires now (gpio changes at 5000) but records executed = "
               "requested = 2000; seq 63 follows")
    r.ack()
    return r


def repeat() -> Recorder:
    r = Recorder("repeat",
                 "A REPEAT series fires count times period apart, detail = "
                 "occurrence index; an occurrence held past its tick by a full "
                 "slot is late, drops (LATE_DROP) and rebases to now + period.")
    r.push("gpio", 70, 1000, report=True, mask=0x2, value=0x2,
           repeat_count=3, period_ticks=1000)
    r.advance(1000)
    r.ack()
    r.advance(2000)
    r.ack()
    r.advance(3000)
    r.ack()
    r.advance(4000, note="series done")
    r.push("gpio", 71, 5000, report=True, mask=0x4, value=0x4,
           repeat_count=3, period_ticks=1000)
    r.advance(5000)
    r.advance(6500, note="occurrence 1 (6000) held by the unacked slot")
    r.ack(note="occurrence 1 is now late: LATE_DROPPED, next at 6500 + 1000")
    r.ack()
    r.advance(7500)
    r.ack()
    r.advance(9000)
    return r


def pps_align() -> Recorder:
    r = Recorder("pps_align",
                 "A PPS edge sets pps_present (sticky); arm_time_next_pps clears "
                 "time_aligned and the next edge loads time_epoch_ticks and sets it.")
    r.advance(1000)
    r.pps(note="unarmed edge: pps_present 1, time unchanged")
    r.epoch(500_000_000)
    r.arm()
    r.advance(1100)
    r.pps(note="armed edge: time = epoch, time_aligned 1")
    r.advance(500_001_000)
    r.arm(note="re-arm clears time_aligned")
    r.pps()
    return r


def late_drop_slot_full() -> Recorder:
    r = Recorder("late_drop_slot_full",
                 "libdatax's default (LATE_DROP): a late descriptor pushed while "
                 "the slot is full is counted late, its report is lost (overflow "
                 "rises) and it never executes. RTL-sourced: the model diverges.")
    r.push("gpio", 80, 1000, report=True, mask=0x4, value=0x4)
    r.advance(2000)
    r.push_rtl_late_lost(81, 1500, report=True,
                         note="reported LATE_DROP, late with the slot full")
    r.push_rtl_late_lost(82, 1600, report=False,
                         note="unreported LATE_DROP, late with the slot full")
    r.ack(note="nothing queued behind: the slot just empties")
    r.advance(3000)
    return r


SCENARIOS = [ordered_queue, late_on_submit_drop, late_execute_now, flush, overflow,
             completion_slot_hold, repeat, pps_align, late_drop_slot_full]


def provenance() -> dict:
    pins = json.loads((ROOT / "pins.json").read_text())
    return {
        "tick_revision": pins["tick"]["revision"],
        "model": "tests/vendor/tick/hdl_model.py",
        "model_sha256": sha256(VENDOR / "tick" / "hdl_model.py"),
        "scheduler_sha256": sha256(VENDOR / "tick" / "scheduler.py"),
        "generator": "tests/traces/gen_traces.py",
        "generator_sha256": sha256(Path(__file__).resolve()),
    }


def render_all() -> dict[str, str]:
    prov = provenance()
    return {
        f"{s.__name__}.json": json.dumps(s().trace(prov), indent=1, sort_keys=True) + "\n"
        for s in SCENARIOS
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="compare, do not write")
    a = ap.parse_args()
    stale = []
    for name, text in render_all().items():
        path = HERE / name
        if a.check:
            if not path.exists() or path.read_text() != text:
                stale.append(name)
        else:
            path.write_text(text)
    if stale:
        print("stale traces: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
