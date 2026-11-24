# SPDX-License-Identifier: MIT
"""The hardware session that segfaulted on tron (P3b S4 run 1, timed-dma),
replayed on the fakes with chip B's FHM wired (map names fhm-b, both chip
fakes bound): P2 stream counters, then P3a's staircase, hop schedule and the
priming-edge restage cycle, plus chip-B tunes. One process, gc forced between
steps, so heap damage from any ctypes call surfaces here (run it under ASan
with PYTHONMALLOC=malloc to see the exact write)."""
import gc
import sys

import pytest

import datax
from conftest import MS, ROOT

sys.path.insert(0, str(ROOT / "tests" / "hardware"))
import hwtimed as ht  # noqa: E402
import loopback as lb  # noqa: E402

CHIPB_MAP = "rx-dma,tx-dma,rx-mode,tx-mode,fhm-b,tx-start-fullmask-only"
TABLE = [2_420_000_000, 2_440_000_000, 2_460_000_000, 2_480_000_000]
PARK = 2_480_000_000


@pytest.fixture
def rig(make_rig):
    return make_rig(dma_fs=1_000_000, gate_map=CHIPB_MAP, chip_b=True)


def _tunes(rig, sched, freqs, t0, period, ch=0):
    seqs = [sched.at(t0 + i * period).tune(f, ch=ch) for i, f in enumerate(freqs)]
    sched.commit()
    rig.run_to(t0 + len(freqs) * period)
    return [sched.wait(s, 3000) for s in seqs]


def _restage(rig, table):
    rig.dev.scheduler.close()
    rig.dev.stage_hop_table(table)
    return rig.make_scheduler()


def test_hw_session_sequence_chip_b(rig):
    dev = rig.dev
    assert "fhm_hop" in dev.caps
    # P2 smoke: an RX stream and its counters (test_rx_throughput_recorded)
    with dev.rx_stream(channels=(0,), block_samples=256) as rx:
        rig.run_to(rig.now + 2000 * 100)
        rx.recv(256, timeout_ms=3000)
        st = lb.stream_stats(rx)
    assert st["blocks_done"] >= 1
    gc.collect()

    # test_staircase: reported GPIO edges on bits 3:2
    sched = rig.make_scheduler()
    t0 = rig.now + 50 * MS
    plan = ht.staircase(t0, 8, 10 * MS)
    seqs = [sched.at(t).gpio(m, v, report=True) for t, m, v in plan]
    sched.at(plan[-1][0] + 10 * MS).gpio(ht.STAIR_MASK, 0)
    sched.commit()
    rig.run_to(plan[-1][0] + 20 * MS)
    done = sched.wait_all(timeout=3.0)
    assert [c.seq for c in done] == seqs and ht.all_on_time(done)
    sched.close()
    gc.collect()

    # test_hop_schedule: restage, four tunes on chip A
    sched = _restage(rig, TABLE)
    done = _tunes(rig, sched, TABLE, rig.now + 200 * MS, 200 * MS)
    assert all(c.status is datax.CompletionStatus.OK for c in done)
    assert rig.pll_hz() == TABLE[-1]
    gc.collect()

    # test_priming_edge_lands_on_init: park, restage, one tune, repeated
    for _ in range(3):
        sched = _restage(rig, [PARK, TABLE[0]])
        assert _tunes(rig, sched, [PARK], rig.now + 150 * MS, 50 * MS)[0].status \
            is datax.CompletionStatus.OK
        sched = _restage(rig, TABLE)
        assert _tunes(rig, sched, [TABLE[0]], rig.now + 200 * MS, 50 * MS)[0].status \
            is datax.CompletionStatus.OK
        gc.collect()

    # chip B: tunes on channel 2 move chip B's fake, not chip A's
    a_before = rig.pll_hz()
    sched = _restage(rig, TABLE)
    done = _tunes(rig, sched, TABLE[1:3], rig.now + 200 * MS, 200 * MS, ch=2)
    assert all(c.status is datax.CompletionStatus.OK for c in done), done
    assert rig.fk.fake_fhm_pll_hz(rig.fhm_b) == TABLE[2]
    assert rig.pll_hz() == a_before
    sched.close()
    gc.collect()
