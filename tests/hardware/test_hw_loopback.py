# SPDX-License-Identifier: MIT
"""REQ-TEST-030, digital part: TX -> digital loopback -> RX, no RF cabling."""
import time

import numpy as np
import pytest

import loopback as lb

PERIOD = 1024
CAPTURE = 8192  # at least 2x the period


def test_digital_loopback(dev, hw_ctx, family, ch, record):
    tx_c = lb.qpsk_block(PERIOD, seed=7)
    tx_raw = np.empty((PERIOD, 2), dtype=np.int16)
    tx_raw[:, 0] = np.rint(tx_c.real * 32768)
    tx_raw[:, 1] = np.rint(tx_c.imag * 32768)
    tx_fs = tx_raw[:, 0].astype(np.float64) + 1j * tx_raw[:, 1]

    lb.set_digital_loopback(hw_ctx, family, True)
    try:
        tx = dev.tx_stream(channels=(ch,), block_samples=PERIOD, cyclic=True)
        try:
            tx.start()
            tx.send(tx_fs, timeout_ms=10000)
            time.sleep(0.5)
            with dev.rx_stream(channels=(ch,), block_samples=CAPTURE) as rx:
                rx.recv_raw(CAPTURE, timeout_ms=10000)  # discard stale data
                raw, _ = rx.recv_raw(CAPTURE, timeout_ms=10000)
        finally:
            try:
                tx.close()
            except Exception:
                pass
    finally:
        lb.set_digital_loopback(hw_ctx, family, False)

    rx_c = raw[:, 0].astype(np.float64) + 1j * raw[:, 1]
    lag, gain, evm = lb.align_and_fit(tx_fs, rx_c)
    record("lag", lag)
    record("gain", [gain.real, gain.imag])
    record("evm_db", evm)
    assert evm <= -40.0, f"EVM {evm:.1f} dB (lag {lag}, gain {gain})"
    if family == "ad936x":
        bad = lb.mismatches_after_shift(tx_raw, raw, lag, 4)
        record("mismatches_after_shift4", bad)
        assert bad == 0, f"{bad} values differ from TX >> 4 (lag {lag})"
