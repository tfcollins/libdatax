# SPDX-License-Identifier: MIT
"""REQ-TEST-001: C descriptor packer and enum constants match the Tick model."""

import ctypes

import pytest

from conftest import DescStruct

PREFIX = {
    "Flag": "FLAG",
    "Capability": "CAPABILITY",
    "CommandID": "COMMAND_ID",
    "TargetFamily": "TARGET_FAMILY",
    "Status": "STATUS",
}
U16, U32, U64 = 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF


def cases(en):
    F, C, T = en.Flag, en.CommandID, en.TargetFamily
    out = [
        # (command_id, target_id, timestamp, flags, sequence_id, args)
        (0, 0, 0, 0, 0, (0, 0, 0, 0)),
        (U16, U16, U64, 0, U32, (U32, U32, U32, U32)),
        (U16, U16, U64, F.REPORT_COMPLETION | F.LATE_DROP | F.REPEAT, U32,
         (U32, U32, U32, U32)),
        (U16, U16, U64, F.REPORT_COMPLETION | F.LATE_EXECUTE_NOW | F.REPEAT,
         U32, (U32, U32, U32, U32)),
        (1, 0, 0x0102030405060708, 0, 0x0A0B0C0D, (1, 2, 3, 4)),
        (1, 1, 1, 0, 1, (0x80000000, 0, 0, 0)),
        (0x1234, 0x5678, 0x8000000000000000, 0, 0x80000000, (0, 0x7FFFFFFF, 0, 5)),
    ]
    for f in (F.REPORT_COMPLETION, F.LATE_DROP, F.LATE_EXECUTE_NOW):
        out.append((C.GPIO_WRITE, T.GPIO, 1000, f, 7, (1, 0, 0, 0)))
    out.append((C.GPIO_WRITE, T.GPIO, 1000, F.REPEAT, 7, (1, 0, 3, 100)))
    for c in C:
        if c == C.SPI_TRANSFER:
            args = (0xAA, (16 << 16) | (8 << 8) | 1, 0, 0)
        else:
            args = (0xDEADBEEF, 2, 3, 4)
        out.append((c, T.GPIO, 0xFFFF_0000_0000 + int(c), F.NONE, int(c), args))
    for t in T:
        out.append((C.MARKER_PULSE, t, int(t) << 8, F.REPORT_COMPLETION,
                    int(t), (int(t), 0, 0, 0)))
    return out


def test_pack_matches_tick_model(lib, tick_vendor):
    d, en = tick_vendor.descriptor, tick_vendor.enums
    all_cases = cases(en)
    assert len(all_cases) >= 20
    for cmd, tgt, ts, flags, seq, args in all_cases:
        ref = d.CommandDescriptor(command_id=cmd, target_id=tgt, timestamp_ticks=ts,
                                  flags=flags, sequence_id=seq, args=args).pack()
        assert len(ref) == 48
        desc = DescStruct(d.DESCRIPTOR_VERSION, d.DESCRIPTOR_SIZE, int(cmd), int(tgt),
                          ts, int(flags), seq, (ctypes.c_uint32 * 4)(*args))
        out = ctypes.create_string_buffer(b"\xff" * 48, 48)
        assert lib.datax_internal_tick_descriptor_pack(ctypes.byref(desc), out) == 0
        assert out.raw == ref, (cmd, tgt, ts, flags, seq, args)


def test_pack_rejects_null(lib):
    out = ctypes.create_string_buffer(48)
    assert lib.datax_internal_tick_descriptor_pack(None, out) < 0


def test_enum_values_match_tick_model(lib, tick_vendor):
    en = tick_vendor.enums
    total = 0
    for cls_name, prefix in PREFIX.items():
        for member in getattr(en, cls_name).__members__.values():  # includes Flag.NONE
            name = f"DATAX_TICK_{prefix}_{member.name}"
            got = lib.datax_internal_tick_enum(name.encode())
            assert got == member.value, name
            total += 1
    assert total >= 40


def test_enum_unknown_is_minus_one(lib):
    assert lib.datax_internal_tick_enum(b"DATAX_TICK_NOPE") == -1
    assert lib.datax_internal_tick_enum(None) == -1
