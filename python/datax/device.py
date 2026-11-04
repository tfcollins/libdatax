# SPDX-License-Identifier: MIT
"""Device and per-channel views with pyadi-iio style properties."""
import ctypes as C
import errno

from ._lib import c_ull, get_lib
from .caps import Capabilities
from .errors import Error
from .time import DeviceTime


class _ChannelProps:
    """Properties shared by Device (channel 0) and ChannelView.

    Channel numbering follows the C API: RX channels first, then TX. The C API
    has no channel-count getter in P1, so the TX base index is
    ``caps.limits["max_channels"]`` (the RX channel count).
    """

    _ch = 0

    def _check(self):
        raise NotImplementedError

    def _rx(self):
        return self._ch

    def _tx(self):
        return self._ch + self._tx_base()

    def _tx_base(self):
        return self.caps.limits["max_channels"]

    def _freq_get(self, ch):
        self._check()
        v = c_ull()
        self._call(get_lib().datax_device_get_frequency(self._ptr, ch, C.byref(v)))
        return float(v.value)

    def _freq_set(self, ch, hz):
        self._check()
        self._call(get_lib().datax_device_set_frequency(self._ptr, ch, int(hz), 0))

    def _gain_get(self, ch):
        self._check()
        v = C.c_double()
        self._call(get_lib().datax_device_get_gain(self._ptr, ch, C.byref(v)))
        return v.value

    def _gain_set(self, ch, db):
        self._check()
        self._call(get_lib().datax_device_set_gain(self._ptr, ch, float(db), 0))

    @property
    def rx_lo(self):
        return self._freq_get(self._rx())

    @rx_lo.setter
    def rx_lo(self, hz):
        self._freq_set(self._rx(), hz)

    @property
    def tx_lo(self):
        return self._freq_get(self._tx())

    @tx_lo.setter
    def tx_lo(self, hz):
        self._freq_set(self._tx(), hz)

    @property
    def rx_gain(self):
        return self._gain_get(self._rx())

    @rx_gain.setter
    def rx_gain(self, db):
        self._gain_set(self._rx(), db)

    @property
    def tx_gain(self):
        return self._gain_get(self._tx())

    @tx_gain.setter
    def tx_gain(self, db):
        self._gain_set(self._tx(), db)

    @property
    def rx_bandwidth(self):
        """Write-only in P1 (the C API has no bandwidth getter)."""
        raise self._ctx._error(errno.ENOTSUP, "rx_bandwidth read-back arrives with P2")

    @rx_bandwidth.setter
    def rx_bandwidth(self, hz):
        self._check()
        self._call(get_lib().datax_device_set_bandwidth(self._ptr, self._rx(), int(hz)))

    @property
    def sample_rate(self):
        """Write-only in P1 (the C API has no sample-rate getter)."""
        raise self._ctx._error(errno.ENOTSUP, "sample_rate read-back arrives with P2")

    @sample_rate.setter
    def sample_rate(self, sps):
        self._check()
        self._call(get_lib().datax_device_set_sample_rate(self._ptr, int(sps)))

    def _call(self, rc):
        if rc:
            raise self._ctx._error(rc)


class ChannelView(_ChannelProps):
    """Same properties as Device, for channel ``n`` (RX n / TX n)."""

    def __init__(self, device, n):
        self._device = device
        self._ch = int(n)

    @property
    def _ptr(self):
        return self._device._ptr

    @property
    def _ctx(self):
        return self._device._ctx

    @property
    def caps(self):
        return self._device.caps

    def _check(self):
        self._device._check()


class Device(_ChannelProps):
    def __init__(self, ctx, ptr):
        self._ctx = ctx
        self._ptr_ = ptr
        self._caps_ptr_ = get_lib().datax_device_get_caps(ptr)
        self._valid = True
        self.caps = Capabilities(self)
        self.time = DeviceTime(self)

    def _invalidate(self):
        self._valid = False
        self._ptr_ = None
        self._caps_ptr_ = None

    def _check(self):
        if not self._valid or self._ctx._closed:
            raise Error(errno.EBADF, "device used after its context was closed")

    @property
    def _ptr(self):
        return self._ptr_

    @property
    def _caps_ptr(self):
        return self._caps_ptr_

    @property
    def name(self):
        self._check()
        n = get_lib().datax_device_get_name(self._ptr)
        return n.decode() if n else ""

    def channel(self, n):
        return ChannelView(self, n)

    def stage_hop_table(self, freqs_hz):
        self._check()
        arr = (c_ull * len(freqs_hz))(*[int(f) for f in freqs_hz])
        self._call(get_lib().datax_device_stage_hop_table(self._ptr, arr, len(freqs_hz)))

    def run_mcs(self):
        self._check()
        self._call(get_lib().datax_device_run_mcs(self._ptr))

    def __repr__(self):
        return f"<datax.Device {self.name!r}>" if self._valid else "<datax.Device closed>"
