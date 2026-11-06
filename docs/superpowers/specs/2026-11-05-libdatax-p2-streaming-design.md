# Design: libdatax P2 — streaming MVP over AXI-DMAC

Date: 2026-11-05
Status: implemented 2026-11-05 (libdatax main, phase exit); both labgrid places green — see "Phase exit evidence". Every decision below was the implementer's call under the owner's standing instruction ("do not wait for my input").
decision below is the implementer's call, marked as such, for the owner to check while reading.
Decisions 2 and 3 are proven on `lablp` by the spike below; `tron` is proven by plan task 1.
Board: issue #24 on <https://github.com/users/tfcollins/projects/4>

## Intent

Roadmap P2: "Stream objects over AXI-DMAC on both boards, stock reference designs. Immediate
start/stop, RX/TX, cyclic TX, zero-copy paths." Exit: REQ-TEST-021 smoke green on both labgrid
places (`tron`, ADRV9009-ZU11EG; `lablp`, ADRV9361-Z7035) and REQ-TEST-030 loopback on both.

P1 left every `datax_stream_*` symbol returning `-ENOTSUP`. P2 fills in the immediate subset so
an application can move correct samples in and out of either radio through one API, from a host
on the lab network, with no DMA vocabulary. Everything timed (`start_at`, gating, timestamps)
stays P3: P2 must leave the stream object shaped so P3 adds the arm/release path without an API
change.

## Decisions (implementer's calls, 2026-11-05)

1. **Transport: libiio v1 `iio_buffer` + `iio_block` over the stock AXI-DMAC devices.** No
   `iio_stream` (its fixed rotation hides the block handle the zero-copy API returns). vrt49's
   `vrt49://` backend is **out of P2**: it needs libiio v0 (decision 1 of P1) and the
   network-sample surface is roadmap P6. The roadmap's "candidate transport" line is amended to
   say so.
2. **libdatax runs on a host and reaches the boards' stock `iiod` over `ip:`.** Both boards run
   Kuiper `2023_R2_P1` images whose `iiod` speaks the v0 text protocol. libiio v1's network
   backend falls back to it: `create_block` returns `-ENOSYS` and the block layer uses
   `readbuf`/`writebuf` (libiio `block.c`, `network.c`). P2 relies on that path and its
   hardware smoke is the first proof that it works; `local:` on the board is not a P2 target
   (it would need libiio v1 cross-built into Kuiper).
