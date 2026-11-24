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
            "(RF PLL phase sync not enabled)") in out


@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_doctor_p3a_lines_without_tick(emu_uri):
    r = _run([emu_uri])
    assert r.returncode == 0, r.stderr
    assert "time: none (no Tick scheduler)" in r.stdout
    assert "scheduler: none" in r.stdout
    assert "mcs: not supported; phase-coherent: no" in r.stdout


def test_doctor_mcs_failing_fsm(tmp_path):
    """A phy stuck mid-FSM: doctor runs run_mcs and shows why it failed."""
    import conftest  # noqa: F401  (EMU_DIR on sys.path below)
    from conftest import EMU_DIR, _emu_available
    if not _emu_available():
        pytest.skip("iiod-emu not found")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    xml = (EMU_DIR / "adrv9009+tick-fsm.xml").read_text()
    bad = tmp_path / "bad-fsm.xml"
    head, sep, tail = xml.partition('name="adrv9009-phy-b"')
    assert sep
    bad.write_text(head + sep + tail.replace(
        'value="opt_post_running_stage"', 'value="opt_pre_clock_stage"', 1))
    with Emulator(bad) as uri:
        r = _run([uri])
    assert r.returncode == 0, r.stderr
    line = next(ln for ln in r.stdout.splitlines() if "mcs:" in ln)
    assert "opt_post_running_stage / opt_pre_clock_stage" in line
    assert "phase-coherent: no" in line
    assert "adrv9009-phy-b" in line and "opt_pre_clock_stage" in line
    assert "not verified" not in line


def _p3b_xml(tmp_path, bitmap="0x000000df", gate_map="rx-dma,tx-dma,rx-mode,tx-mode"):
    from conftest import EMU_DIR
    xml = (EMU_DIR / "adrv9009+tick-fsm.xml").read_text()
    xml = xml.replace('name="capability_bitmap" value="0x0000001f"',
                      f'name="capability_bitmap" value="{bitmap}"')
    xml = xml.replace('name="capture_tag_header_bytes" value="0"',
                      'name="capture_tag_header_bytes" value="32"')
    if gate_map is not None:
        xml = xml.replace('<attribute name="capture_tag_enable"',
                          f'<attribute name="stream_gate_map" value="{gate_map}" />'
                          '<attribute name="capture_tag_enable"')
    out = tmp_path / "p3b.xml"
    out.write_text(xml)
    return out


def _doctor_on(xml):
    from conftest import EMU_DIR, _emu_available
    if not _emu_available():
        pytest.skip("iiod-emu not found")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    with Emulator(xml) as uri:
        return _run([uri])


def test_doctor_streams_line_p3b(tmp_path):
    r = _doctor_on(_p3b_xml(tmp_path))
    assert r.returncode == 0, r.stderr
    line = next(ln for ln in r.stdout.splitlines() if "streams:" in ln)
    assert ("gate-map rx-dma=0 tx-dma=1 rx-mode=2 tx-mode=3 fhm-b=no "
            "tx-start-fullmask-only=no") in line
    for flag in ("rx_arm", "tx_arm", "stream_gate", "timestamps"):
        assert f"{flag}=yes" in line
    assert "tag_bytes=32" in line
    caps = next(ln for ln in r.stdout.splitlines() if "capabilities:" in ln)
    assert "rx_arm" in caps and "tx_arm" in caps and "timestamps" in caps


def test_doctor_streams_line_why_not(tmp_path):
    r = _doctor_on(_p3b_xml(tmp_path, bitmap="0x0000009f", gate_map="rx-dma,tx-dma"))
    assert r.returncode == 0, r.stderr
    line = next(ln for ln in r.stdout.splitlines() if "streams:" in ln)
    assert "gate-map rx-dma=0 tx-dma=1 rx-mode=-1 tx-mode=-1" in line
    assert "rx_arm=no (gate map lacks rx-dma or rx-mode)" in line
    assert "stream_gate=no (neither rx_arm nor tx_arm)" in line
    assert "timestamps=no (no STREAM_TIMESTAMP in the capability bitmap)" in line


def test_doctor_streams_line_no_map(tmp_path):
    r = _doctor_on(_p3b_xml(tmp_path, gate_map=None))
    assert r.returncode == 0, r.stderr
    line = next(ln for ln in r.stdout.splitlines() if "streams:" in ln)
    assert "gate-map none" in line
    assert "rx_arm=no (no gate map)" in line


def test_doctor_streams_line_duplicate_role(tmp_path):
    r = _doctor_on(_p3b_xml(tmp_path, gate_map="rx-dma,rx-dma"))
    assert r.returncode == 0, r.stderr
    line = next(ln for ln in r.stdout.splitlines() if "streams:" in ln)
    assert "gate-map rejected" in line
    assert "timestamps=no (gate map rejected)" in line
