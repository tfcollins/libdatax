# SPDX-License-Identifier: MIT
"""P3b timed streams on the ADRV9009-ZU11EG timed-dma image (REQ-TEST-022/023).

Run with DATAX_HW_URI=<uri> DATAX_HW_IMAGE=timed-dma (see README.md); on every
other image each test here skips. There is no scope on the rig. The evidence
is the block-0 capture tag, Tick's capture-tag registers (edge_capture_*),
sample positions through the Talise framer loopback (TX and RX both start
from start_at), the scheduler's completion records and PLL readback of chip B.
Throughput is never asserted.

Sizing: over ip: the host cannot drain 245.76 MSPS, so whatever a test needs
must lie in the blocks enqueued before T. On a v0 iiod (Kuiper) the kernel
ring is 4 x block_samples whatever block_count asks for, so every stream here
uses block_count=4 (the binary iiod path then agrees). One RX channel (4 bytes
a frame) keeps the DMA's DDR bandwidth low; TX uses every channel because
this image refuses a partial-mask TX start_at (tx-start-fullmask-only).
"""
import contextlib
import ctypes as C
import json
import time
from pathlib import Path

import numpy as np
import pytest

import datax
import hwtimed as ht
import loopback as lb

pytestmark = pytest.mark.p3b

FULL_TABLE = [2_420_000_000, 2_440_000_000, 2_460_000_000, 2_480_000_000]
INIT_HZ = 2_450_000_000          # fhmInitFrequency (device tree)
PARK_B_HZ = 2_430_000_000        # chip B parked away from INIT_HZ and the table
WAIT_MS = 10000

RX_CH = 0                        # chip A, RX channel 0: TX channel 0 through the loopback
NBLOCKS = 4                      # the kernel ring on a v0 iiod, whatever is asked
BIG_BLOCK = 3 << 20              # frames: 12 MiB at one channel; 4 blocks = 51.2 ms
SMALL_BLOCK = 16384              # (a) and (b): only block 0 is read
PERIOD = 4096                    # TX cyclic buffer (frames per channel)
LEAD_MS = 200                    # timed capture: start_at(now + 200 ms), spec decision 10
PAIR_LEAD_MS = 500               # RX and TX start_at back to back
RUNS = 10
GATE_OFF_MS, GATE_ON_MS = 20, 40


def ms(dev, x):
    return dev.time.seconds(x / 1000.0)


def fs_of(dev):
    return int(dev.sample_rate)


def tx_count(dev):
    return int(dev.caps.limits["max_channels"])     # the family has as many TX as RX


@pytest.fixture(autouse=True)
def _cleanup(dev, request):
    """Streams a test opened close first (stop forces their gates open), then
    the scheduler, so no test leaves a gate, a stream or a poller behind."""
    opened = []
    request.node.opened = opened
    yield opened
    for st in reversed(opened):
        try:
            st.close()
        except Exception:
            pass
    s = dev._sched
    if s is not None and not s.closed:
        s.close()


def track(request, st):
    request.node.opened.append(st)
    return st


@contextlib.contextmanager
def digital_loopback(hw_ctx, family, record):
    lb.set_digital_loopback(hw_ctx, family, True)
    try:
        yield
    finally:
        try:
            lb.set_digital_loopback(hw_ctx, family, False)
        except Exception as e:      # never mask the test's own error
            record("loopback_restore_failed", str(e))


def offset_bracket(dev, hw_ctx):
    """Bounds of the soft offset (API = hw + offset): 0 unless SOFT_ALIGNED."""
    a = dev.time.now
    h = ht.tick_attr(hw_ctx, "current_time_ticks")
    b = dev.time.now
    if h is None:
        return None
    return ht.offset_bracket(a, h, b)


def tag_strip(hw_ctx, nch):
    hb = ht.tick_attr(hw_ctx, "capture_tag_header_bytes")
    assert hb, f"Tick has no capture_tag_header_bytes ({hb!r}): no in-band tagger"
    return hb, ht.strip_frames(hb, nch)


def gate_info(dev):
    fn = datax._lib.get_lib().datax_internal_gate_info
    fn.restype, fn.argtypes = C.c_int, [C.c_void_p, C.c_char_p, C.c_size_t]
    buf = C.create_string_buffer(512)
    return buf.value.decode() if fn(dev._ptr, buf, len(buf)) == 0 else None


