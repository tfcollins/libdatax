# SPDX-License-Identifier: MIT
"""Python streams against iiod-emu.

RX data: a temporary copy of the fixture with the counting pattern staged
beside it (the approach of tests/emu/emu_pattern.sh): instant k has
I = k mod 32768, Q = -I (plus 1000 * c on channel c with two channels).
"""
import gc
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

import datax
from conftest import EMU_DIR, _emu_available

import numpy as np

RX_DEV = "cf-ad9361-lpc"
TX_FILE = "cf-ad9361-dds-core-lpc_buf0.bin"


@pytest.fixture
def staged(request):
    """(uri, dir): iiod-emu on a temp copy of ad936x.xml with the RX pattern.

    Parametrize indirectly with the channel count (default 1)."""
    nch = getattr(request, "param", 1)
    if not _emu_available():
        pytest.skip("iiod-emu not found: set DATAX_LIBIIO_BUILD to a libiio build tree")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    from make_pattern import write
    with tempfile.TemporaryDirectory() as tmp:
        xml = Path(tmp) / "ad936x.xml"
        shutil.copy(EMU_DIR / "ad936x.xml", xml)
        write(xml, RX_DEV, nch, 32768, channel_offset=1000 if nch > 1 else 0)
        with Emulator(xml) as uri:
            yield uri, Path(tmp)


@pytest.fixture
def ctx(staged):
    c = datax.Context(staged[0])
    yield c
    c.close()


def expect(n, k0=0, nch=1):
    k = (np.arange(n) + k0) % 32768
    return np.stack([(k + 1000 * c) % 32768 - 1j * ((k + 1000 * c) % 32768)
                     for c in range(nch)]).astype(np.complex64)


def test_rx_stream_pattern(ctx):
    dev = ctx.devices[0]
    with dev.rx_stream(block_samples=1024, block_count=4) as rx:
        a, info = rx.recv(1024)
        b, info2 = rx.recv(2500)
    assert a.dtype == np.complex64 and a.shape == (1, 1024)
    assert np.array_equal(a, expect(1024))
    assert np.array_equal(b, expect(2500, 1024))
    assert info.timestamp_ticks == 0 and info.valid_samples > 0
    assert info.discontinuous is False


@pytest.mark.parametrize("staged", [2], indirect=True)
def test_rx_two_channels_shape(ctx):
    with ctx.devices[0].rx_stream(channels=(1, 0), block_samples=512) as rx:
        assert rx.channels == (0, 1)
        a, _ = rx.recv(700)
    assert a.shape == (2, 700)
    assert np.array_equal(a, expect(700, 0, 2))


@pytest.mark.parametrize("staged", [2], indirect=True)
def test_recv_raw_layout(ctx):
    with ctx.devices[0].rx_stream(channels=(0, 1), block_samples=256) as rx:
        raw, info = rx.recv_raw(256)
    assert raw.dtype == np.int16 and raw.shape == (256, 4)
    k = np.arange(256)
    assert np.array_equal(raw[:, 0], k) and np.array_equal(raw[:, 1], -k)
    assert np.array_equal(raw[:, 2], k + 1000) and np.array_equal(raw[:, 3], -(k + 1000))


def test_tx_oneshot_roundtrip_file(ctx, staged):
    k = np.arange(2048)
    data = (k - 1j * k).astype(np.complex64)
    dev = ctx.devices[0]
    with dev.tx_stream(block_samples=1024, block_count=2) as tx:
        assert tx.send(data) == 2048
    # stop() drained both blocks; the file holds the last block sent.
    raw = np.fromfile(staged[1] / TX_FILE, dtype=np.int16).reshape(-1, 2)
    assert raw.shape[0] == 1024
    assert np.array_equal(raw[:, 0], k[1024:]) and np.array_equal(raw[:, 1], -k[1024:])


def test_tx_cyclic(ctx, staged):
    dev = ctx.devices[0]
    data = np.arange(300) * (1 - 1j)
    with dev.tx_stream(block_samples=1024, cyclic=True) as tx:
        assert not (staged[1] / TX_FILE).exists()  # not enabled at start
        assert tx.send(data) == 300
        assert (staged[1] / TX_FILE).exists()      # enabled at the single send
        with pytest.raises(datax.BusyError) as e:
            tx.send(data)
        assert isinstance(e.value, datax.Error)
        with pytest.raises(ValueError):
            tx.send(np.zeros(1025, dtype=np.complex64))
    # The emulator never dequeues a cyclic block, so the TX file stays empty
    # and the repeating waveform cannot be checked here; hardware covers it:
    # tests/hardware/test_hw_loopback.py::test_digital_loopback