3. **REQ-TEST-030 in P2 is a digital loopback** on both boards: AD9361's `loopback` debug
   attribute (`1` = TX data looped to RX inside the chip) and ADRV9009's
   `bist_framer_a_loopback` / `bist_framer_b_loopback` debug attributes (deframer output into
   the framer). Neither board has a recorded RF loopback cable (`~/dev/infra/HOSTS.md`), and a
   digital loopback is exactly the property the requirement states ("the streaming path moves
   *correct* samples"). RF flatness and EVM through a cable stay open under REQ-TEST-030 for the
   phase that fits cables; the requirements page says so in the same change set.
4. **Raw 16-bit samples, hardware frame layout.** Samples are int16 I/Q, host byte order, in the
   DMA frame order: per sample instant, enabled radio channels in ascending index, each as I then
   Q. One "sample" in every count below is one such instant across all enabled channels. No
   scaling, no format conversion (AD9361's 12-bit samples arrive sign-extended in 16 bits from
   the fabric, as pyadi-iio reads them). This makes copying and zero-copy the same layout.
5. **Python streams need numpy.** `RxStream.recv()` returns a complex64 array shaped
   `(channels, n)`; `recv_raw()` returns the int16 frame array. numpy stays an optional extra of
   the package; importing a stream without it raises `datax.CapabilityError` naming the extra.

## Spike (2026-11-05, lablp, stock Kuiper 2023_R2_P1)

A throwaway C program on the libiio fork's v1 build (`build-emu`), against `ip:10.0.0.167`:

- **RX over the legacy `iiod` path works.** Buffer open, four 16384-sample blocks enqueued,
  start, 64 dequeue/re-enqueue rounds: about 43 MB/s (10.8 MS/s for one I/Q channel pair).
- **Cyclic TX works.** One 1024-sample block enqueued with `cyclic=true`, then start. The legacy
  protocol needs cyclic at buffer open while v1 sets it per enqueue; the fork's fallback carries
  it, so this is not a risk any more.
- **Digital loopback is bit-exact.** With `ad9361-phy` debug attribute `loopback=1`, RX equals
  the TX waveform shifted right by 4 (the 16-bit DAC word into the 12-bit ADC format, gain
  exactly 1/16), 0 mismatches over 1024 samples after alignment. So the AD9361 loopback test
  asserts bit-exactness after the shift, not just EVM.
- **Core registers read through `direct_reg_access`.** Writing `0x80000000 | offset` selects an
  AXI core register and reading returns it: RX core VERSION `0xA0300`; the status word at
  offset `0x88` read `0x4` both before and after streaming. Which bit means overflow is still for
  the builder to confirm from ADI HDL; the mechanism works.
- **No RF loop is wired.** With loopback off and TX running, RX stayed near the noise floor.
- Channel format on AD9361: 12 bits in 16, signed, no shift; the RX sample rate was 30.72 MS/s.
- Lab facts learnt (now in `~/dev/infra/HOSTS.md`): the board's address is `10.0.0.167`, not the
  place's stale `10.0.0.96`; `adi-lg request` has no part entry for this board; power goes
  through the plugin `HomeAssistantPowerDriver`.

Still unproven: the same path on `tron` (stock ZU11EG image; plan task 1), and Tier 1's coverage
of the legacy protocol. `iiod-emu` speaks only the binary protocol, so Tier 1 exercises the
libdatax logic and Tier 2 is the only coverage of the legacy wire path. That gap is accepted for
P2 and listed in the platform page.

## Goals

1. `datax_stream_create/destroy/start/stop/recv/send/get_block/put_block` implemented for the
   ad936x and adrv9009 families, RX and TX, one-shot and cyclic TX.
2. Python `dev.rx_stream(...)`, `dev.tx_stream(...)`, `RxStream`/`TxStream` with `with`-support,
   plus the pyadi-style conveniences `dev.rx(n)` and `dev.tx(data, cyclic=True)`.
3. Tier 1: every stream path exercised against `iiod-emu` in CI under the five-minute budget.
4. Tier 2: a hardware suite (`tests/hardware/`) that collects without a board and, on a board,
   runs REQ-TEST-021's streaming smoke and REQ-TEST-030's digital loopback; a dispatchable
   `hw-test.yml` that runs it on `tron` and `lablp` through labgrid.
5. Phase exit: both suites green on both places, evidence linked, `support.json`
   `streaming-rx` (and a new `streaming-tx`) rows flipped to validated for both platforms.

## Non-goals

- `datax_stream_start_at`, gating, in-band timestamps, `DATAX_CAP_RX_ARM`/`TX_ARM`/
  `STREAM_GATE`/`TIMESTAMPS` (P3). `start_at` keeps returning `-ENOTSUP` with last-error
  "timed start arrives with the scheduler (P3)"; `datax_block_info.timestamp_ticks` is 0.
- The event poller thread and event delivery. Overflow is reported per block
  (`discontinuous`), not as an event, until P3 adds the poller.
- Sustained-rate claims. P2 measures and records what the legacy `iiod` path achieves; it does
  not promise native rates (streaming-model's rate table already says the MVP path is
  host-link bound).
- `vrt49://`, DAQIRI, RF loopback through cables, ORx channels, `local:` on the board.
- Changing the boards' images: both run their stock Kuiper `2023_R2_P1` designs.

## Anti-goals

- A stream API that leaks libiio: no `iio_*` type in the public header beyond the existing
  `datax_context_get_iio_context()` escape hatch.
- Silent sample loss. A detected overflow marks the next block discontinuous; a path that cannot
  detect overflow says so in `doctor`, it does not claim continuity.
- A hardware test that passes on zeros: loopback must match a known waveform, smoke must see
  non-constant RX data.
- Marking a support row validated before both places are green (REQ-TEST-042).

## Constraints

P1's constraints carry over unchanged (C11 warning set with `-Werror`, MIT SPDX headers, opaque
handles, negative-errno returns, per-context last-error, libiio pinned in `pins.json`, Python
ctypes only, `git commit -s`, CI green before merge). Plus:

- The API page (`datax_sdk_sdr/docs/api-c-core.md`) is amended **before** the first code task
  (REQ-DOC-001) with the semantics below; the docs-symbol gate keeps passing.
