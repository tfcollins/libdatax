# SPDX-License-Identifier: MIT
import errno

import pytest

import datax


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_context_devices_and_version(emu_uri):
    with datax.Context(emu_uri) as ctx:
        assert len(ctx.devices) == 1
        assert ctx.version[0] == 0
        assert ctx.iio_context != 0


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_use_after_close_raises(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
    with pytest.raises(datax.Error):
        dev.rx_lo
    with pytest.raises(datax.Error):
        dev.rx_lo = 2.4e9
    with pytest.raises(datax.Error):
        dev.name
    with pytest.raises(datax.Error):
        ctx.devices
    ctx.close()  # idempotent


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_on_event_registers(emu_uri):
    with datax.Context(emu_uri) as ctx:
        ctx.on_event(lambda *a: None)
        assert ctx._event_cb is not None


def test_bad_uri_raises():
    with pytest.raises(datax.Error) as ei:
        datax.Context("ip:127.0.0.1:1")
    assert ei.value.errno != 0


def test_error_types():
    assert issubclass(datax.CapabilityError, datax.Error)
    assert issubclass(datax.LateError, datax.Error)
    assert datax.errors.make_error(errno.ENOTSUP).__class__ is datax.CapabilityError


def test_missing_library_names_env(monkeypatch):
    from datax import _lib
    monkeypatch.setattr(_lib, "_lib", None)
    monkeypatch.setenv("DATAX_LIBRARY", "/nonexistent/libdatax.so")
    with pytest.raises(datax.Error, match="DATAX_LIBRARY"):
        _lib.get_lib()
