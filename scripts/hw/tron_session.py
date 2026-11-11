# SPDX-License-Identifier: MIT
"""Inside an adi-lg reservation of place tron: boot the image described by the
VRT49_CORUNDUM_* environment (hopcap.lab.boot), run tests/hardware against the
booted board, then restore the card and power off. Called by tron-run.sh."""

import os
import subprocess
import sys
from pathlib import Path

from hopcap.lab.boot import BootArtifacts, TronSession

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(os.environ["DATAX_HW_OUT"])
IMAGE = os.environ["DATAX_HW_IMAGE"]


def main() -> int:
    session = TronSession(BootArtifacts.from_env(os.environ))
    rc = 1
    try:
        shell = session.boot_exact()
        bind = os.environ["DATAX_LAB_HTTP_BIND"]
        ip = shell.run(f"ip -4 route get {bind} | sed -n 's/.*src \\([0-9.]*\\).*/\\1/p' | head -1").strip()
        if not ip:
            print("### no board address", flush=True)
            return 4
        print("### board booted; running tests/hardware", flush=True)
        lib = ROOT / "libiio" / "build"
        env = dict(os.environ,
                   DATAX_LIBRARY=str(ROOT / "build" / "libdatax.so"),
                   DATAX_LIBIIO_LIB=str(lib / "libiio.so.1"),
                   LD_LIBRARY_PATH=str(lib),
                   DATAX_HW_URI=f"ip:{ip}",
                   DATAX_HW_REQUIRE="1",
                   DATAX_HW_EVIDENCE=str(OUT / f"evidence-tron-{IMAGE}.json"))
        if IMAGE == "hopcap-sfp":
            env["DATAX_TRACE"] = "fhm"
        rc = subprocess.call([sys.executable, "-m", "pytest", "tests/hardware", "-v", "-rs",
                              f"--junitxml={OUT / f'junit-tron-{IMAGE}.xml'}"],
                             cwd=ROOT, env=env, timeout=2400)
        print("### pytest rc", rc, flush=True)
    finally:
        session.restore_and_power_off()
        print("### restored", session.restored_sums, session.restore_error, flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
