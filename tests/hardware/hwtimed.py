# SPDX-License-Identifier: MIT
"""Helpers for the P3a timed-control and P3b timed-stream hardware tests.

The first half is pure (no board, no libiio): image gating, tick arithmetic,
the staircase plan and evidence formatting; Tier 1 tests it
(tests/python/test_hwtimed_helpers.py). The second half reaches IIO
attributes the SDK does not expose (the Tick counters, the Talise FHM PLL
readback, attribute listings) through the libiio v1 escape hatch of
loopback.py.
"""
import ctypes as C

import loopback as lb

# ---- image gating ------------------------------------------------------------

IMAGES = ("hopcap-sfp", "stock", "timed-dma")
DEFAULT_IMAGE = "stock"


def normalize_image(value, required=False):
    """The DATAX_HW_IMAGE value (None or empty: the default), else ValueError.
    ``required`` (DATAX_HW_REQUIRE=1): an unset value is an error too, so a run
    that must prove P3a cannot go green by skipping it on the default image."""
    if not value:
        if required:
            raise ValueError("DATAX_HW_IMAGE is required when DATAX_HW_REQUIRE=1 "
                             f"(one of {', '.join(IMAGES)})")
        return DEFAULT_IMAGE
    if value not in IMAGES:
        raise ValueError(f"DATAX_HW_IMAGE={value!r}: expected one of {', '.join(IMAGES)}")
    return value


PPS_MODES = ("internal", "none")


def normalize_pps(value):
    """The DATAX_HW_PPS value (None or empty: internal), else ValueError."""
    if not value:
        return "internal"
    if value not in PPS_MODES:
        raise ValueError(f"DATAX_HW_PPS={value!r}: expected one of {', '.join(PPS_MODES)}")
    return value


def image_skip_reason(image, marker_names):
    """Why a test with these marker names must skip on ``image`` (None: it runs).

    ``p3a``: needs Tick, the FHM trigger and the PPS selector: hopcap-sfp or
    timed-dma (a superset of hopcap-sfp).
    ``p3b``: needs the DMA gates, the capture tagger and chip B's FHM pin: timed-dma only.
    ``needs_tx``: needs the stock TX DMA to the DAC: on hopcap-sfp it feeds
    vrt49's deframer instead (timed-dma restores it).
    """
    names = set(marker_names)
    if image == "stock" and "p3a" in names:
        return "P3a timed-control test: needs DATAX_HW_IMAGE=hopcap-sfp or timed-dma"
    if image != "timed-dma" and "p3b" in names:
        return "P3b timed-stream test: needs DATAX_HW_IMAGE=timed-dma"
    if image == "hopcap-sfp" and "needs_tx" in names:
        return "TX DMA feeds vrt49's deframer on the hopcap-sfp image"
    return None


# ---- tick arithmetic and plans ---------------------------------------------------

# GPIO bits 3:2. Bits 1:0 are reserved for tunes: bit 0 is chip A's FHM
# trigger, and bit 1 chip B's on the timed-dma image (libdatax refuses a
# queue_gpio that touches them).
STAIR_MASK = 0xC
LATE_GPIO = 0x4             # the one-bit GPIO test_late_dropped commits in the past
PULSE_MAX_US = 10000        # the proven trigger pulse (src/fhm.c)


