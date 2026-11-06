# SPDX-License-Identifier: MIT
"""P3a timed control on the ADRV9009-ZU11EG hopcap SFP image (Tick + FHM).

Run with DATAX_HW_URI=<uri> DATAX_HW_IMAGE=hopcap-sfp (see README.md); on the
stock image every test here skips. There is no scope on the rig: timing
evidence is the completion records (every reported command reports on time,
see the C API page, "Scheduler semantics") plus independent PLL readback of
chip A through the phy's own FHM attribute.
"""
import ctypes as C
import errno
import os
import threading
import time

import pytest

import datax
import hwtimed as ht

pytestmark = pytest.mark.p3a

FULL_TABLE = [2_420_000_000, 2_440_000_000, 2_460_000_000, 2_480_000_000]
INIT_HZ = 2_450_000_000          # fhmInitFrequency: where a priming edge lands
PARK_HZ = 2_480_000_000          # away from INIT_HZ, to see the priming edge move
WAIT_MS = 10000


def ms(dev, x):
    return dev.time.seconds(x / 1000.0)


def sleep_until(dev, t, timeout=10.0):
    """Block until device time reaches ``t`` ticks."""
    hz = dev.time.tick_hz
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        rem = t - dev.time.now
        if rem <= 0:
            return
        time.sleep(max(0.0005, min(rem / hz * 0.8, 0.05)))
    raise AssertionError(f"device time never reached {t}")


def pause(sched, on):
    fn = datax._lib.get_lib().datax_internal_sched_pause
    fn.restype, fn.argtypes = C.c_int, [C.c_void_p, C.c_bool]
    assert fn(sched._h, on) == 0


@pytest.fixture
def sched(dev, hw_ctx, record):
    s = dev.scheduler
    record("limits", dict(dev.caps.limits))
    record("tick_counters_before", ht.tick_counters(hw_ctx))
    yield s
    try:
        pause(s, False)
        s.flush()
    except Exception as e:  # cleanup only
        record("cleanup_error", str(e))
    record("tick_counters_after", ht.tick_counters(hw_ctx))
    s.close()


def restage(dev, table):
    """Destroy the scheduler, stage the table (re-enables FHM: the next edge
    primes), take a fresh scheduler."""
    dev.scheduler.close()
    dev.stage_hop_table(table)
    return dev.scheduler


def park_pll(dev, hw_ctx, hz=PARK_HZ):
    """Move the PLL to ``hz`` with one reported tune, so INIT_HZ is
    distinguishable afterwards. Returns the scheduler."""
    s = restage(dev, [hz])
    seq = s.at(dev.time.now + ms(dev, 150)).tune(hz)
    s.commit()
    c = s.wait(seq, WAIT_MS)
    assert c.status == datax.CompletionStatus.OK, c
    assert ht.read_pll_hz(hw_ctx) == hz
    return s


def sample_pll(hw_ctx, dev, around, before_ms=25, after_ms=6):
    """Read the PLL back-to-back from ``before_ms`` before device time
    ``around`` until ``after_ms`` after it. Returns ``[(seconds, hz)]``."""
    hz = dev.time.tick_hz
    sleep_until(dev, around - ms(dev, before_ms))
    out, t_end = [], time.monotonic() + (before_ms + after_ms) / 1000.0
    while time.monotonic() < t_end:
        t0 = time.monotonic()
        out.append((t0, ht.read_pll_hz(hw_ctx)))
    return out


def queue_tunes(s, freqs, times):
    seqs = [s.at(t).tune(f) for f, t in zip(freqs, times)]
    s.commit()
    return seqs


def test_staircase(dev, sched, record):
    n = 8
    spacing_us = 2 * dev.caps.limits["report_spacing_us"]
    spacing = ht.us_to_ticks(spacing_us, dev.time.tick_hz)
    t0 = dev.time.now + ms(dev, 50)
    plan = ht.staircase(t0, n, spacing)
    seqs = [sched.at(t).gpio(mask, value, report=True) for t, mask, value in plan]
    sched.at(plan[-1][0] + spacing).gpio(ht.STAIR_MASK, 0)      # park low (unreported)
    sched.commit()
    done = sched.wait_all(timeout=10)
    log = ht.completion_log(done)
    record("spacing_us", spacing_us)
    record("report_spacing_us", dev.caps.limits["report_spacing_us"])
    record("edges", n)
    record("completions", log)
    record("independent_check", "none: no scope on this rig; completion records only")
    assert [c.seq for c in done] == seqs
    assert len(done) == n
    assert ht.all_on_time(done), log


