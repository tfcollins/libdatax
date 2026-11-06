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
        self._check()
        v = c_ull()
        self._call(get_lib().datax_device_get_bandwidth(self._ptr, self._rx(), C.byref(v)))
        return float(v.value)

    @rx_bandwidth.setter
    def rx_bandwidth(self, hz):
        self._check()
        self._call(get_lib().datax_device_set_bandwidth(self._ptr, self._rx(), int(hz)))

    @property
    def sample_rate(self):
        self._check()
        v = c_ull()
        self._call(get_lib().datax_device_get_sample_rate(self._ptr, C.byref(v)))
        return float(v.value)

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

    def rx_stream(self, channels=(0,), block_samples=65536, block_count=4):
        """A new, unstarted RxStream (``with`` starts it). Needs numpy."""
        from .stream import RxStream
        return RxStream(self, channels, block_samples, block_count)

    def tx_stream(self, channels=(0,), block_samples=65536, block_count=4,
                  cyclic=False):
        """A new, unstarted TxStream (``with`` starts it). Needs numpy."""
        from .stream import TxStream
        return TxStream(self, channels, block_samples, block_count, cyclic)

    def rx(self, n, channels=(0,)):
        """One-shot capture: complex64 ``(len(channels), n)``."""
        n = int(n)
        if n <= 0:
            raise ValueError("n must be positive")
        with self.rx_stream(channels, block_samples=min(n, 65536)) as st:
            data, _ = st.recv(n)
        return data

    def tx(self, data, cyclic=True, channels=None):
        """Transmit ``data`` (complex ``(channels, n)`` or 1-D).

        Cyclic (default): repeats until ``tx_stop()`` or the context closes;
        the stream stays on the device. Otherwise sends once and returns after
        the samples reached the DMA."""
        from .stream import _np
        np = _np()
        self._check()
        a = np.asarray(data)
        if a.ndim not in (1, 2):
            raise ValueError(f"data must be 1-D or 2-D, got shape {a.shape}")
        if channels is None:
            channels = (0,) if a.ndim == 1 else tuple(range(a.shape[0]))
        n = a.shape[-1]
        if n == 0:
            raise ValueError("data is empty")
        self.tx_stop()
        st = self.tx_stream(channels, block_samples=min(n, 65536) if not cyclic else n,
                            block_count=4, cyclic=cyclic)
        try:
            st.start()
            st.send(a)
            if cyclic:
                self._tx_stream = st
                return
            st.stop()
        finally:
            if self.__dict__.get("_tx_stream") is not st:
                try:
                    st.close()
                except Error:
                    pass

    def tx_stop(self):
        """Stop the cyclic stream started by ``tx()``, if any."""
        st = self.__dict__.pop("_tx_stream", None)
        if st is not None:
            st.close()

    def run_mcs(self):
        self._check()
        self._call(get_lib().datax_device_run_mcs(self._ptr))

    def __repr__(self):
        return f"<datax.Device {self.name!r}>" if self._valid else "<datax.Device closed>"
