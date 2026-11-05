#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the emulator's RX source file for one streaming device.

    make_pattern.py <xml> <device> <channels> <instants> [--channel-offset N]

iiod-emu (libiio emu.c, emu_make_data_path) reads RX blocks from
``<name>_buf0.bin`` beside the context XML, where <name> is the device's
name, or its id when it has none, with ':' replaced by '_'. The file is the
raw DMA frame stream for the enabled channel set and wraps at its end.

The pattern is a counting sequence: instant k of the enabled-channel set has,
for every one of the <channels> radio channels, I = k mod 32768 and
Q = -(k mod 32768), int16 little-endian, I then Q, channels in ascending order.

--channel-offset N (opt-in, default 0) adds N*c to the count of the c-th
enabled channel (I = (k + N*c) mod 32768, Q = -I) so a test can tell the
channels apart; channel 0 always carries the plain pattern.

Run it on a temporary copy of the fixture directory, never on tests/emu/.
"""
import argparse
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def data_path(xml: Path, device: str) -> Path:
    """The file iiod-emu reads for <device> (matched by name or id), buffer 0."""
    root = ET.parse(xml).getroot()
    for dev in root.iter("device"):
        if device in (dev.get("name"), dev.get("id")):
            ident = dev.get("name") or dev.get("id")
            return xml.parent / (ident.replace(":", "_") + "_buf0.bin")
    raise SystemExit(f"make_pattern: no device {device!r} in {xml}")


def pattern(channels: int, instants: int, channel_offset: int = 0) -> bytes:
    out = bytearray()
    for k in range(instants):
        for c in range(channels):
            i = (k + channel_offset * c) % 32768
            out += struct.pack("<hh", i, -i)
    return bytes(out)


def write(xml: Path, device: str, channels: int, instants: int,
          channel_offset: int = 0) -> Path:
    path = data_path(xml, device)
    path.write_bytes(pattern(channels, instants, channel_offset))
    return path


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("xml", type=Path)
    p.add_argument("device")
    p.add_argument("channels", type=int)
    p.add_argument("instants", type=int)
    p.add_argument("--channel-offset", type=int, default=0)
    a = p.parse_args(argv)
    if a.channels < 1 or a.instants < 1:
        p.error("channels and instants must be >= 1")
    print(write(a.xml, a.device, a.channels, a.instants, a.channel_offset))
    return 0


if __name__ == "__main__":
    sys.exit(main())
