# SPDX-License-Identifier: MIT
"""python -m datax.doctor [uri]: print library, device and capability info."""
import os
import sys

from . import Context, Error


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
