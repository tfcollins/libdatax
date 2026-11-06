# SPDX-License-Identifier: MIT
"""Capture samples. Usage: rx_capture.py [uri] (or DATAX_TEST_URI). Needs numpy."""
import os
import sys

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
with datax.Context(uri) as ctx:
    dev = ctx.devices[0]
    iq = dev.rx(4096)  # complex64, shape (channels, samples)
    print(f"{dev.name}: captured {iq.shape[1]} samples on {iq.shape[0]} channel(s), "
          f"rms {abs(iq).mean():.1f}")