def qpsk_raw(n, seed):
    c = lb.qpsk_block(n, seed)
    raw = np.empty((n, 2), dtype=np.int16)
    raw[:, 0] = np.rint(c.real * 32767)
    raw[:, 1] = np.rint(c.imag * 32767)
    return raw


def tone_raw(n, amp=8000):
    """One cycle per ``n`` frames: constant envelope, never exactly zero, and
    its lag is unique modulo ``n``."""
    k = np.arange(n)
    raw = np.empty((n, 2), dtype=np.int16)
    raw[:, 0] = np.rint(amp * np.cos(2 * np.pi * k / n))
    raw[:, 1] = np.rint(amp * np.sin(2 * np.pi * k / n))
    return raw


def as_complex(raw):
    return raw[:, 0].astype(np.float64) + 1j * raw[:, 1]


def preload_tx(dev, request, per_channel_raw):
    """A cyclic TX stream on every TX channel, preloaded (unstarted)."""
    n = per_channel_raw[0].shape[0]
    tx = track(request, dev.tx_stream(channels=range(len(per_channel_raw)),
                                      block_samples=n, cyclic=True))
    assert tx.send(np.stack([as_complex(r) for r in per_channel_raw])) == n
    return tx


def start_pair(dev, rx, tx, lead_ms):
    """RX then TX start_at at the same T (libdatax orders the RX opening first).
    Returns (T, seconds of each call)."""
    T = dev.time.now + ms(dev, lead_ms)
    t0 = time.monotonic()
    rx.start_at(T)
    t1 = time.monotonic()
    tx.start_at(T)
    t2 = time.monotonic()
    return T, {"rx_start_at_s": t1 - t0, "tx_start_at_s": t2 - t1}


# ---- capabilities and the image's gate map ------------------------------------------

def test_p3b_caps_and_gate_map(dev, hw_ctx, record):
    """The board runs the pinned timed-dma image: Tick's stream_gate_map equals
    images.json's, and every P3b flag is set."""
    images = json.loads((Path(__file__).parent / "images.json").read_text())
    want = images["tron"]["timed-dma"]["stream_gate_map"]
    got = ht.tick_attr_str(hw_ctx, "stream_gate_map")
    caps = sorted(dev.caps)
    record("stream_gate_map", got)
    record("gate_info", gate_info(dev))
    record("caps", caps)
    record("detected", sorted(dev.caps.detected))
    record("capture_tag_header_bytes", ht.tick_attr(hw_ctx, "capture_tag_header_bytes"))
    record("capture_tag_magic", ht.tick_attr_str(hw_ctx, "capture_tag_magic"))
    record("limits", dict(dev.caps.limits))
    assert ht.gate_map_roles(got) == ht.gate_map_roles(want), (got, want)
    for c in ("timed_commands", "rx_arm", "tx_arm", "stream_gate", "timestamps", "fhm_hop"):
        assert c in dev.caps, f"{c} missing on timed-dma: {caps}"


# ---- (a) timed capture: block 0's tag and Tick's edge capture -------------------------

def _timed_capture(dev, hw_ctx, request, lead_ms, strip):
    rx = track(request, dev.rx_stream(channels=(RX_CH,), block_samples=SMALL_BLOCK,
                                      block_count=NBLOCKS))
    before = ht.edge_capture(hw_ctx)
    T = dev.time.now + ms(dev, lead_ms)
    t0 = time.monotonic()
    rx.start_at(T)
    setup_s = time.monotonic() - t0
    raw, info = rx.recv_raw(SMALL_BLOCK - strip, timeout_ms=WAIT_MS)
    after_dev = dev.time.now
    edge = ht.edge_capture(hw_ctx)          # before stop: the opening is the latest assert
    bracket = offset_bracket(dev, hw_ctx)
    stats = lb.stream_stats(rx)
    rx.stop()
    return {"T": T, "info": info, "raw": raw, "edge_before": before, "edge": edge,
            "bracket": bracket, "stats": stats, "setup_s": setup_s,
            "block0_after_T_s": (after_dev - T) / float(dev.time.tick_hz)}


