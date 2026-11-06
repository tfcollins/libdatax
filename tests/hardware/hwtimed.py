# SPDX-License-Identifier: MIT
"""Helpers for the P3a timed-control hardware tests.

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

IMAGES = ("hopcap-sfp", "stock")
DEFAULT_IMAGE = "stock"


def normalize_image(value):
    """The DATAX_HW_IMAGE value (None or empty: the default), else ValueError."""
    if not value:
        return DEFAULT_IMAGE
    if value not in IMAGES:
        raise ValueError(f"DATAX_HW_IMAGE={value!r}: expected one of {', '.join(IMAGES)}")
    return value


def image_skip_reason(image, marker_names):
    """Why a test with these marker names must skip on ``image`` (None: it runs).

    ``p3a``: needs the hopcap SFP image (Tick, FHM trigger, PPS selector).
    ``needs_tx``: needs the stock TX DMA; on hopcap-sfp it feeds vrt49's deframer.
    """
    names = set(marker_names)
    if image == "stock" and "p3a" in names:
        return "P3a timed-control test: needs DATAX_HW_IMAGE=hopcap-sfp"
    if image == "hopcap-sfp" and "needs_tx" in names:
        return "TX DMA feeds vrt49's deframer on the hopcap-sfp image"
    return None


# ---- tick arithmetic and plans ---------------------------------------------------

STAIR_MASK = 0xE            # GPIO bits 3:1; bit 0 is reserved for tunes
PULSE_MAX_US = 10000        # the proven trigger pulse (src/fhm.c)


def us_to_ticks(us, hz):
    """Ticks in ``us`` microseconds, rounded up."""
    return -(-int(us) * int(hz) // 1000000)


def ticks_to_s(ticks, hz):
    return ticks / float(hz)


def staircase(t0, n, spacing_ticks):
    """``n`` GPIO edges ``(tick, mask, value)`` from ``t0``, ``spacing_ticks``
    apart, counting 1..n on bits 3:1 (value wraps after 7)."""
    return [(int(t0) + i * int(spacing_ticks), STAIR_MASK, ((i + 1) & 7) << 1)
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


def read_pll_hz(ctx):
    """The live RF PLL frequency of chip A in Hz (``frequency_hopping_mode``
    reads TALISE_getFhmRfPllFrequency), independent of libdatax's own check."""
    return int(_read(_lo_attr(ctx, FHM_HOP_ATTR)))


def read_fhm_enable(ctx):
    return int(_read(_lo_attr(ctx, FHM_ENABLE_ATTR)))


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
