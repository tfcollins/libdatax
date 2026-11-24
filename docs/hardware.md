# Supported hardware and the hardware suite

`tests/hardware` exercises immediate streaming, digital loopback, (P3a)
timed control and (P3b) timed streams on a real board. It is not part of Tier 1: CI only proves it collects
(`pytest tests/hardware --collect-only -q`). The `hw-test.yml` workflow runs it
on the lab's self-hosted runners, dispatched and twice a week (Sunday and
Wednesday); recorded results are in [evidence/p2](evidence/p2/README.md),
[evidence/p3a](evidence/p3a/README.md) and [evidence/ci](evidence/ci/).

## Supported boards

libdatax drives a board when it has a device family for the board's
transceiver. Two families exist: `ad936x` and `adrv9009`. Status words follow
`support.json` (*validated* means `hw-test.yml` passed on that board and the
evidence is committed here).

| Board | Family | Channels | Image | Validated | Labgrid place | hw-test.yml job | Evidence |
|---|---|---|---|---|---|---|---|
| ADRV9009-ZU11EG on ADRV2CRR-FMC (dual ADRV9009) | `adrv9009` | 4 RX / 4 TX | pinned boot sets (`images.json`): `stock`, `hopcap-sfp`, `timed-dma` | streaming RX/TX; timed GPIO, FHM timed tunes, device time, MCS verification (P3a) | `tron` | `tron` | [2026-11-11](evidence/ci/2026-11-11-tron/README.md) |
| ADRV9009 on ZC706 (ADRV9009-W/PCBZ) | `adrv9009` | 2 RX / 2 TX | Kuiper 2023_R2_P1 | streaming RX/TX | `nemo` | `kuiper` (`zc706`) | [2026-11-23](evidence/ci/2026-11-23-zc706/README.md) |
| ADRV9361-Z7035 on ADRV1CRR-FMC | `ad936x` | 2 RX / 2 TX | Kuiper 2023_R2_P1 | streaming RX/TX | `lablp` | `kuiper` (`lablp`) | [2026-11-23](evidence/ci/2026-11-23-lablp/README.md) |

