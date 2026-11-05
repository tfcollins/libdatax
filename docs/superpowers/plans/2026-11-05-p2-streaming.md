# libdatax P2 Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Immediate RX/TX streams (copying, zero-copy, one-shot and cyclic TX) over the stock AXI-DMAC devices of both families, a Python stream API, a Tier 1 emulator suite, and a Tier 2 hardware suite that passes REQ-TEST-021 streaming smoke and REQ-TEST-030's digital loopback on `lablp` and `tron`.

**Architecture:** `src/stream.c` owns `struct datax_stream`: an `iio_buffer_stream` plus a ring of `iio_block`s over the family's RX or TX streaming device, with a copy cursor for `recv`/`send` and a lend/return path for zero-copy. The family ops gain the streaming device names and a radio-channel → IIO-channel mapping. Overflow comes from the converter core's status register through `direct_reg_access`. Python wraps it with ctypes and numpy. Hardware tests reach the boards' stock `iiod` over `ip:` from a host runner.

**Tech Stack:** C11, CMake, libiio v1 (fork pin in `pins.json`), Python ≥ 3.11 + ctypes + numpy (optional extra `stream`) + pytest, GitHub Actions (ubuntu-latest for Tier 1; self-hosted `hw-nemo` and `hw-bq` for Tier 2), labgrid (`adi_lg_plugins`).