def test_hop_schedule(dev, hw_ctx, record):
    s = park_pll(dev, hw_ctx)
    s = restage(dev, FULL_TABLE)
    period = ms(dev, 200)
    first = dev.time.now + ms(dev, 200)
    times = ht.hop_times(first, len(FULL_TABLE), period)
    seqs = queue_tunes(s, FULL_TABLE, times)
    # The priming edge fires a lead before the first tune: sample the PLL
    # through that window (the PLL was parked away from INIT_HZ).
    samples = sample_pll(hw_ctx, dev, first)
    seen = [h for _, h in samples]
    readbacks = []
    for seq, want in zip(seqs, FULL_TABLE):
        c = s.wait(seq, WAIT_MS)
        got = ht.read_pll_hz(hw_ctx)
        readbacks.append({"seq": seq, "want": want, "got": got, "status": c.status.name,
                          "requested": c.requested_ticks, "executed": c.executed_ticks})
        assert c.status == datax.CompletionStatus.OK, c
        assert c.executed_ticks == c.requested_ticks, c
        assert got == want, (want, got)
        # the next tune is 200 ms away: the PLL cannot have moved yet
    record("park_hz", PARK_HZ)
    record("pll_samples_hz", sorted(set(seen)))
    record("pll_sample_count", len(samples))
    record("priming_seen_at_init", INIT_HZ in seen)
    record("tunes", readbacks)
    assert INIT_HZ in seen, f"priming edge not observed on {INIT_HZ} Hz; saw {sorted(set(seen))}"


def test_late_dropped(dev, hw_ctx, sched, record):
    # (a) a reported GPIO 1 ms in the past is refused at commit
    seq = sched.at(dev.time.now - ms(dev, 1)).gpio(0x2, 0x2, report=True)
    with pytest.raises(datax.LateError) as ei:
        sched.commit()
    assert ei.value.errno == errno.ETIME
    record("past_commit_error", str(ei.value))

    # (b) inside the guard margin by the time the paused poller resumes
    guard_ms = dev.caps.limits["guard_margin_ms"]
    before = ht.tick_counters(hw_ctx)
    pause(sched, True)
    margin_used = None
    seq = None
    for extra in (1, 2, 3, 5):                  # 1 ms first; commit round trips may eat it
        t = dev.time.now + ms(dev, guard_ms + extra)
        seq = sched.at(t).gpio(0x4, 0x4, report=True)
        try:
            sched.commit()
            margin_used = extra
            break
        except datax.LateError:
            seq = None
    record("guard_margin_ms", guard_ms)
    record("margin_over_guard_ms", margin_used)
    assert seq is not None, "commit was late for every margin tried"
    time.sleep(3 * guard_ms / 1000.0)
    pause(sched, False)
    c = sched.wait(seq, WAIT_MS)
    time.sleep(0.1)
    after = ht.tick_counters(hw_ctx)
    record("completion", ht.completion_log([c]))
    record("tick_counters_around", {"before": before, "after": after})
    assert c.status == datax.CompletionStatus.LATE, c
    assert c.executed_ticks == 0, f"a LATE command must not execute: {c}"


def test_trigger_low_after_flush(dev, hw_ctx, sched, record):
    """A flush while a tune's trigger pulse is high must leave GPIO bit 0 low:
    were it still high, the next rising edge would not exist and the next tune
    would land one behind."""
    dev.stage_hop_table(FULL_TABLE)
    a_when = dev.time.now + ms(dev, 80)
    a = sched.at(a_when).tune(FULL_TABLE[0])
    sched.commit()
    sleep_until(dev, a_when + ms(dev, 2))        # the rise has fired, the fall (10 ms) has not
    sched.flush()
    try:
        ca = sched.wait(a, 1000)
        record("first_tune_after_flush", ht.completion_log([ca]))
    except datax.Error as e:
        record("first_tune_wait", str(e))
    results = []
    for want in (FULL_TABLE[2], FULL_TABLE[3]):
        seq = sched.at(dev.time.now + ms(dev, 100)).tune(want)
        sched.commit()
        c = sched.wait(seq, WAIT_MS)
        got = ht.read_pll_hz(hw_ctx)
        results.append({"want": want, "got": got, "status": c.status.name, "detail": c.detail})
        assert c.status == datax.CompletionStatus.OK, c
        assert got == want, results
    record("tunes_after_flush", results)


@pytest.mark.parametrize("factor", [2.0, 1.2], ids=["x2.0", "x1.2"])
def test_dense_hops_readback_every_edge(dev, hw_ctx, sched, record, factor):
    """Hops closer than the 10 ms pulse, each spaced >= REPORT_SPACING so EVERY
    tune reports: the library's PLL readback then covers every edge."""
    rs = dev.caps.limits["report_spacing_us"]
    spacing_us = ht.dense_spacing_us(rs, factor)
    pulse = ht.pulse_us(spacing_us)
    n = 12
    assert pulse < ht.PULSE_MAX_US, "spacing too wide to test narrow pulses"
    freqs = ht.hop_sequence(FULL_TABLE, n)
    dev.stage_hop_table(FULL_TABLE)
    first = dev.time.now + ms(dev, 200)
    times = ht.hop_times(first, n, ht.us_to_ticks(spacing_us, dev.time.tick_hz))
    seqs = queue_tunes(sched, freqs, times)
    done = sched.wait_all(timeout=10)
    log = ht.completion_log(done)
    record("report_spacing_us", rs)
    record("spacing_us", spacing_us)
    record("pulse_us", pulse)
    record("tunes", n)
    record("reported", len(done))
    record("completions", log)
    assert [c.seq for c in done] == seqs, "not every tune was reported"
    assert ht.all_on_time(done), log           # OK means the PLL read back equal to the plan
    got = ht.read_pll_hz(hw_ctx)
    record("final_pll_hz", got)
    assert got == freqs[-1]


