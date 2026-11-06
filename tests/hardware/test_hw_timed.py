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


@pytest.fixture(autouse=True)
def _no_open_scheduler(dev):
    """No test leaves a scheduler (and its poller) open."""
    yield
    s = dev._sched
    if s is not None and not s.closed:
        try:
            pause(s, False)
        except Exception:
            pass
        s.close()


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


def park_pll(dev, hw_ctx, hz=PARK_HZ, table=None):
    """Move the PLL to ``hz`` with one reported tune, so INIT_HZ is
    distinguishable afterwards. Returns the scheduler."""
    s = restage(dev, table or [hz])
    seq = s.at(dev.time.now + ms(dev, 150)).tune(hz)
    s.commit()
    c = s.wait(seq, WAIT_MS)
    assert c.status == datax.CompletionStatus.OK, c
    assert ht.read_pll_hz(hw_ctx) == hz
    return s


def sample_pll(hw_ctx, dev, around, before_ms=25, after_ms=6):
    """Read the PLL back-to-back from ``before_ms`` before device time
    ``around`` until ``after_ms`` after it. Returns ``[(seconds, hz)]``."""
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
    """Four tunes 200 ms apart (the library inserts the priming edge before
    the first): each reported tune is OK, and the independent PLL readback
    equals the plan. No sampler runs here (see test_priming_edge_lands_on_init)."""
    s = restage(dev, FULL_TABLE)
    period = ms(dev, 200)
    first = dev.time.now + ms(dev, 200)
    times = ht.hop_times(first, len(FULL_TABLE), period)
    seqs = queue_tunes(s, FULL_TABLE, times)
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
    record("tunes", readbacks)


def test_priming_edge_lands_on_init(dev, hw_ctx, record):
    """Park the PLL away from the init frequency, restage (re-enable), queue one
    tune and sample the PLL around it: the priming edge, a few ms before the
    tune, must show INIT_HZ. The window is short, so the whole cycle retries
    (up to 5 times); only INIT_HZ in the samples is asserted."""
    attempts = []
    for n in range(5):
        park_pll(dev, hw_ctx, PARK_HZ, table=[PARK_HZ, FULL_TABLE[0]])
        s = restage(dev, FULL_TABLE)
        first = dev.time.now + ms(dev, 200)
        seq = queue_tunes(s, [FULL_TABLE[0]], [first])[0]
        samples = sample_pll(hw_ctx, dev, first)
        s.wait(seq, WAIT_MS)
        seen = sorted({h for _, h in samples})
        span = samples[-1][0] - samples[0][0] if len(samples) > 1 else 0.0
        attempts.append({"attempt": n, "samples": len(samples), "seen_hz": seen,
                         "read_period_ms": 1000.0 * span / max(len(samples) - 1, 1)})
        if INIT_HZ in seen:
            break
    record("attempts", attempts)
    assert INIT_HZ in attempts[-1]["seen_hz"], \
        f"priming edge never observed on {INIT_HZ} Hz in {len(attempts)} attempts: {attempts}"


def test_late_dropped(dev, hw_ctx, record):
    s = park_pll(dev, hw_ctx, PARK_HZ, table=[PARK_HZ, FULL_TABLE[0]])
    record("limits", dict(dev.caps.limits))
    # (a) a reported GPIO 1 ms in the past is refused at commit
    s.at(dev.time.now - ms(dev, 1)).gpio(0x2, 0x2, report=True)
    with pytest.raises(datax.LateError) as ei:
        s.commit()
    assert ei.value.errno == errno.ETIME
    record("past_commit_error", str(ei.value))

    # (b) a tune to another frequency, committed while the poller is paused,
    # falls inside the guard margin before the poller resumes: it completes
    # LATE and the PLL stays where it was parked (independent readback).
    guard_ms = dev.caps.limits["guard_margin_ms"]
    before = ht.tick_counters(hw_ctx)
    seq, extra_used, errors = None, None, []
    pause(s, True)
    try:
        for extra in (12, 16, 24, 40):       # the first tune also needs its priming lead
            when = dev.time.now + ms(dev, guard_ms + extra)
            try:
                seq = s.at(when).tune(FULL_TABLE[0])
                s.commit()
                extra_used = extra
                break
            except datax.Error as e:
                seq = None
                errors.append(str(e))
        assert seq is not None, f"no tune could be committed: {errors}"
        sleep_until(dev, when + ms(dev, 5))      # inside the margin, beyond the edge
    finally:
        pause(s, False)
    c = s.wait(seq, WAIT_MS)
    time.sleep(0.2)
    pll = ht.read_pll_hz(hw_ctx)
    after = ht.tick_counters(hw_ctx)
    record("guard_margin_ms", guard_ms)
    record("margin_over_guard_ms", extra_used)
    record("commit_errors", errors)
    record("completion", ht.completion_log([c]))
    record("pll_hz_after", pll)
    record("tick_counters_around", {"before": before, "after": after})
    record("tick_counter_note", "late_count/overflow_count may stay flat: a command "
           "that is late before the poller feeds it never reaches the Tick")
    assert c.status == datax.CompletionStatus.LATE, c
    assert pll == PARK_HZ, f"PLL moved to {pll} Hz: the late tune executed"


def test_trigger_low_after_flush(dev, hw_ctx, sched, record):
    """A flush while a tune's trigger pulse is high must leave GPIO bit 0 low:
    were it still high, the next rising edge would not exist and the next tune
    would land one behind. The flush is required to land inside the 10 ms
    pulse (device time read before and after it), retrying up to 3 times."""
    dev.stage_hop_table(FULL_TABLE)
    pulse = ms(dev, 10)
    flushes = []
    for _ in range(3):
        a_when = dev.time.now + ms(dev, 80)
        a = sched.at(a_when).tune(FULL_TABLE[0])
        sched.commit()
        sleep_until(dev, a_when + ms(dev, 2))    # the rise has fired, the fall has not
        before = dev.time.now
        sched.flush()
        after = dev.time.now
        ok = ht.flush_while_high(before, after, a_when, pulse)
        flushes.append({"rise": a_when, "before": before, "after": after, "while_high": ok})
        if ok:
            break
    record("flush_attempts", flushes)
    assert flushes[-1]["while_high"], f"flush never landed inside the pulse: {flushes}"
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


