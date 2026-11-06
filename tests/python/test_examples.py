# SPDX-License-Identifier: MIT
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("script", ["list_devices.py", "set_lo_and_read_back.py"])
@pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
def test_example_runs(script, emu_uri):
    env = dict(os.environ, PYTHONPATH=str(ROOT / "python"), DATAX_TEST_URI=emu_uri)
    r = subprocess.run([sys.executable, str(ROOT / "examples" / "python" / script)],
                       capture_output=True, text=True, env=env, timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout.strip()


def _stage(tmp):
    sys.path.insert(0, str(ROOT / "tests" / "emu"))
    import shutil
    from make_pattern import write
    xml = Path(tmp) / "ad936x.xml"
    shutil.copy(ROOT / "tests" / "emu" / "ad936x.xml", xml)
    write(xml, "cf-ad9361-lpc", 1, 32768)
    return xml


@pytest.mark.parametrize("script", ["rx_capture.py", "tx_cyclic.py"])
def test_stream_example_runs(script, tmp_path):
    from conftest import EMU_DIR, _emu_available
    if not _emu_available():
        pytest.skip("iiod-emu not found: set DATAX_LIBIIO_BUILD to a libiio build tree")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    with Emulator(_stage(tmp_path)) as uri:
        env = dict(os.environ, PYTHONPATH=str(ROOT / "python"), DATAX_TEST_URI=uri)
        r = subprocess.run([sys.executable, str(ROOT / "examples" / "python" / script)],
                           capture_output=True, text=True, env=env, timeout=60)
    assert r.returncode == 0, r.stderr
    assert r.stdout.strip()


@pytest.mark.parametrize("script,expect", [("timed_staircase.py", "8 steps on time"),
                                           ("hop_schedule.py", "4 of 4 hops confirmed")])
def test_timed_example_runs(script, expect, make_rig, monkeypatch, capsys):
    """The timed examples in-process against the fake Tick (and Talise FHM)
    on the ZU11EG fixture; a background clock stands in for device time."""
    import runpy
    import datax
    rig = make_rig()
    rig.make_scheduler()                  # measured spacing, as the C tests do
    rig.start_clock()
    monkeypatch.setattr(datax, "Context", lambda uri=None: rig.ctx)
    monkeypatch.setattr(sys, "argv", [script])
    monkeypatch.delenv("DATAX_TEST_URI", raising=False)
    runpy.run_path(str(ROOT / "examples" / "python" / script), run_name="__main__")
    out = capsys.readouterr().out
    assert expect in out, out
