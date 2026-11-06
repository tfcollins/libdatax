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
        # the two measured limits appear only once measured
        assert set(lim) == set(datax.LIMIT_NAMES) - {"report_spacing_us", "hop_stage_us"}
        assert lim["max_channels"] >= 1


@pytest.mark.parametrize("emu_uri", ["adrv9009+tick"], indirect=True)
def test_adrv9009_fhm_needs_phy_attributes(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        # P3a: Tick is usable (public), but this fixture has no FHM attributes
        assert "timed_commands" in dev.caps
        assert "fhm_hop" not in dev.caps
        assert "fhm_hop" in dev.caps.detected
        assert dev.time.tick_hz == 100_000_000
        assert dev.caps.limits["tick_hz"] == 100_000_000


@pytest.mark.parametrize("emu_uri", ["adrv9009-zu11eg"], indirect=True)
def test_zu11eg_mcs_detected(emu_uri):
    with datax.Context(emu_uri) as ctx:
        assert "mcs" in ctx.devices[0].caps.detected
        assert "mcs" not in ctx.devices[0].caps


@pytest.mark.parametrize("emu_uri", ["adrv9009+tick-fsm"], indirect=True)
def test_p3a_gates_full_board(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        assert set(dev.caps) == {"timed_commands", "fhm_hop", "mcs"}
        assert "phase_coherent" not in dev.caps
        dev.run_mcs()
        assert "phase_coherent" not in dev.caps   # no RF PLL phase sync