def test_second_stream_same_direction_is_busy(ctx):
    dev = ctx.devices[0]
    with dev.rx_stream(block_samples=256):
        with pytest.raises(datax.BusyError):
            dev.rx_stream(block_samples=256)
    dev.rx_stream(block_samples=256).close()  # free again


def test_send_out_of_range(ctx):
    with ctx.devices[0].tx_stream(block_samples=256) as tx:
        with pytest.raises(ValueError):
            tx.send(np.array([40000 + 0j]))
        with pytest.raises(ValueError):
            tx.send(np.array([1 + 32768j]))
        with pytest.raises(ValueError):
            tx.send(np.array([np.nan + 0j]))
        assert tx.send(np.array([32767.4 - 32768j])) == 1  # rounds into range


def test_send_bad_shape(ctx):
    with ctx.devices[0].tx_stream(block_samples=256) as tx:
        with pytest.raises(ValueError):
            tx.send(np.zeros((2, 8), dtype=np.complex64))
        with pytest.raises(ValueError):
            tx.send(np.zeros((1, 2, 3), dtype=np.complex64))
        with pytest.raises(ValueError):
            tx.send(np.zeros(0, dtype=np.complex64))


def test_use_after_close(ctx):
    dev = ctx.devices[0]
    with dev.rx_stream(block_samples=256) as rx:
        rx.recv(256)
    with pytest.raises(datax.Error):
        rx.recv(256)
    with pytest.raises(datax.Error):
        rx.start()
    rx.close()  # idempotent
    with dev.tx_stream(block_samples=256) as tx:
        pass
    with pytest.raises(datax.Error):
        tx.send(np.zeros(4, dtype=np.complex64))


def test_rx_convenience(ctx):
    a = ctx.devices[0].rx(5000)
    assert a.shape == (1, 5000) and a.dtype == np.complex64
    assert np.array_equal(a, expect(5000))
    ctx.devices[0].rx(10)  # the stream was released


def test_tx_convenience_and_stop(ctx, staged):
    dev = ctx.devices[0]
    dev.tx(np.arange(100) * (1 + 1j))
    assert (staged[1] / TX_FILE).exists()
    with pytest.raises(datax.BusyError):
        dev.tx_stream()
    dev.tx(np.arange(50) * (1 + 1j))  # replaces the running one
    dev.tx_stop()
    dev.tx_stop()  # idempotent
    dev.tx_stream(block_samples=64).close()  # direction free again
    dev.tx(np.arange(100, dtype=np.float32), cyclic=False)  # one-shot, returns
    dev.tx_stream(block_samples=64).close()


def test_context_close_destroys_streams(staged):
    c = datax.Context(staged[0])
    dev = c.devices[0]
    rx = dev.rx_stream(block_samples=256)
    rx.start()
    tx = dev.tx_stream(block_samples=256)
    tx.start()
    c.close()
    assert rx.closed and tx.closed


def test_context_close_with_open_streams(staged):
    c = datax.Context(staged[0])
    dev = c.devices[0]
    rx = dev.rx_stream(block_samples=256)
    rx.start()
    rx.recv(256)
    c.close()
    assert rx.closed
    with pytest.raises(datax.Error):
        rx.recv(256)
    with pytest.raises(datax.Error):
        rx.stop()
    rx.close()
    del rx
    gc.collect()


def test_context_close_with_cyclic_convenience(staged):
    c = datax.Context(staged[0])
    dev = c.devices[0]
    dev.tx(np.arange(64) * (1 + 1j))
    st = dev._tx_stream
    c.close()
    assert st.closed
    with pytest.raises(datax.Error):
        st.send(np.zeros(4, dtype=np.complex64))
    dev.tx_stop()  # after close: no-op, no touch of freed memory


def test_gc_order_stream_outlives_context(staged):
    c = datax.Context(staged[0])
    rx = c.devices[0].rx_stream(block_samples=256)
    rx.start()
    del c
    gc.collect()
    # The stream keeps its context alive, so it still works ...
    assert rx.recv(256)[0].shape == (1, 256)
    ctx = rx._ctx
    ctx.close()
    del ctx
    # ... and dropping it after the context closed must not touch freed memory.
    del rx
    gc.collect()


def test_gc_order_context_collected_first(staged):
    c = datax.Context(staged[0])
    rx = c.devices[0].rx_stream(block_samples=256)
    rx.start()
    dev_tx = c.devices[0]
    dev_tx.tx(np.arange(8) * (1 + 0j))
    del c, dev_tx, rx
    gc.collect()


def test_no_numpy_message(ctx, monkeypatch):
    monkeypatch.setitem(sys.modules, "numpy", None)
    with pytest.raises(datax.CapabilityError, match=r"streams need numpy: pip install libdatax\[stream\]"):
        ctx.devices[0].rx_stream()
    with pytest.raises(datax.CapabilityError, match="streams need numpy"):
        ctx.devices[0].tx_stream()