def test_timed_capture_block0(dev, hw_ctx, request, record):
    """Block 0 of RX start_at(now + 200 ms) carries T + ticks(1 + stripped
    frames) (G2/G3 ruling 6), and, independently, Tick's edge capture holds
    T for the opening (raw ticks: equal to T unless the time is soft-aligned).
    The block-0 assertion alone only proves the header was written."""
    dev.scheduler                            # before start_at (else start_at makes one)
    fs, hz = fs_of(dev), dev.time.tick_hz
    hb, strip = tag_strip(hw_ctx, 1)
    record("tick_counters_before", ht.tick_counters(hw_ctx))
    attempts = []
    r = None
    for lead in (LEAD_MS, 2 * LEAD_MS + 100):
        try:
            r = _timed_capture(dev, hw_ctx, request, lead, strip)
            attempts.append({"lead_ms": lead, "result": "ok"})
            break
        except datax.LateError as e:     # setup outlasted the lead: recorded, retried once
            attempts.append({"lead_ms": lead, "result": "late", "error": str(e)})
            for st in request.node.opened:
                with contextlib.suppress(Exception):
                    st.close()
    record("attempts", attempts)
    assert r is not None, f"start_at late at every lead: {attempts}"
    info, edge, T = r["info"], r["edge"], r["T"]
    want = ht.block0_timestamp(T, strip, fs, hz)
    source = dev.time.source
    record("T", T)
    record("fs", fs)
    record("tick_hz", hz)
    record("header_bytes", hb)
    record("strip_frames", strip)
    record("timestamp_ticks", info.timestamp_ticks)
    record("timestamp_want", want)
    record("timestamp_minus_T_ticks", info.timestamp_ticks - T if info.timestamp_ticks else None)
    record("valid_samples", info.valid_samples)
    record("discontinuous", info.discontinuous)
    record("edge_capture_before", r["edge_before"])
    record("edge_capture", edge)
    record("time_source", source.name)
    record("offset_bracket", r["bracket"])
    record("start_at_setup_s", r["setup_s"])
    record("block0_after_T_s", r["block0_after_T_s"])
    record("ring_coverage_s", ht.ring_coverage_s(SMALL_BLOCK, NBLOCKS, fs))
    record("stream_stats", r["stats"])
    record("tick_counters_after", ht.tick_counters(hw_ctx))
    record("rx_first_frames", r["raw"][:8].tolist())

    assert info.valid_samples == SMALL_BLOCK - strip, info
    why = (f"block 0 timestamp {info.timestamp_ticks}, want T + ticks(1 + {strip} frames) "
           f"= {want} (T {T}); discontinuous={info.discontinuous}, stats={r['stats']}, "
           f"block 0 read {r['block0_after_T_s'] * 1e3:.1f} ms after T")
    assert info.timestamp_ticks == want, why
    assert not info.discontinuous, why
    # Independent: Tick latched the opening's tick (raw hardware ticks).
    assert edge["valid"] == 1, edge
    assert edge["ticks"] is not None, edge
    if r["edge_before"].get("count") is not None and edge.get("count") is not None:
        assert edge["count"] > r["edge_before"]["count"], (r["edge_before"], edge)
    lo, hi = r["bracket"]
    assert lo <= T - edge["ticks"] <= hi, (T, edge, r["bracket"])
    if source != datax.TimeSource.SOFT_ALIGNED:
        assert edge["ticks"] == T, f"edge_capture_ticks {edge['ticks']} != T {T}"


# ---- (b) loopback alignment: TX and RX from the same T -----------------------------------

ALIGN = {}        # (b)'s result, the reference for (c)


def _alignment_run(dev, request, family, strip, pattern, lead_ms):
    rx = track(request, dev.rx_stream(channels=(RX_CH,), block_samples=SMALL_BLOCK,
                                      block_count=NBLOCKS))
    tx = preload_tx(dev, request, pattern)
    T, cost = start_pair(dev, rx, tx, lead_ms)
    raw, info = rx.recv_raw(SMALL_BLOCK - strip, timeout_ms=WAIT_MS)
    rx.close()
    tx.close()
    exp = lb.expected_loopback(family, pattern[RX_CH])
    aud = ht.audible(raw)
    if not aud.any():
        return {"T": T, "first": None, **cost, "valid_samples": info.valid_samples}
    first = int(np.argmax(aud))
    lag = ht.pattern_lag(exp, raw, first + 2)
    edge = np.zeros(raw.shape[0], dtype=bool)
    edge[first:first + 2] = True
    tail = np.zeros(raw.shape[0], dtype=bool)
    tail[first + 2:] = True
    return {"T": T, "first": first, "lag": lag, "first_minus_lag": first - lag,
            "released_index": first + 1 + strip,
            "edge_mismatches": ht.mismatches_at_lag(exp, raw, lag, edge),
            "mismatches_after_first": ht.mismatches_at_lag(exp, raw, lag, tail),
            "valid_samples": info.valid_samples, "discontinuous": info.discontinuous,
            **cost}