Timed control (P3a) needs Tick in the PL, which only the ZU11EG's `hopcap-sfp`
and `timed-dma` images have; on the other boards and images the P3a tests
skip. Timed streams (P3b) need the `timed-dma` image (Tick's DMA gates, the
capture tagger, chip B's FHM pin). They are validated nowhere yet: the
`timed-dma` column below is not yet validated.

**Not supported.** The other lab boards have no libdatax device family, and
opening them fails with `ENOTSUP` "unsupported device family": AD9081 on ZCU102
(`mini2`), ADRV9371 on ZC706 (`bq`) and FMCDAQ3 on VCU118 (`nuc`). tick and
vrt49 test those boards in their own repositories.

### Known-good versions

What the validating runs recorded (the evidence JSON's `iio_context` block).
Use these when reproducing a result; other versions are untested, not
known-bad.

| | ADRV9009-ZU11EG (`stock`) | ADRV9009-ZU11EG (`hopcap-sfp`) | ADRV9009-ZU11EG (`timed-dma`), not yet validated | ADRV9009 / ZC706 | ADRV9361-Z7035 |
|---|---|---|---|---|---|
| PL (`hdl_system_id`) | HDL `main` `085a6ed7` (recovery bitstream) | `adrv9009zu11eg_vrt49` on `corundum` (hopcap `ac07d56`: Tick, FHM trigger, PPS selector; XSA `6292e762`) | hopcap `--profile timed-dma` (hopcap `c342514`, boot set from `36a1710`; tick `1787c33`; vrt49 PR #91 `0c8764d`, `VRT_DAC_TX_ENABLE=0`; XSA `44cce693`): hopcap-sfp plus Tick's DMA gates, the capture tagger, chip B's FHM pin and the stock TX DMA | `hdl_2023_r2` `2156ac7e`, project `adrv9009` on `zc706` | `hdl_2023_r2` `2156ac7e`, project `adrv9361z7035_ccfmc`, LVDS |
| Kernel | Kuiper 2023_R2 `6.1.70-284114-g2e8908932dfd` (aarch64) | hopcap's build `6.1.70-g2e8908932dfd-dirty` | hopcap's build (ADI linux `2e89089`), to be recorded by the first run | Kuiper 2023_R2 `6.1.70-284114-g2e8908932dfd` (armv7l) | same as ZC706 (armv7l) |
| Device tree | Kuiper 2023_R2 | hopcap's | hopcap's, with `adi,stream-gate-map` on the Tick node | Kuiper boot folder `zynq-zc706-adv7511-adrv9009` | Kuiper boot folder `zynq-adrv9361-z7035-fmc` |
| iiod | stock Kuiper `0.26-5e1e9cb` | same | same (expected) | same | same |
| RX DMA device | `axi-adrv9009-rx-hpc` | same | same (expected) | `axi-adrv9009-rx-hpc` | `cf-ad9361-lpc` |
| libiio (host) | the pinned v1 fork (`pins.json`) | same | same | same | same |
| labgrid-plugins | `pins.json` | same | same | same | same |

### ADRV9009-ZU11EG (`tron`)

- **Image.** Not a stock Kuiper image. `hw-test.yml` JTAG-boots one of the boot
  sets pinned by SHA-256 in `tests/hardware/images.json` (`stock` for P2,
  `hopcap-sfp` for P3a, `timed-dma` for P3b with P2 and P3a as its
  regression). The files live on the lab runner, not in this
  repository, and `scripts/hw/tron-run.sh` refuses any file whose hash differs.
  There is no public download for them yet.
- **Never reprogram the PL at runtime.** It kills the JESD links; reboot instead.
- **Run.** In the lab, dispatch `hw-test.yml` with `place=tron` (`image=both`
  boots `stock` then `hopcap-sfp`; `image=timed-dma` boots only that one). By
  hand, boot one of the images, then follow [Run it](#run-it) with
  `DATAX_HW_IMAGE` set to its name.
- **Expected.** `stock`: the P2 suite passes, P3a and P3b skip. `hopcap-sfp`: P3a
  and P2 RX pass; the TX tests skip, because TX DMA feeds vrt49's deframer on
  that image; P3b skips. `timed-dma` (not yet validated): everything runs, P2
  with TX and loopback, P3a and P3b; nothing skips.
- **Board facts.** TX scan channels are `voltage0..7` (interleaved I/Q), and RX
  uses `voltage{n}_i/_q`. The first timed tune must be at least now + 20 ms ahead.
  `PHASE_COHERENT` stays false: the device tree enables no RF PLL phase sync.

### ADRV9009 on ZC706 (`nemo`)

- **Image.** Kuiper 2023_R2_P1 on the ZC706's SD card, with the boot partition
  set up from it: `BOOT.BIN` and `devicetree.dtb` from
  `zynq-zc706-adv7511-adrv9009`, `uImage` from `zynq-common`. This is the stock ADI reference design, and it needs no extra
  setup.
- **Run.** In the lab, dispatch `hw-test.yml` with `place=zc706`, or from a host
  with labgrid-plugins at the `pins.json` revision:

      DATAX_LAB_COORDINATOR=<coordinator> scripts/hw/kuiper-run.sh zc706 adrv9009 zc706 out/

  This asks labgrid for part `adrv9009` on carrier `zc706`. The place powers the
  board and waits for it to autoboot from SD, then exports its IIO URI. By hand,
  boot the board, then follow [Run it](#run-it) with `DATAX_HW_IMAGE=stock`.
- **Expected.** 10 passed and 21 skipped (the 15 P3a and 6 P3b tests): RX on both channels,
  receive-timeout recovery, one-shot and cyclic TX, start/stop cycles, RX
  throughput, and digital loopback bit-exact on ch0 and ch1.
- **Board facts.** A single ADRV9009, so 2 RX / 2 TX, no MCS, and chip B absent.
  Loopback is the same framer loopback as the ZU11EG: I and Q exchanged, unit gain.

### ADRV9361-Z7035 on ADRV1CRR-FMC (`lablp`)

- **Image.** Kuiper 2023_R2_P1 on the SOM's SD card, with the boot partition set
  up from it: `BOOT.BIN` and `devicetree.dtb` from `zynq-adrv9361-z7035-fmc`,
  `uImage` from `zynq-common`. This is the stock ADI reference design
  (`adrv9361z7035_ccfmc`, LVDS).
- **Run.** In the lab, dispatch `hw-test.yml` with `place=lablp`, or:

      DATAX_LAB_COORDINATOR=<coordinator> scripts/hw/kuiper-run.sh lablp adrv9361z7035 adrv1crr-fmc out/

  By hand, boot the board, then follow [Run it](#run-it) with
  `DATAX_HW_IMAGE=stock`. The board takes about 3 minutes to boot.
- **Expected.** 10 passed and 21 skipped (P3a and P3b): the same set as the ZC706. Digital loopback
  (`loopback`=1 on `ad9361-phy`) is bit-exact as RX = TX >> 4.
- **Board facts.** The carrier's USB console (CP2103) is powered by the board,
  so it appears only a few seconds after power-on. labgrid-plugins `ec8bf30`
  and later reconnect a console that drops then; older pins fail the first boot
  with "Broken pipe".

### When a kuiper leg fails

`kuiper-run.sh` passes through `adi-lg request`'s exit code: 10 = no catalog
entry for the part (or no place with it), 11 = the board stayed busy past
`--wait`, 12 = the boot failed. Failures seen while bringing these legs up,
and their fixes:

- `unknown part` (10): the part was missing from the coordinator's board
  catalog (labgrid-plugins `coordinator/api/board_catalog.yaml`). Add it and
  redeploy the coordinator API.
- Boot failed with rsync code 255 (12): the exporter host did not trust the
  runner host's SSH key, and labgrid stages files over SSH.
- Boot failed with "Broken pipe" (12): the board-powered console on the
  Z7035 carrier. Use labgrid-plugins `ec8bf30` or later.
- The retry failed with "Address already in use" (12): the TFTP server's
  fixed port was still bound from the first attempt. labgrid-plugins #193
  (included in that pin) takes the port from the OS instead.

### Throughput and overflows (all boards)

`test_rx_throughput_recorded` pulls one channel for 10 s over the `ip:`
transport (iiod over TCP) and records MB/s and the RX core's overflow count.
Neither is asserted. That transport runs far below the converter rate, so the RX
DMA overflows during the pull on every board. The overflows are expected and do
not mean the board is broken.

| Board | Run | MB/s | Overflows in 10 s |
|---|---|---|---|
| ADRV9009-ZU11EG (`stock`) | 2026-11-11 | 52.1 | 92 |
| ADRV9009 / ZC706 | 2026-11-23 | 31.8 | 64 |
| ADRV9361-Z7035 | 2026-11-23 | 34.5 | 68 |
| ADRV9361-Z7035 (controller-run P2) | 2026-11-05 | 47.1 | 46 |

## In CI (hw-test.yml)

- **tron** (ADRV9009-ZU11EG, runner label `hw-nemo`): `scripts/hw/tron-run.sh`
  JTAG-boots each image pinned by hash in `tests/hardware/images.json` through
  labgrid, runs this suite, then restores and powers the board off. Lab facts
  (coordinator, addresses, artifact directory) come from the runner's `.env`,
  never from this repository. Logs and artifacts pass through
  `scripts/hw/redact.py` before they are printed or uploaded.
- **kuiper** (the ZC706 and Z7035 boards, also on `hw-nemo`):
  `scripts/hw/kuiper-run.sh` asks labgrid for the board by part
  (`adi-lg request --mode uri`). The coordinator's board catalog picks the
  Kuiper image, and the place's own boot strategy boots it. The suite then runs
  against the exported URI with `DATAX_HW_IMAGE=stock`.
- The `place` input picks one leg (`tron`, `zc706`, `lablp`) or `all`. The
  `image` input picks the tron boot sets: `both` (default, and the schedule:
  `stock` then `hopcap-sfp`), one of them, or `timed-dma` (dispatch only until
  its rows are validated).
- The repository is public, so the lab runners accept only this workflow on
  `main`, dispatched or scheduled by the owner (a job-started hook on the runner
  enforces it), and fork pull requests need approval.

## Run it

    cmake -S . -B build -DDATAX_LIBIIO_SOURCE=<libiio src> -DDATAX_LIBIIO_BUILD=<libiio build>
    cmake --build build -j
    pip install -e . numpy pytest
    export DATAX_LIBRARY=$PWD/build/libdatax.so
    export DATAX_HW_URI=<libiio URI of the booted board>
    export DATAX_HW_REQUIRE=1                       # fail, not skip, without a URI
    export DATAX_HW_EVIDENCE=$PWD/evidence.json     # default ./evidence.json
    export DATAX_HW_IMAGE=timed-dma                 # or hopcap-sfp; stock (default) skips P3a and P3b
    python -m pytest tests/hardware -v --junitxml=junit.xml

The libiio build must be the pinned v1 fork (`pins.json`); it talks to the
boards' stock v0 `iiod` unchanged. The evidence JSON records the redacted URI,
the libdatax commit, `datax_module`, `libdatax_library` (the loaded
`libdatax.so`), `libdatax_version`, the libiio pin, the IIO context attributes
and per-test measurements (throughput, overflow counters, loopback lag and
mismatches). Check it for host names, addresses and board serials before
committing one anywhere.

## Boot prerequisites (all boards)

The suite does not boot or reconfigure boards. Before running:

- The board is booted with the image its runbook above names, and reachable over
  IIO (an `ip:` URI to its iiod).
- `direct_reg_access` and the loopback debug attributes must be present
  (stock kernels have them). A missing one fails the test naming it.
- Nothing else uses the board's RX/TX DMA (one stream per direction).

## Loopback facts

The digital loopback test enables the transceiver's debug loopback, restores it
in `finally`, and compares RX to the expected transform of TX:

- ad936x: `loopback` on `ad9361-phy`; RX = TX >> 4.
- adrv9009: `bist_framer_a_loopback` on each phy (`adrv9009-phy`, and
  `adrv9009-phy-b` when present); I and Q are exchanged (RX I = TX Q,
  RX Q = TX I), gain 1, no shift.

## P3a timed control

Needs the ADRV9009-ZU11EG with hopcap's SFP image (Tick scheduler, FHM trigger
on chip A, Tick PPS selector) and `DATAX_HW_IMAGE=hopcap-sfp`, or the
`timed-dma` image, which carries the same. On `hopcap-sfp` the TX tests skip
(the TX DMA feeds vrt49's deframer) and the P2 RX smoke runs as the
regression. `DATAX_HW_PPS=internal|none` says what drives Tick's PPS input
(Corundum's internal PTP PPS by default). Timing evidence is completion records
and PLL readback; there is no scope, and the evidence says so. See
`tests/hardware/README.md` for the test list.

## P3b timed streams

Needs the `timed-dma` image and `DATAX_HW_IMAGE=timed-dma` (not yet
validated). `test_hw_timed_stream.py` checks block 0's hardware timestamp
against Tick's edge capture, the first-sample offset of a TX pattern through
the framer loopback over 10 runs (within +-1 sample: REQ-TEST-023, and the
image's release latency), a TX burst gated off and on at 20 ms through the
loopback, and chip B's FHM (timed tunes with PLL readback; enabling FHM must
not move its LO). Over `ip:` only the blocks enqueued before T carry
timestamps: the ring overflows before the host reads it, and libdatax
attributes that to the first re-armed block, which reads discontinuous with
`timestamp_ticks = 0` from there on. See `tests/hardware/README.md` for the details.

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
