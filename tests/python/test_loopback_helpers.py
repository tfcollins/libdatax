# SPDX-License-Identifier: MIT
"""Tier 1 tests for the pure helpers of the hardware suite (tests/hardware/loopback.py)."""
import sys

import numpy as np
import pytest

import datax
from conftest import ROOT

sys.path.insert(0, str(ROOT / "tests" / "hardware"))
import loopback as lb  # noqa: E402


def test_qpsk_block_shape_and_levels():
    x = lb.qpsk_block(1024, seed=3)
    assert x.shape == (1024,) and np.iscomplexobj(x)
    assert set(np.unique(x.real)) == {-0.5, 0.5}
    assert set(np.unique(x.imag)) == {-0.5, 0.5}
    assert np.array_equal(x, lb.qpsk_block(1024, seed=3))
    assert not np.array_equal(x, lb.qpsk_block(1024, seed=4))


@pytest.mark.parametrize("lag", [0, 1, 17, 1023])
def test_align_and_fit_recovers_lag_and_gain(lag):
    tx = lb.qpsk_block(1024, seed=1)
    gain = 0.0625 * np.exp(1j * 0.3)
    rx = gain * np.roll(tx, lag)
    got_lag, got_gain, evm = lb.align_and_fit(tx, rx)
    assert got_lag == lag
    assert got_gain == pytest.approx(gain, abs=1e-9)
    assert evm <= -200  # exact up to float rounding


def test_align_and_fit_uses_first_period_of_a_longer_capture():
    tx = lb.qpsk_block(256, seed=2)
    rx = np.tile(np.roll(tx, 5), 4)
    lag, gain, evm = lb.align_and_fit(tx, rx)
    assert lag == 5 and gain == pytest.approx(1.0) and evm <= -200


def test_align_and_fit_noise_gives_expected_evm():
    rng = np.random.default_rng(0)
    tx = lb.qpsk_block(4096, seed=5)
    sig = 0.5 * np.roll(tx, 100)
    # complex noise at -30 dB relative to the signal power
    p = np.mean(np.abs(sig) ** 2)
    sigma = np.sqrt(p * 10 ** (-30 / 10) / 2)
    rx = sig + sigma * (rng.standard_normal(4096) + 1j * rng.standard_normal(4096))
    lag, gain, evm = lb.align_and_fit(tx, rx)
    assert lag == 100
    assert abs(gain - 0.5) < 0.01
    assert evm == pytest.approx(-30, abs=0.5)


def test_align_and_fit_unrelated_signal_has_poor_evm():
    tx = lb.qpsk_block(1024, seed=1)
    rx = lb.qpsk_block(1024, seed=99)
    _, _, evm = lb.align_and_fit(tx, rx)
    assert evm > -10


def test_align_and_fit_rejects_short_rx_and_zero_tx():
    tx = lb.qpsk_block(64, seed=1)
    with pytest.raises(ValueError):
        lb.align_and_fit(tx, tx[:32])
    with pytest.raises(ValueError):
        lb.align_and_fit(np.zeros(64, complex), tx)


def test_mismatches_after_shift():
    tx = np.array([[16, -32], [48, 64], [-17, 5]], dtype=np.int16)
    rx = np.roll(tx, 1, axis=0) >> 4
    assert lb.mismatches_after_shift(tx, rx, lag=1, shift=4) == 0
    rx[0, 0] += 1
    assert lb.mismatches_after_shift(tx, rx, lag=1, shift=4) == 1


@pytest.mark.parametrize("uri,host", [
    ("ip:192.168.7.2", "192.168.7.2"),
    ("ip:board.example.org:30431", "board.example.org"),
    ("ip:[fe80::1]", "[fe80::1]"),
    ("local:", None),
])
def test_redact_uri_hides_host(uri, host):
    out = lb.redact_uri(uri)
    if host:
        assert host not in out
        assert out.startswith("ip:host")
        assert "-" not in out.split(":")[1]  # no hash of the host
    else:
        assert out == uri
    if uri.endswith(":30431"):
        assert out.endswith(":30431")


def test_redact_text_hides_addresses_and_hostname():
    d = "192.168.10.55 Linux analog 4.19.0-g17f4223 #1848 SMP armv7l 10.0.0.1"
    out = lb.redact_text(d)
    assert "192.168" not in out and "10.0.0.1" not in out and " analog " not in out
    assert "4.19.0-g17f4223" in out and "Linux host 4.19" in out


@pytest.mark.parametrize("name,family", [
    ("ad9361-phy", "ad936x"), ("ad9363-phy", "ad936x"), ("ad9364-phy", "ad936x"),
    ("adrv9009", "adrv9009"), ("adrv9009-phy", "adrv9009"),
])
def test_family_of(name, family):
    assert lb.family_of(name) == family


def test_family_of_unknown_raises():
    with pytest.raises(ValueError, match="no digital loopback"):
        lb.family_of("ad9081")


# ---- set_digital_loopback against the emulator fixtures ----------------------

@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_set_digital_loopback_ad936x(emu_uri):
    with datax.Context(emu_uri) as ctx:
        lb.set_digital_loopback(ctx, "ad936x", True)
        assert lb.read_debug_attr(ctx, "ad9361-phy", "loopback") == "1"
        lb.set_digital_loopback(ctx, "ad936x", False)
        assert lb.read_debug_attr(ctx, "ad9361-phy", "loopback") == "0"


@pytest.mark.parametrize("emu_uri", ["adrv9009-single"], indirect=True)
def test_set_digital_loopback_missing_attribute_names_it(emu_uri):
    # The fixture has no BIST attributes: the error must name the attribute.
    with datax.Context(emu_uri) as ctx:
        with pytest.raises(lb.LoopbackError, match="bist_framer_a_loopback"):
            lb.set_digital_loopback(ctx, "adrv9009", True)


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_set_digital_loopback_missing_device_names_it(emu_uri):
    with datax.Context(emu_uri) as ctx:
        with pytest.raises(lb.LoopbackError, match="adrv9009-phy"):
            lb.set_digital_loopback(ctx, "adrv9009", True)