@pytest.mark.needs_tx
def test_loopback_alignment(dev, hw_ctx, family, request, record):
    """REQ-TEST-023: the first sample of a TX pattern started at T lands at the
    same RX index (RX started at the same T) in every run, within +-1 sample.
    The offset is TX release -> DAC path -> framer loopback -> RX, measured
    against the RX release; the RX-only fabric latency is not separable here."""
    dev.scheduler                            # before start_at
    fs, hz = fs_of(dev), dev.time.tick_hz
    _, strip = tag_strip(hw_ctx, 1)
    ntx = tx_count(dev)
    pattern = [qpsk_raw(PERIOD, seed=11 + c) for c in range(ntx)]
    runs, late = [], []
    with digital_loopback(hw_ctx, family, record):
        while len(runs) < RUNS and len(late) < 3:
            try:
                runs.append(_alignment_run(dev, request, family, strip, pattern, PAIR_LEAD_MS))
            except datax.LateError as e:
                late.append(str(e))
                for st in request.node.opened:
                    with contextlib.suppress(Exception):
                        st.close()
    record("fs", fs)
    record("tick_hz", hz)
    record("strip_frames", strip)
    record("tx_channels", ntx)
    record("period", PERIOD)
    record("runs", runs)
    record("late_runs", late)
    firsts = [r["first"] for r in runs]
    assert len(runs) == RUNS, f"only {len(runs)} runs; late: {late}"
    assert None not in firsts, f"no TX pattern reached RX in some run: {runs}"
    for r in runs:
        assert r["mismatches_after_first"] == 0, \
            f"RX differs from the looped-back pattern after its first 2 samples: {r}"
    ok, med, spread = ht.spread_ok(firsts)
    lok, lmed, lspread = ht.spread_ok([r["lag"] for r in runs])
    record("first_sample_offsets", firsts)
    record("first_sample_offset_median", med)
    record("first_sample_offset_spread", spread)
    record("released_index_median", med + 1 + strip)
    record("release_latency_ns", (med + 1 + strip) * 1e9 / fs)
    record("lags", [r["lag"] for r in runs])
    record("lag_spread", lspread)
    fml = ht.spread_ok([r["first_minus_lag"] % PERIOD for r in runs])
    record("first_minus_lag_median", fml[1])
    assert ok, f"first-sample offsets {firsts} not identical within +-1 sample (max - min)"
    assert lok, f"pattern lags {[r['lag'] for r in runs]} not identical within +-1 sample"
    # (c)'s reference, only from a passing (b)
    ALIGN.update(first=med, lag=lmed, strip=strip, first_minus_lag=fml[1], passed=True)


# ---- (c) gated burst: TX gated off and on, observed through the loopback -------------------

