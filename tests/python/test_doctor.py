# SPDX-License-Identifier: MIT
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def _run(args):
    env = dict(os.environ, PYTHONPATH=str(ROOT / "python"))
    return subprocess.run([sys.executable, "-m", "datax.doctor", *args],
                          capture_output=True, text=True, env=env, timeout=60)


@pytest.mark.parametrize("emu_uri", ["adrv9009+tick"], indirect=True)
def test_doctor_output(emu_uri):
    r = _run([emu_uri])
    assert r.returncode == 0, r.stderr
    assert "libdatax version" in r.stdout
    assert "capabilities: none" in r.stdout
    assert "detected hardware:" in r.stdout and "fhm_hop" in r.stdout
    assert "tick_hz=100000000" in r.stdout


def test_doctor_failure_exit_1():
    r = _run(["ip:127.0.0.1:1"])
    assert r.returncode == 1
    assert r.stderr.strip()


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_doctor_streams_line(emu_uri):
    r = _run([emu_uri])
    assert r.returncode == 0, r.stderr
    # direct_reg_access is present on both DMA devices but no stream has
    # been opened to probe it.
    assert ("streams: rx=cf-ad9361-lpc tx=cf-ad9361-dds-core-lpc "
            "overflow-detect=unknown") in r.stdout


@pytest.mark.parametrize("emu_uri", ["adrv9009-single"], indirect=True)
def test_doctor_streams_line_none(emu_uri):
    r = _run([emu_uri])
    assert r.returncode == 0, r.stderr
    assert "streams: rx=none tx=none overflow-detect=no" in r.stdout
