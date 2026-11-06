# SPDX-License-Identifier: MIT
"""Timed control: the scheduler builder, completions and events.

``dev.scheduler`` is the device's one scheduler. ``sched.at(t)`` returns a
builder whose calls queue commands client-side (each returns the sequence
id); ``commit()`` submits the batch; ``wait`` / ``wait_all`` collect
completions.

Reporting is sparse (see the C API page): only ``report=True`` commands and
tunes can be waited for. A tune reports its hop edge itself, but with dense
hops only every Nth tune does, so ``wait`` on the others raises an
``EINVAL`` error and ``wait_all`` skips them.
"""
import ctypes as C
import enum
import errno
import threading
import time
from dataclasses import dataclass

from ._lib import CompletionC, get_lib
from .errors import DeadlockError, Error, LateError


# Set while an event callback runs (on the poller thread): destroying a
# scheduler or context there would join the thread from itself.
_in_callback = threading.local()


def in_event_callback():
    return getattr(_in_callback, "active", False)


class CompletionStatus(enum.IntEnum):
    """``enum datax_completion_status``."""
    OK = 0
    LATE = 1
    DROPPED = 2
    FLUSHED = 3


class EventType(enum.IntEnum):
    """``enum datax_event_type``."""
    LATE_COMMAND = 0
    QUEUE_OVERFLOW = 1
    STREAM_OVERFLOW = 2
    STREAM_UNDERFLOW = 3
    PPS_LOCK_CHANGE = 4


@dataclass(frozen=True)
class Completion:
    """The outcome of a reported command. ``detail`` is libdatax's text for a
    non-OK status (empty for OK). It is read from the context's last-error
    text right after the wait, which is shared by the whole context: another
    thread's failing call in between can replace it, so treat it as a hint."""
    seq: int
    status: CompletionStatus
    requested_ticks: int
    executed_ticks: int
    detail: str = ""


class _At:
    """Builder returned by ``Scheduler.at(t)``: each call queues one command
    at device time ``t`` (ticks) and returns its sequence id."""

    def __init__(self, sched, when):
        self._s = sched
        self._t = int(when)

    def gpio(self, mask, value, report=False):
        return self._s._queue(get_lib().datax_sched_queue_gpio,
                              (self._t, int(mask), int(value)), report)

    def marker(self, line, width=1, report=False):
        return self._s._queue(get_lib().datax_sched_queue_marker,
                              (self._t, int(line), int(width)), report)

    def tune(self, hz, ch=0):
        """Timed tune (needs ``dev.stage_hop_table`` first). Reports its hop
        edge itself."""
        return self._s._queue(get_lib().datax_sched_queue_tune,
                              (self._t, int(ch), int(hz)), False, tune=True)


