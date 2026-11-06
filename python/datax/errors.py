# SPDX-License-Identifier: MIT
"""datax exception types."""
import errno as _errno
import os


class Error(Exception):
    """A libdatax failure: ``errno`` (positive) and a human-readable message."""

    def __init__(self, errno, message=None):
        self.errno = abs(int(errno))
        self.message = message or os.strerror(self.errno)
        super().__init__(self.errno, self.message)

    def __str__(self):
        return f"[errno {self.errno}] {self.message}"


class CapabilityError(Error):
    """The device or context lacks the feature (ENOTSUP)."""


class LateError(Error):
    """Reserved: a deadline could not be met (ETIME). Not raised in P1."""


class BusyError(Error):
    """The resource is in use (EBUSY): a second stream in one direction, or a
    second send on a cyclic TX stream."""


def make_error(err, message=None):
    err = abs(int(err))
    if err in (_errno.ENOTSUP, _errno.EOPNOTSUPP):
        return CapabilityError(err, message)
    if err == _errno.EBUSY:
        return BusyError(err, message)
    if err == _errno.ETIME:
        return LateError(err, message)
    return Error(err, message)
