# SPDX-License-Identifier: MIT
"""Device time helpers (ticks)."""
import ctypes as C
import enum

from ._lib import c_ull, get_lib


class TimeSource(enum.IntEnum):
    """Where device time comes from (``enum datax_time_source``)."""
    FREE_RUN = 0       # internal clock, never aligned
    PPS_ALIGNED = 1    # epoch loaded on a fabric PPS edge
    SOFT_ALIGNED = 2   # aligned to the host clock in software (no PPS)
    PPS_HOLDOVER = 3   # was PPS-aligned; PPS lost; running on the internal clock


class DeviceTime:
    """``dev.time``: ``now``, ``tick_hz`` (CapabilityError without a Tick),
    ``seconds(x)`` -> integer ticks, ``source`` (a ``TimeSource``) and
    ``align_next_pps(epoch)``."""

    def __init__(self, device):
        self._dev = device

    def _read(self):
        self._dev._check()
        t, hz = c_ull(), c_ull()
        rc = get_lib().datax_device_get_time(self._dev._ptr, C.byref(t), C.byref(hz))
        if rc:
            raise self._dev._ctx._error(rc)
        return t.value, hz.value

    @property
    def now(self):
        return self._read()[0]

    @property
    def tick_hz(self):
        return self._read()[1]

    def seconds(self, x):
        return int(round(x * self.tick_hz))

    @property
    def source(self):
        self._dev._check()
        v = C.c_int()
        rc = get_lib().datax_device_get_time_source(self._dev._ptr, C.byref(v))
        if rc:
            raise self._dev._ctx._error(rc)
        return TimeSource(v.value)

    def align_next_pps(self, epoch=0):
        """Align device time to ``epoch`` ticks on the next PPS edge; without a
        PPS this falls back to the host clock (``source`` becomes
        ``SOFT_ALIGNED``, with a ``PPS_LOCK_CHANGE`` event) and still returns."""
        self._dev._check()
        rc = get_lib().datax_device_set_time_next_pps(self._dev._ptr, int(epoch))
        if rc:
            raise self._dev._ctx._error(rc)
