# SPDX-License-Identifier: MIT
"""List the devices in a context. Usage: list_devices.py [uri] (or DATAX_TEST_URI)."""
import os
import sys

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
with datax.Context(uri) as ctx:
    for i, dev in enumerate(ctx.devices):
        print(f"{i}: {dev.name} detected={sorted(dev.caps.detected)} limits={dev.caps.limits}")
