# SPDX-License-Identifier: MIT
"""Python timed control (scheduler, hops, device time, events) against the C
fake Tick and fake Talise FHM bound through the test-only hooks."""
import errno
import gc
import logging
import threading
import time

import pytest

import datax
from conftest import MS, US

A, B, C3 = 2_410_000_000, 2_420_000_000, 2_430_000_000


def _wait_for(cond, secs=3.0):
    end = time.monotonic() + secs
    while not cond() and time.monotonic() < end:
        time.sleep(0.002)
    assert cond()


def _starve(rig, sched, lead=30 * MS):
    """Commit two reported GPIO commands while the poller is paused, then let
    time pass inside their guard margin and resume: both complete LATE."""
    assert rig.lib.datax_internal_sched_pause(sched._h, True) == 0
    t = rig.now + lead
    s1 = sched.at(t).gpio(0xe, 0x2, report=True)
    s2 = sched.at(t + 3 * MS).gpio(0xe, 0x4, report=True)
    sched.commit()
    rig.advance(25 * MS)             # both are now inside the 10 ms guard margin
    assert rig.lib.datax_internal_sched_pause(sched._h, False) == 0
    return s1, s2


def test_scheduler_is_lazy_and_cached(make_rig):
    rig = make_rig()
    assert "timed_commands" in rig.dev.caps
    assert "scheduler" not in vars(rig.dev) and rig.dev._sched is None
    s = rig.make_scheduler()
    assert isinstance(s, datax.Scheduler) and not s.closed
    assert rig.dev.scheduler is s


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_scheduler_refused_without_tick(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        assert "timed_commands" not in dev.caps
        with pytest.raises(datax.CapabilityError):
            dev.scheduler
        with pytest.raises(datax.CapabilityError):
            dev.run_mcs()


@pytest.mark.parametrize("emu_uri", ["adrv9009+tick-fsm"], indirect=True)
def test_run_mcs_and_source_without_fake(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        dev.run_mcs()
        assert dev.time.source in datax.TimeSource


def test_sched_builder_and_wait(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    t = rig.now + 50 * MS
    seq = sched.at(t).gpio(0xe, 0x4, report=True)
    m = sched.at(t + 20 * MS).marker(1, 5)
    assert isinstance(seq, int) and seq > 0 and m > seq
    sched.commit()
    rig.run_to(t + 5 * MS)
    c = sched.wait(seq, 2000)
    assert isinstance(c, datax.Completion)
    assert c.seq == seq
    assert c.status is datax.CompletionStatus.OK
    assert c.requested_ticks == t and c.executed_ticks == t
    assert rig.gpio() & 0xe == 0x4
    sched.flush()


def test_report_flag(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    t = rig.now + 50 * MS
    quiet = sched.at(t).gpio(0xe, 0x2)
    loud = sched.at(t + 20 * MS).gpio(0xe, 0x4, report=True)
    sched.commit()
    rig.run_to(t + 40 * MS)
    with pytest.raises(datax.Error) as ei:
        sched.wait(quiet, 500)
    assert ei.value.errno == errno.EINVAL
    assert sched.wait(loud, 2000).status is datax.CompletionStatus.OK
    assert sched.wait_all(timeout=1.0) == []     # nothing outstanding


def test_late_error(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    # (1) commit with a command inside the guard margin: LateError, whole
    # batch discarded
    ok = sched.at(rig.now + 80 * MS).gpio(0xe, 0x2, report=True)
    late = sched.at(rig.now + 1 * MS).gpio(0xe, 0x4)
    with pytest.raises(datax.LateError) as ei:
        sched.commit()
    assert ei.value.errno == errno.ETIME
    with pytest.raises(datax.Error):
        sched.wait(ok, 100)                    # discarded: unknown seq
    assert rig.writes() == 0                   # nothing of the batch was written
    # (2) the poller could not feed in time: wait returns LATE, wait_all raises
    s1, s2 = _starve(rig, sched)
    c = sched.wait(s1, 3000)
    assert c.status is datax.CompletionStatus.LATE
    assert "keep the queue fed" in c.detail
    with pytest.raises(datax.LateError) as ei:
        sched.wait_all(timeout=3.0)
    assert [x.seq for x in ei.value.completions] == [s2]
    assert late != s1 != s2


def test_wait_timeout_is_error(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    seq = sched.at(rig.now + 500 * MS).gpio(0xe, 0x2, report=True)
    sched.commit()
    with pytest.raises(datax.Error) as ei:
        sched.wait(seq, 30)
    assert ei.value.errno == errno.ETIMEDOUT
    with pytest.raises(datax.Error) as ei:
        sched.wait_all(timeout=0.03)
    assert ei.value.errno == errno.ETIMEDOUT


def test_tune_flow(make_rig):
    rig = make_rig()
    plan = [A, B, C3]
    rig.dev.stage_hop_table(plan)
    sched = rig.make_scheduler()
    t0 = rig.now + 200 * MS
    seqs = [sched.at(t0 + i * 50 * MS).tune(f) for i, f in enumerate(plan)]
    assert all(isinstance(s, int) and s > 0 for s in seqs)
    sched.commit()
    rig.run_to(t0 + 3 * 50 * MS)
    done = sched.wait_all(timeout=3.0)
    assert [c.seq for c in done] == seqs
    assert all(c.status is datax.CompletionStatus.OK for c in done)
    assert rig.pll_hz() == C3


def test_tune_chip_b_is_capability_error(make_rig):
    rig = make_rig()
    rig.dev.stage_hop_table([A])
    sched = rig.make_scheduler()
    t = rig.now + 300 * MS
    for ch in (2, 3, 6, 7):                     # RX 2-3, TX 2-3: chip B
        with pytest.raises(datax.CapabilityError, match="chip B"):
            sched.at(t).tune(A, ch=ch)
    assert sched.at(t).tune(A, ch=0) > 0         # chip A


def test_reserved_gpio_refused(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    with pytest.raises(datax.Error) as ei:
        sched.at(rig.now + 100 * MS).gpio(0x1, 0x1)
    assert ei.value.errno == errno.EINVAL


def test_align_soft_fallback_event(make_rig):
    rig = make_rig()
    events = []
    rig.ctx.on_event(lambda *e: events.append(e))
    rig.make_scheduler()
    assert rig.dev.time.source is datax.TimeSource.FREE_RUN
    epoch = 7 * rig.dev.time.tick_hz
    rig.dev.time.align_next_pps(epoch)               # no PPS: soft fallback
    assert rig.dev.time.source is datax.TimeSource.SOFT_ALIGNED
    assert rig.dev.time.now - epoch < 100 * MS
    _wait_for(lambda: any(e[0] == datax.EventType.PPS_LOCK_CHANGE and
                          "aligned to the host clock" in e[3] for e in events))


def test_event_callback_thread_and_exceptions(make_rig, caplog):
    rig = make_rig()
    got, main = [], threading.get_ident()

    def cb(typ, ticks, seq, detail):
        got.append((typ, seq, detail, threading.get_ident()))
        raise RuntimeError("boom")                   # must not reach C

    rig.ctx.on_event(cb)
    sched = rig.make_scheduler()
    with caplog.at_level(logging.ERROR, logger="datax"):
        s1, s2 = _starve(rig, sched)
        _wait_for(lambda: len([g for g in got if g[0] == datax.EventType.LATE_COMMAND]) >= 1)
        _wait_for(lambda: any("boom" in r.getMessage() or (r.exc_info and "boom" in str(r.exc_info[1]))
                              for r in caplog.records))
    assert all(g[3] != main for g in got)            # delivered on the poller thread
    assert sched.wait(s1, 3000).status is datax.CompletionStatus.LATE   # C side unharmed


def test_event_wait_is_deadlock_error(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    errs, ev = [], threading.Event()

    def cb(typ, ticks, seq, detail):
        if typ != datax.EventType.LATE_COMMAND or ev.is_set():
            return
        try:
            sched.wait(seq, 1000)
        except datax.Error as e:
            errs.append(e)
        finally:
            ev.set()

    rig.ctx.on_event(cb)
    _starve(rig, sched)
    assert ev.wait(3.0)
    assert errs and errs[0].errno == errno.EDEADLK


def test_event_close_is_deadlock_error(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    errs, ev = [], threading.Event()

    def cb(typ, ticks, seq, detail):
        if ev.is_set():
            return
        for fn in (sched.close, rig.ctx.close):      # both would join the poller from itself
            try:
                fn()
            except datax.Error as e:
                errs.append(e)
        ev.set()

    rig.ctx.on_event(cb)
    _starve(rig, sched)
    assert ev.wait(3.0)
    assert [e.errno for e in errs] == [errno.EDEADLK, errno.EDEADLK]
    assert not sched.closed
    # the scheduler still works afterwards
    with pytest.raises(datax.LateError):
        sched.wait_all(timeout=3.0)


def test_event_after_close(make_rig):
    rig = make_rig()
    count = [0]
    release = threading.Event()

    def cb(typ, ticks, seq, detail):
        count[0] += 1
        release.wait(0.05)                           # dawdle on the poller thread

    rig.ctx.on_event(cb)
    sched = rig.make_scheduler()
    _starve(rig, sched)
    _wait_for(lambda: count[0] >= 1)
    ctx = rig.ctx
    ctx.close()                                      # joins the poller: no callback after
    n = count[0]
    assert ctx._event_cb is None
    time.sleep(0.15)
    assert count[0] == n
    with pytest.raises(datax.Error):
        ctx.on_event(cb)


def test_event_callback_kept_alive(make_rig):
    rig = make_rig()
    seen = []

    def make():
        def cb(*e):
            seen.append(e)
        return cb

    rig.ctx.on_event(make())
    gc.collect()
    sched = rig.make_scheduler()
    _starve(rig, sched)
    _wait_for(lambda: seen)                          # the trampoline survived gc


def test_gc_order_scheduler_before_context(make_rig):
    rig = make_rig()
    sched = rig.make_scheduler()
    sched.at(rig.now + 100 * MS).gpio(0xe, 0x2, report=True)
    sched.commit()
    ctx = rig.ctx
    ctx.close()
    assert sched.closed
    with pytest.raises(datax.Error):
        sched.commit()
    sched.close()                                    # idempotent
    gc.collect()


def test_gc_order_context_dropped_first(make_rig):
    import weakref
    rig = make_rig()
    sched = rig.make_scheduler()
    sched.at(rig.now + 100 * MS).gpio(0xe, 0x2, report=True)
    sched.commit()
    ref = weakref.ref(rig.ctx)
    rig.release()                  # the test's only handles are sched's
    gc.collect()
    assert ref() is not None and not sched.closed   # sched keeps its context
    del sched
    gc.collect()                   # cycle collected: scheduler then context torn down
    assert ref() is None


def test_scheduler_close_and_recreate(make_rig):
    rig = make_rig()
    s1 = rig.make_scheduler()
    s1.close()
    assert s1.closed
    s2 = rig.dev.scheduler
    assert s2 is not s1 and not s2.closed
    with pytest.raises(datax.Error):
        s1.at(0)


def test_dense_tunes_wait_all_skips_unreported(make_rig):
    rig = make_rig()
    plan = [A, B, C3]
    rig.dev.stage_hop_table(plan)
    sched = rig.make_scheduler()
    t0 = rig.now + 100 * MS
    n = 6
    seqs = [sched.at(t0 + i * 2 * MS).tune(plan[i % 3]) for i in range(n)]
    sched.commit()
    rig.run_to(t0 - 5 * MS)
    rig.run_to(t0 + n * 2 * MS + 15 * MS, step=100 * US, pause=0.0015)   # edge by edge
    with pytest.raises(datax.Error) as ei:       # tunes 2 ms after a reported one
        sched.wait(seqs[1], 0)
    assert ei.value.errno == errno.EINVAL
    done = sched.wait_all(timeout=3.0)
    assert [c.seq for c in done] == seqs[0::2]    # every second tune reports
    assert all(c.status is datax.CompletionStatus.OK for c in done)
    assert rig.pll_hz() == plan[(n - 1) % 3]