**Spec:** `docs/superpowers/specs/2026-11-05-libdatax-p2-streaming-design.md` (mirrored in the SDK repo's `specs/`). API contract: the SDK repo's `docs/api-c-core.md` § Stream, "Immediate streaming semantics" (amended before this plan, commits de8577f and 20cda33), and `docs/api-python.md`.

## Global Constraints

- P1's constraints hold unchanged: MIT SPDX header on every file; `-std=c11 -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Werror`; every `-errno` return sets the context's last error; no global mutable state; `git commit -s`; one worktree branch `task/p2-<n>-<slug>` per task, merged to `main` after its tests pass.
- Public symbols: exactly the `datax_*` prototypes in `api-c-core.md`; test-only additions use the `datax_internal_` prefix and go in `src/libdatax.map`.
- Sample layout: int16 I/Q, host order, enabled radio channels ascending, I then Q, per instant. A "sample" is one instant across all enabled channels.
- One stream per direction per device (`-EBUSY`); `cyclic` is TX only and forces `block_count` 1.
- `start_at` stays `-ENOTSUP` with last-error `"timed start arrives with the scheduler (P3)"`; `datax_block_info.timestamp_ticks` is 0.
- Tier 1 stays under five minutes and needs no lab host. Hardware tests collect without a board (CI runs `--collect-only`).
- Never put lab host names or IPs in the repo; hardware tests take the board URI from `DATAX_HW_URI`.
- Never reprogram the ZU11EG PL at runtime. tron jobs hold `/tmp/vrt49-corundum-hardware.lock` on nemo. A tron SD reflash is the owner's call: image the card and stop.

## Review Focus

1. `recv` asked for more instants than a block, or a count that is not a multiple of the block, must return contiguous data across block boundaries with no repeated or skipped instant — Task 2 `test_recv_spans_blocks`.
2. A cyclic TX stream must not enable the hardware at `start()`; it enables after its one enqueue (the boards' v0 protocol fixes cyclic at OPEN, and the emulator's binary protocol hides a wrong order) — Task 3 `test_cyclic_enables_at_send` via `datax_internal_stream_stats().enabled`.
3. Destroying a started stream, or destroying the context while a stream is alive, must free everything without a crash or a libiio leak — Task 2 `test_destroy_started_stream`, `test_context_destroy_frees_streams` (under valgrind when available).
4. `channel_mask` of 0, or with a bit at or above the device's channel count for that direction, must fail `-EINVAL` before any IIO call — Task 2 `test_create_rejects_bad_mask`.
5. Python `TxStream.send` with values outside int16 after rounding must raise `ValueError` and send nothing; any stream call after the `with` block exits must raise `datax.Error`, never crash — Task 5 `test_send_out_of_range`, `test_use_after_close`.

---

### Task 0 (controller, done): API page and spec

The SDK's `api-c-core.md` carries the immediate-stream semantics, and the roadmap and REQ-TEST-030 are amended (SDK commits de8577f and 20cda33). The spec records the lablp spike and the builder-check rulings.

---

### Task 1: Streaming device map and emulator fixtures

**Files:**
- Modify: `src/platform/family.h`, `src/platform/ad936x.c`, `src/platform/adrv9009.c`, `src/internal.h`
- Modify: `tests/emu/adrv9009-zu11eg.xml`, `tests/emu/README.md`
- Create: `tests/c/test_stream_map.c`

**Interfaces:**
- Produces in `family.h`, added to `struct datax_family_ops`:
  - `const char *rx_stream_dev, *tx_stream_dev;`: `"cf-ad9361-lpc"`/`"cf-ad9361-dds-core-lpc"` for ad936x, `"axi-adrv9009-rx-hpc"`/`"axi-adrv9009-tx-hpc"` for adrv9009. Confirm against the stock Kuiper device trees, and record the source in a comment.
  - `int (*stream_channel_ids)(unsigned int radio_ch, enum datax_direction dir, const char **id_i, const char **id_q);`: ad936x radio channel n → `voltage{2n}`, `voltage{2n+1}`; adrv9009 → `voltage{n}_i`, `voltage{n}_q`. The ids point at static strings. Out of range returns `-EINVAL`.
- In `internal.h`, `struct datax_device` gains `struct iio_device *stream_dev[2]` (indexed by `enum datax_direction`, NULL when absent, filled at fold time) and `struct datax_stream *stream[2]`.
- In `tests/emu/adrv9009-zu11eg.xml`, add `axi-adrv9009-rx-hpc` (8 input scan channels `voltage0_i … voltage3_q`, 16-bit signed, plus a buffer element) and `axi-adrv9009-tx-hpc` (8 output scan channels, same naming). Copy the scan-element and buffer XML shape from `ad936x.xml`'s `cf-ad9361-*` devices. Record it in the README table.

- [ ] **Step 1: Failing test** `test_stream_map.c`: `test_ad936x_map` (radio 0 → `voltage0`/`voltage1`, radio 1 → `voltage2`/`voltage3`, radio 2 → `-EINVAL`); `test_adrv9009_map` (radio 3 → `voltage3_i`/`voltage3_q`, radio 4 → `-EINVAL`); `test_fold_finds_stream_devs` (the emulated ad936x and adrv9009 contexts have both `stream_dev` entries non-NULL; `adrv9009-single.xml` has none, and P1 behaviour is unchanged).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_stream_map --output-on-failure`. Expected: FAIL (fields missing).
- [ ] **Step 3: Implement** the ops fields, the fold-time lookup with `iio_context_find_device`, and the fixture devices.
- [ ] **Step 4: Run** the full `ctest` and `pytest tests/python`. Expected: all pass (P1 tests unchanged).
- [ ] **Step 5: Commit** `stream: streaming device map per family; adrv9009 fixture gains the DMA devices`.

---

### Task 2: Stream lifecycle and RX copying

**Files:**
- Create: `src/stream.c`, `src/stream.h`, `tests/c/test_stream_rx.c`, `tests/emu/data/` (pattern generator script `tests/emu/make_pattern.py`)
- Modify: `src/stubs.c` (remove the stream stubs this task implements), `src/context.c` (destroy live streams), `src/libdatax.map`, `CMakeLists.txt`, `tests/c/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `stream_dev`, `stream_channel_ids` and `stream[2]`.
- Produces in `stream.h`:
  ```c
  struct datax_stream {
      struct datax_device *dev; struct datax_stream_config cfg;
      struct iio_channels_mask *mask; struct iio_buffer_stream *bs;
      struct iio_block **blocks; unsigned int nblocks;
      size_t frame_bytes;                 /* 4 * popcount(channel_mask) */
      bool started, enabled, zero_copy_used, copy_used;
      unsigned int cur; size_t cur_off;   /* RX copy cursor: block index, byte offset */
      struct datax_internal_stream_stats stats;
  };
  ```
- Public (header already declares): `datax_stream_create`, `datax_stream_destroy`, `datax_stream_start`, `datax_stream_stop`, `datax_stream_recv`.
- Test-only: `struct datax_internal_stream_stats { bool enabled; unsigned long long blocks_done, overflows, underflows; bool overflow_detect; };` and `int datax_internal_stream_stats(const struct datax_stream *s, struct datax_internal_stream_stats *out);`, declared in a new `include/datax/datax_internal.h` (installed, but not part of the API page).
- Fixture data: `make_pattern.py <xml> <device> <channels> <instants>` writes `<device-id>_buf0.bin` beside the XML. The emulator's RX source is a counting pattern where instant k of the enabled-channel set has I = k mod 32768 and Q = −(k mod 32768). The tests generate it at session start into a temp copy of the fixture directory, never into the repo.

- [ ] **Step 1: Failing tests** in `test_stream_rx.c`:
  - `test_create_rejects_bad_mask`: mask 0 → `-EINVAL`; mask bit ≥ `rx_channels` → `-EINVAL`; `block_samples` 0 → `-EINVAL`; cyclic RX → `-EINVAL`. Each sets last-error.
  - `test_one_stream_per_direction`: a second RX create → `-EBUSY`; destroying the first allows it.
  - `test_start_stop_rules`: start twice → `-EALREADY`; stop on a stopped stream → 0; `start_at` → `-ENOTSUP` with the P3 message.
  - `test_recv_block`: mask `0x1`, block 4096 instants, 4 blocks; `recv(4096)` returns 4096 and the counting pattern from instant 0. `info.valid_samples` 4096, `info.timestamp_ticks` 0.
  - `test_recv_spans_blocks`: block 1000; three `recv` calls of 1500, 700 and 2300 return a contiguous pattern (instants 0..4499) with no repeat or gap.
  - `test_recv_two_channels`: mask `0x3` on ad936x gives frames of 8 bytes in channel order.
  - `test_destroy_started_stream` and `test_context_destroy_frees_streams`: no crash; when `valgrind` is on PATH, ctest runs this binary under `valgrind --error-exitcode=1 --leak-check=full`.
  - `test_recv_timeout`: an RX stream on a device whose buffer file is absent returns `-ETIMEDOUT` (or the emulator's error mapped to `-EIO`, asserted exactly as observed and documented in a comment) within 2× `timeout_ms`.
- [ ] **Step 2: Run** `ctest -R test_stream_rx`. Expected: FAIL (still the stubs).
- [ ] **Step 3: Implement** create (validate, then mask, `iio_device_get_buffer(dev, 0)`, `iio_buffer_open`, blocks of `block_samples * frame_bytes`; on any failure free in reverse and set last-error), start (RX: enqueue all blocks, then `iio_buffer_stream_start`), stop (`iio_buffer_stream_stop` + cancel, cursor reset), destroy (stop, free, clear `dev->stream[dir]`), and `recv`. `recv` dequeues the block at `cur` when `cur_off == 0`, copies `min(remaining, wanted)`, and re-enqueues the block when it is fully drained. Map `timeout_ms` through `iio_context_set_timeout` for the dequeue: < 0 sets 0 (infinite), 0 polls with nonblock, > 0 sets the value. Restore the previous timeout afterwards.
- [ ] **Step 4: Run** `ctest` (all) and `pytest`. Expected: all pass.
- [ ] **Step 5: Commit** `stream: lifecycle and RX copying over libiio v1 blocks`.

---

### Task 3: TX (one-shot, cyclic) and zero-copy

**Files:**
- Modify: `src/stream.c`, `src/stubs.c`
- Create: `tests/c/test_stream_tx.c`, `tests/c/test_stream_zero_copy.c`

**Interfaces:**
- Consumes: Task 2's `struct datax_stream` and stats.
- Public: `datax_stream_send`, `datax_stream_get_block`, `datax_stream_put_block`.
- Emulator TX sink: `emu_dequeue_block` writes each dequeued TX block to `<tx-device-id>_buf0.bin`, overwriting from the start. Tests read that file back.

- [ ] **Step 1: Failing tests:**
  - `test_tx_oneshot_full_blocks`: block 1024, send 2048 instants of the counting pattern, then `stop()` returns 0; the TX file holds the second block (the emulator overwrites per block).
  - `test_tx_partial_block_padded_on_stop`: block 1024, send 1500; after `stop()` the file holds instants 1024..1499 followed by 548 zero instants.
  - `test_cyclic_enables_at_send`: cyclic stream; after `start()` stats `enabled == false`; after `send(300)` it is true; the file holds exactly 300 instants; a second `send` → `-EBUSY`; `send(block_samples + 1)` on a fresh cyclic stream → `-EINVAL`.
  - `test_tx_stop_drain_timeout_reported`: covered only if the emulator can stall a dequeue. Otherwise write the test as `skip` with a comment naming the Tier 2 test that covers it (Task 6 `test_tx_oneshot_drains`).
  - `test_zero_copy_rx`: `get_block` returns `block_samples` of pattern and `put_block` re-enqueues; then `recv` on the same stream → `-EINVAL` (no mixing).
  - `test_zero_copy_tx`: `get_block`, fill, `put_block`; the file matches. Cyclic zero-copy repeats the whole `block_samples` block.
  - `test_put_foreign_pointer`: `put_block` with a pointer that is not a lent block → `-EINVAL`.
- [ ] **Step 2: Run** `ctest -R 'test_stream_(tx|zero_copy)'`. Expected: FAIL.
- [ ] **Step 3: Implement** per the spec's Lifecycle and I/O sections and its builder-check rulings 1–3. Cyclic `send` destroys the create-time blocks and opens one block of `samples * frame_bytes`, enqueues it with `cyclic = true`, then calls `iio_buffer_stream_start`. A one-shot `stop()` pads and enqueues the held partial block, dequeues every outstanding block (each bounded by the context timeout), then stops.
- [ ] **Step 4: Run** the full `ctest` and `pytest`. Expected: all pass.
- [ ] **Step 5: Commit** `stream: TX one-shot and cyclic, zero-copy both directions`.

---

### Task 4: Overflow and underflow detection, doctor

**Files:**
- Create: `src/stream_status.c`, `tests/c/test_stream_status.c`
- Modify: `src/stream.c` (call it after RX dequeue and TX enqueue), `python/datax/doctor.py`, `tests/python/test_doctor.py`, `tests/emu/ad936x.xml` (debug attribute `direct_reg_access` on both `cf-ad9361-*` devices, if absent)

**Interfaces:**
- `int datax_stream_status_poll(struct datax_stream *s, bool *overflow, bool *underflow);` (internal). It writes `0x80000000 | REG` to the streaming device's `direct_reg_access` debug attribute, reads back the value, then clears the sticky bits by writing them back (write-1-to-clear) through the same attribute. `REG` and the bit positions come from ADI HDL `library/common/up_adc_common.v` and `up_dac_common.v` at the stock design's release. Cite the file and line in a comment. The lablp spike read `0x4` at byte offset `0x88` on the RX core; check that against the HDL.
- At most one poll per 100 ms per stream (monotonic clock). An overflow sets `info.discontinuous` on the next RX block and increments `stats.overflows`. `stats.overflow_detect` is false when the attribute is missing or the read fails; detection then stays off for the stream's life.
- `doctor` prints, per device, `streams: rx=<name>|none tx=<name>|none overflow-detect=<yes|no|unknown>`. "unknown" means no stream has been opened to probe it.

- [ ] **Step 1: Failing tests:**
  - `test_status_parses_bits`: drive `datax_stream_status_poll` against the emulator, whose debug attribute is a static value set in the fixture to a word with the overflow bit set. Expect `overflow == true`.
  - `test_no_debug_attr_disables_detect`: on a fixture without `direct_reg_access`, `stats.overflow_detect == false` and `discontinuous` is always false.
  - `test_poll_rate_limited`: over 10 `recv` calls within 100 ms, the poll count is ≤ 1. Count it through a test hook in stats.
  - Python `test_doctor_streams_line`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** everything. Expected: pass.
- [ ] **Step 5: Commit** `stream: DMA overflow/underflow via the core status register; doctor streams line`.

---

### Task 5: Python streams

**Files:**
- Create: `python/datax/stream.py`, `tests/python/test_stream.py`, `examples/python/rx_capture.py`, `examples/python/tx_cyclic.py`
- Modify: `python/datax/_lib.py` (prototypes), `python/datax/device.py` (`rx_stream`, `tx_stream`, `rx`, `tx`, `tx_stop`), `python/datax/__init__.py`, `pyproject.toml` (extra `stream = ["numpy>=1.24"]`; numpy also goes in the test requirements), `tests/python/test_examples.py`

**Interfaces:**
- `Device.rx_stream(channels=(0,), block_samples=65536, block_count=4) -> RxStream`. `Device.tx_stream(channels=(0,), block_samples=65536, block_count=4, cyclic=False) -> TxStream`. Both are context managers: `__enter__` starts the stream, `__exit__` stops and destroys it.
- `RxStream.recv(n=None, timeout_ms=-1) -> tuple[np.ndarray, BlockInfo]`: complex64, shape `(len(channels), n or block_samples)`. `recv_raw(...)` returns int16 `(n, 2*len(channels))`. `BlockInfo` is a frozen dataclass with `timestamp_ticks`, `discontinuous`, `valid_samples`.
- `TxStream.send(samples, timeout_ms=-1) -> int` takes complex `(channels, n)`, or 1-D for one channel; it rounds to int16 and raises `ValueError` for out-of-range values or a wrong shape.
- `Device.rx(n, channels=(0,)) -> np.ndarray` (one-shot). `Device.tx(data, cyclic=True)` keeps its stream on the device until `tx_stop()` or the device closes.
- Importing `datax` works without numpy. `rx_stream`/`tx_stream` without numpy raise `datax.CapabilityError("streams need numpy: pip install libdatax[stream]")`.

- [ ] **Step 1: Failing tests** (pytest against the emulator fixtures from Tasks 1–3): `test_rx_stream_pattern`, `test_rx_two_channels_shape`, `test_recv_raw_layout`, `test_tx_oneshot_roundtrip_file`, `test_tx_cyclic`, `test_send_out_of_range`, `test_send_bad_shape`, `test_use_after_close` (`recv` after `with` → `datax.Error`), `test_rx_convenience`, `test_tx_convenience_and_stop`, `test_no_numpy_message` (monkeypatch the import to fail), and the two examples executed by `test_examples.py`.
- [ ] **Step 2: Run** `pytest tests/python -q`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `pytest tests/python -q` and `ctest`. Expected: all pass, Tier 1 total still under five minutes in CI.
- [ ] **Step 5: Commit** `python: RxStream/TxStream, rx()/tx() conveniences, numpy extra`.

---

### Task 6: Hardware suite (collects without a board)

**Files:**
- Create: `tests/hardware/conftest.py`, `tests/hardware/test_hw_stream_smoke.py`, `tests/hardware/test_hw_loopback.py`, `tests/hardware/loopback.py` (pure helpers), `tests/python/test_loopback_helpers.py`, `tests/hardware/README.md`
- Modify: `.github/workflows/ci.yml` (add `pytest tests/hardware --collect-only -q`)

**Interfaces:**
- `conftest.py`: the session fixture `hw_ctx` uses `DATAX_HW_URI`, and skips the whole directory when it is unset, unless `DATAX_HW_REQUIRE=1`, which makes a missing URI a failure. It also provides `evidence` (a dict written to `$DATAX_HW_EVIDENCE` or `evidence.json` at session end: URI host redacted to a hash, the libiio pin, the libdatax commit, the board's `iio_context` description and version attributes, per-test measurements).
- `loopback.py`:
  - `qpsk_block(n, seed) -> np.ndarray` (complex, ±0.5 full scale);
  - `align_and_fit(tx, rx) -> tuple[int, complex, float]` (lag by circular cross-correlation, least-squares complex gain, EVM in dB);
  - `set_digital_loopback(ctx, family, on) -> None` (ad936x: `ad9361-phy` debug attribute `loopback` = 1/0; adrv9009: `bist_framer_a_loopback` and, when `adrv9009-phy-b` exists, `bist_framer_b_loopback` on it, = 1/0).
- Smoke, REQ-TEST-021 streaming part:
  - `test_family_and_channels`;
  - `test_caps_row` (the P2 caps are all false: no flag changes in P2);
  - `test_rx_block_nonconstant_per_channel` (one block per radio channel; per-channel std > 0);
  - `test_tx_oneshot_drains` (`stop()` returns 0);
  - `test_tx_cyclic_start_stop`;
  - `test_start_stop_cycles` (5 RX cycles);
  - `test_rx_throughput_recorded` (10 s; the MB/s figure goes to evidence and is not asserted).
- Loopback, REQ-TEST-030 digital part, `test_digital_loopback[ch]` per radio channel:
  - transmit a cyclic `qpsk_block(1024)` and capture an RX block of 8192 (at least 2× the period);
  - fit, then assert EVM ≤ −40 dB;
  - on ad936x, also assert exact equality after `>> 4` (spike result);
  - restore the loopback in `finally`.
- Unit tests for `align_and_fit` on synthetic data (a known lag and gain gives back the same; noise gives the expected EVM) run in Tier 1.

- [ ] **Step 1: Failing tests:** `tests/python/test_loopback_helpers.py`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** the helpers and the hardware tests.
- [ ] **Step 4: Run** `pytest tests/python -q` (pass) and `pytest tests/hardware --collect-only -q` (collects; with no URI the run skips cleanly).
- [ ] **Step 5: Commit** `tests: hardware streaming smoke and digital loopback suites`.

---

### Task 7 (controller, lab): runners and `hw-test.yml`

**Files:** `.github/workflows/hw-test.yml`, `scripts/hw/boot-lablp.py`, `scripts/hw/boot-tron-stock.sh`, `docs/hardware.md`, plus host setup that is not in the repo.

- Register self-hosted runners for `tfcollins/libdatax`: nemo with labels `hw-nemo`, and bq with `hw-bq`. Follow the hopcap runner's service pattern (`~/actions-runner-hopcap` on nemo). On each host, build the pinned libiio v1 (`-DWITH_EMU_BACKEND=OFF`) into the runner's tool cache.
- `boot-lablp.py`: acquire the place; power through the plugin `HomeAssistantPowerDriver` from the coordinator's boot-tier env (`http://<coordinator>:8000/api/places/lablp/env-yaml?tier=boot`); wait for `iiod` on the board's address; export `DATAX_HW_URI`; power off and release on exit. The board address is a repository variable, not committed.
- `hw-test.yml`: `workflow_dispatch` (input `place`: lablp | tron | both). The lablp job runs on `hw-bq`, and the tron job on `hw-nemo` under the lock. Each job runs `pytest tests/hardware` with `DATAX_HW_REQUIRE=1` and uploads JUnit plus `evidence.json`.
- Verify: a dispatch with `place=lablp` runs on the bq runner and gets past the boot.

### Task 8 (controller, lab): lablp proof

Dispatch `hw-test.yml` with `place=lablp`. Fix every failure at its root, in libdatax or (for the legacy `iiod` path) in the libiio fork with a pin bump. The task is done when the suite is green and the evidence and measured throughput are recorded in the SDK's `docs/platform-adrv9361-z7035.md`.

### Task 9 (controller, lab): tron stock boot and proof

- `boot-tron-stock.sh` JTAG-boots tron's stock Kuiper set. It passes the stock PL by hash from nemo's `~/zu11eg-recovery/` master copy, never overwriting the shared `system-top-xsdb.bin`. Before the suite it checks `iiod` and its libraries on the board.
- If the rootfs is broken, image the card to nemo and stop and report (the owner decides on a reflash).
- Read the RX and TX `sampling_frequency` and the framer and deframer converter counts. If they don't match, write the mismatch into the platform page and limit the loopback to the lanes that do loop.
- Done when `hw-test.yml` with `place=tron` is green, with evidence in the SDK's `docs/platform-adrv9009-zu11eg.md`.

### Task 10 (controller): phase exit

- libdatax `support.json`: `streaming-rx` and `streaming-tx` validated, tier 2, workflow `hw-test.yml`, evidence linking the green runs, on `adrv9361-z7035` and `adrv9009-zu11eg`.
- README: a streaming quick start.
- SDK: pin libdatax in `sources.json` and re-render.
- Spec status: implemented. Close datax_sdk_sdr#24 with links.
- Flip the rows only if both places are green (REQ-TEST-042).
