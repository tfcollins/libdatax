# Hardware suite

Streaming smoke (REQ-TEST-021, streaming part), digital loopback
(REQ-TEST-030, digital part), the P3a timed-control tests (scheduler, FHM
hops, device time and PPS, MCS) and the P3b timed-stream tests (timed
capture, loopback alignment, gated TX burst, chip-B FHM) against a real board. Not part of Tier 1: CI only
runs `pytest tests/hardware --collect-only -q` to prove it collects.

    export DATAX_HW_URI=<libiio uri of the board>      # required to run
    export DATAX_HW_REQUIRE=1                          # fail, not skip, without a URI
    export DATAX_HW_EVIDENCE=/path/evidence.json       # default ./evidence.json
    export DATAX_LIBRARY=$PWD/build/libdatax.so
    pip install -e . numpy pytest
    python -m pytest tests/hardware -v

    export DATAX_HW_IMAGE=timed-dma                    # or hopcap-sfp, or stock (default)
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

## Images and markers

`DATAX_HW_IMAGE` names the PL image (the boot sets are pinned by hash in
`images.json`). Markers decide what runs where:

| Marker | `stock` | `hopcap-sfp` | `timed-dma` |
|---|---|---|---|
| (none: P2 RX smoke) | runs | runs | runs |
| `needs_tx` (one-shot and cyclic TX, digital loopback) | runs | skips: TX DMA feeds vrt49's deframer | runs: stock TX DMA restored |
| `p3a` (`test_hw_timed.py`, `test_hw_mcs.py`) | skips | runs | runs |
| `p3b` (`test_hw_timed_stream.py`) | skips | skips | runs |

`timed-dma` is hopcap's `timed-dma` profile: the hopcap-sfp image plus Tick's
RX and TX DMA gates, the RX capture tagger and chip B's FHM pin, with the
stock TX DMA to the DAC (vrt49 `VRT_DAC_TX_ENABLE=0`). So on it the whole P2
suite (TX included) and the P3a suite run as the regression of the P3b
suite. `test_caps_row` expects the P3b flags (`rx_arm`, `tx_arm`,
`stream_gate`, `timestamps`) on `timed-dma` and nowhere else.

## P3a timed control (`test_hw_timed.py`, `test_hw_mcs.py`)

- Runs on `hopcap-sfp` (ADRV9009-ZU11EG with hopcap's SFP image: Tick, the FHM
  trigger and the Tick PPS selector) and on `timed-dma`; on `stock` it skips.
- The GPIO staircase and the late-GPIO test use Tick GPIO bits 3:2. Bits 1:0
  are the FHM triggers (chip A; chip B on `timed-dma`), and libdatax refuses a
  `queue_gpio` that touches them.
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

## P3b timed streams (`test_hw_timed_stream.py`)

Runs on `timed-dma` only (REQ-TEST-022/023). There is no scope on the rig.
The evidence is the block-0 capture tag, Tick's capture-tag registers
(`edge_capture_*`), sample positions through the Talise framer loopback,
completion records and chip B's PLL readback. Throughput is never asserted.

- `test_p3b_caps_and_gate_map`: Tick's `stream_gate_map` equals `images.json`'s
  (so the board runs the pinned image), and every P3b flag is set.
- `test_timed_capture_block0` (a): RX `start_at(now + 200 ms)` on one channel.
  Block 0's `timestamp_ticks` must be `T + round_half_up((1 + strip) x tick_hz / fs)`
  (`strip = ceil(capture_tag_header_bytes / 4)` = 8 frames), with
  `valid_samples` short by the strip. Independently, Tick's
  `edge_capture_ticks` (raw ticks of the latest `STREAM_GATE` assert, read
  before `stop`) must equal T (bracketed by the soft offset when the time is
  soft-aligned). A late opening is retried once with a 500 ms lead and
  recorded.
- `test_loopback_alignment` (b): with the framer loopback on, a cyclic QPSK TX on
  every channel (this image refuses a partial-mask TX `start_at`) and one RX
  channel start at the same T, 10 times. Recorded per run: the RX index of the
  first non-zero sample, the pattern's lag, both `start_at` call times (the
  `ip:` setup cost) and mismatches against the looped-back pattern (asserted 0
  after the first 2 samples). Every run's offset must be within +-1 sample of the median (spread up
  to 2): the offset combines two independent gate releases, TX and RX,
  each a fixed latency +-1 device clock.
  A late `start_at` is retried (at most 3 late runs); (c) uses the median as
  its reference only when this test passed.
  The offset is TX release, DAC path, framer loopback and RX together, against
  the RX release, so the median is recorded as the release-latency measurement
  of this image; the RX-only fabric latency cannot be separated here.
- `test_gated_burst` (c), run 3 times (the gate latencies must be fixed): a
  cyclic tone (one cycle per 4096 frames) on every TX
  channel and one RX channel start at T; the TX gate closes at T + 20 ms and
  opens at T + 40 ms (both reported; both must complete OK on time). The
  muted gap's edges must sit at (b)'s offset minus the 3 reads a TX
  `start_at` mutes (G2/G3 ruling 1) plus the commanded offset, +-1 sample;
  the gap length and the two residuals must agree within 1 sample too.
  Contiguity is proven by matching the whole capture exactly against the
  looped-back tone at one lag (a closed gate mutes, it does not pause). The lag
  is found by exact equality, not by correlation: every shift of a single
  complex tone is the tone times a complex gain, so a gain-fitted correlation
  has no peak (tron run 2 took lag 1135 for a tone at 111 that way). To locate
  any break, the evidence holds the lag of each audible stretch and of each
  block's first period, mismatches per block, the first mismatching indices
  and each block's first and last frames. Every block read is one of
  the 4 enqueued before T (3 Mi frames each: 48 MiB, 51 ms), so each must
  carry libdatax's derived timestamp, not discontinuous, equal to the
  suite's own derivation from block 0's tag. The start mute measured from
  the edges is recorded beside the assumed 3.
- `test_chip_b_lo_unmoved_by_fhm_enable` (plan gap 4): with chip B's FHM off
  and its LO parked at 2.43 GHz, `stage_hop_table` (which enables FHM on both
  chips when the map names `fhm-b`) must leave chip B's live PLL there.
- `test_chip_b_timed_tune`: four timed tunes on channel 2 (chip B, Tick GPIO bit
  1), each OK on time with chip B's PLL reading the plan; chip A's PLL must not
  move.

Over `ip:` the host cannot drain 245.76 MSPS: the ring enqueued before T fills
and the DMA overflows before the host reads block 0. Every block enqueued
before T is filled back to back from the release, so libdatax attributes an
overflow seen while those blocks are taken to the first re-armed block
(block `block_count`): the pre-T blocks keep their timestamps, and that block
and every later one read discontinuous with `timestamp_ticks = 0`. On a v0 iiod the kernel ring is 4
x `block_samples` whatever `block_count` asks for, so the suite always uses
4 blocks.
