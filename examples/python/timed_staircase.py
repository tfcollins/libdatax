# SPDX-License-Identifier: MIT
"""A hard-timed GPIO staircase: eight steps, 20 ms apart, on scheduler GPIO
lines 3:1 (line 0 is the ZU11EG hop trigger, only tunes may drive it), every
step reported so the executed time can be read back.

Needs a board with the timed scheduler (the ``timed_commands`` capability).
Usage: timed_staircase.py [uri] (or DATAX_TEST_URI).
"""
import os
import sys

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
STEPS, DWELL_S, LEAD_S = 8, 0.020, 0.2

with datax.Context(uri) as ctx:
    dev = ctx.devices[0]
    if "timed_commands" not in dev.caps:
        sys.exit(f"{dev.name}: no timed scheduler on this device")
    sched = dev.scheduler
    t0 = dev.time.now + dev.time.seconds(LEAD_S)
    dwell = dev.time.seconds(DWELL_S)
    seqs = [sched.at(t0 + i * dwell).gpio(0xE, i << 1, report=True) for i in range(STEPS)]
    sched.at(t0).marker(line=0)                 # scope-visible start edge
    sched.commit()
    done = sched.wait_all(timeout=LEAD_S + STEPS * DWELL_S + 5)   # LateError on a late step
    us = 1e6 / dev.time.tick_hz
    for c, i in zip(done, range(STEPS)):
        print(f"step {i}: value {i << 1:#04x} at t0+{(c.requested_ticks - t0) * us:.0f} us, "
              f"executed {(c.executed_ticks - c.requested_ticks) * us:+.1f} us from request")
    assert [c.seq for c in done] == seqs
    print(f"{len(done)} steps on time")
