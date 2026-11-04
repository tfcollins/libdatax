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
