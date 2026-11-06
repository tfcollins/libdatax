# SPDX-License-Identifier: MIT
"""Immediate (untimed) sample streams: RxStream and TxStream.

Sample layout follows the C API: int16 I/Q in host order, per instant the
enabled channels in ascending index, each I then Q. ``channels`` is therefore
always handled in ascending order, and array rows follow it.

numpy is imported lazily so ``import datax`` works without it.
"""
import ctypes as C
import errno
from dataclasses import dataclass

from ._lib import BlockInfoC, StreamConfig, get_lib
from .errors import CapabilityError, Error

_RX, _TX = 0, 1
_INT16_MIN, _INT16_MAX = -32768, 32767


def _np():
    try:
        import numpy
    except ImportError as e:
        raise CapabilityError(errno.ENOTSUP,
                              "streams need numpy: pip install libdatax[stream]") from e
    return numpy


@dataclass(frozen=True)
class BlockInfo:
    """Describes the first block a ``recv`` touched."""
    timestamp_ticks: int
    discontinuous: bool
    valid_samples: int


def _norm_channels(channels):
    try:
        chans = tuple(int(c) for c in channels)
    except TypeError:
        chans = (int(channels),)
    if not chans or len(set(chans)) != len(chans) or any(c < 0 or c > 31 for c in chans):
        raise ValueError(f"channels must be distinct integers in 0..31, got {channels!r}")
    return tuple(sorted(chans))


class _Stream:
    _direction = _RX

    def __init__(self, device, channels=(0,), block_samples=65536, block_count=4,
                 cyclic=False):
        _np()  # fail early, with the install hint, when numpy is missing
        device._check()
        self.channels = _norm_channels(channels)
        if int(block_samples) <= 0 or int(block_count) <= 0:
            raise ValueError("block_samples and block_count must be positive")
        self.block_samples = int(block_samples)
        self.block_count = 1 if cyclic else int(block_count)
        self.cyclic = bool(cyclic)
        self._device = device
        self._ctx = device._ctx
        self._h = None
        self._started = False
        cfg = StreamConfig(self._direction, sum(1 << c for c in self.channels),
                           self.block_samples, self.block_count, self.cyclic)
        h = C.c_void_p()
        rc = get_lib().datax_stream_create(device._ptr, C.byref(cfg), C.byref(h))
        if rc:
            raise self._ctx._error(rc)
        self._h = h
        self._ctx._streams.add(self)

    # -- lifetime ----------------------------------------------------------

    def _live(self):
        """The C handle, or Error when closed (also when its context closed)."""
        if self._h is None or self._ctx._closed:
            raise Error(errno.EBADF, "stream is closed")
        return self._h

    @property
    def closed(self):
        return self._h is None

    def _close_for_context(self):
        """Called by Context.close() before the C context is destroyed."""
        h, self._h = self._h, None
        self._started = False
        if h is not None:
            get_lib().datax_stream_destroy(h)  # context still alive here

    def start(self):
        h = self._live()
        rc = get_lib().datax_stream_start(h)
        if rc:
            raise self._ctx._error(rc)
        self._started = True

    def stop(self):
        """Stop the stream. One-shot TX drains first and may raise on timeout."""
        h = self._live()
        self._started = False
        rc = get_lib().datax_stream_stop(h)
        if rc:
            raise self._ctx._error(rc)

    def close(self):
        """Stop and destroy. Idempotent; a drain failure is raised after the
        stream has been destroyed."""
        if self._h is None:
            return
        err = None
        if self._started and not self._ctx._closed:
            try:
                self.stop()
            except Error as e:
                err = e
        self._close_for_context()
        if err:
            raise err

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            self.close()
        except Error:
            if exc_type is None:
                raise

    def __del__(self):
        # Only destroys while the context lives; Context.close() has already
        # destroyed (and detached) every stream otherwise.
        try:
            if self._h is not None and not self._ctx._closed:
                self._close_for_context()
        except Exception:
            pass


class RxStream(_Stream):
    """Receive stream. ``recv`` gives complex64 ``(len(channels), n)``."""
    _direction = _RX

    def recv_raw(self, n=None, timeout_ms=-1):
        """Receive ``n`` instants as int16 ``(n, 2 * len(channels))``.

        Returns ``(array, BlockInfo)``. The stream must be started."""
        np = _np()
        h = self._live()
        n = self.block_samples if n is None else int(n)
        if n <= 0:
            raise ValueError("n must be positive")
        raw = np.empty((n, 2 * len(self.channels)), dtype=np.int16)
        info = BlockInfoC()
        rc = get_lib().datax_stream_recv(h, raw.ctypes.data, n, C.byref(info),
                                         int(timeout_ms))
        if rc < 0:
            raise self._ctx._error(rc)
        if rc < n:
            raw = raw[:rc]
        return raw, BlockInfo(int(info.timestamp_ticks), bool(info.discontinuous),
                              int(info.valid_samples))

    def recv(self, n=None, timeout_ms=-1):
        """Receive ``n`` instants (default ``block_samples``) as complex64
        ``(len(channels), n)``, rows in ascending channel order."""
        np = _np()
        raw, info = self.recv_raw(n, timeout_ms)
        out = np.empty((len(self.channels), raw.shape[0]), dtype=np.complex64)
        out.real = raw[:, 0::2].T
        out.imag = raw[:, 1::2].T
        return out, info


class TxStream(_Stream):
    """Transmit stream. Cyclic streams transmit at their single ``send``."""
    _direction = _TX

    def __init__(self, device, channels=(0,), block_samples=65536, block_count=4,
                 cyclic=False):
        super().__init__(device, channels, block_samples, block_count, cyclic)

    def _to_raw(self, samples):
        np = _np()
        a = np.asarray(samples)
        nch = len(self.channels)
        if a.ndim == 1 and nch == 1:
            a = a[np.newaxis, :]
        if a.ndim != 2 or a.shape[0] != nch:
            raise ValueError(f"samples must have shape ({nch}, n)"
                             + (" or (n,)" if nch == 1 else "") + f", got {a.shape}")
        if a.shape[1] == 0:
            raise ValueError("samples is empty")
        if a.dtype.kind not in "iufc":
            raise ValueError(f"samples must be numeric, got dtype {a.dtype}")
        n = a.shape[1]
        if self.cyclic and n > self.block_samples:
            raise ValueError(f"a cyclic send holds at most block_samples "
                             f"({self.block_samples}) instants, got {n}")
        re = np.rint(a.real.astype(np.float64))
        im = np.rint(a.imag.astype(np.float64)) if a.dtype.kind == "c" \
            else np.zeros_like(re)
        for part in (re, im):
            if not np.all(np.isfinite(part)) or part.min() < _INT16_MIN \
                    or part.max() > _INT16_MAX:
                raise ValueError("sample values out of int16 range (-32768..32767) "
                                 "after rounding, or not finite")
        raw = np.empty((n, 2 * nch), dtype=np.int16)
        raw[:, 0::2] = re.T
        raw[:, 1::2] = im.T
        return raw

    def send(self, samples, timeout_ms=-1):
        """Send complex ``(len(channels), n)`` samples (1-D for one channel).

        Values are rounded to int16; out-of-range values or a wrong shape
        raise ``ValueError``. Returns the number of instants accepted (a
        partial count if the wait for a free block timed out after some data
        was taken)."""
        h = self._live()
        raw = self._to_raw(samples)
        rc = get_lib().datax_stream_send(h, raw.ctypes.data, raw.shape[0],
                                         int(timeout_ms))
        if rc < 0:
            raise self._ctx._error(rc)
        return rc