@pytest.mark.needs_tx
def test_gated_burst(dev, hw_ctx, family, request, record):
    """RX runs from a start_at; a constant-tone cyclic TX started at the same T
    is gated off at T + 20 ms and on at T + 40 ms. The edges must sit at the
    commanded times plus (b)'s offset, +-1 sample, and both gate commands
    complete on time.

    Times are libdatax's derived block timestamps: every block read here is
    one of the 4 enqueued before T, which libdatax keeps clean even though
    the ring overflows before the host reads it (the overflow is attributed
    to the first re-armed block). They must equal the suite's own derivation
    from block 0's tag (or, if block 0 has none, Tick's edge capture).
    Contiguity is proven independently by matching the whole capture against
    the tone at one lag (a closed TX gate mutes, it does not pause, so a lost
    frame anywhere would break the match after the gate opens again)."""
    sched = dev.scheduler                    # before start_at
    fs, hz = fs_of(dev), dev.time.tick_hz
    _, strip = tag_strip(hw_ctx, 1)
    ntx = tx_count(dev)
    d_off = ht.ticks_to_frames(ms(dev, GATE_OFF_MS), fs, hz)
    d_on = ht.ticks_to_frames(ms(dev, GATE_ON_MS), fs, hz)
    ref_first = ALIGN.get("first") if ALIGN.get("passed") else None   # (b) passed
    need = d_on + (PERIOD if ref_first is None else ref_first) + 3 * PERIOD
    cover = ht.ring_coverage_s(BIG_BLOCK, NBLOCKS, fs)
    record("ring_coverage_s", cover)
    assert BIG_BLOCK * NBLOCKS - strip >= need, \
        f"the pre-T ring ({BIG_BLOCK} x {NBLOCKS}) cannot hold the {need} frames needed"
    tone = tone_raw(PERIOD)
    attempts, r = [], None
    with digital_loopback(hw_ctx, family, record):
        for lead in (PAIR_LEAD_MS, 2 * PAIR_LEAD_MS):    # as (a) and (b): one retry when late
            try:
                r = _burst_capture(dev, hw_ctx, request, sched, ntx, tone, strip, need, lead)
                attempts.append({"lead_ms": lead, "result": "ok"})
                break
            except datax.LateError as e:
                attempts.append({"lead_ms": lead, "result": "late", "error": str(e)})
                for st in request.node.opened:
                    with contextlib.suppress(Exception):
                        st.close()
                with contextlib.suppress(Exception):
                    sched.flush()
    record("attempts", attempts)
    assert r is not None, f"late at every lead: {attempts}"
    T, cost, t_off, t_on = r["T"], r["cost"], r["t_off"], r["t_on"]
    blocks, edge, edge_read_at = r["blocks"], r["edge"], r["edge_read_at"]
    done, stats = r["done"], r["stats"]
    stream = np.concatenate(r["parts"])
    exp = lb.expected_loopback(family, tone)

    # The anchor: block 0's tag, else Tick's edge capture of the opening.
    b0 = blocks[0]["timestamp_ticks"]
    if b0:
        anchor, anchor_src = b0 - ht.frames_to_ticks(1 + strip, fs, hz), "block0_tag"
    else:
        off = 0
        if dev.time.source == datax.TimeSource.SOFT_ALIGNED:
            lo, hi = offset_bracket(dev, hw_ctx)
            off = (lo + hi) // 2
        ok = edge["ticks"] is not None and edge_read_at < t_on   # before gate_on's assert
        anchor, anchor_src = (edge["ticks"] + off if ok else None), "edge_capture"
    derived = [None if anchor is None else ht.sample_time(anchor, strip, b["start"], fs, hz)
               for b in blocks]
    lib_vs_derived = [{"block": i, "lib": b["timestamp_ticks"], "derived": derived[i]}
                      for i, b in enumerate(blocks) if b["timestamp_ticks"]]

    aud = ht.audible(stream)
    rs = ht.runs(aud)
    record("T", T)
    record("t_off", t_off)
    record("t_on", t_on)
    record("start_at_cost", cost)
    record("blocks", blocks)
    record("anchor", {"ticks": anchor, "source": anchor_src, "edge_capture": edge})
    record("lib_vs_derived", lib_vs_derived)
    record("audible_runs", rs[:6])
    record("completions", ht.completion_log(done))
    record("stream_stats", stats)
    record("reference_first", ref_first)
    assert stream.shape[0] >= need, f"read {stream.shape[0]} frames, need {need}"
    assert ht.all_on_time(done), ht.completion_log(done)
    assert len(rs) >= 2, f"expected an audible run, a muted gap, an audible run: {rs[:6]}"
    first, e_off, e_on = rs[0][0], rs[0][1], rs[1][0]
    ref = ref_first if ref_first is not None else first
    n0 = ht.TX_START_MUTE_READS
    want_off, want_on = ref - n0 + d_off, ref - n0 + d_on
    res = {"first": first - ref, "off": e_off - want_off, "on": e_on - want_on,
           "spacing": (e_on - e_off) - (d_on - d_off)}
    # The start mute measured here: the gate edges are tied to the time-aligned
    # read counter, the start edge is late by the muted reads (ruling 1). (b)
    # alone cannot give it: it has no mid-stream edge, only first - lag, the
    # pattern words those reads consumed.
    n_meas = {"off": ref + d_off - e_off, "on": ref + d_on - e_on}
    record("edges", {"first": first, "off": e_off, "on": e_on,
                     "want_off": want_off, "want_on": want_on,
                     "start_mute_reads_assumed": n0, "start_mute_reads_measured": n_meas,
                     "first_minus_lag_from_b": ALIGN.get("first_minus_lag"),
                     "residuals": res})
    if anchor is not None:
        record("edge_times", {
            "off": ht.sample_time(anchor, strip, e_off, fs, hz),
            "on": ht.sample_time(anchor, strip, e_on, fs, hz),
            "off_minus_cmd_ticks": ht.sample_time(anchor, strip, e_off, fs, hz) - t_off,
            "on_minus_cmd_ticks": ht.sample_time(anchor, strip, e_on, fs, hz) - t_on,
            "first_minus_T_ticks": ht.sample_time(anchor, strip, first, fs, hz) - T})

    # Contiguity: one lag over the whole capture, away from the edges.
    lag = ht.pattern_lag(exp, stream, first + 2)
    keep = np.zeros(stream.shape[0], dtype=bool)
    keep[first + 2:e_off - 2] = True
    keep[e_on + 2:] = True
    mism = ht.mismatches_at_lag(exp, stream, lag, keep)
    muted = int(np.count_nonzero(aud[e_off + 2:e_on - 2]))
    record("contiguity", {"lag": lag, "mismatches": mism, "muted_nonzero": muted,
                          "frames": int(stream.shape[0])})
    assert mism == 0, f"{mism} frames off the tone at lag {lag}: frames lost or repeated"
    assert muted == 0, f"{muted} non-zero frames while the gate was closed"
    for row in lib_vs_derived:
        assert row["derived"] is None or row["lib"] == row["derived"], row
    assert all(b["timestamp_ticks"] and not b["discontinuous"] for b in blocks), \
        f"a pre-T block lost its timestamp or reads discontinuous: {blocks}"
    assert abs(res["spacing"]) <= 1, res
    assert abs(res["off"] - res["on"]) <= 1, res
    if ref_first is not None:
        assert abs(res["first"]) <= 1, f"start offset {first} vs (b)'s {ref_first}"
    assert abs(res["off"]) <= 1 and abs(res["on"]) <= 1, \
        f"gate edges off the commanded times + (b)'s offset - {n0}: {res}"