- Tier 1 stays under five minutes and needs no lab host.
- Hardware jobs share the lab lock discipline: tron's jobs hold
  `/tmp/vrt49-corundum-hardware.lock` on nemo; lablp's run on the `hw-bq` runner (only bq has
  SSH to lablp, HOSTS.md). Never reprogram the ZU11EG PL at runtime.

## The stream object

### Device mapping (platform table gains streaming devices)

| Family | RX device | TX device | Radio channel n ↔ IIO channels |
|---|---|---|---|
| ad936x | `cf-ad9361-lpc` | `cf-ad9361-dds-core-lpc` | `voltage{2n}` (I), `voltage{2n+1}` (Q) |
| adrv9009 | `axi-adrv9009-rx-hpc` | `axi-adrv9009-tx-hpc` | `voltage{n}_i`, `voltage{n}_q` |

The builder confirms the exact names from the stock device trees and the drivers
(`cf_axi_adc_core.c`, `cf_axi_dds.c`, the adrv9009 platform XML) and records them in the
platform files, as P1 did for attributes. On the dual-chip ZU11EG both phys feed one RX and one
TX device (4R4T), matching P1's folded `datax_device`. A family whose streaming device is absent
returns `-ENOTSUP` from `stream_create` with last-error naming the missing device.

### Lifecycle

- `create(dev, cfg)`: validates `cfg` (direction; `channel_mask` non-zero and within
  `max_channels`; `block_samples` > 0; `block_count` ≥ 1, forced to 1 for cyclic; `cyclic` only
  for TX), enables the IIO channels, creates the `iio_buffer` and `block_count` blocks. Failure
  frees everything and sets last-error. A device holds at most one stream per direction
  (`-EBUSY` otherwise).
