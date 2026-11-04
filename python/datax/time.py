# SPDX-License-Identifier: MIT
"""Device time helpers (ticks)."""
import ctypes as C

from ._lib import c_ull, get_lib


class DeviceTime:
    """``dev.time``: ``now``, ``tick_hz`` (CapabilityError without a Tick) and
    ``seconds(x)`` -> integer ticks."""

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

    def align_next_pps(self, epoch=0):
        self._dev._check()
        rc = get_lib().datax_device_set_time_next_pps(self._dev._ptr, int(epoch))
        if rc:
            raise self._dev._ctx._error(rc)
