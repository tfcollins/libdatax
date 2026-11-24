# SPDX-License-Identifier: MIT
"""The ctypes mirrors of C structs match their C definitions.

Regression for the P3b S4 hardware run 1 segfault (tron, timed-dma, ea865bb):
S3 added ``tag_errors`` to ``struct datax_internal_stream_stats`` but the
hardware suite's ``loopback.StatsC`` kept 6 fields (48 of 56 bytes), so every
``stream_stats()`` call wrote 8 bytes past a Python-owned ctypes object. The
heap corruption surfaced two tests later as a segfault in CPython's GC. Only
the hardware suite calls it, so Tier 1 never saw it.
"""
import ctypes as C
import re
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

import datax
from datax import _lib as L
import conftest as cf
from conftest import EMU_DIR, ROOT, _emu_available

sys.path.insert(0, str(ROOT / "tests" / "hardware"))
import loopback as lb  # noqa: E402

HEADERS = [ROOT / "include" / "datax" / "datax.h", ROOT / "include" / "datax" / "datax_internal.h",
           ROOT / "tests" / "c" / "fake_dma.h", ROOT / "tests" / "c" / "fake_tick.h"]


def _c_fields(struct):
    """Field names of ``struct <struct> { ... };`` in HEADERS, in order."""
    for h in HEADERS:
        m = re.search(r"struct\s+" + struct + r"\s*\{(.*?)\};", h.read_text(), re.S)
        if m:
            body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
            names = []
            for decl in body.split(";"):
                decl = decl.strip()
                if not decl:
                    continue
                # "type a, b" -> a, b (no arrays or bit-fields in these structs)
                head, *rest = [d.strip() for d in decl.split(",")]
                names.append(re.findall(r"\w+", head)[-1])
                names.extend(re.findall(r"\w+", r)[-1] for r in rest)
            return names
    raise AssertionError(f"struct {struct} not found in {HEADERS}")


@pytest.mark.parametrize("cls,struct", [
    (lb.StatsC, "datax_internal_stream_stats"),
    (L.BlockInfoC, "datax_block_info"),
    (L.StreamConfig, "datax_stream_config"),
    (L.CompletionC, "datax_completion"),
    (L.EventC, "datax_event"),
    (cf.FakeDmaConfig, "fake_dma_config"),
    (cf.TxSegment, "fake_dma_tx_segment"),
    (cf.FakeTickConfig, "fake_tick_config"),
])
def test_ctypes_fields_match_header(cls, struct):
    assert [f for f, _ in cls._fields_] == _c_fields(struct), struct


@pytest.fixture
def staged_uri():
    """iiod-emu on a temp copy of ad936x.xml with an RX pattern (as test_stream.py)."""
    if not _emu_available():
        pytest.skip("iiod-emu not found: set DATAX_LIBIIO_BUILD to a libiio build tree")
    sys.path.insert(0, str(EMU_DIR))
    from emu import Emulator
    from make_pattern import write
    with tempfile.TemporaryDirectory() as tmp:
        xml = Path(tmp) / "ad936x.xml"
        shutil.copy(EMU_DIR / "ad936x.xml", xml)
        write(xml, "cf-ad9361-lpc", 1, 32768)
        with Emulator(xml) as uri:
            yield uri


def test_stream_stats_writes_only_inside_statsc(staged_uri):
    """datax_internal_stream_stats must not write past sizeof(StatsC)."""
    with datax.Context(staged_uri) as ctx:
        with ctx.devices[0].rx_stream(block_samples=1024, block_count=4) as rx:
            rx.recv(1024, timeout_ms=5000)
            fn = L.get_lib().datax_internal_stream_stats
            fn.argtypes, fn.restype = [C.c_void_p, C.c_void_p], C.c_int
            buf = (C.c_ubyte * 256)(*([0xA5] * 256))
            assert fn(rx._live(), C.addressof(buf)) == 0
            n = C.sizeof(lb.StatsC)
            assert all(b == 0xA5 for b in buf[n:]), \
                f"C wrote past the {n}-byte StatsC mirror"
            st = lb.stream_stats(rx)
    assert st["blocks_done"] >= 1 and "tag_errors" in st
