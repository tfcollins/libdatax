# SPDX-License-Identifier: MIT
"""A timed TX burst: preload a cyclic tone, start it at a device time, mute it
for 20 ms with the scheduler's stream gate, then let it play again.

Needs a board with the P3b stream gate (the ``stream_gate`` capability).
Usage: timed_burst.py [uri] (or DATAX_TEST_URI).
"""
import os
import sys

import numpy as np

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
LEAD_S, MUTE_AT_S, MUTE_S = 0.1, 0.02, 0.02

with datax.Context(uri) as ctx:
    dev = ctx.devices[0]
    if "stream_gate" not in dev.caps:
        sys.exit(f"{dev.name}: no stream gate on this device")
    sched = dev.scheduler
    n = 1024
    tone = 8000 * np.exp(2j * np.pi * 0.05 * np.arange(n)).astype(np.complex64)
    tx = dev.tx_stream(channels=[0], block_samples=n, cyclic=True)
    tx.send(tone)                          # unstarted: preloads, transmits nothing
    t1 = dev.time.now + dev.time.seconds(LEAD_S)
    t_off = t1 + dev.time.seconds(MUTE_AT_S)
    t_on = t_off + dev.time.seconds(MUTE_S)
    with tx.start_at(t1):
        sched.at(t_off).gate_off(tx, report=True)
        sched.at(t_on).gate_on(tx, report=True)
        sched.commit()
        done = sched.wait_all(timeout=LEAD_S + MUTE_AT_S + MUTE_S + 5)   # LateError if late
    print(f"burst: started at t1, muted {MUTE_S * 1e3:.0f} ms at t1+{MUTE_AT_S * 1e3:.0f} ms "
          f"({len(done)} gate commands on time)")
