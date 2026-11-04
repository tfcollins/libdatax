# SPDX-License-Identifier: MIT
import pytest

import datax


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_ad936x_caps_empty_in_p1(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        assert "timed_commands" not in dev.caps
        assert "bogus" not in dev.caps
        assert dev.caps.detected == frozenset()
        lim = dev.caps.limits
        assert set(lim) == set(datax.LIMIT_NAMES)
        assert lim["max_channels"] >= 1


@pytest.mark.parametrize("emu_uri", ["adrv9009+tick"], indirect=True)
def test_adrv9009_detected_not_public(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        assert "fhm_hop" not in dev.caps
        assert "fhm_hop" in dev.caps.detected
        assert dev.time.tick_hz == 100_000_000
        assert dev.caps.limits["tick_hz"] == 100_000_000


@pytest.mark.parametrize("emu_uri", ["adrv9009-zu11eg"], indirect=True)
def test_zu11eg_mcs_detected(emu_uri):
    with datax.Context(emu_uri) as ctx:
        assert "mcs" in ctx.devices[0].caps.detected
        assert "mcs" not in ctx.devices[0].caps