def _run_dense(dev, hw_ctx, sched, factor, n=12):
    rs = dev.caps.limits["report_spacing_us"]
    spacing_us = ht.dense_spacing_us(rs, factor)
    freqs = ht.hop_sequence(FULL_TABLE, n)
    dev.stage_hop_table(FULL_TABLE)
    first = dev.time.now + ms(dev, 200)
    times = ht.hop_times(first, n, ht.us_to_ticks(spacing_us, dev.time.tick_hz))
    seqs = queue_tunes(sched, freqs, times)
    done = sched.wait_all(timeout=10)
    return {"report_spacing_us": rs, "spacing_us": spacing_us,
            "pulse_us_derived": ht.pulse_us(spacing_us),   # computed from the library rule
            "tunes": n, "reported": len(done), "all_seqs_reported": [c.seq for c in done] == seqs,
            "all_on_time": ht.all_on_time(done), "completions": ht.completion_log(done),
            "final_pll_hz": ht.read_pll_hz(hw_ctx), "want_final_hz": freqs[-1]}


def test_dense_hops_library_readback_each_edge(dev, hw_ctx, sched, record):
    """Dense hops (pulse < 10 ms, spacing 2 x REPORT_SPACING) with every tune
    reported: the library's per-edge PLL readback (completion OK) covers every
    edge, plus one independent readback of the final frequency."""
    r = _run_dense(dev, hw_ctx, sched, 2.0)
    for k, v in r.items():
        record(k, v)
    assert r["pulse_us_derived"] < ht.PULSE_MAX_US, "spacing too wide to test narrow pulses"
    assert r["all_seqs_reported"], "not every tune was reported"
    assert r["all_on_time"], r["completions"]
    assert r["final_pll_hz"] == r["want_final_hz"]


def test_dense_hops_x1_2_measurement(dev, hw_ctx, sched, record):
    """Measurement only (never asserts): 1.2 x REPORT_SPACING hops. Records
    whether the narrower spacing held."""
    try:
        r = _run_dense(dev, hw_ctx, sched, 1.2)
        r["outcome"] = "held" if (r["all_seqs_reported"] and r["all_on_time"]
                                  and r["final_pll_hz"] == r["want_final_hz"]) else "degraded"
    except Exception as e:  # a measurement: the failure is the result
        r = {"outcome": "error", "error": str(e)}
    for k, v in r.items():
        record(k, v)


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
    """Run, destroy the scheduler, stage again: FHM is enabled again and the
    first tune after it lands on the plan (the priming edge itself is
    test_priming_edge_lands_on_init)."""
    park_pll(dev, hw_ctx, PARK_HZ, table=[PARK_HZ, FULL_TABLE[0]])
    s = restage(dev, FULL_TABLE)
    assert ht.read_fhm_enable(hw_ctx) == 1
    seq = queue_tunes(s, [FULL_TABLE[0]], [dev.time.now + ms(dev, 200)])[0]
    c = s.wait(seq, WAIT_MS)
    got = ht.read_pll_hz(hw_ctx)
    record("completion", ht.completion_log([c]))
    record("final_pll_hz", got)
    assert c.status == datax.CompletionStatus.OK, c
    assert got == FULL_TABLE[0]


def test_pps_alignment(dev, hw_ctx, sched, record):
    expect_pps = ht.normalize_pps(os.environ.get("DATAX_HW_PPS")) != "none"
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
            if after.get("pps_present") is not None:
                assert after["pps_present"] == 1, after
            assert ht.offset_after_align_ok(now0, epoch, hz), (now0 - epoch) / hz
            assert ht.advance_ok(now1 - now0, hz, w1 - w0), (now1 - now0) / hz
        else:
            assert src == datax.TimeSource.SOFT_ALIGNED, src
            assert any(e["type"] == "PPS_LOCK_CHANGE" for e in seen), seen
    finally:
        hw_ctx.on_event(None)


def test_tune_after_alignment(dev, hw_ctx, sched, record):
    """Final review I2: an alignment that moves device time back must not
    block the next tune. The FHM history is reset when the mapping changes,
    so the tune primes and lands. Runs after test_pps_alignment (the session
    context has just been aligned) and aligns again to epoch 0 itself, so it
    also holds when run alone."""
    dev.stage_hop_table(FULL_TABLE)
    seq = sched.at(dev.time.now + ms(dev, 150)).tune(FULL_TABLE[0])
    sched.commit()
    assert sched.wait(seq, WAIT_MS).status == datax.CompletionStatus.OK
    sleep_until(dev, dev.time.now + ms(dev, 20))    # its 10 ms pulse has ended
    before = dev.time.now
    dev.time.align_next_pps(0)                      # time moves back
    after = dev.time.now
    record("align", {"before": before, "after": after, "source": dev.time.source.name})
    assert after < before, (before, after)
    want = FULL_TABLE[2]
    seq = sched.at(dev.time.now + ms(dev, 200)).tune(want)
    sched.commit()
    c = sched.wait(seq, WAIT_MS)
    got = ht.read_pll_hz(hw_ctx)
    record("tune_after_alignment", {"completion": ht.completion_log([c]), "pll_hz": got})
    assert c.status == datax.CompletionStatus.OK, c
    assert got == want, (want, got)
