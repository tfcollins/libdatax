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

    try:
        lb.set_digital_loopback(hw_ctx, family, True)
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
        # Restore even after a partial enable; never mask the original error.
        try:
            lb.set_digital_loopback(hw_ctx, family, False)
        except Exception as e:
            record("loopback_restore_failed", str(e))
            restore_err = e
        else:
            restore_err = None

    assert restore_err is None, f"loopback restore failed: {restore_err}"
    # Expected waveform per family. adrv9009: I/Q exchanged by the stock framer
    # loopback path (tron, 2026-11-05: rx_I == tx_Q, rx_Q == tx_I, gain 1.000).
    # libdatax's convention matches pyadi-iio, so this is not a libdatax bug; an
    # RF loopback (REQ-TEST-030 RF part) is what would confirm TX I/Q orientation.
    exp_raw = lb.expected_loopback(family, tx_raw)
    record("loopback_transform",
           "tx>>4" if family == "ad936x" else "iq_exchange_no_shift")
    exp_c = exp_raw[:, 0].astype(np.float64) + 1j * exp_raw[:, 1]
    rx_c = raw[:, 0].astype(np.float64) + 1j * raw[:, 1]
    lag, gain, evm = lb.align_and_fit(exp_c, rx_c)
    record("lag", lag)
    record("gain", [gain.real, gain.imag])
    record("evm_db", evm)
    assert evm <= -40.0, f"EVM {evm:.1f} dB (lag {lag}, gain {gain})"
    bad = lb.mismatches_vs_expected(exp_raw, raw, lag)
    record("mismatches_vs_expected", bad)
    assert bad == 0, f"{bad} values differ from the expected loopback (lag {lag})"
