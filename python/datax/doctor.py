# SPDX-License-Identifier: MIT
"""python -m datax.doctor [uri]: print library, device and capability info."""
import ctypes as C
import os
import sys

from . import Context, Error
from ._lib import c_ull, get_lib


def _streams_line(dev):
    """rx=<name>|none tx=<name>|none overflow-detect=yes|no|unknown."""
    lib = get_lib()
    names, states = [], []
    for direction in (0, 1):  # DATAX_RX, DATAX_TX
        buf = C.create_string_buffer(64)
        detect = C.c_int()
        rc = lib.datax_internal_device_stream_info(
            dev._ptr, direction, buf, len(buf), C.byref(detect))
        if rc:
            names.append("none")
        else:
            names.append(buf.value.decode())
            states.append(detect.value)
    if not states or 0 in states:
        det = "no"
    elif 1 in states:
        det = "yes"
    else:
        det = "unknown"
    return f"streams: rx={names[0]} tx={names[1]} overflow-detect={det}"


# enum datax_time_source order; limit indices follow enum datax_limit.
_TIME_SOURCES = ("free-run", "pps-aligned", "soft-aligned", "pps-holdover")
_LIMIT_REPORT_SPACING_US, _LIMIT_HOP_STAGE_US = 5, 6


def _time_line(dev):
    """time: <source> (pps present|no pps), or none without a Tick."""
    lib = get_lib()
    src = C.c_int()
    if lib.datax_device_get_time_source(dev._ptr, C.byref(src)):
        return "time: none (no Tick scheduler)"
    pps = lib.datax_internal_pps_present(dev._ptr) == 1
    name = _TIME_SOURCES[src.value] if 0 <= src.value < len(_TIME_SOURCES) else "unknown"
    return f"time: {name} ({'pps present' if pps else 'no pps'})"


def _scheduler_line(dev):
    """Measured report spacing and hop stage, plus the exclusivity note."""
    if "timed_commands" not in dev.caps:
        return "scheduler: none (no usable Tick scheduler)"
    lib = get_lib()
    vals = []
    for idx in (_LIMIT_REPORT_SPACING_US, _LIMIT_HOP_STAGE_US):
        v = c_ull()
        vals.append(v.value if lib.datax_caps_get_limit(dev._caps_ptr, idx, C.byref(v)) == 0
                    else None)
    spacing = "not measured" if vals[0] is None else f"{vals[0]} \u00b5s"
    stage = "not measured" if vals[1] is None else f"{vals[1]} \u00b5s"
    tag = "(measured)" if any(v is not None for v in vals) else "(not measured yet)"
    return (f"scheduler: report spacing {spacing}, hop stage {stage} {tag}; "
            "exclusive use of the board's scheduler")


def _mcs_line(dev):
    """mcs: <state>; phase-coherent: <yes|no> (<reason>)."""
    state, reason = C.create_string_buffer(128), C.create_string_buffer(128)
    rc = get_lib().datax_internal_mcs_info(dev._ptr, state, len(state), reason, len(reason))
    if rc < 0:
        return "mcs: not supported; phase-coherent: no (no multi-chip sync on this board)"
    return (f"mcs: {state.value.decode()}; phase-coherent: {'yes' if rc == 1 else 'no'} "
            f"({reason.value.decode()})")


def run(uri, out):
    with Context(uri) as ctx:
        major, minor = ctx.version
        print(f"libdatax version: {major}.{minor}", file=out)
        devs = ctx.devices
        print(f"devices: {len(devs)}", file=out)
        for i, d in enumerate(devs):
            lim = d.caps.limits
            print(f"[{i}] {d.name}", file=out)
            print(f"    channels: {lim.get('max_channels', 0)}", file=out)
            caps = sorted(d.caps)
            print(f"    capabilities: {', '.join(caps) if caps else 'none'}", file=out)
            det = sorted(d.caps.detected)
            print(f"    detected hardware: {', '.join(det) if det else 'none'}", file=out)
            print("    " + _streams_line(d), file=out)
            print("    " + _time_line(d), file=out)
            print("    " + _scheduler_line(d), file=out)
            print("    " + _mcs_line(d), file=out)
            print("    limits: " + ", ".join(f"{k}={v}" for k, v in lim.items()), file=out)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    uri = argv[0] if argv else os.environ.get("DATAX_TEST_URI")
    try:
        run(uri, sys.stdout)
    except Error as e:
        print(f"datax doctor: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
