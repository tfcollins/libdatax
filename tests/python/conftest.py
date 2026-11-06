# SPDX-License-Identifier: MIT
"""Shared fixtures: the libdatax shared object and the vendored Tick model."""

import ctypes
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / "tests" / "vendor"


class DescStruct(ctypes.Structure):
    """Mirror of struct datax_tick_descriptor (src/tick_descriptor.h)."""

    _fields_ = [
        ("version", ctypes.c_uint16),
        ("size_bytes", ctypes.c_uint16),
        ("command_id", ctypes.c_uint16),
        ("target_id", ctypes.c_uint16),
        ("timestamp_ticks", ctypes.c_uint64),
        ("flags", ctypes.c_uint32),
        ("sequence_id", ctypes.c_uint32),
        ("args", ctypes.c_uint32 * 4),
    ]


@pytest.fixture(scope="session")
def lib():
    path = os.environ.get("DATAX_LIBRARY") or str(ROOT / "build" / "libdatax.so")
    cdll = ctypes.CDLL(path)
    cdll.datax_internal_tick_descriptor_pack.argtypes = [
        ctypes.POINTER(DescStruct), ctypes.c_char_p]
    cdll.datax_internal_tick_descriptor_pack.restype = ctypes.c_int
    cdll.datax_internal_tick_enum.argtypes = [ctypes.c_char_p]
    cdll.datax_internal_tick_enum.restype = ctypes.c_long
    return cdll


@pytest.fixture(scope="session")
def tick_vendor():
    """Vendored Tick model as a package: tick_vendor.descriptor / .enums."""
    if str(VENDOR) not in sys.path:
        sys.path.insert(0, str(VENDOR))
    import tick as pkg  # tests/vendor/tick
    from tick import descriptor, enums
    assert Path(pkg.__file__).resolve().parent == VENDOR / "tick"
    return type("TickVendor", (), {"descriptor": descriptor, "enums": enums})


# ---- Strict mode: DATAX_REQUIRE_EMU=1 turns every skip into a failure --------

def _strict():
    return os.environ.get("DATAX_REQUIRE_EMU") == "1"


def _fail_report(rep, what):
    rep.outcome = "failed"
    rep.longrepr = f"DATAX_REQUIRE_EMU=1: {what} was skipped: {rep.longrepr!r}"


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if _strict() and rep.skipped and not hasattr(rep, "wasxfail"):
        _fail_report(rep, item.nodeid)


@pytest.hookimpl(hookwrapper=True)
def pytest_make_collect_report(collector):
    outcome = yield
    rep = outcome.get_result()
    if _strict() and rep.skipped:
        _fail_report(rep, str(collector.nodeid))


# ---- Python package fixtures -------------------------------------------------

sys.path.insert(0, str(ROOT / "python"))
os.environ.setdefault("DATAX_LIBRARY", str(ROOT / "build" / "libdatax.so"))
EMU_DIR = ROOT / "tests" / "emu"


def _emu_available():
    build = os.environ.get("DATAX_LIBIIO_BUILD")
    return bool(build) and (Path(build) / "iiod-emu" / "iiod-emu").is_file()


@pytest.fixture
def emu_uri(request):
    """Start iiod-emu for the fixture named by the 'fixture' marker param.

    Usage: @pytest.mark.parametrize("emu_uri", ["ad936x"], indirect=True)
    """
    if not _emu_available():
        pytest.skip("iiod-emu not found: set DATAX_LIBIIO_BUILD to a libiio build tree")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    with Emulator(EMU_DIR / f"{request.param}.xml") as uri:
        yield uri


# ---- Fake Tick / fake FHM rig (Tier 1 timed control) --------------------------
#
# The C fake (tests/c/fake_tick.c, built as libdatax_fake_tick) is bound to a
# live emulator device through the test-only datax_internal_* hooks. Device
# time moves only when a test advances the fake.

import ctypes as C  # noqa: E402
import threading  # noqa: E402
import time as _time  # noqa: E402

HZ = 100_000_000          # the fake's (tron) tick rate
MS = HZ // 1000
US = HZ // 1_000_000


def _fake_path():
    env = os.environ.get("DATAX_FAKE_TICK_LIBRARY")
    if env:
        return Path(env)
    lib = os.environ.get("DATAX_LIBRARY") or str(ROOT / "build" / "libdatax.so")
    return Path(lib).resolve().parent / "tests" / "c" / "libdatax_fake_tick.so"


_fake = None


def fake_lib():
    """The typed libdatax_fake_tick (loaded once); skips when it is not built."""
    global _fake
    if _fake is not None:
        return _fake
    path = _fake_path()
    if not path.is_file():
        pytest.skip(f"fake Tick library not built: {path}")
    f = C.CDLL(str(path))
    vp, u64 = C.c_void_p, C.c_uint64
    for name, res, args in [
        ("fake_tick_new", vp, [vp]), ("fake_tick_free", None, [vp]),
        ("fake_tick_advance", None, [vp, u64]), ("fake_tick_now", u64, [vp]),
        ("fake_tick_pps_edge", None, [vp]), ("fake_tick_gpio", C.c_uint32, [vp]),
        ("fake_tick_set_access_cost", None, [vp, u64]),
        ("fake_tick_access_count", C.c_ulong, [vp, C.c_char_p, C.c_bool]),
        ("fake_tick_inject_error", None, [vp, C.c_char_p, C.c_int]),
        ("fake_fhm_new", vp, [u64]), ("fake_fhm_free", None, [vp]),
        ("fake_fhm_attach", None, [vp, vp]), ("fake_fhm_pll_hz", u64, [vp]),
        ("fake_fhm_edges", C.c_uint, [vp]),
    ]:
        fn = getattr(f, name)
        fn.restype, fn.argtypes = res, args
    _fake = f
    return f


