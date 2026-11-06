# P2 hardware evidence

Recorded runs of `tests/hardware` (immediate streaming, digital loopback) on
two boards, from controller-run sessions on 2026-11-05 with the same suite the
`hw-test.yml` workflow runs. Each pair is the evidence JSON written by the
suite (`DATAX_HW_EVIDENCE`) and the pytest JUnit XML. Host names and addresses
are redacted (the JUnit `hostname` attribute is removed).

| Files | Board | libdatax commit | Result |
|---|---|---|---|
| `lablp-adrv9361-z7035.json` / `.xml` | ADRV9361-Z7035 (AD9361) | `dedc635f2c9e34fc32570697bf86322c24241d32` | 10/10 passed |
| `tron-adrv9009-zu11eg.json` / `.xml` | ADRV9009-ZU11EG | `d49c56ca1d5ec8fd241f0bac48624804ddab8a4c` (tree-identical to `dedc635`) | 12/12 passed |

libiio pin for both: `6534f14215f4738cec224bf50f13f475fe5264a6`
(`feature/daqiri-udp`, see `pins.json`); libiio context version `0.26-5e1e9cb`.

## PL provenance

- ADRV9361-Z7035: stock Kuiper 2023_R2 PL (`hdl_system_id` branch
  `hdl_2023_r2`, git `2156ac7e`), stock kernel 6.1.70.
- ADRV9009-ZU11EG: the PL was **not** stock 2023_R2. It is the HDL `main`
  build at git `085a6ed7` (2026-06-09; the recovery bitstream), run with the
  stock 2023_R2 Kuiper kernel and device tree. Results here describe that
  combination only.

The evidence files predate the provenance fields added to the suite
(`datax_module`, `libdatax_library`, `libdatax_version`); later runs record them.

## Results

Both: no failures, no skips. Throughput is recorded, never asserted (the
10 s RX test counts overflows and reports them via `status_polls`/`overflows`).

| Test | ADRV9361-Z7035 | ADRV9009-ZU11EG |
|---|---|---|
| `test_family_and_channels`, `test_caps_row` | pass (2 channels) | pass (4 channels) |
| `test_rx_block_nonconstant_per_channel` | pass | pass |
| `test_recv_timeout_recovers` | pass | pass |
| `test_tx_oneshot_drains` | pass (drain 5.4 ms) | pass (drain 3.0 ms) |
| `test_tx_cyclic_start_stop`, `test_start_stop_cycles` | pass | pass |
| `test_rx_throughput_recorded` | 47.1 MB/s in this run (about 41-47 MB/s across runs) | 53.1 MB/s |
| `test_digital_loopback[ch*]` | ch0, ch1: 0 mismatches | ch0-ch3: 0 mismatches, gain 1 |

Loopback transforms (asserted bit-exact after aligning the lag):

- ad936x (AD9361): RX = TX >> 4.
- adrv9009: I and Q exchanged (RX I = TX Q, RX Q = TX I), gain 1, no shift.
