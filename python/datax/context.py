# SPDX-License-Identifier: MIT
"""Context: owns the C handle and the devices derived from it."""
import ctypes as C
import errno

from ._lib import EVENT_CB, get_lib
from .device import Device
from .errors import Error, make_error


class Context:
    """``Context(uri)`` opens a libiio URI; ``Context()`` opens the local one.

    Closing the context (``close()`` or leaving ``with``) invalidates its
    devices; any later use raises ``datax.Error``.
    """

    def __init__(self, uri=None):
        self._closed = True
        self._ptr = None
        self._event_cb = None
        lib = get_lib()
        h = C.c_void_p()
        if uri is None:
            rc = lib.datax_context_create_local(C.byref(h))
        else:
            rc = lib.datax_context_create_from_uri(uri.encode(), C.byref(h))
        if rc:
            raise make_error(rc, f"cannot open context {uri!r}" if uri else "cannot open local context")
        self._ptr = h
        self._closed = False
        self._devices = []
        for i in range(lib.datax_context_get_device_count(self._ptr)):
            d = C.c_void_p()
            rc = lib.datax_context_get_device(self._ptr, i, C.byref(d))
            if rc:
                err = self._error(rc)
                self.close()
                raise err
            self._devices.append(Device(self, d))

    def _error(self, rc, message=None):
        """Build an exception, preferring libdatax's last-error text."""
        if message is None and not self._closed:
            m = get_lib().datax_context_get_last_error(self._ptr)
            message = m.decode(errors="replace") if m else None
        return make_error(rc if rc > 0 else -rc, message)

    def _check(self):
        if self._closed:
            raise Error(errno.EBADF, "context is closed")

    @property
    def devices(self):
        self._check()
        return list(self._devices)

    @property
    def iio_context(self):
        """The underlying ``struct iio_context *`` as an int."""
        self._check()
        return get_lib().datax_context_get_iio_context(self._ptr) or 0

    @property
    def version(self):
        self._check()
        a, b = C.c_uint(), C.c_uint()
        rc = get_lib().datax_context_get_version(self._ptr, C.byref(a), C.byref(b))
        if rc:
            raise self._error(rc)
        return (a.value, b.value)

    def on_event(self, fn):
        """Register ``fn(type, ticks, seq, detail)``. P1 never fires it.

        The callback object is kept alive for the lifetime of the context.
        """
        self._check()

        class _Evt(C.Structure):
            _fields_ = [("type", C.c_int), ("ticks", C.c_ulonglong),
                        ("seq", C.c_long), ("detail", C.c_char_p)]

        def tramp(evt, _user):
            e = C.cast(evt, C.POINTER(_Evt)).contents
            fn(e.type, e.ticks, e.seq, e.detail.decode() if e.detail else "")

        cb = EVENT_CB(tramp)
        rc = get_lib().datax_context_set_event_callback(self._ptr, cb, None)
        if rc:
            raise self._error(rc)
        self._event_cb = cb  # keep alive

    def close(self):
        if self._closed:
            return
        for d in self._devices:
            d._invalidate()
        self._closed = True
        get_lib().datax_context_destroy(self._ptr)
        self._ptr = None
        self._event_cb = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
