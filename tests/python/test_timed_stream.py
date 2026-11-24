# SPDX-License-Identifier: MIT
"""P3b S3 Python surface: start_at, block timestamps, TX preload, stream gating,
LateError from failed timed streams, the P3b capability flags.

Runs against the fake Tick and the fake DMA (1 MSPS: exactly 100 ticks per
frame) bound to the adrv9009-zu11eg emulator fixture. Device time moves only
when a test advances it.
"""
import numpy as np
import pytest

import datax
from conftest import MS

BS = 256
FS = 1_000_000


@pytest.fixture
def rig(make_rig):
    r = make_rig(dma_fs=FS)
    r.make_scheduler()
    return r


def _t(rig, ms):
    return rig.align(rig.now + ms * MS)


def test_rx_start_at_block0_timestamp(rig):
    ft = rig.frame_ticks
    rx = rig.dev.rx_stream(channels=[0], block_samples=BS)
    T = _t(rig, 20)
    assert rx.start_at(T) is rx
    rig.run_to(T + 3 * BS * ft)
    # one cpack drop plus the 8-frame tag strip (1 channel)
    _, info = rx.recv(BS - 8, timeout_ms=2000)
    assert info.timestamp_ticks == T + 9 * ft
    assert info.valid_samples == BS - 8 and not info.discontinuous
    _, info = rx.recv(BS, timeout_ms=2000)
    assert info.timestamp_ticks == T + (1 + BS) * ft
    assert isinstance(info.timestamp_ticks, int)
    rx.close()


def test_blockinfo_zero_when_unknown(rig):
    ft = rig.frame_ticks
    with rig.dev.rx_stream(channels=[0], block_samples=BS) as rx:
        rig.run_to(rig.now + 3 * BS * ft)
        _, info = rx.recv(BS, timeout_ms=2000)
        assert info.timestamp_ticks == 0 and isinstance(info.timestamp_ticks, int)


def test_tx_preload_start_at_gate_on_off(rig):
    ft = rig.frame_ticks
    sched = rig.dev.scheduler
    tx = rig.dev.tx_stream(channels=[0], block_samples=BS, cyclic=True)
    tone = (np.arange(BS) % 100 + 1).astype(np.complex64)
    assert tx.send(tone) == BS            # unstarted: preload
    assert rig.tx_segments() == []        # nothing transmitted yet
    T = _t(rig, 30)
    off, on = T + 1000 * ft, T + 2000 * ft
    with tx.start_at(T) as same:
        assert same is tx
        s1 = sched.at(off).gate_off(tx)
        s2 = sched.at(on).gate_on(tx)
        assert s2 > s1 >= 0
        sched.commit()
        rig.run_to(T + 3000 * ft)
        segs = rig.tx_segments()
    assert not segs[0].muted and segs[0].t_start == T
    muted = [s for s in segs if s.muted]
    assert len(muted) == 1 and muted[0].t_start == off and muted[0].t_end == on
    assert segs[-1].t_start >= on and not segs[-1].muted
    assert not any(s.underflow for s in segs)


def test_gate_rx_capability_error(rig):
    rx = rig.dev.rx_stream(channels=[0], block_samples=BS)
    rx.start_at(_t(rig, 30))
    with pytest.raises(datax.CapabilityError):
        rig.dev.scheduler.at(_t(rig, 60)).gate_on(rx)
    rx.close()


def test_gate_untimed_error(rig):
    with rig.dev.tx_stream(channels=[0], block_samples=BS, cyclic=True) as tx:
        tx.send(np.ones(BS, dtype=np.complex64))
        for call in ("gate_on", "gate_off"):
            with pytest.raises(datax.Error) as e:
                getattr(rig.dev.scheduler.at(_t(rig, 60)), call)(tx)
            assert e.value.errno == 22 and not isinstance(e.value, datax.CapabilityError)


def test_recv_late_error(rig):
    ft = rig.frame_ticks
    rx = rig.dev.rx_stream(channels=[0], block_samples=BS)
    T = _t(rig, 30)
    rig.pause(True)
    rx.start_at(T)
    rig.advance(T - 5 * MS - rig.now)    # the poller cannot write the open in time
    rig.pause(False)
    rig.run_to(T + 3 * BS * ft)
    with pytest.raises(datax.LateError):
        rx.recv(BS, timeout_ms=2000)
    assert not rig.gate_level(rig.cfg.rx_level)   # never opened, never shifted
    rx.close()


def test_with_after_start_at(rig):
    ft = rig.frame_ticks
    T = _t(rig, 30)
    with rig.dev.rx_stream(channels=[0], block_samples=BS).start_at(T) as rx:
        # entering did not call start() again: no forced-open gate before T
        assert not rig.gate_level(rig.cfg.rx_level)
        rig.run_to(T + 3 * BS * ft)
        _, info = rx.recv(BS - 8, timeout_ms=2000)
        assert info.timestamp_ticks == T + 9 * ft
    assert rx.closed


@pytest.mark.parametrize("gate_map,flags", [
    ("rx-dma,tx-dma,rx-mode,tx-mode",
     {"timed_commands", "rx_arm", "tx_arm", "stream_gate", "timestamps"}),
    (None, {"timed_commands"}),
    ("-,tx-dma,-,tx-mode", {"timed_commands", "tx_arm", "stream_gate"}),
])
def test_caps_p3b_flags(make_rig, gate_map, flags):
    r = make_rig(fhm=False, dma_fs=FS, gate_map=gate_map)
    assert set(r.dev.caps) == flags
