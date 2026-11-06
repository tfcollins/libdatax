# Running the hardware suite

`tests/hardware` exercises immediate streaming and digital loopback on a real
board. It is not part of Tier 1: CI only proves it collects
(`pytest tests/hardware --collect-only -q`). The `hw-test.yml` workflow runs it
on self-hosted runners (none registered yet); recorded results are in
[evidence/p2](evidence/p2/README.md).

## Run it

    cmake -S . -B build -DDATAX_LIBIIO_SOURCE=<libiio src> -DDATAX_LIBIIO_BUILD=<libiio build>
    cmake --build build -j
    pip install -e . numpy pytest
    export DATAX_LIBRARY=$PWD/build/libdatax.so
    export DATAX_HW_URI=<libiio URI of the booted board>
    export DATAX_HW_REQUIRE=1                       # fail, not skip, without a URI
    export DATAX_HW_EVIDENCE=$PWD/evidence.json     # default ./evidence.json
    python -m pytest tests/hardware -v --junitxml=junit.xml

The libiio build must be the pinned v1 fork (`pins.json`). The evidence JSON
records the redacted URI, libdatax commit, `datax_module`, `libdatax_library`
(the loaded `libdatax.so`), `libdatax_version`, the libiio pin, the IIO
context attributes and per-test measurements (throughput, overflow counters,
loopback lag and mismatches). Check it for host names and addresses before
committing one anywhere.

## Boot prerequisites

The suite does not boot or reconfigure boards. Before running:

- The board is booted from its Kuiper SD image (stock 2023_R2 kernel and
  device tree) and reachable over IIO (an `ip:` URI to its iiod).
- ADRV9361-Z7035: the stock 2023_R2 PL (`hdl_system_id` branch
  `hdl_2023_r2`). No further setup.
- ADRV9009-ZU11EG: a PL that provides the ADRV9009 JESD links and DMA RX/TX
  paths; P2 was validated with the HDL `main` PL at `085a6ed7` plus the stock
  2023_R2 kernel and device tree. Never reprogram the PL at runtime: that
  kills the JESD links; reboot instead.
- `direct_reg_access` and the loopback debug attributes must be present
  (stock kernels have them). A missing one fails the test naming it.
- Nothing else uses the board's RX/TX DMA (one stream per direction).

## Loopback facts

The digital loopback test enables the transceiver's debug loopback, restores it
in `finally`, and compares RX to the expected transform of TX:

- ad936x: `loopback` on `ad9361-phy`; RX = TX >> 4.
- adrv9009: `bist_framer_a_loopback` on each phy; I and Q are exchanged
  (RX I = TX Q, RX Q = TX I), gain 1, no shift.

## Troubleshooting timed tunes

`DATAX_TRACE=fhm` (a comma list; other names are ignored) makes libdatax print
one `datax-fhm:` line per FHM pipeline event to stderr. Events are queue
(edge number since FHM was enabled, seq, hardware time, priming lead), stage
(how it was triggered: `via=poll` or `via=record`, start estimate, end time,
the neighbouring edges, `result=ok|late`), stage-hold, stage-skip, fired, late,
noedge, verdict (every input of a tune's final status), park and staged table.
It adds no I/O and is off by default. Capture it with pytest's `-s`:

    DATAX_TRACE=fhm python -m pytest tests/hardware/test_hw_timed.py -k "hop_schedule or dense_hops" -s -v 2> fhm-trace.log
    grep -E "queue E|stage(-hold|-skip)? E|fired E|verdict E" fhm-trace.log

A tune completing LATE "stage late" shows a `stage E<n> ... result=late` whose
`end` is past `prev_hw`, or a `stage-skip E<n>`. The first tune that needs a
priming edge must be at least now + guard + lead (20 ms on tron) ahead;
earlier, `queue_tune` returns `DATAX_ERR_LATE`.
