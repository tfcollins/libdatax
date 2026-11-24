# SPDX-License-Identifier: MIT
"""Tier 1 for the gated-burst analysis of the P3b hardware suite
(tests/hardware/test_hw_timed_stream.py::_burst_analysis) on synthetic
captures: an ideal burst passes; a lost frame, a long burst and the tron
run 2 tone (where the gain-fitted lag made every frame 'mismatch') are each
reported for what they are."""
import sys

import numpy as np
import pytest

from conftest import ROOT

sys.path.insert(0, str(ROOT / "tests" / "hardware"))
import hwtimed as ht  # noqa: E402
import loopback as lb  # noqa: E402
import test_hw_timed_stream as ts  # noqa: E402

P, FS, HZ, STRIP = 64, 1000, 1000, 8       # one frame per tick keeps the numbers readable
D_OFF, D_ON, T = 200, 400, 10_000
N_MUTE = ht.TX_START_MUTE_READS


class _Done:
    def __init__(self, req):
        self.seq, self.requested_ticks, self.executed_ticks, self.detail = 1, req, req, ""
        self.status = type("S", (), {"name": "OK"})()


def _capture(first=11, extra_off=0, extra_on=0, drop_at=None, total=800, nblk=4):
    tone = ht.tone_raw(P)
    exp = lb.expected_loopback("adrv9009", tone)
    k = np.arange(total + 1)
    s = exp[(k - first) % P].copy()
    s[:first] = 0
    e_off, e_on = first - N_MUTE + D_OFF + extra_off, first - N_MUTE + D_ON + extra_on
    s[e_off:e_on] = 0
    if drop_at is not None:
        s = np.delete(s, drop_at, axis=0)
    s = s[:total]
    size = total // nblk
    parts = [s[i * size:(i + 1) * size] for i in range(nblk)]
    blocks, start = [], 0
    for prt in parts:
        blocks.append({"start": start, "frames": prt.shape[0], "discontinuous": False,
                       "valid_samples": prt.shape[0],
                       "timestamp_ticks": ht.sample_time(T, STRIP, start, FS, HZ)})
        start += prt.shape[0]
    r = {"T": T, "t_off": T + D_OFF, "t_on": T + D_ON, "cost": {}, "blocks": blocks,
         "parts": parts, "edge": {"ticks": T}, "edge_read_at": T,
         "done": [_Done(T + D_OFF), _Done(T + D_ON)], "stats": {}}
    return r, exp


def _run(r, exp, ref_first=11, c=0):
    return ts._burst_analysis(None, None, r, exp, STRIP, FS, HZ, D_OFF, D_ON, ref_first,
                              need=D_ON + 3 * P, c=c)


def test_ideal_burst_passes():
    out = _run(*_capture())
    assert out["fails"] == [], out["fails"]
    assert out["contiguity"]["lag"] == 11 and out["contiguity"]["mismatches"] == 0
    assert out["edges"]["residuals"] == {"first": 0, "off": 0, "on": 0, "spacing": 0}
    # blocks 1 and 2 begin beside or inside the muted span: no full period there
    assert out["contiguity"]["block_lags"][3] == 11


def test_lost_frame_is_located():
    out = _run(*_capture(drop_at=500))           # after the gate opens again
    c = out["contiguity"]
    assert c["mismatches"] > 0 and c["first_mismatches"][0] == 500
    assert c["stretch_lags"]["before_off"] == 11
    assert c["block_lags"][3] == 10                 # a dropped frame: one frame earlier after it
    assert any("frames lost or repeated" in f for f in out["fails"])


@pytest.mark.parametrize("extra_off,extra_on,why", [
    (30, 72, "burst length off"),                # tron run 2: off +30, on +72 frames late
    (5, 5, "gate edges off the commanded times"),
])
def test_edge_latency_reported(extra_off, extra_on, why):
    out = _run(*_capture(extra_off=extra_off, extra_on=extra_on))
    assert out["contiguity"]["mismatches"] == 0     # the tone itself is intact
    assert any(why in f for f in out["fails"]), out["fails"]


def test_image_edge_constant_is_folded_into_the_expected_edges():
    """Option 1 item 7: tron's timed-dma edges land 7 frames early every burst
    (spacing 0). With the image's constant they pass; the raw residual stays in
    the evidence. Without it, they fail as before."""
    r, exp = _capture(extra_off=-7, extra_on=-7)
    out = _run(r, exp, c=-7)
    assert out["fails"] == [], out["fails"]
    assert out["edges"]["residuals"] == {"first": 0, "off": 0, "on": 0, "spacing": 0}
    assert out["edges"]["raw_residuals"] == {"off": -7, "on": -7}
    assert out["edges"]["edge_offset_frames"] == -7
    out = _run(*_capture(extra_off=-7, extra_on=-7), c=0)
    assert any("gate edges off the commanded times" in f for f in out["fails"]), out["fails"]


def test_image_edge_constant_pinned_for_timed_dma():
    assert ts.gate_edge_offset_frames() == -7