def test_readback_exact_across_band(dev, hw_ctx, sched, record):
    plan = [2_400_000_000, 2_500_000_000, 2_450_000_000, 2_400_000_000, 2_450_000_000]
    dev.stage_hop_table(sorted(set(plan)))
    first = dev.time.now + ms(dev, 200)
    seqs = queue_tunes(sched, plan, ht.hop_times(first, len(plan), ms(dev, 200)))
    rows = []
    for seq, want in zip(seqs, plan):
        c = sched.wait(seq, WAIT_MS)
        got = ht.read_pll_hz(hw_ctx)
        rows.append({"want": want, "got": got, "status": c.status.name})
        assert c.status == datax.CompletionStatus.OK, c
        assert got == want, rows
    record("band_readback", rows)


def test_fhm_enable_readback(dev, hw_ctx, sched, record):
    dev.stage_hop_table(FULL_TABLE)
    on = ht.read_fhm_enable(hw_ctx)
    record("fhm_enable_after_stage", on)
    assert on == 1
    seq = sched.at(dev.time.now + ms(dev, 150)).tune(FULL_TABLE[1])
    sched.commit()
    assert sched.wait(seq, WAIT_MS).status == datax.CompletionStatus.OK
    assert ht.read_fhm_enable(hw_ctx) == 1          # still on while running
    assert ht.read_pll_hz(hw_ctx) == FULL_TABLE[1]


def test_reenable_reprimes(dev, hw_ctx, record):
    """Run, destroy the scheduler, stage again: re-enabling FHM re-primes the
    engine, so the first edge lands on the init frequency, then the plan."""
    s = park_pll(dev, hw_ctx)                       # run once; PLL now at PARK_HZ
    s = restage(dev, FULL_TABLE)                    # destroy + stage again
    assert ht.read_fhm_enable(hw_ctx) == 1
    first = dev.time.now + ms(dev, 200)
    seq = queue_tunes(s, [FULL_TABLE[0]], [first])[0]
    samples = sample_pll(hw_ctx, dev, first)
    c = s.wait(seq, WAIT_MS)
    got = ht.read_pll_hz(hw_ctx)
    seen = sorted({h for _, h in samples})
    record("pll_samples_hz", seen)
    record("pll_sample_count", len(samples))
    record("completion", ht.completion_log([c]))
    record("final_pll_hz", got)
    assert c.status == datax.CompletionStatus.OK, c
    assert got == FULL_TABLE[0]
    assert INIT_HZ in seen, f"re-priming edge not observed on {INIT_HZ} Hz; saw {seen}"


def test_pps_alignment(dev, hw_ctx, sched, record):
    expect_pps = (os.environ.get("DATAX_HW_PPS") or "internal") != "none"
    hz = dev.time.tick_hz
    events = []
    lock = threading.Lock()

    def on_event(typ, ticks, seq, detail):
        with lock:
            events.append({"type": typ.name, "ticks": ticks, "seq": seq, "detail": detail})

    hw_ctx.on_event(on_event)
    try:
        before = ht.tick_counters(hw_ctx)
        record("tick_before", before)
        epoch = 1000 * hz
        t_call = time.monotonic()
        dev.time.align_next_pps(epoch)
        t_ret = time.monotonic()
        src = dev.time.source
        now0 = dev.time.now
        w0 = time.monotonic()
        after = ht.tick_counters(hw_ctx)
        time.sleep(2.0)
        now1 = dev.time.now
        w1 = time.monotonic()
        with lock:
            seen = list(events)
        record("align_call_s", t_ret - t_call)
        record("source", src.name)
        record("pps_present_before", before.get("pps_present"))
        record("pps_present_after", after.get("pps_present"))
        record("time_aligned", after.get("time_aligned"))
        record("device_offset_after_align_s", (now0 - epoch) / hz)
        record("device_advance_s", (now1 - now0) / hz)
        record("wall_advance_s", w1 - w0)
        record("events", seen)
        if expect_pps:
            assert src == datax.TimeSource.PPS_ALIGNED, src
            assert after.get("time_aligned") == 1, after
            assert ht.offset_after_align_ok(now0, epoch, hz), (now0 - epoch) / hz
            assert ht.advance_ok(now1 - now0, hz, w1 - w0), (now1 - now0) / hz
        else:
            assert src == datax.TimeSource.SOFT_ALIGNED, src
            assert any(e["type"] == "PPS_LOCK_CHANGE" for e in seen), seen
    finally:
        hw_ctx.on_event(None)
