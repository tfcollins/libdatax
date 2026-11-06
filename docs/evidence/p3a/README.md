# P3a hardware evidence

This is the recorded run of `tests/hardware` (P3a timed control, plus the P2
regression) on the ADRV9009-ZU11EG at labgrid place `tron`. It comes from a
controller-run session on 2026-11-06, using the same suite that the
`hw-test.yml` workflow runs. The files are:

- the evidence JSON written by the suite (`DATAX_HW_EVIDENCE`);
- the pytest JUnit XML.

Host names and addresses are redacted: the JUnit `hostname` attribute is
removed and absolute paths are made relative to the repo.

| Files | Board | libdatax commit | Result |
|---|---|---|---|
| `tron-adrv9009-zu11eg.json` / `.xml` | ADRV9009-ZU11EG | `3392cd7088bfee9e0958da634c8ed95a03701471` (main, phase exit) | 21 passed, 6 skipped |

The libiio pin is `6534f14215f4738cec224bf50f13f475fe5264a6` (`feature/daqiri-udp`;
see `pins.json`). The libiio context version is `0.26-5e1e9cb`.

## Image (REQ-TEST-024)

The image is hopcap's SFP image with the Tick PPS selector (hopcap `ac07d56`),
JTAG-booted. It is not a stock design.

- XSA sha256 `6292e7622810612a4cd19b3a1774ac84630bbbbedc997720fb0f650de13304c9`
  (`hdl_system_id` project `adrv9009zu11eg_vrt49`).
- Kernel `Image` `18e592b58b4e589f2d61b7d4fe5db925233a2f1cf2a16c44da518f8341a925be`
  (6.1.70) and device tree `1acf636a0ac124bbdb2dc94c3ef63aa4f152f47a0571a3d83e05df21a809963b`.
- Tick drives FHM on chip A (`TAL_GPIO_0`, GPIO bit 0), and bits 3:1 drive the hop
  controller's slot index.
- Tick's PPS input comes from `tick_pps_select`. It uses the external PMOD input
  when that input qualifies, and otherwise Corundum's PTP PPS. Nothing was
  connected externally, so this run used the internal PPS (`pps_source` =
  `internal`).
- On this image TX DMA feeds vrt49's deframer, so the six P2 TX and loopback
  tests skip with that reason. P2 RX runs as the regression.

## Results

Timing evidence is the Tick completion records (`executed` vs `requested`) and
PLL readback. This rig has no scope, so REQ-TEST-023's scope captures are not
part of this evidence, and the records say so (`independent_check`).

| Test | Result |
|---|---|
| `test_staircase` | 8 GPIO edges at 14.4 ms spacing (2x report spacing); every completion `OK`, skew 0 ticks |
| `test_hop_schedule` | 4 timed tunes, all `OK`, PLL readback equal to the plan |
| `test_priming_edge_lands_on_init` | the priming edge lands on `fhmInitFrequency` (2450 MHz seen between hops) |
| `test_dense_hops_library_readback_each_edge` | 12 hops at 15.0 ms (2x report spacing), all on time, all reported |
| `test_dense_hops_x1_2_measurement` | 12 hops at 9.0 ms (1.2x report spacing, 4.5 ms pulses): held, all on time (a measurement, not a guarantee) |
| `test_late_dropped` | a deliberately late command completes `LATE`, `executed` 0, PLL unchanged |
| `test_trigger_low_after_flush` | the FHM trigger bit is low after a flush; the next tune is on time |
| `test_readback_exact_across_band` | exact PLL readback at 2400, 2450 and 2500 MHz |
| `test_fhm_enable_readback`, `test_reenable_reprimes` | FHM enable reads back; re-enable re-primes, and the next tune is on time |
| `test_pps_alignment` | `pps_present` 1 with nothing on the external input; `PPS_ALIGNED`, Tick `time_aligned` 1; device time advanced 2.0022 s over 2 PPS |
| `test_tune_after_alignment` | a hard PPS load moves device time from about 1002.5 s back to 0; the next tune primes again and completes `OK`, skew 0, PLL read back at 2460 MHz |
| `test_run_mcs_verifies` | JESD FSM at `opt_post_running_stage`; `run_mcs` returns 0 |
| `test_phase_coherent_false`, `test_phase_sync_attribute_listing` | `PHASE_COHERENT` false: `adi,dig-clocks-rf-pll-phase-sync-mode` is present but not enabled on either phy |
| P2 RX (`test_family_and_channels`, `test_rx_block_nonconstant_per_channel`, `test_recv_timeout_recovers`, `test_rx_throughput_recorded`) | pass; 51.7 MB/s on one channel (recorded) |

## Measured limits on this board

From `datax_sched` limits, as measured at scheduler create:

- tick rate: 100 MHz;
- report spacing: about 7.2–7.5 ms in this run (3 × the measured completion drain; 5.4–5.8 ms in run 4, so it varies between runs);
- FHM stage write: about 0.55 ms;
- guard margin: 10 ms.

The first timed tune needs at least now + 20 ms.

## History

- **Run 2** (tests at `5e36906` on library `50401d4`): 18 passed and 2 failed
  with "stage late" on the second tune. The root cause was a priming lead
  shorter than the poll-hook path. Fixed in `1d8c6c9` (lead ≥ guard; the
  poller wakes at the priming edge).
- **Run 3** (`1d8c6c9`): 20 passed.
- **Run 4** (`ac2d54a`, after the stage-late review follow-ups): 20 passed.
- **Run 5** (`fb2c0cf`, with the final-review fixes): 20 passed and 1 failed.
  The new `test_tune_after_alignment` was refused as LATE after a hard PPS
  load, because the poller's cached device time was still in the pre-load
  domain. Fixed in `7679d86`: the snapshot is re-read on every change to the
  time mapping.
- **Run 6** (this evidence, `3392cd7`): 21 passed.
