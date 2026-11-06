# SPDX-License-Identifier: MIT
"""Context: owns the C handle and the devices derived from it."""
import ctypes as C
import errno
import logging
import weakref

from ._lib import EVENT_CB, EventC, get_lib
from .device import Device
from .errors import DeadlockError, Error, make_error
from .sched import EventType, _in_callback, in_event_callback

_log = logging.getLogger("datax")


class Context:
    """``Context(uri)`` opens a libiio URI; ``Context()`` opens the local one.

    Closing the context (``close()`` or leaving ``with``) invalidates its
    devices; any later use raises ``datax.Error``.
    """

    def __init__(self, uri=None):
        self._closed = True
        self._ptr = None
        self._event_cb = None
        self._event_old = []   # replaced trampolines, alive until close
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
        self._streams = weakref.WeakSet()  # live RxStream/TxStream
        self._scheds = weakref.WeakSet()   # live Scheduler
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

    def _last_error(self):
        """libdatax's last-error text for this context ('' when none)."""
        if self._closed:
            return ""
        m = get_lib().datax_context_get_last_error(self._ptr)
        return m.decode(errors="replace") if m else ""

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
        """Register ``fn(type, ticks, seq, detail)`` (``type`` is an
        ``EventType``); ``None`` unregisters.

        ``fn`` runs on the scheduler's poller thread, never after
        ``close()``, and must not block: calling ``Scheduler.wait`` from it
        raises ``DeadlockError``. An exception it raises is logged on the
        ``datax`` logger and swallowed (it never reaches C). The context
        keeps the callback alive until it is replaced or closed.
        """
        self._check()
        lib = get_lib()
        if fn is None:
            rc = lib.datax_context_set_event_callback(self._ptr, EVENT_CB(), None)  # NULL fn pointer
            if rc:
                raise self._error(rc)
            self._retire_event_cb()
            return

        def tramp(evt, _user):
            _in_callback.active = True
            try:
                e = C.cast(evt, C.POINTER(EventC)).contents
                try:
                    typ = EventType(e.type)
                except ValueError:
                    typ = e.type
                fn(typ, e.ticks, e.seq, e.detail.decode(errors="replace") if e.detail else "")
            except BaseException:
                _log.exception("datax event callback raised")
            finally:
                _in_callback.active = False

        cb = EVENT_CB(tramp)
        rc = lib.datax_context_set_event_callback(self._ptr, cb, None)
        if rc:
            raise self._error(rc)
        self._retire_event_cb()
        self._event_cb = cb  # keep alive

    def _retire_event_cb(self):
        # The poller may still be inside the old trampoline when the C side
        # switches callbacks: keep it until close.
        if self._event_cb is not None:
            self._event_old.append(self._event_cb)
        self._event_cb = None

    def close(self):
        if self._closed:
            return
        if in_event_callback():
            raise DeadlockError(errno.EDEADLK, "Context.close from an event callback "
                                "(the poller thread) would join itself")
        # Schedulers first: destroying one joins its poller, so no event
        # callback is running or can start afterwards. Then the streams: C
        # handles die with the C context, so none is touched afterwards.
        for sc in list(self._scheds):
            sc._close_for_context()
        self._scheds.clear()
        for st in list(self._streams):
            st._close_for_context()
        self._streams.clear()
        for d in self._devices:
            d._invalidate()
        # Unregister the callback before the context goes; the trampolines
        # (kept in `keep`) outlive the C context.
        get_lib().datax_context_set_event_callback(self._ptr, EVENT_CB(), None)
        keep = [self._event_cb, *self._event_old]
        self._closed = True
        get_lib().datax_context_destroy(self._ptr)
        self._ptr = None
        self._event_cb = None
        self._event_old = []
        del keep

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            if not in_event_callback():   # leak rather than join the poller from itself
                self.close()
        except Exception:
            pass
