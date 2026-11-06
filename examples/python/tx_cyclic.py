# SPDX-License-Identifier: MIT
"""Transmit a repeating tone. Usage: tx_cyclic.py [uri] (or DATAX_TEST_URI). Needs numpy."""
import os
import sys

import numpy as np

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
with datax.Context(uri) as ctx:
    dev = ctx.devices[0]
    n = 1024
    tone = 10000 * np.exp(2j * np.pi * np.arange(n) * 16 / n)  # 16 cycles per period
    dev.tx(tone, cyclic=True)  # repeats until tx_stop() or the context closes
    print(f"{dev.name}: transmitting a {n}-sample cyclic tone")
    dev.tx_stop()