def _burst_capture(dev, hw_ctx, request, sched, ntx, tone, strip, need, lead_ms):
    """One gated-burst capture: RX and the tone TX from T, the gate off/on
    commands, the pre-T blocks read, both completions. LateError from a late
    start_at or commit (retried); a gate command that completes late is a
    result, asserted by the caller."""
    rx = track(request, dev.rx_stream(channels=(RX_CH,), block_samples=BIG_BLOCK,
                                      block_count=NBLOCKS))
    tx = preload_tx(dev, request, [tone] * ntx)
    T, cost = start_pair(dev, rx, tx, lead_ms)
    t_off, t_on = T + ms(dev, GATE_OFF_MS), T + ms(dev, GATE_ON_MS)
    s_off = sched.at(t_off).gate_off(tx, report=True)
    s_on = sched.at(t_on).gate_on(tx, report=True)
    sched.commit()
    blocks, parts, total, edge, edge_read_at = [], [], 0, None, None
    while total < need and len(blocks) < NBLOCKS:
        n = BIG_BLOCK - strip if not blocks else BIG_BLOCK
        raw, info = rx.recv_raw(n, timeout_ms=WAIT_MS)
        if not blocks:
            edge = ht.edge_capture(hw_ctx)
            edge_read_at = dev.time.now
        blocks.append({"start": total, "timestamp_ticks": info.timestamp_ticks,
                       "discontinuous": info.discontinuous,
                       "valid_samples": info.valid_samples, "frames": raw.shape[0]})
        parts.append(raw)
        total += raw.shape[0]
    done = [sched.wait(s, WAIT_MS) for s in (s_off, s_on)]
    stats = lb.stream_stats(rx)
    rx.close()
    tx.close()
    return {"T": T, "cost": cost, "t_off": t_off, "t_on": t_on, "blocks": blocks,
            "parts": parts, "edge": edge, "edge_read_at": edge_read_at, "done": done,
            "stats": stats}


# ---- (d) chip-B FHM ---------------------------------------------------------------------

def restage(dev, table):
    """Destroy the scheduler, stage the table (FHM on both chips when the map
    names fhm-b), take a fresh scheduler."""
    if dev._sched is not None and not dev._sched.closed:
        dev._sched.close()
    dev.stage_hop_table(table)
    return dev.scheduler


