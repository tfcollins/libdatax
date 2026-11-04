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
