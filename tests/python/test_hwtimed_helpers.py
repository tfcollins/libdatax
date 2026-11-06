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
    with pytest.raises(ValueError, match="DATAX_HW_IMAGE"):
        ht.normalize_image("sfp")


@pytest.mark.parametrize("image,marks,skipped", [
    ("stock", ["p3a"], True),
    ("stock", ["needs_tx"], False),
    ("stock", [], False),
    ("hopcap-sfp", ["p3a"], False),
    ("hopcap-sfp", ["needs_tx"], True),
    ("hopcap-sfp", [], False),
])
def test_image_skip_reason(image, marks, skipped):
    assert (ht.image_skip_reason(image, marks) is not None) == skipped


def test_us_to_ticks_rounds_up():
    assert ht.us_to_ticks(1000, 245_760_000) == 245_760
    assert ht.us_to_ticks(1, 245_760_000) == 246       # 245.76 -> 246
    assert ht.us_to_ticks(0, 245_760_000) == 0


def test_staircase_counts_on_bits_3_1():
    s = ht.staircase(1000, 8, 50)
    assert [t for t, _, _ in s] == [1000 + 50 * i for i in range(8)]
    assert {m for _, m, _ in s} == {0xE}
    assert [v for _, _, v in s] == [0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE, 0x0]
    assert all(v & ~0xE == 0 for _, _, v in s)   # bit 0 never touched


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