- `start()`: RX enqueues every block, then enables the buffer. One-shot TX enables the buffer;
  blocks are enqueued by `send`/`put_block`. **Cyclic TX defers the hardware enable** to its single
  `send`/`put_block`, which sizes the buffer, enqueues the block cyclic and then enables: over the
  v0 `iiod` protocol cyclic is fixed at buffer OPEN, which libiio derives from the first enqueue
  (libiio `buffer.c`/`block.c`; the spike's order). Enable errors of a cyclic stream therefore
  surface at `send`. Starting a started stream is `-EALREADY`.
- `stop()`: RX drains its queued blocks (stopping at the first failure), then disables; cancel
  left the buffer unable to restart (Task 2 finding). **One-shot TX drains**: it zero-pads and enqueues a held
  partial block, waits until every queued block has been handed to the device (each bounded by
  the context timeout), then disables; it returns `-ETIMEDOUT` if a block did not drain, after
  disabling anyway. When `stop()` returns 0, every sample passed to `send` has reached the DMA
  (the tail may still be in flight in fabric). Cyclic TX first takes its block back (on the legacy path that dequeue is where a failed
  write surfaces, and disabling first would park the worker under a pending token), then
  disables. Blocks return to the
  stream's free list; a later `start()` begins afresh. Stopping a stopped stream is 0.
- `destroy()`: stops if needed, frees blocks, buffer and channel masks. NULL is a no-op.
- `start_at()`: `-ENOTSUP` (P3).

### I/O

- `recv(dst, samples, info, timeout_ms)`: copies up to `samples` instants. It drains the current
  block from an internal cursor and dequeues the next only when needed, re-enqueuing drained
  blocks, so any `samples` value works against any block size. Returns the number of instants
  copied (> 0), or `-ETIMEDOUT` when nothing arrived in time, or another negative errno. `info`
  describes the first block touched by this call.
- `send(src, samples, timeout_ms)`: one-shot TX fills free blocks and enqueues each full block;
  a partial final block is held until the next `send` fills it or `stop()` flushes it padded
  with zeros. Cyclic TX takes exactly one call whose `samples` ≤ `block_samples`; **exactly
  `samples` instants repeat** (the buffer and block are sized to `samples` at that call, so
  `create()` allocates no hardware buffer for a cyclic stream) until `stop()`; a second `send`
  returns `-EBUSY`. Cyclic zero-copy: `get_block` lends a `block_samples` block and `put_block`
  repeats all of it.
- `get_block(&data, info, timeout_ms)`: RX returns the next filled block (`valid_samples` =
  `block_samples`); TX returns a free block to fill. `put_block(data)`: RX re-enqueues; TX
  enqueues the whole block (cyclic if the stream is cyclic). Mixing copying and zero-copy calls
  on one stream returns `-EINVAL`.
- `timeout_ms`: < 0 waits forever; 0 polls; > 0 waits up to that long. Over the legacy `iiod`
  path the block layer is synchronous, so the builder bounds the wait with the libiio context
  timeout; the API page states that the bound is approximate there.

### Overflow and underflow

The AXI ADC/DAC cores latch DMA overflow/underflow in their common `DMA_STATUS` register
(write-one-to-clear). After each RX dequeue (TX enqueue) libdatax reads it through the device's
`direct_reg_access` debug attribute, no more than once per 100 ms per stream, sets
`info.discontinuous` on the next RX block when the overflow bit was set, clears it, and counts
both in a per-stream counter readable through `datax_internal_stream_stats()` (test-only,
P1's pattern). If the register cannot be read (no debug access), detection is reported as
unavailable by `doctor` and `discontinuous` is always false. The builder confirms the register
offsets from ADI HDL `up_adc_common.v` / `up_dac_common.v`.

### Capabilities

No capability flag changes: the enum has no flag for immediate streaming, and every stream flag
in it is timed (P3). `doctor` gains a "streams" line per device: RX/TX device names, channel
count, and overflow detection available or not.

## Python

`dev.rx_stream(channels=[0], block_samples=65536, block_count=4)` and
`dev.tx_stream(channels=[0], block_samples=..., cyclic=False)` return `RxStream`/`TxStream`
(context managers: start on enter, stop and destroy on exit). `RxStream.recv(n=None)` returns
`(samples, info)` with `samples` complex64 `(channels, n)` (one block when `n` is None);
`recv_raw` the int16 array. `TxStream.send(samples)` takes complex `(channels, n)` or 1-D for one
channel and rounds to int16, refusing values outside the int16 range with `ValueError`.
`dev.rx(n, channels=[0])` and `dev.tx(data, cyclic=True)` are the pyadi-style conveniences
(`dev.tx` keeps a cyclic stream alive until `dev.tx_stop()` or the device closes).
`datax.OverflowError` stays reserved for P3's events; P2 exposes `info.discontinuous`.

## Tests

### Tier 1 (CI, emulator)

`iiod-emu` serves RX from `<device>_buf<n>.bin` beside the XML and writes TX blocks to the same
naming. Tests write a known int16 pattern to the RX file and assert what `recv` returns
(layout, channel selection, cursor across block boundaries, `-ETIMEDOUT` with no data), and read
back the TX file after `send`/cyclic/zero-copy. Config validation, lifecycle errors,
one-stream-per-direction, and the `-ENOTSUP` paths (`start_at`, missing streaming device) are
pure C tests. Python mirrors them, plus the conveniences and the numpy-absent error. The fixtures
gain the adrv9009 streaming devices (hand-written, like P1's adrv9009 XML). If `iiod-emu`'s
buffer behaviour blocks a test (for example a file that must pre-exist), the fixture creates it;
a fix to the emulator itself goes to the libiio fork with its own PR and a pin bump.

### Tier 2 (hardware, labgrid)

`tests/hardware/` (collects without a board; CI runs collection only):

- **Smoke (REQ-TEST-021, streaming part):** context over `ip:<board>`, family and channel count
  as expected, caps match the P2 matrix row; RX one block per channel with non-constant data and
  no error; TX one-shot and cyclic start/stop without error; stop/start/stop cycles; RX
  throughput over 10 s recorded (not asserted).
- **Digital loopback (REQ-TEST-030, P2 part):** enable the chip loopback (decision 3), transmit a
  known cyclic complex waveform (a pseudo-random QPSK sequence of one block) on each TX channel,
  capture on the matching RX channel, align by circular cross-correlation, fit one complex gain,
  and assert EVM ≤ −40 dB per channel; on AD9361, where the spike showed the loop is bit-exact
  after a 4-bit shift, also assert exact equality. The measured value is recorded. The test always restores the loopback attribute to 0, even on failure.
- Evidence: JUnit plus a JSON per run (board, image hashes, libiio pin, measured throughput and
  EVM per channel) uploaded as the run's artifact (REQ-TEST-024).

`hw-test.yml` (dispatch, later nightly): a `tron` job on `hw-nemo` holding the shared lock and a
`lablp` job on `hw-bq`, each booting its place's stock image through labgrid (`adi-lg`), then
running the suite against the board's address.

## Risks the plan must handle

- **tron's stock boot.** tron is JTAG-strapped; the stock PL file on tron
  (`system-top-xsdb.bin`) no longer matches the stock hash (HOSTS.md, 2026-11-04; writer unknown),
  and its SD rootfs lost files in the 2026-11-05 fsck. The card is not stock either: it carries
  vrt49 units (`iiod-vrt49`, `vrt49-sfp-network`) and `/boot/vrt49-*` sets that the hopcap and
  vrt49 tron runs use. So the plan **passes the stock PL explicitly per boot** (from nemo's
  `~/zu11eg-recovery/` master copy, by hash) instead of overwriting the shared file, checks that
  `iiod` and its libraries are intact, and if they are not, images the card to nemo and **stops
  and reports**: a reflash is the owner's call. tron runs second, after lablp proves the path.
- **The legacy `iiod` buffer path in libiio v1** has not been exercised in this lab, and Tier 1
  cannot cover it (`iiod-emu` speaks the binary protocol). A spike on lablp proves it before the
  plan (section "Spike"). If it is broken, the fix goes to the libiio fork (PR, pin bump), not
  around it in libdatax.
- **Overflow over GigE is normal.** The legacy path cannot drain native rates, so the DMA
  overflow bit will be set on most blocks; no smoke assertion requires `discontinuous` false, and
  loopback is scored within one block, with the RX block at least twice the TX cyclic period.
- **ADRV9009 framer loopback** needs the RX framer and TX deframer converter counts, and the RX
  and TX IQ rates, to agree in the stock profile; if they do not, the loopback test documents the mismatch and covers the
  channels that do loop, and the gap is written into the platform page.

## Docs changed in the same phase

- SDK `api-c-core.md`: the semantics above (layout, return values, timeouts, cyclic, mixing
  rule, one stream per direction) before code.
- SDK `roadmap.md`: P2's transport line (vrt49:// is P6) and its exit (digital loopback).
- SDK `requirements-test.md`: REQ-TEST-030 split into the digital part (P2) and the RF part.
- SDK `platform-*.md`: measured RX throughput and loopback EVM at exit.
- libdatax README: streaming quick start.

## Rulings from the builder check (2026-11-05)

1. Cyclic TX defers the hardware enable to its single `send`/`put_block` (lifecycle above). The
   v0 protocol fixes cyclic at OPEN, and the emulator's binary protocol would hide a wrong order,
   so a Tier 2 test asserts the waveform repeats (the loopback test does).
2. A cyclic `send` of `samples` < `block_samples` repeats exactly `samples` instants.
3. One-shot TX `stop()` drains with a bounded wait and returns `-ETIMEDOUT` if it could not; the
   Tier 1 read-back relies on the drain (the emulator writes TX data at dequeue).
4. libdatax gets self-hosted runners on nemo (`hw-nemo`, for tron) and bq (`hw-bq`, for lablp),
   registered like hopcap's; each needs the pinned libiio v1 built on the host.

5. (Task 2 review) `timeout_ms` is bounded by the libiio context timeout captured at block
   creation on both protocols; `recv(0)` polling is unreliable over binary-protocol `iiod` until
   datax_sdk_sdr#26; stop/destroy wait up to one context timeout on a stalled stream; stream
   handles die with their context.

## Phase exit evidence (2026-11-05)

- **lablp (ADRV9361-Z7035)**: stock Kuiper 2023_R2 (`hdl_2023_r2` PL); 10/10 at libdatax `dedc635`.
- **tron (ADRV9009-ZU11EG)**: 12/12 at `d49c56c`, a tree identical to `dedc635`. The PL was
  **not** the stock 2023_R2 design: it was the recovery bitstream `a22f4cf6…`, HDL `main`
  `085a6ed7` (2026-06-09), with the stock 2023_R2 kernel and DTB. The "stock reference designs"
  goal therefore holds for lablp, and for tron's software stack but not its PL. Booting the true
  stock 2023_R2 PL, extracted from the stock BOOT.BIN, is open.
- tron's hardware findings changed the code: TX scan channels are named `voltage0..7`
  (interleaved), and the Talise loopback needs framer A on both phys and exchanges I and Q.

## Left to the builder

- The exact sample-pattern files and the emulator fixture layout.
- Whether `block_count` has a default in C (Python defaults to 4).
- How the TX partial-block padding is tracked.
- The labgrid recipe details per place, within the lock and runner rules above.
