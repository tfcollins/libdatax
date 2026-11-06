# SPDX-License-Identifier: MIT
"""Pure helpers for the hardware suite (no board needed to import or test).

Debug attributes (digital loopback) are reached through the libiio context
escape hatch: ``Context.iio_context`` is ``datax_context_get_iio_context``,
and the attributes are read and written with libiio v1 through ctypes
(``iio_context_find_device``, ``iio_device_find_debug_attr``,
``iio_attr_write_string``, ``iio_attr_read_raw``). pylibiio is not used: the
``iio`` module installable from PyPI targets libiio v0, whose C API differs
from the pinned v1 build that libdatax links. The v1 shared object is the one
already mapped into the process by libdatax.so (found in /proc/self/maps), so
the calls always go to the same libiio as the context pointer; override with
``DATAX_LIBIIO_LIB=/path/to/libiio.so`` when /proc is unavailable.
"""
import ctypes as C
import hashlib
import os
import re

import numpy as np

EVM_FLOOR_DB = -300.0


class LoopbackError(RuntimeError):
    """The digital loopback could not be switched (names the device/attribute)."""


# ---- signals -----------------------------------------------------------------

def qpsk_block(n, seed):
    """``n`` QPSK symbols at +-0.5 full scale per rail (complex128)."""
    rng = np.random.default_rng(seed)
    bits = rng.integers(0, 2, size=(2, int(n)))
    return (np.where(bits[0], 0.5, -0.5) + 1j * np.where(bits[1], 0.5, -0.5))


def align_and_fit(tx, rx):
    """Fit ``rx ~= gain * roll(tx, lag)`` for a periodic ``tx``.

    ``rx`` may be longer than ``tx`` (a capture of several periods); its first
    ``len(tx)`` samples are one circular shift of the period. Returns
    ``(lag, gain, evm_db)``: lag by circular cross-correlation, the
    least-squares complex gain, and the error power relative to the fitted
    signal power in dB (floored at -300 dB so exact matches stay finite).
    """
    tx = np.asarray(tx, dtype=np.complex128)
    rx = np.asarray(rx, dtype=np.complex128)
    n = tx.shape[0]
    if rx.shape[0] < n:
        raise ValueError(f"rx ({rx.shape[0]}) must be at least as long as tx ({n})")
    if not np.any(tx):
        raise ValueError("tx is all zero")
    rx = rx[:n]
    corr = np.fft.ifft(np.fft.fft(rx) * np.conj(np.fft.fft(tx)))
    lag = int(np.argmax(np.abs(corr)))
    ref = np.roll(tx, lag)
    gain = complex(np.vdot(ref, rx) / np.vdot(ref, ref))
    sig = float(np.sum(np.abs(gain * ref) ** 2))
    err = float(np.sum(np.abs(rx - gain * ref) ** 2))
    if sig == 0.0:
        return lag, gain, 0.0  # rx carries nothing of tx
    evm = 10 * np.log10(err / sig) if err > 0 else EVM_FLOOR_DB
    return lag, gain, float(max(evm, EVM_FLOOR_DB))


def mismatches_after_shift(tx_raw, rx_raw, lag, shift):
    """Count int16 values where ``rx != roll(tx, lag) >> shift`` (arithmetic).

    Both arrays are ``(n, 2*nch)`` int16 (I,Q per channel); ``rx_raw`` may be
    longer than ``tx_raw``; only its first ``len(tx_raw)`` rows are compared.
    """
    tx_raw = np.asarray(tx_raw, dtype=np.int16)
    n = tx_raw.shape[0]
    ref = np.roll(tx_raw, lag, axis=0) >> shift
    return int(np.count_nonzero(ref != np.asarray(rx_raw, dtype=np.int16)[:n]))


# ---- evidence helpers --------------------------------------------------------

def redact_uri(uri):
    """Replace the host of a libiio URI by a stable hash; keep scheme and port."""
    m = re.match(r"^(?P<scheme>[a-z]+):(?P<rest>.+)$", uri)
    if not m or m.group("scheme") not in ("ip", "tcp", "udp", "daqiri"):
        return uri
    rest = m.group("rest")
    if rest.startswith("["):  # bracketed IPv6
        end = rest.index("]") + 1
        host, tail = rest[:end], rest[end:]
    else:
        host, _, tail = rest.partition(":")
        tail = (":" + tail) if tail else ""
    digest = hashlib.sha256(host.encode()).hexdigest()[:10]
    return f"{m.group('scheme')}:host-{digest}{tail}"


def redact_text(text):
    """Hide IPv4 addresses and the hostname of a ``... Linux <host> ...`` description."""
    text = re.sub(r"\b\d{1,3}(?:\.\d{1,3}){3}\b", "<ip>", text)
    return re.sub(r"(\bLinux )(\S+)",
                  lambda m: m.group(1) + "host-" +
                  hashlib.sha256(m.group(2).encode()).hexdigest()[:10], text)


# ---- libiio v1 access --------------------------------------------------------

_iio = None


