# SPDX-License-Identifier: MIT
"""Run iiod-emu on a free port for pytest.

    with Emulator(Path("tests/emu/tick.xml")) as uri:
        ...  # uri == "ip:127.0.0.1:<port>"

Needs DATAX_LIBIIO_BUILD (libiio build tree containing iiod-emu/iiod-emu).
"""
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path


class Emulator:
    def __init__(self, xml: Path):
        self.xml = Path(xml)
        self.proc = None
        self.uri = None
        self._log = None

    @staticmethod
    def _free_port() -> int:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def __enter__(self) -> str:
        build = os.environ.get("DATAX_LIBIIO_BUILD")
        emu = Path(build or "") / "iiod-emu" / "iiod-emu"
        if not build or not emu.is_file():
            raise RuntimeError("iiod-emu not found (set DATAX_LIBIIO_BUILD)")
        if not self.xml.is_file():
            raise FileNotFoundError(self.xml)
        env = dict(os.environ)
        ld = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = build + (":" + ld if ld else "")
        self._log = tempfile.TemporaryFile()
        try:
            for _ in range(5):
                port = self._free_port()
                self.proc = subprocess.Popen(
                    [str(emu), "-p", str(port), f"emu:{self.xml}"],
                    stdout=self._log, stderr=subprocess.STDOUT, env=env)
                if self._wait(port):
                    self.uri = f"ip:127.0.0.1:{port}"
                    return self.uri
                self._kill()
            self._log.seek(0)
            raise RuntimeError("iiod-emu failed to start: "
                               + self._log.read().decode(errors="replace"))
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def _wait(self, port: int) -> bool:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return False
            try:
                socket.create_connection(("127.0.0.1", port), 1).close()
                return True
            except OSError:
                time.sleep(0.1)
        return False

    def _kill(self):
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def __exit__(self, *exc):
        self._kill()
        if self._log is not None:
            self._log.close()
            self._log = None
        return False