def test_chip_b_lo_unmoved_by_fhm_enable(dev, hw_ctx, record):
    """Plan gap 4: stage_hop_table enables FHM on both chips when the map names
    fhm-b. Enabling it must not move chip B's LO: turn B's FHM off through the
    phy (libdatax turns FHM off only when the context closes, so an earlier
    test's stage leaves it on), park B away from the FHM init frequency,
    stage, read B's live PLL back. Chip B is left as found: its LO, then its
    FHM enable."""
    if dev._sched is not None and not dev._sched.closed:
        dev._sched.close()
    phys = (ht.PHY_A, ht.PHY_B)
    found = {p: ht.read_fhm_enable(hw_ctx, p) for p in phys}
    record("fhm_enable_found", found)
    ht.write_fhm_enable(hw_ctx, 0, ht.PHY_B)
    before = {p: {"fhm_enable": ht.read_fhm_enable(hw_ctx, p), "lo_hz": ht.read_lo_hz(hw_ctx, p)}
              for p in phys}
    record("before", before)
    orig_b = before[ht.PHY_B]["lo_hz"]
    try:
        assert before[ht.PHY_B]["fhm_enable"] == 0, \
            f"chip B's FHM did not turn off: no enable transition to test ({before})"
        ht.write_lo_hz(hw_ctx, PARK_B_HZ, ht.PHY_B)
        parked = ht.read_lo_hz(hw_ctx, ht.PHY_B)
        record("parked_b", parked)
        assert parked == PARK_B_HZ, f"chip B did not park at {PARK_B_HZ}: {parked}"
        dev.stage_hop_table(FULL_TABLE)
        after = {p: {"fhm_enable": ht.read_fhm_enable(hw_ctx, p),
                     "pll_hz": ht.read_pll_hz(hw_ctx, p),
                     "lo_hz": ht.read_lo_hz(hw_ctx, p)} for p in phys}
        record("after_stage", after)
        assert after[ht.PHY_B]["fhm_enable"] == 1, after
        assert after[ht.PHY_B]["pll_hz"] == PARK_B_HZ, \
            f"enabling FHM moved chip B's PLL from {PARK_B_HZ} to {after[ht.PHY_B]['pll_hz']}"
    finally:
        try:                                 # B back as found: its LO (FHM off), then FHM
            ht.write_fhm_enable(hw_ctx, 0, ht.PHY_B)
            ht.write_lo_hz(hw_ctx, orig_b, ht.PHY_B)
            ht.write_fhm_enable(hw_ctx, found[ht.PHY_B], ht.PHY_B)
            record("restored_b", {"fhm_enable": ht.read_fhm_enable(hw_ctx, ht.PHY_B),
                                  "lo_hz": ht.read_lo_hz(hw_ctx, ht.PHY_B)})
        except Exception as e:
            record("restore_error", str(e))


def test_chip_b_timed_tune(dev, hw_ctx, record):
    """Four timed tunes on channel 2 (chip B, Tick GPIO bit 1) 200 ms apart:
    each completes OK on time, chip B's live PLL reads the plan, and chip A's
    PLL does not move (the pipelines are independent)."""
    s = restage(dev, FULL_TABLE)
    a_before = ht.read_pll_hz(hw_ctx, ht.PHY_A)
    first = dev.time.now + ms(dev, 200)
    times = ht.hop_times(first, len(FULL_TABLE), ms(dev, 200))
    seqs = [s.at(t).tune(f, ch=2) for f, t in zip(FULL_TABLE, times)]
    s.commit()
    rows = []
    for seq, want in zip(seqs, FULL_TABLE):
        c = s.wait(seq, WAIT_MS)
        got = ht.read_pll_hz(hw_ctx, ht.PHY_B)
        rows.append({"seq": seq, "want": want, "got": got, "status": c.status.name,
                     "requested": c.requested_ticks, "executed": c.executed_ticks,
                     "detail": c.detail})
        record("tunes", rows)
        assert c.status == datax.CompletionStatus.OK, c
        assert c.executed_ticks == c.requested_ticks, c
        assert got == want, (want, got)
    a_after = ht.read_pll_hz(hw_ctx, ht.PHY_A)
    record("chip_a_pll", {"before": a_before, "after": a_after})
    record("tick_counters", ht.tick_counters(hw_ctx))
    assert a_after == a_before, f"chip A moved from {a_before} to {a_after} on chip-B tunes"
