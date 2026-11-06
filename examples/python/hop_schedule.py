# SPDX-License-Identifier: MIT
"""A hard-timed frequency-hop schedule: stage the table, queue one tune per
dwell, commit once, then wait for every hop to be confirmed on time.

Needs a board with timed hops (the ``fhm_hop`` or ``fastlock_hop``
capability). Usage: hop_schedule.py [uri] (or DATAX_TEST_URI).
"""
import os
import sys

import datax

uri = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DATAX_TEST_URI")
PLAN = [2.41e9, 2.42e9, 2.43e9, 2.44e9]
DWELL_S, LEAD_S = 0.050, 0.3

with datax.Context(uri) as ctx:
    dev = ctx.devices[0]
    if "timed_commands" not in dev.caps or not ({"fhm_hop", "fastlock_hop"} & set(dev.caps)):
        sys.exit(f"{dev.name}: no timed hop support on this device")
    dev.stage_hop_table(PLAN)                   # untimed, milliseconds: do it early
    sched, dwell = dev.scheduler, dev.time.seconds(DWELL_S)
    # The first tune needs the guard margin plus a priming lead ahead of it.
    t0 = dev.time.now + dev.time.seconds(LEAD_S)
    for i, f in enumerate(PLAN):
        sched.at(t0 + i * dwell).tune(f)
    sched.commit()
    done = sched.wait_all(timeout=LEAD_S + len(PLAN) * DWELL_S + 5)   # LateError on a late hop
    for c in done:
        print(f"hop at t0+{(c.requested_ticks - t0) / dev.time.tick_hz * 1e3:.0f} ms: "
              f"{c.status.name}")
    print(f"{len(done)} of {len(PLAN)} hops confirmed")