def _libiio_path():
    env = os.environ.get("DATAX_LIBIIO_LIB")
    if env:
        return env
    try:
        with open("/proc/self/maps") as f:
            for line in f:
                m = re.search(r"(/\S*libiio\.so\S*)", line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return None


def iio_lib():
    """The libiio v1 library already used by libdatax, with signatures set."""
    global _iio
    if _iio is not None:
        return _iio
    path = _libiio_path()
    if not path:
        raise LoopbackError("cannot locate the libiio v1 library in this process "
                            "(open a datax Context first, or set DATAX_LIBIIO_LIB)")
    lib = C.CDLL(path)
    vp, cp = C.c_void_p, C.c_char_p
    for name, res, args in (
        ("iio_context_find_device", vp, (vp, cp)),
        ("iio_device_find_debug_attr", vp, (vp, cp)),
        ("iio_attr_write_string", C.c_ssize_t, (vp, cp)),
        ("iio_attr_read_raw", C.c_ssize_t, (vp, C.c_char_p, C.c_size_t)),
        ("iio_context_get_description", cp, (vp,)),
        ("iio_context_get_version_major", C.c_uint, (vp,)),
        ("iio_context_get_version_minor", C.c_uint, (vp,)),
        ("iio_context_get_version_tag", cp, (vp,)),
        ("iio_context_get_attrs_count", C.c_uint, (vp,)),
        ("iio_context_get_attr", vp, (vp, C.c_uint)),
        ("iio_attr_get_name", cp, (vp,)),
        ("iio_attr_get_static_value", cp, (vp,)),
    ):
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = res, list(args)
    _iio = lib
    return lib


def _dec(b):
    return b.decode(errors="replace") if b else ""


def find_device(ctx, name):
    """The ``struct iio_device *`` named ``name``, or None."""
    return iio_lib().iio_context_find_device(ctx.iio_context, name.encode()) or None


def debug_attr(ctx, device, attr):
    """The debug attribute handle, or raises LoopbackError naming both."""
    dev = find_device(ctx, device)
    if not dev:
        raise LoopbackError(f"IIO device {device!r} not found in the context")
    h = iio_lib().iio_device_find_debug_attr(dev, attr.encode())
    if not h:
        raise LoopbackError(f"debug attribute {attr!r} not found on IIO device "
                            f"{device!r}")
    return h


def has_debug_attr(ctx, device, attr):
    try:
        debug_attr(ctx, device, attr)
        return True
    except LoopbackError:
        return False


def write_debug_attr(ctx, device, attr, value):
    h = debug_attr(ctx, device, attr)
    rc = iio_lib().iio_attr_write_string(h, str(value).encode())
    if rc < 0:
        raise LoopbackError(f"writing {value!r} to debug attribute {attr!r} of "
                            f"{device!r} failed: {os.strerror(-rc)}")


def read_debug_attr(ctx, device, attr):
    h = debug_attr(ctx, device, attr)
    buf = C.create_string_buffer(256)
    rc = iio_lib().iio_attr_read_raw(h, buf, len(buf))
    if rc < 0:
        raise LoopbackError(f"reading debug attribute {attr!r} of {device!r} failed: "
                            f"{os.strerror(-rc)}")
    return buf.raw[:rc].split(b"\0", 1)[0].decode(errors="replace").strip()


def context_info(ctx):
    """Description and version attributes of the IIO context, as plain data
    (addresses and hostnames redacted)."""
    lib, p = iio_lib(), ctx.iio_context
    attrs = {}
    for i in range(lib.iio_context_get_attrs_count(p)):
        a = lib.iio_context_get_attr(p, i)
        if a:
            k, v = _dec(lib.iio_attr_get_name(a)), _dec(lib.iio_attr_get_static_value(a))
            attrs[k] = redact_uri(v) if k == "uri" else redact_text(v)
    return {
        "description": redact_text(_dec(lib.iio_context_get_description(p))),
        "version": f"{lib.iio_context_get_version_major(p)}."
                   f"{lib.iio_context_get_version_minor(p)}"
                   f"-{_dec(lib.iio_context_get_version_tag(p))}",
        "attributes": attrs,
    }


# ---- digital loopback --------------------------------------------------------

def family_of(device_name):
    """``"ad936x"`` or ``"adrv9009"`` from a datax device name."""
    if re.match(r"^ad936[134]", device_name):
        return "ad936x"
    if device_name.startswith("adrv9009"):
        return "adrv9009"
    raise ValueError(f"no digital loopback known for device {device_name!r}")


def set_digital_loopback(ctx, family, on):
    """Switch the transceiver's digital loopback (TX samples appear on RX).

    ad936x: debug attribute ``loopback`` of ``ad9361-phy`` (1: RX = TX >> 4
    bit-exact on stock Kuiper). adrv9009: ``bist_framer_a_loopback`` of
    ``adrv9009-phy`` and, when ``adrv9009-phy-b`` exists, ``bist_framer_b_loopback``
    of it. A missing device or attribute raises LoopbackError naming it (the
    adrv9009 attributes are unverified on hardware; this is a failure, not a skip).
    """
    val = "1" if on else "0"
    if family == "ad936x":
        write_debug_attr(ctx, "ad9361-phy", "loopback", val)
    elif family == "adrv9009":
        write_debug_attr(ctx, "adrv9009-phy", "bist_framer_a_loopback", val)
        if find_device(ctx, "adrv9009-phy-b"):
            write_debug_attr(ctx, "adrv9009-phy-b", "bist_framer_b_loopback", val)
    else:
        raise ValueError(f"unknown family {family!r}")


# ---- stream counters ----------------------------------------------------------

class StatsC(C.Structure):
    """struct datax_internal_stream_stats."""
    _fields_ = [("enabled", C.c_bool), ("blocks_done", C.c_ulonglong),
                ("overflows", C.c_ulonglong), ("underflows", C.c_ulonglong),
                ("status_polls", C.c_ulonglong), ("overflow_detect", C.c_bool)]


def stream_stats(stream):
    """Counters of a datax.RxStream/TxStream (datax_internal_stream_stats)."""
    from datax._lib import get_lib
    lib = get_lib()
    fn = lib.datax_internal_stream_stats
    fn.argtypes, fn.restype = [C.c_void_p, C.POINTER(StatsC)], C.c_int
    out = StatsC()
    rc = fn(stream._live(), C.byref(out))
    if rc:
        raise OSError(-rc, "datax_internal_stream_stats failed")
    return {f: getattr(out, f) for f, _ in StatsC._fields_}
