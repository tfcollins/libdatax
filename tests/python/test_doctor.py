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
    # Tick has the GPIO target, but this fixture lacks the FHM and FSM attributes.
    assert "capabilities: timed_commands\n" in r.stdout
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


@pytest.mark.parametrize("emu_uri", ["adrv9009+tick-fsm"], indirect=True)
def test_doctor_p3a_lines(emu_uri):
    r = _run([emu_uri])
    assert r.returncode == 0, r.stderr
    out = r.stdout
    assert "capabilities: fhm_hop, mcs, timed_commands" in out
    assert "time: free-run (no pps)" in out
    assert ("scheduler: report spacing not measured, hop stage not measured "
            "(not measured yet); exclusive use of the board's scheduler") in out
    assert ("mcs: opt_post_running_stage; phase-coherent: no "
            "(MCS not verified; call run_mcs)") in out


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_doctor_p3a_lines_without_tick(emu_uri):
    r = _run([emu_uri])
    assert r.returncode == 0, r.stderr
    assert "time: none (no Tick scheduler)" in r.stdout
    assert "scheduler: none" in r.stdout
    assert "mcs: not supported; phase-coherent: no" in r.stdout
