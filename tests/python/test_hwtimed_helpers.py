# SPDX-License-Identifier: MIT
"""Tier 1 tests for the pure helpers of the P3a hardware suite
(tests/hardware/hwtimed.py) and for the evidence redaction it reuses."""
import sys
from types import SimpleNamespace

import pytest

from conftest import ROOT

sys.path.insert(0, str(ROOT / "tests" / "hardware"))
import hwtimed as ht  # noqa: E402
import loopback as lb  # noqa: E402


def test_normalize_image():
    assert ht.normalize_image(None) == "stock"
    assert ht.normalize_image("") == "stock"
    assert ht.normalize_image("hopcap-sfp") == "hopcap-sfp"
    assert ht.normalize_image("timed-dma") == "timed-dma"
    with pytest.raises(ValueError, match="DATAX_HW_IMAGE"):
        ht.normalize_image("sfp")


def test_image_required_when_hw_required():
    with pytest.raises(ValueError, match="DATAX_HW_IMAGE.*required"):
        ht.normalize_image(None, required=True)
    with pytest.raises(ValueError, match="DATAX_HW_IMAGE.*required"):
        ht.normalize_image("", required=True)
    assert ht.normalize_image("stock", required=True) == "stock"


def test_normalize_pps():
    assert ht.normalize_pps(None) == "internal"
    assert ht.normalize_pps("") == "internal"
    assert ht.normalize_pps("none") == "none"
    with pytest.raises(ValueError, match="DATAX_HW_PPS"):
        ht.normalize_pps("external")


@pytest.mark.parametrize("image,marks,skipped", [
    ("stock", ["p3a"], True),
    ("stock", ["p3b"], True),
    ("stock", ["needs_tx"], False),
    ("stock", [], False),
    ("hopcap-sfp", ["p3a"], False),
    ("hopcap-sfp", ["p3b"], True),
    ("hopcap-sfp", ["needs_tx"], True),
    ("hopcap-sfp", [], False),
    ("timed-dma", ["p3a"], False),
    ("timed-dma", ["p3b"], False),
    ("timed-dma", ["needs_tx"], False),
    ("timed-dma", ["p3b", "needs_tx"], False),
    ("timed-dma", [], False),
    ("hopcap-sfp", ["p3b", "needs_tx"], True),
])
def test_image_skip_reason(image, marks, skipped):
    assert (ht.image_skip_reason(image, marks) is not None) == skipped


def test_us_to_ticks_rounds_up():
    assert ht.us_to_ticks(1000, 245_760_000) == 245_760
    assert ht.us_to_ticks(1, 245_760_000) == 246       # 245.76 -> 246
    assert ht.us_to_ticks(0, 245_760_000) == 0


def test_staircase_counts_on_bits_3_2():
    s = ht.staircase(1000, 8, 50)
    assert [t for t, _, _ in s] == [1000 + 50 * i for i in range(8)]
    assert {m for _, m, _ in s} == {0xC}
    assert [v for _, _, v in s] == [0x4, 0x8, 0xC, 0x0, 0x4, 0x8, 0xC, 0x0]
    assert all(v & 0x3 == 0 for _, _, v in s)    # bits 1:0 (the FHM triggers) never touched
    assert ht.LATE_GPIO & 0x3 == 0 and ht.LATE_GPIO & ht.STAIR_MASK


def test_dense_spacing_and_pulse():
    assert ht.dense_spacing_us(5280, 1.5) == 8000
    assert ht.dense_spacing_us(5300, 1.0) == 5300
    assert ht.pulse_us(8000) == 4000
    assert ht.pulse_us(40000) == 10000          # never above the proven pulse
    with pytest.raises(ValueError):
        ht.dense_spacing_us(5300, 0.9)


def test_hop_sequence_and_times():
    seq = ht.hop_sequence([1, 2, 3], 7)
    assert seq == [1, 2, 3, 1, 2, 3, 1]
    assert all(a != b for a, b in zip(seq, seq[1:]))
    with pytest.raises(ValueError):
        ht.hop_sequence([1], 3)
    assert ht.hop_times(10, 3, 5) == [10, 15, 20]


def test_names_matching():
    got = ht.names_matching(["multichip_sync", "Phase_offset", "gain", "jesd204_fsm_ctrl"])
    assert got == ["Phase_offset", "multichip_sync"]
    assert ht.names_matching([]) == []


def test_alignment_checks():
    hz = 1000
    assert ht.offset_after_align_ok(5000 + 100, 5000, hz)
    assert not ht.offset_after_align_ok(5000 + 900, 5000, hz)
    assert not ht.offset_after_align_ok(4999, 5000, hz)
    assert ht.advance_ok(2000, hz, 2.01)
    assert not ht.advance_ok(2000, hz, 2.5)


def test_completion_log_and_on_time():
    ok = SimpleNamespace(seq=1, status=SimpleNamespace(name="OK"),
                         requested_ticks=100, executed_ticks=100, detail="")
    late = SimpleNamespace(seq=2, status=SimpleNamespace(name="LATE"),
                           requested_ticks=200, executed_ticks=0, detail="x")
    log = ht.completion_log([ok, late])
    assert log[0] == {"seq": 1, "status": "OK", "requested": 100, "executed": 100,
                      "skew_ticks": 0, "detail": ""}
    assert log[1]["status"] == "LATE" and log[1]["skew_ticks"] == -200
    assert ht.all_on_time([ok]) and not ht.all_on_time([ok, late])


def test_evidence_redaction_is_reused():
    # The timed suite records through lb.redact_*: no address survives.
    assert lb.redact_uri("ip:10.1.2.3") == "ip:host"
    assert "10.1.2.3" not in lb.redact_text("BOOT.BIN from 10.1.2.3 on Linux tron 6.1")