def _ops_addr(lib, name):
    return C.addressof((C.c_void_p * 2).in_dll(lib, name))


class FakeRig:
    """A Context on an emulator fixture with the fake Tick (and Talise FHM)
    bound to device 0."""

    def __init__(self, uri, fhm):
        import datax
        from datax._lib import get_lib
        self.fk = fake_lib()
        self.lib = get_lib()
        for name, args in [("datax_internal_tick_bind_fake", [C.c_void_p, C.c_void_p, C.c_void_p]),
                           ("datax_internal_fhm_bind_fake", [C.c_void_p, C.c_void_p, C.c_void_p]),
                           ("datax_internal_sched_pause", [C.c_void_p, C.c_bool])]:
            fn = getattr(self.lib, name)
            fn.restype, fn.argtypes = C.c_int, args
        self.tick = self.fk.fake_tick_new(None)
        self.fhm = self.fk.fake_fhm_new(0) if fhm else None
        self.ctx = datax.Context(uri)
        self.dev = self.ctx.devices[0]
        self.bind(self.ctx)
        self._clock = None

    def bind(self, ctx):
        dev = ctx.devices[0]
        assert self.lib.datax_internal_tick_bind_fake(
            dev._ptr, _ops_addr(self.fk, "fake_tick_ops"), self.tick) == 0
        if self.fhm:
            self.fk.fake_fhm_attach(self.fhm, self.tick)
            assert self.lib.datax_internal_fhm_bind_fake(
                dev._ptr, _ops_addr(self.fk, "fake_fhm_ops"), self.fhm) == 0
        if self.now == 0:
            self.fk.fake_tick_advance(self.tick, 1000 * MS)   # start away from 0

    # -- time ---------------------------------------------------------------
    @property
    def now(self):
        return self.fk.fake_tick_now(self.tick)

    def advance(self, ticks):
        self.fk.fake_tick_advance(self.tick, int(ticks))

    def gpio(self):
        return self.fk.fake_tick_gpio(self.tick)

    def writes(self, attr="command_descriptor"):
        return self.fk.fake_tick_access_count(self.tick, attr.encode(), True)

    def inject_error(self, attr, err):
        """The next access to ``attr`` fails with -err (one shot)."""
        self.fk.fake_tick_inject_error(self.tick, attr.encode(), -abs(err))

    def pause(self, on=True):
        """Pause / resume the scheduler's poller (feed and drain)."""
        assert self.lib.datax_internal_sched_pause(self.dev.scheduler._h, on) == 0

    def pps_edge(self):
        self.fk.fake_tick_pps_edge(self.tick)

    def pll_hz(self):
        return self.fk.fake_fhm_pll_hz(self.fhm)

    def settle(self, quiet=0.03):
        """Wait until the poller has stopped writing descriptors."""
        last, t = -1, _time.monotonic()
        while _time.monotonic() - t < quiet:
            n = self.writes()
            if n != last:
                last, t = n, _time.monotonic()
            _time.sleep(0.002)

    def run_to(self, until, step=MS, pause=0.002):
        """Advance device time to ``until`` in steps, giving the poller real
        time between steps (feed ahead, drain, ack)."""
        self.settle()
        while self.now < until:
            self.advance(min(step, until - self.now))
            _time.sleep(pause)
        _time.sleep(0.01)

    def make_scheduler(self):
        """dev.scheduler, created with a 100 us access cost so the measured
        report spacing is 2.7 ms (as the C tests do)."""
        self.fk.fake_tick_set_access_cost(self.tick, 100 * US)
        try:
            return self.dev.scheduler
        finally:
            self.fk.fake_tick_set_access_cost(self.tick, 0)

    def start_clock(self, hz_ratio=1.0):
        """Advance device time in the background, ``hz_ratio`` ms per ms of wall
        time. A step is taken only while the poller is idle (no descriptor
        written during the last millisecond), so a loaded host slows the
        clock instead of making the poller miss a deadline."""
        stop = threading.Event()

        def run():
            while not stop.is_set():
                n = self.writes()
                _time.sleep(0.001)
                if self.writes() != n:
                    continue
                self.advance(int(MS * hz_ratio))

        t = threading.Thread(target=run, daemon=True)
        t.start()
        self._clock = (stop, t)

    def stop_clock(self):
        if self._clock:
            self._clock[0].set()
            self._clock[1].join()
            self._clock = None

    def release(self):
        """Drop this rig's handles on the Context (the test keeps its own)."""
        self.ctx = self.dev = None

    def close(self):
        self.stop_clock()
        if self.ctx is not None:
            self.ctx.close()
        if self.fhm:
            self.fk.fake_fhm_free(self.fhm)
        self.fk.fake_tick_free(self.tick)


@pytest.fixture
def make_rig():
    """Factory: ``make_rig(fixture="adrv9009-zu11eg", fhm=True)`` -> FakeRig.
    Everything (rigs, emulators) is torn down after the test, context first."""
    if not _emu_available():
        pytest.skip("iiod-emu not found: set DATAX_LIBIIO_BUILD to a libiio build tree")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    made, emus = [], []

    def factory(fixture="adrv9009-zu11eg", fhm=True):
        em = Emulator(EMU_DIR / f"{fixture}.xml")
        uri = em.__enter__()
        emus.append(em)
        rig = FakeRig(uri, fhm)
        made.append(rig)
        return rig

    yield factory
    for r in made:
        r.close()
    for e in emus:
        e.__exit__(None, None, None)
