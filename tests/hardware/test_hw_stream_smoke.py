# SPDX-License-Identifier: MIT
"""REQ-TEST-021, streaming part: immediate-stream smoke on a real board.

Run with DATAX_HW_URI=<uri> (see README.md). Needs numpy.
"""
import ctypes as C
import errno
import time

import numpy as np
import pytest

import datax
import loopback as lb
from loopback import stream_stats

RX_BLOCK = 4096


def _channels(dev):
    return list(range(int(dev.caps.limits["max_channels"])))


def _cw(n, amp=2000):
    """A small complex tone, safe for any TX path."""
    k = np.arange(n)
    return amp * np.exp(2j * np.pi * k * 16 / n)


def test_family_and_channels(dev, family, record):
    n = len(_channels(dev))
    record("device", dev.name)
    record("family", family)
    record("channels", n)
    assert family in ("ad936x", "adrv9009")
    assert n >= 1
    assert dev.sample_rate > 0


def test_caps_row(dev):
    # P2 changes no capability flag: the public set is empty.
    assert list(dev.caps) == []


def test_rx_block_nonconstant_per_channel(dev, record):
    stds = {}
    for ch in _channels(dev):
        with dev.rx_stream(channels=(ch,), block_samples=RX_BLOCK) as rx:
            data, info = rx.recv(RX_BLOCK, timeout_ms=10000)
        assert data.shape == (1, RX_BLOCK)
        stds[ch] = float(np.std(data.real) + np.std(data.imag))
    record("std_by_channel", stds)
    flat = [c for c, s in stds.items() if not s > 0]
    assert not flat, f"constant RX data on channels {flat}"


def test_recv_timeout_recovers(dev):
    """A 1 ms recv right after start (before a block can arrive) must not
    wedge the stream: a later recv with a normal timeout succeeds."""
    with dev.rx_stream(channels=(0,), block_samples=65536) as rx:
        try:
            rx.recv(65536, timeout_ms=1)
        except datax.Error as e:
            assert e.errno == errno.ETIMEDOUT, e
        data, _ = rx.recv(65536, timeout_ms=10000)
        assert data.shape == (1, 65536)
        data, _ = rx.recv(RX_BLOCK, timeout_ms=10000)
        assert data.shape == (1, RX_BLOCK)


def test_tx_oneshot_drains(dev, record):
    n = 16384
    with dev.tx_stream(channels=(0,), block_samples=n, block_count=4) as tx:
        sent = tx.send(_cw(n), timeout_ms=10000)
        assert sent == n
        t0 = time.monotonic()
        tx.stop()  # drains; raises on a drain timeout
        record("drain_s", time.monotonic() - t0)


def test_tx_cyclic_start_stop(dev):
    for _ in range(2):
        tx = dev.tx_stream(channels=(0,), block_samples=1024, cyclic=True)
        tx.start()
        assert tx.send(_cw(1024), timeout_ms=10000) == 1024
        time.sleep(0.3)
        tx.stop()
        tx.close()


def test_start_stop_cycles(dev):
    for i in range(5):
        rx = dev.rx_stream(channels=(0,), block_samples=RX_BLOCK)
        rx.start()
        data, _ = rx.recv(RX_BLOCK, timeout_ms=10000)
        assert data.shape == (1, RX_BLOCK), f"cycle {i}"
        rx.stop()
        rx.close()


def test_rx_throughput_recorded(dev, record):
    """10 s capture: MB/s and overflow counts go to the evidence (not asserted).

    Where the status register is readable (direct_reg_access on the RX
    streaming device) the poll must actually have run: status_polls > 0 and
    overflow_detect true. Over GigE overflows are normal and not a failure.
    """
    lib = datax._lib.get_lib()
    name = C.create_string_buffer(64)
    detect = C.c_int()
    rc = lib.datax_internal_device_stream_info(dev._ptr, 0, name, len(name),
                                               C.byref(detect))
    assert rc == 0, "no RX streaming device"
    rxdev = name.value.decode()
    has_reg = lb.has_debug_attr(dev._ctx, rxdev, "direct_reg_access")

    nch, block = 1, 65536
    total = 0
    with dev.rx_stream(channels=(0,), block_samples=block) as rx:
        t0 = time.monotonic()
        while time.monotonic() - t0 < 10.0:
            raw, _ = rx.recv_raw(block, timeout_ms=10000)
            total += raw.shape[0]
        elapsed = time.monotonic() - t0
        rx.stop()
        st = stream_stats(rx)
    mbps = total * 4 * nch / elapsed / 1e6
    record("rx_device", rxdev)
    record("direct_reg_access", has_reg)
    record("mb_per_s", mbps)
    record("samples", total)
    record("seconds", elapsed)
    record("stats", st)
    assert total > 0
    if has_reg:
        assert st["status_polls"] > 0, f"status register never polled: {st}"
        assert st["overflow_detect"], f"overflow detection off despite direct_reg_access: {st}"
