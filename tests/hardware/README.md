# Hardware suite

Streaming smoke (REQ-TEST-021, streaming part) and digital loopback
(REQ-TEST-030, digital part) against a real board. Not part of Tier 1: CI only
runs `pytest tests/hardware --collect-only -q` to prove it collects.

    export DATAX_HW_URI=<libiio uri of the board>      # required to run
    export DATAX_HW_REQUIRE=1                          # fail, not skip, without a URI
    export DATAX_HW_EVIDENCE=/path/evidence.json       # default ./evidence.json
    export DATAX_LIBRARY=$PWD/build/libdatax.so
    pip install -e . numpy pytest
    python -m pytest tests/hardware -v

Without `DATAX_HW_URI` every test skips (or fails with `DATAX_HW_REQUIRE=1`).

Notes:

- The loopback test switches a debug attribute and restores it to 0 in `finally`.
  ad936x: `loopback` on `ad9361-phy` (RX = TX >> 4, asserted bit-exact).
  adrv9009: `bist_framer_a_loopback` on `adrv9009-phy` and, when present,
  `bist_framer_b_loopback` on `adrv9009-phy-b` (unverified on hardware; a
  missing attribute fails with its name).
- Debug attributes use libiio v1 through ctypes on the libiio already loaded by
  libdatax (`DATAX_LIBIIO_LIB` overrides); pylibiio targets v0 and is not used.
- `evidence.json` holds the redacted URI, the libiio pin, the libdatax commit,
  the IIO context description/version attributes and per-test measurements
  (throughput, overflow counters, EVM). Never commit one with a real host.
- RX status-register polling is asserted only when the RX streaming device has
  `direct_reg_access`; overflows are recorded, never asserted.
