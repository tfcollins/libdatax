# SPDX-License-Identifier: MIT
"""Set the RX LO and read it back. Usage: set_lo_and_read_back.py [uri] (or DATAX_TEST_URI)."""
import os
import sys

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
with datax.Context(uri) as ctx:
    dev = ctx.devices[0]
    dev.rx_lo = 2.4e9
    print(f"{dev.name}: rx_lo = {dev.rx_lo:.0f} Hz")
