# SPDX-License-Identifier: MIT
import pytest

import datax


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_rx_tx_lo_gain(emu_uri):
    with datax.Context(emu_uri) as ctx:
        dev = ctx.devices[0]
        assert dev.name
        dev.rx_lo = 2.4e9
        assert dev.rx_lo == 2.4e9
        dev.tx_lo = 2.45e9
        assert dev.tx_lo == 2.45e9
        dev.rx_gain = 20
        assert dev.rx_gain == 20
        dev.sample_rate = 30.72e6
        assert dev.sample_rate == 30.72e6
        dev.rx_bandwidth = 18e6
        assert dev.rx_bandwidth == 18e6
        ch1 = dev.channel(1)
        ch1.rx_gain = 15
        assert ch1.rx_gain == 15
        ch1.rx_bandwidth = 12e6
        assert ch1.rx_bandwidth == 12e6
        assert dev.rx_bandwidth == 18e6


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_no_tick_time_raises_capability(emu_uri):
    with datax.Context(emu_uri) as ctx:
        with pytest.raises(datax.CapabilityError):
            ctx.devices[0].time.tick_hz
        with pytest.raises(datax.CapabilityError):
            ctx.devices[0].time.now


@pytest.mark.parametrize("emu_uri", ["ad936x+tick"], indirect=True)
def test_time_with_tick(emu_uri):
    with datax.Context(emu_uri) as ctx:
        t = ctx.devices[0].time
        assert t.tick_hz == 100_000_000
        assert t.seconds(0.5) == 50_000_000
        assert isinstance(t.now, int)


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_out_of_range_channel_error(emu_uri):
    with datax.Context(emu_uri) as ctx:
        with pytest.raises(datax.Error) as ei:
            ctx.devices[0].channel(99).rx_gain = 1
        assert "channel" in ei.value.message