class Scheduler:
    """The device's scheduler (``dev.scheduler``). Closing it (``close()``, or
    closing the context) completes outstanding commands ``FLUSHED``."""

    def __init__(self, device):
        device._check()
        self._ctx = device._ctx
        self._h = None
        self._building = []      # reported / tune seqs queued, not yet committed
        self._committed = []     # ... committed, not yet waited for
        h = C.c_void_p()
        rc = get_lib().datax_sched_create(device._ptr, C.byref(h))
        if rc:
            raise self._ctx._error(rc)
        self._h = h
        self._ctx._scheds.add(self)

    # -- lifetime ----------------------------------------------------------

    def _live(self):
        if self._h is None or self._ctx._closed:
            raise Error(errno.EBADF, "scheduler is closed")
        return self._h

    @property
    def closed(self):
        return self._h is None or self._ctx._closed

    def _close_for_context(self):
        """Destroy the C scheduler (joins its poller). Context.close() calls
        this before the C context goes away."""
        h, self._h = self._h, None
        self._building, self._committed = [], []
        if h is not None:
            get_lib().datax_sched_destroy(h)

    def close(self):
        """Idempotent. ``DeadlockError`` from an event callback."""
        if self._h is not None and in_event_callback():
            raise DeadlockError(errno.EDEADLK, "Scheduler.close from an event callback "
                                "(the poller thread) would join itself")
        self._close_for_context()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            # A finalizer that cyclic GC runs inside a callback leaks instead
            # of joining the poller from itself.
            if self._h is not None and not self._ctx._closed:
                if in_event_callback():
                    # cannot join the poller from itself: park it on the
                    # context, whose close() destroys it from another thread
                    self._ctx._leaked_scheds.append(self)
                else:
                    self._close_for_context()
        except Exception:
            pass

    # -- building ----------------------------------------------------------

    def at(self, when):
        """A builder for commands at device time ``when`` (ticks)."""
        self._live()
        return _At(self, when)

    def _queue(self, fn, args, report, tune=False):
        h = self._live()
        seq = fn(h, *args)
        if seq < 0:
            raise self._ctx._error(seq)
        if report:
            rc = get_lib().datax_sched_report(h, seq)
            if rc:
                raise self._ctx._error(rc)
        if report or tune:
            self._building.append(seq)
        return int(seq)

    def commit(self):
        """Submit the queued batch. Raises ``LateError`` when a command is
        already inside the guard margin or ``Error`` (EINVAL) for reported
        commands closer than the report spacing; only those two discard the
        whole uncommitted batch. Any other failure leaves it queued, so the
        commit can be retried."""
        h = self._live()
        rc = get_lib().datax_sched_commit(h)
        if rc:
            # C discards the batch only for LATE (guard margin) and EINVAL
            # (report spacing); any other failure leaves it queued, so keep
            # tracking it for the retry.
            if -rc in (errno.ETIME, errno.EINVAL):
                self._building = []
            raise self._ctx._error(rc)
        self._committed.extend(self._building)
        self._building = []

    def flush(self):
        """Cancel everything outstanding and return the ``Completion`` list of
        the reported commands it ended: those it cancelled (``FLUSHED``) and
        any that had already finished but were not yet waited for. They are
        consumed here (the C completion table keeps nothing for commands
        Python no longer tracks), so ``wait`` on their seqs then raises
        ``Error`` (EINVAL). On a failure (Tick access) nothing was flushed and
        tracking is kept."""
        h = self._live()
        lib = get_lib()
        rc = lib.datax_sched_flush(h)
        if rc:
            raise self._ctx._error(rc)
        self._building, self._committed = [], []
        done = []
        buf = (CompletionC * 32)()
        n = C.c_size_t(0)
        while True:
            rc = lib.datax_sched_get_completions(h, buf, len(buf), C.byref(n))
            if rc:
                raise self._ctx._error(rc)
            done.extend(Completion(int(c.seq), CompletionStatus(c.status),
                                   int(c.requested_ticks), int(c.executed_ticks))
                        for c in buf[:n.value])
            if n.value < len(buf):
                return done

    # -- completions -------------------------------------------------------

    def wait(self, seq, timeout_ms=-1):
        """Block for ``seq``'s completion (``timeout_ms`` < 0: forever).

        Returns a ``Completion``; ``LATE`` / ``DROPPED`` / ``FLUSHED`` are
        statuses, not exceptions. Raises ``Error`` (ETIMEDOUT) when it is not
        known yet, EINVAL for a command that was not reported, and
        ``DeadlockError`` (EDEADLK) from an event callback.
        """
        h = self._live()
        c = CompletionC()
        rc = get_lib().datax_sched_wait(h, int(seq), int(timeout_ms), C.byref(c))
        if rc:
            raise self._ctx._error(rc)
        for lst in (self._committed, self._building):
            if c.seq in lst:
                lst.remove(c.seq)
        status = CompletionStatus(c.status)
        detail = ""
        if status != CompletionStatus.OK:
            detail = self._ctx._last_error()
        return Completion(int(c.seq), status, int(c.requested_ticks),
                          int(c.executed_ticks), detail)

    def wait_all(self, timeout=None):
        """Wait (``timeout`` seconds, None: forever) for every committed
        reported command, then return their ``Completion`` list.

        Raises ``LateError`` when any completed late, ``Error`` (EIO) when one
        was dropped, ``Error`` (ECANCELED) when one was flushed; the exception's
        ``completions`` lists every completion collected. A tune that was not
        reported (dense hops) is skipped.
        """
        self._live()
        end = None if timeout is None else time.monotonic() + timeout
        done = []
        for seq in list(self._committed):
            ms = -1 if end is None else max(0, int((end - time.monotonic()) * 1000))
            try:
                done.append(self.wait(seq, ms))
            except Error as e:
                if e.errno == errno.EINVAL:
                    # Only tunes can be tracked yet unreported (dense hops);
                    # whether still pending ("not reported") or already
                    # finished and dropped ("unknown"), there is nothing to wait for.
                    self._committed.remove(seq)
                    continue
                raise
        bad = [c for c in done if c.status != CompletionStatus.OK]
        if bad:
            first = bad[0]
            text = (f"{len(bad)} of {len(done)} commands not on time: seq {first.seq} "
                    f"{first.status.name}" + (f" ({first.detail})" if first.detail else ""))
            if first.status == CompletionStatus.LATE:
                err = LateError(errno.ETIME, text)
            elif first.status == CompletionStatus.DROPPED:
                err = Error(errno.EIO, text)
            else:
                err = Error(errno.ECANCELED, text)
            err.completions = done
            raise err
        return done