def us_to_ticks(us, hz):
    """Ticks in ``us`` microseconds, rounded up."""
    return -(-int(us) * int(hz) // 1000000)


def ticks_to_s(ticks, hz):
    return ticks / float(hz)


def staircase(t0, n, spacing_ticks):
    """``n`` GPIO edges ``(tick, mask, value)`` from ``t0``, ``spacing_ticks``
    apart, counting 1..n on bits 3:2 (value wraps after 3)."""
    return [(int(t0) + i * int(spacing_ticks), STAIR_MASK, ((i + 1) & 3) << 2)
            for i in range(n)]


def dense_spacing_us(report_spacing_us, factor):
    """Tune spacing for a dense-hop run: ``factor`` x REPORT_SPACING, rounded
    up to 100 us (factor >= 1 keeps every rise reportable)."""
    if factor < 1:
        raise ValueError("factor must be >= 1: closer tunes cannot all report")
    return -(-int(round(report_spacing_us * factor)) // 100) * 100


def pulse_us(spacing_us):
    """Trigger pulse the library uses between tunes ``spacing_us`` apart in one
    commit: half the distance, at most the proven 10 ms."""
    return min(PULSE_MAX_US, int(spacing_us) // 2)


def hop_sequence(table, n):
    """``n`` frequencies cycling through ``table`` (no repeats back to back)."""
    if len(table) < 2:
        raise ValueError("need at least two frequencies")
    return [table[i % len(table)] for i in range(n)]


def hop_times(first, n, period_ticks):
    return [int(first) + i * int(period_ticks) for i in range(n)]


def names_matching(names, words=("phase", "sync")):
    """The names containing any of ``words`` (case-insensitive), sorted."""
    return sorted(n for n in names if any(w in n.lower() for w in words))


def offset_after_align_ok(now_ticks, epoch, hz, max_s=0.25):
    """Right after a PPS alignment, device time is the epoch plus the (small)
    time since the edge: 0 <= (now - epoch) <= max_s seconds."""
    d = now_ticks - epoch
    return 0 <= d <= max_s * hz


def advance_ok(dev_ticks, hz, wall_s, tol_s=0.05):
    """Device time advanced ``dev_ticks`` while ``wall_s`` seconds passed."""
    return abs(dev_ticks / float(hz) - wall_s) <= tol_s


def redact_tree(x):
    """Copy of nested evidence data with every string run through the
    suite's redaction (addresses, hostnames); tuples become lists."""
    if isinstance(x, str):
        return lb.redact_text(x)
    if isinstance(x, dict):
        return {k: redact_tree(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [redact_tree(v) for v in x]
    return x


def flush_while_high(before, after, rise, pulse):
    """Device times read just before and after a flush show it happened while
    the trigger pulse was high: the rise had fired and the fall had not."""
    return rise <= before and after < rise + pulse


def completion_log(done):
    """Plain data for the evidence: one dict per Completion."""
    return [{"seq": c.seq, "status": getattr(c.status, "name", str(c.status)),
             "requested": c.requested_ticks, "executed": c.executed_ticks,
             "skew_ticks": c.executed_ticks - c.requested_ticks, "detail": c.detail}
            for c in done]


def all_on_time(done):
    """Every completion OK with executed == requested."""
    return all(getattr(c.status, "name", c.status) == "OK" and
               c.executed_ticks == c.requested_ticks for c in done)


# ---- P3b timed streams (pure) ------------------------------------------------------

TX_START_MUTE_READS = 3     # G2/G3 ruling 1: a TX start_at mutes its first N reads (8 converters)


def frames_to_ticks(n, fs, hz):
    """Ticks of ``n`` sample frames at ``fs`` frames/s on an ``hz`` timebase,
    rounded half up (integers only: Python's round() is half-even)."""
    n, fs, hz = int(n), int(fs), int(hz)
    if fs <= 0:
        raise ValueError("fs must be positive")
    return (2 * n * hz + fs) // (2 * fs)


def ticks_to_frames(ticks, fs, hz):
    """Sample frames in ``ticks`` device ticks, rounded half up."""
    ticks, fs, hz = int(ticks), int(fs), int(hz)
    if hz <= 0:
        raise ValueError("hz must be positive")
    return (2 * ticks * fs + hz) // (2 * hz)


def strip_frames(header_bytes, nch):
    """Whole frames the capture tag costs: ceil(header_bytes / frame_bytes),
    frame_bytes = 4 per channel (G2/G3 ruling 6)."""
    fb = 4 * int(nch)
    return -(-int(header_bytes) // fb)


def block0_timestamp(t_open, strip, fs, hz):
    """Block 0's ``timestamp_ticks`` for an opening at ``t_open`` (API ticks):
    util_cpack2 drops the first released sample, the tag costs ``strip``
    frames, so the first delivered sample is released sample 1 + strip."""
    return int(t_open) + frames_to_ticks(1 + int(strip), fs, hz)


def sample_time(anchor, strip, k, fs, hz):
    """API time of delivered sample ``k`` (0: block 0's first) of a release
    whose tag says ``anchor``: libdatax's derived-timestamp rule."""
    return int(anchor) + frames_to_ticks(1 + int(strip) + int(k), fs, hz)


def offset_bracket(api_before, hw, api_after):
    """(lo, hi) bounds of the soft offset (API = hw + offset) from an API time
    read, a raw Tick read and another API read, in that order."""
    return int(api_before) - int(hw), int(api_after) - int(hw)


def audible(raw):
    """Per frame of int16 ``(n, 2)`` I/Q: True where it is not exactly zero
    (a muted DAC through the digital loopback reads exactly 0)."""
    import numpy as np
    raw = np.asarray(raw)
    return np.any(raw != 0, axis=1)


def runs(mask):
    """``[(start, end)]`` of the True runs of a boolean array (end exclusive)."""
    import numpy as np
    m = np.asarray(mask, dtype=np.int8)
    d = np.diff(np.concatenate(([0], m, [0])))
    return list(zip(np.flatnonzero(d == 1).tolist(), np.flatnonzero(d == -1).tolist()))


def pattern_lag(expected, raw, start):
    """The lag ``l`` with ``raw[k] == expected[(k - l) % P]`` (int16 ``(n, 2)``
    rows), found from the ``P`` frames at ``start``; returns ``l % P``."""
    import numpy as np
    exp = np.asarray(expected, dtype=np.int16)
    p = exp.shape[0]
    seg = np.asarray(raw, dtype=np.int16)[start:start + p]
    if seg.shape[0] < p:
        raise ValueError(f"need {p} frames from {start}, have {seg.shape[0]}")
    e = exp[:, 0].astype(np.float64) + 1j * exp[:, 1]
    r = seg[:, 0].astype(np.float64) + 1j * seg[:, 1]
    lag, _, _ = lb.align_and_fit(e, r)
    return (int(start) + int(lag)) % p


def mismatches_at_lag(expected, raw, lag, mask=None):
    """Frames of ``raw`` (where ``mask`` is True) that differ from
    ``expected[(k - lag) % P]``."""
    import numpy as np
    exp = np.asarray(expected, dtype=np.int16)
    raw = np.asarray(raw, dtype=np.int16)
    idx = (np.arange(raw.shape[0]) - int(lag)) % exp.shape[0]
    bad = np.any(raw != exp[idx], axis=1)
    if mask is not None:
        bad &= np.asarray(mask, dtype=bool)
    return int(np.count_nonzero(bad))


def spread_ok(values, tol=1):
    """Every value within ``tol`` of the median (the "identical within +-1
    sample" rule); returns ``(ok, median, max - min)``."""
    v = sorted(int(x) for x in values)
    if not v:
        return False, None, None
    med = v[len(v) // 2]
    return all(abs(x - med) <= tol for x in v), med, v[-1] - v[0]


def ring_coverage_s(block_samples, block_count, fs):
    """Seconds of samples the blocks enqueued before T hold."""
    return int(block_samples) * int(block_count) / float(fs)


def gate_map_roles(text):
    """The roles of a stream_gate_map text (commas, surrounding blanks dropped)."""
    return [r.strip() for r in (text or "").split(",") if r.strip()]


# ---- libiio v1 access ---------------------------------------------------------

TICK_DEVICE = "axi-timed-command-scheduler"
PHY_A = "adrv9009-phy"
PHY_B = "adrv9009-phy-b"
FHM_HOP_ATTR = "frequency_hopping_mode"
FHM_ENABLE_ATTR = "frequency_hopping_mode_enable"

_ready = False


def _lib():
    global _ready
    lib = lb.iio_lib()
    if not _ready:
        vp, cp, ui = C.c_void_p, C.c_char_p, C.c_uint
        for name, res, args in (
            ("iio_device_find_attr", vp, (vp, cp)),
            ("iio_device_get_attrs_count", ui, (vp,)),
            ("iio_device_get_attr", vp, (vp, ui)),
            ("iio_device_get_debug_attrs_count", ui, (vp,)),
            ("iio_device_get_debug_attr", vp, (vp, ui)),
            ("iio_device_get_channels_count", ui, (vp,)),
            ("iio_device_get_channel", vp, (vp, ui)),
            ("iio_device_find_channel", vp, (vp, cp, C.c_bool)),
            ("iio_channel_get_id", cp, (vp,)),
            ("iio_channel_get_attrs_count", ui, (vp,)),
            ("iio_channel_get_attr", vp, (vp, ui)),
            ("iio_channel_find_attr", vp, (vp, cp)),
        ):
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = res, list(args)
        _ready = True
    return lib


def _read(attr_handle):
    buf = C.create_string_buffer(256)
    rc = _lib().iio_attr_read_raw(attr_handle, buf, len(buf))
    if rc < 0:
        raise lb.LoopbackError(f"attribute read failed (errno {-rc})")
    return buf.raw[:rc].split(b"\0", 1)[0].decode(errors="replace").strip()


def tick_attr(ctx, attr):
    """An integer attribute of the Tick IIO device, or None when it has none."""
    lib = _lib()
    dev = lb.find_device(ctx, TICK_DEVICE)
    if not dev:
        return None
    h = lib.iio_device_find_attr(dev, attr.encode())
    if not h:
        return None
    try:
        return int(_read(h), 0)
    except (ValueError, lb.LoopbackError):
        return None


def tick_counters(ctx):
    """Tick counters for the evidence (None for an attribute this Tick lacks)."""
    return {a: tick_attr(ctx, a) for a in
            ("late_count", "overflow_count", "pps_present", "time_aligned")}


def tick_attr_str(ctx, attr):
    """A text attribute of the Tick IIO device, or None when it has none."""
    dev = lb.find_device(ctx, TICK_DEVICE)
    if not dev:
        return None
    h = _lib().iio_device_find_attr(dev, attr.encode())
    if not h:
        return None
    try:
        return _read(h)
    except lb.LoopbackError:
        return None


EDGE_CAPTURE_ATTRS = ("edge_capture_ticks", "edge_capture_valid", "edge_capture_count",
                      "edge_capture_overrun", "edge_capture_target_id",
                      "edge_capture_command_id")


def edge_capture(ctx):
    """Tick's capture tag registers: the raw (hardware) tick of the most
    recent STREAM_GATE assert, and its status (None for a missing attribute)."""
    return {a[len("edge_capture_"):]: tick_attr(ctx, a) for a in EDGE_CAPTURE_ATTRS}


def _lo_attr(ctx, attr, phy=PHY_A):
    dev = lb.find_device(ctx, phy)
    if not dev:
        raise lb.LoopbackError(f"IIO device {phy!r} not found")
    ch = _lib().iio_device_find_channel(dev, b"altvoltage0", True)
    if not ch:
        raise lb.LoopbackError(f"{phy}: no output channel altvoltage0 (TRX LO)")
    h = _lib().iio_channel_find_attr(ch, attr.encode())
    if not h:
        raise lb.LoopbackError(f"{phy}: altvoltage0 has no attribute {attr!r}")
    return h


def read_pll_hz(ctx, phy=PHY_A):
    """The live RF PLL frequency of a chip in Hz (``frequency_hopping_mode``
    reads TALISE_getFhmRfPllFrequency), independent of libdatax's own check."""
    return int(_read(_lo_attr(ctx, FHM_HOP_ATTR, phy)))


def read_fhm_enable(ctx, phy=PHY_A):
    return int(_read(_lo_attr(ctx, FHM_ENABLE_ATTR, phy)))


def write_fhm_enable(ctx, on, phy=PHY_A):
    """Switch a chip's FHM through its driver (the suite's own escape hatch:
    libdatax enables FHM at stage_hop_table and disables it at context close)."""
    rc = _lib().iio_attr_write_string(_lo_attr(ctx, FHM_ENABLE_ATTR, phy),
                                      b"1" if on else b"0")
    if rc < 0:
        raise lb.LoopbackError(f"{phy}: writing {FHM_ENABLE_ATTR}={int(bool(on))} failed "
                               f"(errno {-rc})")


LO_ATTR = "frequency"


def read_lo_hz(ctx, phy=PHY_A):
    """The TRX LO a chip's driver reports (``frequency`` of altvoltage0)."""
    return int(_read(_lo_attr(ctx, LO_ATTR, phy)))


def write_lo_hz(ctx, hz, phy=PHY_A):
    """Set a chip's TRX LO through its driver (FHM must be off)."""
    rc = _lib().iio_attr_write_string(_lo_attr(ctx, LO_ATTR, phy), str(int(hz)).encode())
    if rc < 0:
        raise lb.LoopbackError(f"{phy}: writing {LO_ATTR}={int(hz)} failed (errno {-rc})")


def attribute_names(ctx, device):
    """All attribute names of an IIO device: ``{"attrs", "debug", "channels"}``
    (channels: ``{"<in|out>:<id>": [names]}``)."""
    lib = _lib()
    dev = lb.find_device(ctx, device)
    if not dev:
        return None

    def name(h):
        return lib.iio_attr_get_name(h).decode()

    out = {"attrs": [name(lib.iio_device_get_attr(dev, i))
                     for i in range(lib.iio_device_get_attrs_count(dev))],
           "debug": [name(lib.iio_device_get_debug_attr(dev, i))
                     for i in range(lib.iio_device_get_debug_attrs_count(dev))],
           "channels": {}}
    for i in range(lib.iio_device_get_channels_count(dev)):
        ch = lib.iio_device_get_channel(dev, i)
        cid = lib.iio_channel_get_id(ch).decode()
        out["channels"][cid + "#" + str(i)] = [
            name(lib.iio_channel_get_attr(ch, k))
            for k in range(lib.iio_channel_get_attrs_count(ch))]
    return out
