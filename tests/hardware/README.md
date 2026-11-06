# Hardware suite

Streaming smoke (REQ-TEST-021, streaming part), digital loopback
(REQ-TEST-030, digital part) and the P3a timed-control tests (scheduler, FHM
hops, device time and PPS, MCS) against a real board. Not part of Tier 1: CI only
runs `pytest tests/hardware --collect-only -q` to prove it collects.

    export DATAX_HW_URI=<libiio uri of the board>      # required to run
    export DATAX_HW_REQUIRE=1                          # fail, not skip, without a URI
    export DATAX_HW_EVIDENCE=/path/evidence.json       # default ./evidence.json
    export DATAX_LIBRARY=$PWD/build/libdatax.so
    pip install -e . numpy pytest
    python -m pytest tests/hardware -v

    export DATAX_HW_IMAGE=hopcap-sfp                   # or stock (default)
    export DATAX_HW_PPS=internal                       # or none (see below)
    export DATAX_HW_IMAGE_HASHES="BOOT.BIN=<sha256>"   # optional, copied to the evidence

Without `DATAX_HW_URI` every test skips (or fails with `DATAX_HW_REQUIRE=1`).

Notes:

- The loopback test switches a debug attribute and restores it to 0 in `finally`.
  ad936x: `loopback` on `ad9361-phy` (RX = TX >> 4, asserted bit-exact).
  adrv9009: `bist_framer_a_loopback` on both `adrv9009-phy` and, when present,
  `adrv9009-phy-b`; register 0x1532 bit 7 of each phy is read back through its
  `direct_reg_access` (a failure names the phy). The stock framer loopback
  exchanges I and Q (RX I = TX Q, RX Q = TX I, no shift; tron, 2026-11-05), so
  the test compares against `expected_loopback()`; an RF loopback would confirm
  TX I/Q orientation independently. A missing attribute fails with its name.
- Debug attributes use libiio v1 through ctypes on the libiio already loaded by
  libdatax (`DATAX_LIBIIO_LIB` overrides); pylibiio targets v0 and is not used.
- `evidence.json` holds the redacted URI, the libiio pin, the libdatax commit,
  the IIO context description/version attributes and per-test measurements
  (throughput, overflow counters, EVM). Never commit one with a real host.
- RX status-register polling is asserted only when the RX streaming device has
  `direct_reg_access`; overflows are recorded, never asserted.

## P3a timed control (`test_hw_timed.py`, `test_hw_mcs.py`)

- `DATAX_HW_IMAGE` names the PL image. `hopcap-sfp` (ADRV9009-ZU11EG with
  hopcap's SFP image: Tick, the FHM trigger and the Tick PPS selector) runs the
  P3a tests; on `stock` they skip. On `hopcap-sfp` the TX tests (marker
  `needs_tx`: one-shot and cyclic TX, digital loopback) skip, because the TX DMA
  feeds vrt49's deframer there; the RX tests of the P2 smoke run unchanged as
  the regression.
- `DATAX_HW_PPS=internal` (default) expects the selector image to drive Tick's
  PPS input from Corundum's internal PTP PPS: `align_next_pps` must give
  `PPS_ALIGNED`, Tick `time_aligned` 1 and device time within 0.25 s of the
  epoch right after, then advancing at wall rate. `none` expects nothing on the
  input: `SOFT_ALIGNED` and the `PPS_LOCK_CHANGE` event.
- There is no scope on the rig. Timing evidence is the completion records
  (`executed == requested`, valid because libdatax spaces reported commands at
  `REPORT_SPACING`) plus an independent PLL readback of chip A through the phy's
  `frequency_hopping_mode` attribute. Evidence records the measured limits, the
  Tick counters (`late_count`, `overflow_count`, `pps_present`, `time_aligned`)
  and the image hashes.
- Beyond the planned set, each of these exists because only hardware can prove it:
  `test_trigger_low_after_flush` (a flush with the trigger high must not leave
  the next tune one behind), `test_dense_hops_library_readback_each_edge` (pulses under
  10 ms move the PLL; pulse width recorded), `test_readback_exact_across_band`
  (2400/2450/2500 MHz), `test_fhm_enable_readback` / `test_reenable_reprimes`
  (a re-stage lands the first edge on the init frequency, observed by sampling
  the PLL from a parked frequency) and `test_phase_sync_attribute_listing` (lists
  the phy attributes mentioning phase or sync; documents why `PHASE_COHERENT`
  is false).
- The tests restage and recreate the scheduler as needed; libdatax disables FHM
  when the context closes. Run the file once per boot of the board.