def test_redact_tree_scrubs_nested_strings():
    got = ht.redact_tree({"a": ["fail at 10.1.2.3", {"b": "Linux tron 6.1"}], "n": 5, "t": ("x",)})
    assert got == {"a": ["fail at <ip>", {"b": "Linux host 6.1"}], "n": 5, "t": ["x"]}


def test_flush_while_high():
    pulse = 100
    assert ht.flush_while_high(before=1010, after=1050, rise=1000, pulse=pulse)
    assert not ht.flush_while_high(before=990, after=1010, rise=1000, pulse=pulse)   # before the rise
    assert not ht.flush_while_high(before=1010, after=1100, rise=1000, pulse=pulse)  # after the fall


# ---- P3b timed-stream helpers ----------------------------------------------------------

def test_frames_to_ticks_rounds_half_up():
    assert ht.frames_to_ticks(1, 2, 1) == 1          # 0.5 -> 1 (round() would give 0)
    assert ht.frames_to_ticks(3, 2, 1) == 2          # 1.5 -> 2
    assert ht.frames_to_ticks(5, 2, 1) == 3          # 2.5 -> 3 (round() would give 2)
    assert ht.frames_to_ticks(9, 245_760_000, 100_000_000) == 4      # 3.662 -> 4
    assert ht.frames_to_ticks(4_915_200, 245_760_000, 100_000_000) == 2_000_000   # 20 ms
    assert ht.ticks_to_frames(2_000_000, 245_760_000, 100_000_000) == 4_915_200
    assert ht.ticks_to_frames(1, 245_760_000, 100_000_000) == 2      # 2.4576 -> 2
    with pytest.raises(ValueError):
        ht.frames_to_ticks(1, 0, 1)


def test_frames_to_ticks_matches_libdatax():
    """The suite's own rounding equals libdatax's datax_internal_frames_to_ticks."""
    import ctypes as C
    from datax._lib import get_lib
    fn = get_lib().datax_internal_frames_to_ticks
    fn.restype, fn.argtypes = C.c_ulonglong, [C.c_ulonglong] * 3
    for n, fs, hz in [(9, 245_760_000, 100_000_000), (1 + 8 + 65536, 245_760_000, 100_000_000),
                      (3, 30_720_000, 100_000_000), (7, 2, 1), (10**12, 245_760_000, 100_000_000)]:
        assert ht.frames_to_ticks(n, fs, hz) == fn(n, fs, hz), (n, fs, hz)


def test_strip_and_block0_timestamp():
    assert [ht.strip_frames(32, n) for n in (1, 2, 3, 4)] == [8, 4, 3, 2]   # 2/4/6/8 converters
    # one channel at 1 MSPS on a 100 MHz Tick: cpack drop + 8 tag frames = 9 frames
    assert ht.block0_timestamp(1000, 8, 1_000_000, 100_000_000) == 1000 + 900
    assert ht.sample_time(1000, 8, 0, 1_000_000, 100_000_000) == 1900
    assert ht.sample_time(1000, 8, 256, 1_000_000, 100_000_000) == 1000 + 265 * 100


def test_offset_bracket():
    assert ht.offset_bracket(1000, 1003, 1010) == (-3, 7)
    lo, hi = ht.offset_bracket(5_000_100, 100, 5_000_110)     # soft offset ~5e6
    assert lo <= 5_000_000 <= hi


def test_audible_and_runs():
    import numpy as np
    raw = np.zeros((12, 2), dtype=np.int16)
    raw[2:5, 0] = 7
    raw[8:11, 1] = -3
    aud = ht.audible(raw)
    assert aud.tolist() == [False, False, True, True, True, False, False, False,
                            True, True, True, False]
    assert ht.runs(aud) == [(2, 5), (8, 11)]
    assert ht.runs(np.ones(3, dtype=bool)) == [(0, 3)]
    assert ht.runs(np.zeros(3, dtype=bool)) == []


def test_pattern_lag_and_mismatches():
    import numpy as np
    p = 64
    rng = np.random.default_rng(3)
    exp = rng.integers(-2000, 2000, size=(p, 2)).astype(np.int16)
    lag = 23
    raw = np.zeros((200, 2), dtype=np.int16)
    raw[30:] = exp[(np.arange(30, 200) - lag) % p]
    assert ht.pattern_lag(exp, raw, 32) == lag
    mask = np.zeros(200, dtype=bool)
    mask[30:] = True
    assert ht.mismatches_at_lag(exp, raw, lag, mask) == 0
    raw[100] += 1                                             # one bad frame
    assert ht.mismatches_at_lag(exp, raw, lag, mask) == 1
    assert ht.mismatches_at_lag(exp, raw, lag + 1, mask) > 100
    with pytest.raises(ValueError):
        ht.pattern_lag(exp, raw, 180)                         # fewer than P frames left


def test_spread_ok():
    assert ht.spread_ok([10, 10, 11, 10]) == (True, 10, 1)
    assert ht.spread_ok([9, 10, 11]) == (False, 10, 2)         # max - min must be <= 1
    assert ht.spread_ok([7, 7, 7]) == (True, 7, 0)
    assert ht.spread_ok([9, 10, 12])[0] is False
    assert ht.spread_ok([])[0] is False


def test_ring_coverage_and_gate_map_roles():
    assert abs(ht.ring_coverage_s(1 << 20, 4, 245_760_000) - 0.017067) < 1e-5
    assert ht.gate_map_roles("rx-dma,tx-dma, rx-mode ,tx-mode,fhm-b\n") == \
        ["rx-dma", "tx-dma", "rx-mode", "tx-mode", "fhm-b"]
    assert ht.gate_map_roles(None) == []
