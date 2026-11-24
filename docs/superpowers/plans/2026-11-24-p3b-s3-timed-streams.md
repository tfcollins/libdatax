# libdatax P3b S3 Timed Streams (Tier 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement on the fake Tick and the emulator, with no board:
- `datax_stream_start_at` for RX and TX;
- `datax_sched_queue_stream_gate`, TX only;
- RX block timestamps;
- the P3b capability probe;
- chip-B timed tunes;
- the Python surface.

S4 then runs the same code on `tron`.

**Architecture:**
- **Two seams.**
  - The Tick seam is P3a's `datax_tick_ops`, unchanged.
  - New: a block-I/O seam for streams, `struct datax_stream_src_ops`. The libiio implementation is the default. Tests bind `fake_dma`, which gates RX frames and TX playback on the fake Tick's gate bits and stamps RX block 0 with a `tick.capture` header.
- **The gate map.** It is read once at the capability probe, through one parse function, from Tick's `stream_gate_map` attribute (spec decision 8). Everything downstream uses `dev->caps.gate_map`.
- **The timed sequence.** It lives in `src/stream_timed.c`:
  1. execute-now close;
  2. arm the DMA;
  3. the opening `STREAM_GATE` at T, through the device's scheduler, with a completion hook that fails the stream.

  `stop()` cancels the stream's gate commands with a targeted cancel, then forces the gate open.

**Tech Stack:** C11 + pthreads, libiio v1 (pin in `pins.json`), Python ctypes, pytest, ctest, the emulator fixture `adrv9009-zu11eg.xml`, and Tick's `capture.py` at the `pins.json` Tick revision (vendored, test-time only).

**Spec:** `docs/superpowers/specs/2026-11-23-libdatax-p3b-timed-streams-design.md`: decisions 2–5 and 7–9, G4/G5/G6 and "Builder check rulings".

The coordinator's G2/G3 rulings of 2026-11-24 (TX mute, full-mask guard, frame strip, cpack first-sample drop, idle overflow, same-T order) are carried here ahead of the spec update and win over older spec text.

API contract: SDK `docs/api-c-core.md`, `streaming-model.md`, `api-object-model.md` and `api-python.md` at 7e74f25, plus the Task 0 amendments.

## Global Constraints

- **Carried over:** the P1–P3a constraints hold.
  - MIT SPDX headers, the C11 warning set with `-Werror`, and a last-error on every `-errno`.
  - `git commit -s`, one worktree branch per task (`task/p3b-<n>-<slug>`), no lab host names or IPs.
  - No skipped Tier 1 tests (`DATAX_REQUIRE_EMU=1`).
  - No new public symbols: the docs-symbol gate must stay green. Test-only symbols take `datax_internal_` and go in `src/libdatax.map` and `include/datax/datax_internal.h`.
- **Gate bits** (spec decision 2), as `dev->caps.gate_map` gives them, never as literals outside the parse:
  - 0 = RX level, 1 = TX level, 2 = RX mode, 3 = TX mode.
  - `pass = !mode | level`, and reset (all 0) is forced open.
  - `STREAM_GATE` is `command_id 0x0003`, `target_id 0x0200 | bit`, `args {bit, level, 0, 0}` (RTL :787-806 decodes `args[0]`).
  - libdatax never emits `CAPTURE_ARM` or `TRANSMIT_ARM`.
- **The attribute name is not final.** `#define DATAX_TICK_GATE_MAP_ATTR "stream_gate_map"` and the role table both live in `src/tick_ops.c`, next to `datax_tick_parse_gate_map()`. A driver rename is a one-line change there.
  - The role table is `{"rx-dma", "tx-dma", "rx-mode", "tx-mode", "fhm-b", "tx-start-fullmask-only"}`.
  - The position *k* of a gate role among the gate tokens is gate bit *k*.
  - Separators are comma, space or newline. `-` holds a spare position. Unknown tokens are ignored, but advance the position.
  - A duplicated role rejects the whole map, which then counts as absent.
  - The last two are presence tokens. They are not gate bits and do **not** advance the position:
    - `fhm-b`: chip B's trigger is Tick **GPIO** bit 1 (decision 7).
    - `tx-start-fullmask-only`: the `util_upack2` residue until S2 fixes it.
- **TX gate semantics** (G2 ruling 1):
  - TX `start_at` holds FIFO reads until the first opening at T.
  - After that, a closed TX gate **mutes**: reads continue, the DAC gets zeros, and a cyclic buffer keeps playing time-aligned.
  - With `tx-start-fullmask-only`, TX `start_at` on a mask that is not every TX channel returns `-ENOTSUP`. The check is one function, `tx_mask_ok()`.
- **Execute-now** (G5): timestamp 0, `LATE_EXECUTE_NOW`, no `REPORT_COMPLETION`.
  - It applies at the push and raises `late_count` by 1, which `datax_tick_cmd_now` re-baselines under `s->lock` while the caller holds `s->io`.
  - With no scheduler (`s == NULL`) it still works; an untimed `start()` uses that path.
  - Nothing waits for a completion.
- **The opening command:** `STREAM_GATE(level=1)` at T, `LATE_DROP | REPORT_COMPLETION`, `consume = true`, hook `open_done`.
  - Because it is reported, the commit's report-spacing rule can refuse `start_at` with `-EINVAL`. The API sentence says so (Task 0).
- **`capture_tag_enable` belongs to libdatax** (gap 2):
  - RX `start_at` sets the RX level bit (read-modify-write);
  - `stop()` and an untimed RX `start()` clear it.
- **Tag strip** (G3 ruling 3): `strip_frames = ceil(tag_bytes / frame_bytes)` whole frames, where `tag_bytes` is `capture_tag_header_bytes`, 32.
  - That gives 8, 4, 3 or 2 frames at 1, 2, 3 or 4 channels.
  - `valid_samples` drops by `strip_frames`.
- **Block timestamps** (G3 ruling 4): `util_cpack2` drops the first sample after a gap, so stored frame 0 is the second released sample.
  - The first delivered sample of block b is released sample `n_b = 1 + b × block_samples + (b == 0 ? strip_frames : 0)`.
  - `timestamp_ticks = from_hw(capture_tick + datax_frames_to_ticks(n_b, fs, tick_hz))`.
  - The fixed fabric release latency (about 18 device clocks) is not added. S4 measures it.
- **Same-T order** (ruling 6): at an equal hardware timestamp the RX opening `STREAM_GATE` reaches Tick before the TX one, whatever order the two `start_at` calls ran in.
- **Fake parameters** for P3b tests:
  - fake Tick bitmap `0xdf` (tron's `0x9f` plus `STREAM_TIMESTAMP`), map `"rx-dma,tx-dma,rx-mode,tx-mode"`, with `",fhm-b"` added in chip-B mode;
  - `fake_dma` sample rates 1 MSPS (exactly 100 ticks per frame) and 30.72 MSPS (rounding).
- **Lifetime:** a stream never holds a scheduler reference across calls, so `datax_sched_destroy` is never blocked by a stream. The hooks' `user` is the stream. `stop()` finishes every one of its commands before `destroy` frees it.

## Review Focus

1. **Never a shifted capture.** An opening command that is late fails the stream. There are three ways it can be late:
   - the host was late ("could not keep the queue fed");
   - Tick dropped it, late on submit (`LATE_DROPPED`);
   - a held completion slot made it run late. Tick then still records and tags `executed = requested` (tick#38; `fake_tick.c` dispatch), and only poller-lag detection reports it LATE.

   In every case `DATAX_EVT_LATE_COMMAND` fires and `recv`/`send` return `DATAX_ERR_LATE`. No stamped block is ever delivered: RX holds block 0 until `open_done` has seen `OK`.

   A flush, scheduler destroy or PPS adoption of the open fails the stream with `-ECANCELED`, never a silent hang.

   Owner: Task 3, `test_start_at_late_at_dispatch_fails`, `test_held_slot_open_never_delivers_shifted` and `test_sched_flush_fails_armed`.
2. **Targeted cancel and refeed.** `stop()` cancels the stream's gate commands in any state (BUILD, PENDING, WRITTEN) and nothing else.
   - An unrelated GPIO command and a tune survive and execute exactly once, at their time.
   - No hook runs after the stream is freed: ASan/valgrind clean.
   - The same refeed puts an RX opening ahead of an already-written TX opening at the same T.
   - Owner: Task 3, `test_cancel_keeps_others`, `test_sched_destroy_while_armed` and `test_same_t_rx_open_before_tx`.
3. **The gate is never left closed.** After `stop()`, an aborted `start_at` or a crashed process, the next untimed `start()` streams.
   - `stop()` cancels, then forces open, then drains, so an armed RX stream that never opened stops within 1 s with a 5 s context timeout.
   - Owner: Task 3, `test_stop_armed_rx_bounded` and `test_start_forces_open`.
4. **Timestamps.**
   - Block 0 is `capture_tick + (1 + strip_frames)` frames, taken from gate 0's tag, not the late tag latched by the mode-bit setup.
   - Derived timestamps are exact at 1 MSPS, round correctly at 30.72 MSPS and do not overflow at n = 2^50.
   - Every block after an overflow reads 0.
   - The sticky idle overflow of forced-open mode is cleared at `start_at`, so block 0 is not `discontinuous`.
   - Soft-aligned time round-trips.
   - Owner: Task 4; Task 3 for `test_start_at_clears_idle_overflow`.
5. **No regression without the map.**
   - Every P3b flag is false.
   - `start()` makes no Tick access.
   - The P2 and P3a suites are unchanged, apart from the one P2 assertion Task 3 changes deliberately.
   - Chip B still returns `-ENOTSUP` without `fhm-b`.
   - Owner: Task 2, `test_caps_p3b_matrix`; Task 3, `test_start_no_map_no_tick_writes`; Task 6.

## Spec gaps

Each gap below is reported to the owner. Until the owner rules, the plan's choice stands.

1. **Tick has no single-command cancel,** but "stop flushes the pending open" needs one. The plan's choice: `flush_and_repend` (Task 3).
2. **Nobody owns `capture_tag_enable`.** The plan's choice: libdatax does (Global Constraints).
3. **A TX `start_at` has no data source.** The spec loads TX data inside `start_at`, and the P2 API refuses `send` before `start`. The plan's choice: a host-side preload (Task 0, Task 3). The `api-python.md` gating example cannot work under any design.
4. **`stage_hop_table` has no chip argument.** The plan's choice: enable FHM on both chips when `fhm-b` is present. S4 checks whether that moves chip B's LO.
5. **"After an overflow … with `discontinuous` set"** is ambiguous. The plan's choice: sticky for the rest of a timed stream, following the API text. Untimed P2 streams keep "the next block".
6. **`fhm-b` in a gate-bit map mixes namespaces.** The plan's choice: a presence token; GPIO bit 1 comes from decision 7.
7. **`streaming-model.md` step 1 contradicts G5.** It says to "wait for their completions" (Task 0).

---

### Task 0 (controller, before Task 3's TX half and Task 7): SDK doc amendments

**Done 2026-11-24:** datax_sdk_sdr e9d630c (mute, timestamp, strip) and d987b86 (preload, stop cancel, unreported setup, Python example).

REQ-DOC-001. One SDK PR against 7e74f25:

- **`api-c-core.md` "Timed start".**
  - *TX preload:* before `start_at`, copy-mode `send()` on an unstarted TX stream preloads.
    - It fills blocks and enqueues nothing: at most `block_count` blocks, or the one cyclic send.
    - When every block is full it returns a partial count, or `-EBUSY` if it took nothing.
    - `start_at` transmits the preload from T. An untimed `start()` transmits it at once. `get_block`/`put_block` cannot preload.
    - `start_at` on TX with nothing preloaded returns `-EINVAL`.
  - The report-spacing `-EINVAL` from `start_at`.
  - TX `start_at` returns `-ENOTSUP` for a partial mask on images that say so.
  - `queue_stream_gate` with `when` before the stream's T returns `-EINVAL`.
  - On TX, a closed gate leaves the stream "muted; the buffer keeps playing". This replaces "the TX FIFO is not read, so a cyclic buffer resumes where it paused" (also in `streaming-model.md`).
  - `stop()` cancels the stream's queued gate commands as well as the opening command.
  - Block 0's `timestamp_ticks` is the first delivered sample: `capture_tick + round((1 + strip_frames) × tick_hz / fs)`, fabric latency excluded. It replaces "the device time of the opening command".
  - The strip is whole frames, `ceil(32 / frame_bytes)`.
- **`api-python.md`:**
  - Fix the gating example. `with` calls `start()`, so `start_at` would return `-EALREADY`, and the cyclic `send` would already have transmitted ungated.
  - The new idiom:

    ```python
    tx = dev.tx_stream(..., cyclic=True)
    tx.send(tone)
    with tx.start_at(t1):
        ...
    ```

  - `start_at` returns the stream, and `with` skips `start()` on a started stream.
- **`streaming-model.md` "Start sequence" step 1:** drop "and wait for their completions". G5 sends them unreported.

---

### Task 1 (#13): Fake Tick gate state, fake DMA endpoint, stream block seam, golden tag vectors

**Files:**
- Create:
  - `src/stream_src.h`
  - `tests/c/fake_dma.c`, `tests/c/fake_dma.h` (in `libdatax_fake_tick`)
  - `tests/vendor/tick/capture.py` (byte-for-byte at the pin, with a PROVENANCE row; `test_provenance.py` hashes it)
  - `tests/vectors/gen_tag_vectors.py`, `tests/vectors/capture_tags.json` (not under `tests/traces/`, whose `*.json` glob registers every file as a fake-Tick trace)
  - `tests/python/test_tag_vectors.py` (`--check`)
- Modify: `src/stream.c`, `src/stream.h`, `src/stream_status.c`, `src/internal.h`, `tests/c/fake_tick.{c,h}`, `tests/c/test_fake_tick.c`, `tests/c/CMakeLists.txt`.

**Interfaces:**
- **Stream seam, `stream_src.h`.** `h` is the bound handle; the libiio implementation is bound with `h` = the stream.

  ```c
  struct datax_stream_src_ops {
      int   (*open)(void *h, size_t block_bytes, unsigned int nblocks, size_t frame_bytes);
      void  (*close)(void *h);
      void *(*data)(void *h, unsigned int i);
      int   (*enqueue)(void *h, unsigned int i, bool cyclic);
      int   (*dequeue)(void *h, unsigned int i, bool nonblock); /* 0, -EBUSY, -ETIMEDOUT, -EPERM */
      int   (*start)(void *h);
      int   (*stop)(void *h);
      void  (*cancel)(void *h);
      int   (*status)(void *h, bool *ovf, bool *unf);           /* NULL: core register */
  };
  ```

  - `dev->src_ops[2]` and `dev->src_h[2]` (NULL: libiio) are copied into the stream at create.
  - `datax_internal_stream_bind_source(dev, dir, ops, h)` returns 0, `-EINVAL`, or `-EBUSY` while a stream exists in that direction.
  - `stream.c`'s eleven libiio buffer and block calls move behind `datax_stream_iio_src`, with no behaviour change.
  - The mask, channel lookup and sample-size check stay on the emulator device.
- **Fake Tick:**
  - `fake_tick_set_gate_map(f, text)`: NULL means the attribute is absent (`-ENOENT`).
  - `fake_tick_hide_tag_attrs(f, bool)`.
  - `fake_tick_set_gate_hook(f, fn, user)`: `fn(user, old, new, tick)` on every `stream_gate_level` change, called with the lock held.
  - `fake_tick_gate_tag(f, gate, struct fake_tick_tag *)`: a latch kept **per gate** (`ticks`, `seq`, `target`, `cmd`, `late`, `count`).
    - `latch_tag` now records `late` (late on submit, or dispatched late) into the shared registers and into that gate's latch.
- **`fake_dma`:** `fake_dma_new(f, &(struct fake_dma_config){ sample_hz, bits from the map })`, with `fake_dma_rx_ops` and `fake_dma_tx_ops`.
  - **RX.**
    - Frame *n* exists at tick `n × hz / sample_hz`.
    - It is stored only while the RX gate passes, judged from the gate-hook history.
    - The first passing frame after a gap is dropped, as `util_cpack2` drops it.
    - Frame content encodes *n* (I = n & 0xffff, Q = n >> 16, every channel), so a test can locate frames.
    - **Release:** a closed-to-open transition while mode = 1 and `capture_tag_enable` has the level bit. The first stored frame then starts a block.
    - The first 32 bytes of that block are overwritten with the header built from the RX level gate's latch: `VALID | gate << 4 | LATE? | STALE?`, `tick_hz` = the fake's, `burst_index` = the release count.
    - **Idle overflow:** a sticky `ovf` asserts while the gate passes and the DMA is not running. That is forced-open mode with no buffer, the same as stock.
    - `dequeue`: non-blocking returns `-EBUSY`; blocking polls `fake_tick_now()` every 1 ms for up to 50 ms, then returns `-ETIMEDOUT` (`rx_take` retries to the deadline).
  - **TX.**
    - Enqueued blocks form a FIFO; cyclic blocks repeat.
    - After `start` with mode = 1, nothing is read until the first pass.
    - From then on one frame is consumed per sample clock, gate open or not. Closed, the DAC gets zeros (muted).
    - `fake_dma_tx_segments(d, out, max)` returns `{t_start, t_end, first_frame, muted, underflow}`.
  - **Faults:**
    - `fake_dma_inject_overflow(d, lost)`: drops frames, and the next `status` reports `ovf`;
    - `fake_dma_corrupt_next_tag(d, what)`: magic or flags.
  - `fake_dma_log(d, ...)`: an ordered op log (open, enqueue, start, stop).
- **`gen_tag_vectors.py`** drives vendored `tick.capture.build_capture_header` and `parse_capture_header` and writes `capture_tags.json`. Cases:
  - on-time gate 0;
  - `late_executed`; `stale`;
  - gate 2 late (the mode-bit latch);
  - `capture_tick` 2^64 − 1;
  - non-zero `tick_hz` and `burst`;
  - negatives: bad magic, bad version.

- [ ] Write the failing tests:
  - `test_fake_tick unit`: `gate_map_attr`, `gate_hook`, `per_gate_tag_late_flag`.
  - `test_fake_dma` (new, no emulator):
    - `rx_frames_only_while_pass`, `rx_drops_first_after_gap`, `release_tags_block0` (bytes equal to every positive vector), `mode0_untagged`;
    - `overflow_drops_and_flags`, `idle_overflow_sticky`;
    - `tx_held_until_first_open`, `tx_mute_keeps_playing_cyclic`, `tx_underflow_segment`.
- [ ] Run `ctest -R 'fake_tick|fake_dma'` and `pytest tests/python/test_tag_vectors.py tests/python/test_provenance.py`. Expect FAIL.
- [ ] Implement the seam and the fakes, and generate the vectors.
- [ ] Run the full `ctest` and `pytest`. Expect all to pass, P2 stream suites unchanged.
- [ ] Commit: `test: stream block seam, fake DMA gated on Tick gate bits, golden capture-tag vectors`.

### Task 2 (#17): Capability probe and gate map

**Files:**
- Modify: `src/tick_ops.{c,h}`, `src/caps.c`, `src/internal.h`, `python/datax/doctor.py`, `tests/c/test_caps.c`, `tests/c/CMakeLists.txt`, `tests/python/test_doctor.py`.

**Interfaces:**
- `struct datax_gate_map { bool present; int rx_level, tx_level, rx_mode, tx_mode; bool fhm_b, tx_fullmask_only; }`, where −1 means absent.
- `int datax_tick_parse_gate_map(const char *text, struct datax_gate_map *out)`. Test export: `datax_internal_gate_map_parse(text, int out[7])`, with the values in field order.
- `dev->caps` gains `gate_map`, `tag_bytes` (`capture_tag_header_bytes`) and `tag_attrs` (`capture_tag_enable`, `_header_bytes` and `_magic` all readable).
- `compute_flags`, where `timed` is the existing `TIMED_COMMANDS` gate:
  - `RX_ARM` = timed ∧ bitmap & 0x08 ∧ `rx_level` ≥ 0 ∧ `rx_mode` ≥ 0.
  - `TX_ARM` is the same with the TX bits.
  - `STREAM_GATE` = `RX_ARM` ∨ `TX_ARM`.
  - `TIMESTAMPS` = timed ∧ bitmap & 0x40 ∧ `tag_attrs` ∧ `rx_level` ≥ 0.
- Doctor line `streams:` prints the map as parsed, which timed flags are set and why the others are not, and `tag_bytes`.

- [ ] Write the failing tests:
  - `test_gate_map_parse`:
    - separators, spare `-`, unknown token, empty text;
    - a duplicate rejects the map;
    - `fhm-b` and `tx-start-fullmask-only` presence;
    - a presence token placed before a gate role does not shift the bits.
  - `test_caps_p3b_matrix`, on the fake bound to `adrv9009-zu11eg`, one row per input:
    - no map;
    - map without mode bits;
    - no 0x08;
    - no 0x40;
    - hidden tag attributes;
    - TX-only map;
    - `tick_usable` false.
  - The existing `test_caps_*` emulator fixtures assert every P3b flag false.
  - Python `test_doctor_streams_line`.
- [ ] Run, implement, run, commit: `caps: probe RX_ARM/TX_ARM/STREAM_GATE/TIMESTAMPS from Tick stream_gate_map`.

### Task 3 (#14): `datax_stream_start_at`, stop cancel, untimed start forces open

**Files:**
- Create: `src/stream_timed.c`, `tests/c/test_stream_timed.c`.
- Modify: `src/stream.{c,h}`, `src/stream_status.c`, `src/sched.c`, `src/scheduler.h`, `src/poller.c`, `tests/c/test_stream_tx.c` (line 243: `send` before start now preloads), `tests/c/CMakeLists.txt`.

**Interfaces:**
- `datax_tick_cmd_now(dev, s, command_id, target_id, const uint32_t args[4])`: `datax_tick_gpio_now` becomes a wrapper around it. `datax_tick_gate_now(dev, s, bit, level)`.
- `int datax_sched_cancel_gates(struct datax_sched *s, uint32_t gate_mask, const char *detail)`. It takes `io` itself and returns `-EDEADLK` if the caller holds it.
  1. It finishes every non-DONE `STREAM_GATE` command on `gate_mask` as `FLUSHED`, running its hook.
  2. If none of them is WRITTEN, no Tick I/O happens.
  3. Otherwise it first waits, with `io` released and for at most one guard margin, until no other WRITTEN command is due before now + guard.
     - A hop edge or GPIO due in the flush round trip must run, not be classed as dropped.
     - If the bound expires, it goes on, and step 5's `DROPPED` rule is the fallback.
  4. It then reads time t1, drains, writes `flush_queue`, reads time t2 and drains again.
  5. Every other WRITTEN command still not DONE then either:
     - returns to PENDING when `hw_ts > t2` (the feeder re-writes it, and its normal late rule applies; `lagged` is cleared); or
     - finishes `DROPPED` with "flushed by a stream stop before it ran" (unreported: "…may have run").
- `int datax_sched_refeed(struct datax_sched *s)`: steps 3–5 with nothing cancelled. Both functions share one static `flush_and_repend()`.
  - Refeed is on a normal path, not an edge case: a TX `start_at` followed by an RX `start_at` at the same T is decision 10's loopback setup.
  - If step 3's bound expires, refeed skips the reorder and sets a last-error warning instead of risking a drop.
- **Same-T order:**
  - `datax_sched_cmd_spec` and `sched_cmd` gain `bool lead`.
  - `earliest_pending` orders by `(hw_ts, !lead, order)`.
  - Only the RX open sets `lead`.
- `datax_stream_status_clear(s)` polls through the seam and discards the result whatever the 100 ms rate limit says, then resets `last_poll_us`.
- **Stream fields:**
  - `timed`, `stopping`;
  - `_Atomic int fail` (0, `DATAX_ERR_LATE` or `-ECANCELED`);
  - `open_seq`, `t_open` (API ticks), `fs`;
  - `held[]`: TX preload, filled and not enqueued.
- **`start_at(s, T)`:**
  1. Checks, in order:
     - NULL gives `-EINVAL`; started gives `-EALREADY`;
     - the direction's `RX_ARM`/`TX_ARM`, otherwise `-ENOTSUP` naming the missing map roles;
     - TX failing `tx_mask_ok()` gives `-ENOTSUP` "TX start_at needs every TX channel on this image";
     - TX with nothing held gives `-EINVAL` "preload with send() first".
  2. Read `fs`.
  3. Take the device scheduler, created lazily as in `datax_fhm_timed_set_frequency`. Uncommitted builder commands give `-EBUSY`; a call from a hook gives `-EDEADLK`.
  4. `now + guard > T` gives `DATAX_ERR_LATE`, with nothing written.
  5. Under `io`, write execute-now `gate(mode, 1)` and `gate(level, 0)`. RX also sets the `capture_tag_enable` bit.
     - Release `io` straight after these writes. Arming makes many libiio round trips and must not starve the poller.
     - Commit and refeed take `io` themselves (`-EDEADLK` otherwise).
  6. Arm:
     - RX: `hw_open` if needed, enqueue every block, `start`, then `datax_stream_status_clear`. This drops the sticky idle overflow of forced-open mode (ruling 5) and clears `disc_pending`.
     - TX: `start`, then enqueue the held blocks in ring order (a partial one stays held at `cur`, as `send` leaves it); cyclic uses `cyclic_arm`.
  7. Enqueue the open (Global Constraints; `lead` for RX) and commit.
     - For RX, if the device's TX open for the same `hw_ts` is already WRITTEN, call `datax_sched_refeed`, so the RX open is written ahead of it.
  8. Mark the stream `started` and `timed`.

  If step 7 fails (`DATAX_ERR_LATE` or the spacing `-EINVAL`), undo: `gate(mode, 0)`, `gate(level, 0)`, `hw_close`. Then return the error.
- **`open_done` hook:**
  - It sets `_Atomic int open_state` and signals the stream's condition variable.
  - `OK` gives 1.
  - `LATE` gives `fail = DATAX_ERR_LATE`, including "poller lagged" for a held slot. `datax_sched_finish` already raises the event.
  - `FLUSHED`/`DROPPED` give `fail = -ECANCELED`, unless `stopping` is set.
- **Block 0 is held:** RX `rx_take` keeps a dequeued block 0 back until `open_state` is resolved, within the caller's `timeout_ms`.
  - `OK` delivers it.
  - A failure returns `fail`.
  - A timeout returns `-ETIMEDOUT`, and the next call delivers the same block.
  - `recv`, `send`, `get_block`, `put_block` and `rx_take`'s retry loop check `fail` and return it with a last-error naming the cause. A blocked `recv` returns within one context timeout.
- **`stop()` on a timed stream:**
  1. Set `stopping`.
  2. `datax_sched_cancel_gates(level bit)` on the device scheduler, if it still exists.
  3. Execute-now `gate(mode, 0)`, `gate(level, 0)`; clear the tag bit.
  4. The P2 teardown.
- **Untimed `start()`:**
  - With the direction's map bits: execute-now `gate(mode, 0)` and `gate(level, 0)`, plus a tag-bit clear for RX, first. It holds `io` if a scheduler exists; otherwise `s` is NULL. A failed write fails `start()`.
  - Without the map: no Tick access.
  - Held TX blocks are enqueued after enable.
- **TX preload:** copy-mode `send` on an unstarted TX stream fills `held` blocks (Task 0's rule). `get_block` on an unstarted stream stays `-EINVAL`.

- [ ] Write the failing tests in `test_stream_timed.c` (emulator `adrv9009-zu11eg`, fake Tick and `fake_dma` at 1 MSPS):
  - `test_start_at_guard_margin_late`: no descriptor writes and no gate change.
  - `test_start_at_order`: the exec log and the DMA log show mode 1, level 0, then enqueue and start, then the open at T. `late_count` +2 and no LATE event.
  - `test_start_at_rx_first_frame_at_T`.
  - `test_start_at_late_at_dispatch_fails`, two ways:
    - host-late, through `datax_internal_sched_pause`;
    - Tick `LATE_DROPPED` (late on submit): raise the fake access cost after `sched_create`, so the push lands after T. Check this against `feed()`'s device-time estimate.
  - `test_held_slot_open_never_delivers_shifted`:
    - an unacked reported command holds the open past T;
    - the gate opens late and the tag reads T;
    - `recv` returns `DATAX_ERR_LATE` and no frame is delivered.
  - `test_refeed_keeps_near_edge`: an unreported FHM edge due about 1 ms after an RX `start_at`'s refeed executes exactly once.
  - `test_stop_cancels_open`, `test_cancel_keeps_others`, `test_stop_armed_rx_bounded`.
  - `test_start_after_aborted_start_at_open`, `test_start_forces_open` (the fake preset to mode 1), `test_start_no_map_no_tick_writes`.
  - `test_start_at_enotsup_without_map`, `test_start_at_ealready`.
  - `test_start_at_refuses_uncommitted_batch`, `test_start_at_report_spacing_restores_gate`.
  - `test_start_at_tx_partial_mask_enotsup`: with the token; allowed without it.
  - `test_start_at_clears_idle_overflow`: forced-open idle, then `start_at` within 100 ms of a previous poll. Block 0 is not `discontinuous` and `overflows == 0`.
  - `test_same_t_rx_open_before_tx`, two ways: TX then RX `start_at` with the TX open already written, and both pending in one feed. The exec log shows gate 0 before gate 1 at T.
  - `test_start_at_tx_cyclic`: segment 0 starts at T with frame 0, and nothing is read before T.
  - `test_start_at_tx_stream_preload`: underflow is recorded, not asserted.
  - `test_tx_preload_rules`.
  - `test_sched_flush_fails_armed`.
  - `test_sched_destroy_while_armed`, under ASan when available and valgrind when on PATH.
- [ ] Run, implement, run (the full `ctest`, P2 and P3a included), commit: `stream: start_at closes, arms and opens at T; stop cancels gate commands and forces open`.

### Task 4 (#16): RX block timestamps

**Files:**
- Create: `src/stream_tag.c`.
- Modify:
  - `src/stream.{c,h}`, `src/stream_timed.c`, `tests/c/test_stream_timed.c`;
  - `include/datax/datax.h`: the field comment becomes `/* first delivered sample; 0 = unknown */`;
  - `include/datax/datax_internal.h`: `tag_errors` in `datax_internal_stream_stats`.

**Interfaces:**
- `int datax_tag_parse(const void *buf, size_t len, struct datax_tag *out)`.
  - `struct datax_tag` mirrors `CaptureHeader`: `magic`, `version`, `header_words`, `flags`, `capture_tick`, `seq`, `target`, `command`, `tick_hz`, `burst`.
  - It returns `-EINVAL` on length, magic or version.
  - Test export: `datax_internal_tag_parse`.
- `unsigned long long datax_frames_to_ticks(n, fs, hz) = (n / fs) * hz + ((n % fs) * hz + fs / 2) / fs`. Test export: `datax_internal_frames_to_ticks`.
- **Stream fields:** `strip_frames`, `anchor_hw`, `anchored`, `blk` (blocks since release), `unknown` (sticky after an overflow), `lent_ptr[]`.
- **First block dequeued after `start_at`**, with `TIMESTAMPS`:
  - If it parses, strip `strip_frames` (Global Constraints), so `cur_off` and the zero-copy `*data` start past the tag. `valid_samples = block_samples − strip_frames`.
  - The tag anchors (`anchor_hw = capture_tick`) only if all of these hold: `VALID`, `!STALE`, `!LATE_EXECUTED`, gate = `rx_level`, and `tick_hz` is 0 or equals Tick's.
  - If it parses but does not anchor: stripped, unanchored.
  - If it does not parse: not stripped, unanchored.
  - Both cases count in `stats.tag_errors` and set a last-error warning.
- **`timestamp_ticks` of block b** is `from_hw(anchor_hw + datax_frames_to_ticks(n_b, fs, tick_hz))` (`n_b` from Global Constraints) when anchored and not `unknown`, otherwise 0. Untimed streams always read 0.
- **After an overflow** (`disc_pending`), `unknown` is set. Every later block then has `discontinuous = true` and `timestamp_ticks = 0` until the next `start_at`, which resets it. Gap 5 notes the stickiness.
- `info` still describes the first block a `recv` touched. `put_block` matches `lent_ptr[i]`.

- [ ] Write the failing tests:
  - `test_tag_vectors`: the parser against `capture_tags.json`, negatives included.
  - `test_block0_tag_parsed_stripped`: 2 channels, 4 frames stripped, `ts == T + 5 frames`, and the first delivered frame is frame(T) + 5 (one cpack drop plus the strip).
  - `test_strip_by_channel_count`: 1, 2, 3 and 4 channels give 8, 4, 3 and 2 frames.
  - `test_derived_timestamps` at 1 MSPS (exact) and 30.72 MSPS (rounded).
  - `test_frames_to_ticks_no_overflow`: n = 2^50 against an `unsigned __int128` reference.
  - `test_overflow_zeroes_timestamps`, re-anchored by stop then `start_at`.
  - `test_tag_bad_magic_unanchored`, `test_tag_flags_rejected`.
  - `test_mode_bit_tag_ignored`.
  - `test_untimed_ts_zero`, `test_zero_copy_block0`.
  - `test_soft_offset_timestamps`: after a soft alignment, block 0 reads the API T.
- [ ] Run, implement, run, commit: `stream: RX block timestamps from the capture tag, derived blocks, unknown after overflow`.

### Task 5 (#15): `datax_sched_queue_stream_gate`, TX gated bursts

**Files:**
- Modify: `src/sched.c`, `src/stream_timed.c`, `tests/c/test_stream_timed.c`.

**Interfaces:**
- **Checks, in order:**
  1. A NULL `s` or `stream` gives `-EINVAL`; a stream on another device gives `-EINVAL`.
  2. No `STREAM_GATE` gives `-ENOTSUP`.
  3. RX gives `-ENOTSUP` "RX gating needs a DMA with end-of-burst (tlast) on this image" (G6, datax_sdk_sdr#39).
  4. A stream that is not started or not timed gives `-EINVAL` "stream is not in timed mode; use start_at".
  5. `when < t_open` gives `-EINVAL`.
  6. A failed stream returns its `fail` code.
- It then enqueues `STREAM_GATE(tx_level, enable)` at `when`, with the default `LATE_DROP` and reporting through `datax_sched_report`. It needs no hook: `stop()`'s `datax_sched_cancel_gates` finds these commands by their gate bit.

- [ ] Write the failing tests:
  - `test_gate_tx_burst`:
    - cyclic TX, `start_at(T)`, `gate_off(T + 20 ms)`, `gate_on(T + 40 ms)`, both reported;
    - muted, not paused: the segment after `gate_on` starts at the frame index 40 ms of playback after T (mod the cyclic length), and the closed segment reads `muted`;
    - every completion `OK` with `executed == requested`.
  - `test_gate_rx_enotsup`, `test_gate_untimed_einval`, `test_gate_before_open_einval`, `test_gate_other_device_einval`, `test_gate_no_cap_enotsup`.
  - `test_gate_guard_margin_late`: commit returns `DATAX_ERR_LATE`.
  - `test_gate_cancelled_by_stop`: a queued `gate_on` does not fire after a later `start_at`.
- [ ] Run, implement, run, commit: `sched: queue_stream_gate for TX on timed streams; RX -ENOTSUP`.

### Task 6 (#18): Chip-B timed tunes

**Files:**
- Modify: `src/fhm.{c,h}`, `src/internal.h`, `src/device.c` (fold binds chip B's TRX LO), `src/sched.c` (`reserved_mask`), `tests/c/fake_tick.{c,h}`, `tests/c/test_fhm.c`, `tests/c/CMakeLists.txt`.
- Task 6 does not depend on Tasks 3–5.

**Interfaces:**
- **Per-chip pipelines.**
  - `struct datax_fhm` keeps the shared table and `stage_us`, plus `struct fhm_pipe pipe[2]`. Each pipe has its edges, `fresh`, `enabled`, `next_num`, `trigger = 1u << chip`, and a phy seam.
  - `dev->fhm_ops` and `dev->fhm_h` become arrays indexed by chip.
  - The hooks' `user` is the pipe.
  - The lock order is unchanged: `fhm->lock` covers both pipes.
- **`check_tune`:** chip B (channels 2–3) is allowed iff `gate_map.fhm_b` is set and chip B's seam is bound. Otherwise it keeps P3a's `-ENOTSUP` text.
- **`reserved_mask`:** bit 0, plus bit 1 with `fhm_b`.
- **`stage_hop_table`:**
  - It parks, enables and reads back each wired chip.
  - It measures the stage time on chip A.
  - Both pipes start fresh.
- **Other paths:**
  - `datax_fhm_align_begin`/`_end` and `datax_fhm_free` iterate both pipes.
  - A tune on either chip sets `single_chip_tuned`.
- **Test-only:**
  - `datax_internal_fhm_bind_fake_chip(dev, chip, ops, h)`: the P3a symbol stays as chip A.
  - Fake: `fake_tick_add_gpio_hook` (up to 4 hooks; `set_gpio_hook` still replaces them all) and `fake_fhm_attach_bit(m, f, bit)`.

- [ ] Write the failing tests:
  - P3a's `test_tune_chip_b_enotsup` stays, now meaning "no `fhm-b`".
  - In a new `chipb` mode:
    - `test_tune_chip_b_on_bit1`: bit 1 only; chip A untouched.
    - `test_two_pipelines_independent`: interleaved A/B tunes, each primed and one-behind.
    - `test_chip_b_stage_late`, `test_chip_b_readback_mismatch`.
    - `test_reserved_bit1_with_fhm_b`.
    - `test_phase_coherent_cleared_by_chip_b`.
    - `test_stage_enables_both_chips`.
  - The P3a `test_fhm` suite runs unchanged.
- [ ] Run, implement, run, commit: `fhm: chip-B timed tunes on Tick GPIO bit 1, one pipeline per chip`.

### Task 7 (#19): Python surface

**Files:**
- Create: `tests/python/test_timed_stream.py`, `examples/python/timed_burst.py`.
- Modify: `python/datax/{_lib,stream,sched,caps}.py`, `tests/python/conftest.py` (`fake_dma` through ctypes and `datax_internal_stream_bind_source`), `tests/python/test_examples.py`.

**Interfaces** (amended `api-python.md`):
- `RxStream.start_at(t)` and `TxStream.start_at(t)` return `self`. `__enter__` calls `start()` only when not started.
- `recv()` keeps `(samples, BlockInfo)`, with `timestamp_ticks: int` 0 when unknown. There is no `None` path.
- `send()` before start preloads.
- `sched.at(t).gate_on(stream, report=False)` and `.gate_off(stream, report=False)` return the seq.
  - RX raises `CapabilityError`.
  - A stream not started with `start_at` raises `Error` (`EINVAL`).
- `LateError` comes from `recv`/`send` on a failed timed stream (ETIME, already mapped). `-ECANCELED` raises `Error`.
- The `argtypes` for `datax_stream_start_at` and `datax_sched_queue_stream_gate`.

- [ ] Write the failing tests:
  - `test_rx_start_at_block0_timestamp`
  - `test_blockinfo_zero_when_unknown`
  - `test_tx_preload_start_at_gate_on_off`
  - `test_gate_rx_capability_error`
  - `test_gate_untimed_error`
  - `test_recv_late_error`
  - `test_with_after_start_at`
  - `test_caps_p3b_flags`
  - `timed_burst.py` runs
- [ ] Run, implement, run, commit: `python: start_at, block timestamps, gate_on/gate_off, LateError on failed streams`.

### Task 8 (controller): hand-off to S4

- When S2's driver lands, set `DATAX_TICK_GATE_MAP_ATTR` and the role spellings to the driver's in one commit.
  - Bump the Tick pin.
  - Re-hash the vendored `capture.py` if it moved.
  - Drop the `tx-start-fullmask-only` guard once S2's `util_upack2` fix ships and the map no longer carries the token.
- S4 owns:
  - `images.json` `timed-dma` and the `p3b` marker;
  - the release-latency measurement;
  - checking on `tron` whether enabling chip B's FHM moves its LO (gap 4).
- **S4's timed-capture check must change.** It now asserts `timestamp_ticks == T + round((1 + strip_frames) × tick_hz / fs)`, not `== T`.

## Execution record (2026-11-24)

Executed by subagent-driven development on `feat/p3b-s3-timed-streams` (bcb2075..a141190): Tasks 1–7 each reviewed (spec + quality), fix rounds where needed, a whole-branch final review on the most capable model and one fix wave, all clean. Final tree: ctest 116/116 (ASan clean on the timed and FHM suites), pytest 155 with the emulator required, hardware suite collects 25, docs-symbol gate green.

### Rulings made during execution

- Ruling: one branch (feat/p3b-s3-timed-streams) for all S3 tasks, committed in order, instead of task/p3b-<n> branches per task — tasks are a strict chain (T1→T2→T3→{T4,T5}→T7) so per-task branches only add merges — costs: per-task revert is by commit range, not branch.
- Ruling: T3's `test_start_at_rx_first_frame_at_T` asserts on frames past the tag region (block 0's first ceil(32/frame_bytes) frames carry the header until T4 strips them) — T3 precedes T4 — costs: T4 must update that test when stripping lands.
- Ruling: models — T1, T2, T4, T5, T7 sonnet; T3, T6 opus (cancel/refeed concurrency; fhm.c refactor) — costs: extra fix rounds if a sonnet task was harder than it looked.
- Ruling: vector coverage — encoder-level check of stale/gate2/tick_max/hz_and_burst accepted; they are unreachable through real Tick commands and Task 4's parser test covers every vector — costs: an encoder/writer mismatch only on those flags would slip to T4.
- Ruling: fake_dma bit positions — Tasks 2+ build fake_dma's config from dev->caps.gate_map, not fake_dma_default_config's literals — costs: none if carried.
- Ruling: TX hold — fake must match hardware: reads start at the first pass regardless of enqueue; an empty FIFO then is an underflow segment (start_at itself refuses an empty preload) — costs: P2-style fake tests may now show a brief start underflow, which is honest.
- Ruling: every TX opening STREAM_GATE is sent UNREPORTED (not only at a shared T); TX lateness = host-late via the feeder + late_count sampled under io immediately before/after that push (LATE_DROPPED on submit); RX openings stay reported with open_done; same-T order still via lead + refeed — why: unreported commands never occupy the completion slot so they cannot be held late, and "always" avoids rewriting an already-written reported TX open — costs: TX start_at leaves no Tick completion record (S4 evidence for TX start is the loopback alignment); a coincident unrelated late_count rise during the push window fails the TX stream spuriously (safe direction).
- Ruling: accept implementer defaults 1–8 (stop's own cancel raises no event via a `cancelled` flag; partial-only TX preload → -EINVAL; unreadable fs → 0 = timestamps unknown; unreported WRITTEN with hw_ts < t1 pruned as executed; step-3 wait re-evaluated vs estimated device time, bounded by guard_ms; tag bit only when caps.tag_attrs; untimed start forcing open gated on RX_ARM/TX_ARM; per-test ctest layout + fake_dma_config_from_map/fake_dma_set_wait_ms) — costs: small; each reversible in review.
- Ruling: stop() on a TX stream armed but never opened cancels the TX DMA buffer (seam cancel, no drain) BEFORE forcing the gate open, so the cancelled preload never reaches the DAC — why: forcing open then draining transmits data the user cancelled — costs: the stop path differs by direction and open state (one more branch to test). Carried into Task 3's fix round.
- Ruling: TX held-slot lateness — ruling 1's premise (unreported never held) is false: a held reported head blocks Tick's whole queue. Adopt the reviewer's mechanism: when the poller classes a reported command LATE "poller lagged", also finish LATE every WRITTEN late_probe (TX open) command with hw_ts in [that command's hw_ts, ack time] — costs: a TX open may be failed when the hold ended just before its time (safe direction).
- Ruling: "never opened" for TX is decided on device time (open hw_ts vs a fresh time read / t2), never on table presence — an unreported open stays WRITTEN after running until prune.
- Ruling: promote reviewer Minor "stop() -EDEADLK then destroy frees stream with hook pending" into fix round 1 — use-after-free risk; destroy/stop from the poller thread must refuse (-EDEADLK, last-error) without freeing — costs: callers destroying streams from event callbacks get an error instead of a crash.
- Ruling: ⚠️ mode-bit tag end-to-end — fake_dma builds block 0's header from the RX level gate's own latch (fake_dma.c:180), modelling S2's tick-domain tag filter, so no gate-2 header can reach block 0; rejection is covered by the datax_tag_anchors unit rules (gate check is defence in depth) and S4 checks on hardware — costs: a hardware tagger without the S2 filter would be caught only by the unit rule, not an end-to-end fake test.
- Ruling: Task 6 concern 1 — accept "a chip-B tune requires the last stage to have enabled FHM on that chip" — safe direction (refuses rather than mis-hops) — costs: a user binding chip B after staging must re-stage.
- Ruling: Task 6 concern 2 — accept chip A re-priming after a partial stage failure — safer than assuming chip A's engine state — costs: one extra priming edge in that rare case.
- Ruling: Task 6 fix — armed is cleared once a stage passes its busy checks and starts touching hardware, not on entry (a refused stage writes nothing, so arming is unchanged) — costs: none found.
- Ruling: LateError e.requested/e.margin (api-python.md:180-181) is a pre-existing doc/code gap (absent at 4442be0, never implemented in P3a) — fix the SDK doc example at wrap-up rather than add API surface — costs: users of the doc example lose two attributes they never had.
- Ruling: one fix dispatch also folds these minors: bind chip phys by name (a→0, b→1); timed_burst.py uses every TX channel (tx-start-fullmask-only images); refeed failure in start_at stops+undoes the stream; Task 5 stale comments; Task 7 dead `_timed`; ci.yml test job timeout-minutes 5→10 (suite grew ~50 tests; a 5m13s cancellation already seen 2026-11-23) — costs: slightly larger fix diff.
- Ruling: the five changed P2/P3a assertions (test_stream_tx, test_stream_rx, test_sched:1005, test_stubs x2) are confirmed as justified by the stubs going live and the TX preload amendment — costs: none.
- Ruling: EDEADLK-from-callback refusal and LateError attrs are SDK doc fixes done by the controller at wrap-up.

### Deferred minors (shipped; candidates for follow-up)

- Task 1: minor (deferred): fake_dma realloc failure silently drops gate history (make it abort/sticky)
- Task 1: minor (deferred): dma_close does not reset tag_pending/drop_pending/prev_pass
- Task 1: minor (deferred): injected overflow covering the first stored frame discards the tag (undocumented)
- Task 1: minor (deferred): test_fake_tick.c:684 comment says arm-release pulse, code pushes STREAM_GATE
- Task 1: minor (deferred): seam_stream_source silently returns without an RX device (should CHECK)
- Task 1: minor (deferred): stream.c:850 NULL data() reports -EIO instead of libiio code
- Task 1: minor (deferred): process — seam/fakes written before tests (mutation RED evidence)
- Task 1: minor (deferred): fake_dma.h comment over-long line; task-1-report "Decisions" bullet on TX hold is stale (superseded by fix section)
- Task 2: minor (deferred): probe_streams treats any -EINVAL read error as "rejected" (doctor wording for I/O errors)
- Task 2: minor (deferred): no parse test for a gate role at position >= 8 (the -2 branch)
- Task 2: minor (deferred): process — implementation before red run (mutation check instead)
- Task 3: minor (deferred): repeat-late late_count rise inside the TX probe window is blamed on the TX open; late_seen re-baseline swallows the real culprit's event
- Task 3: minor (deferred): flush_and_repend skips step 5 if a read/drain fails after flush_queue succeeded (same pattern as datax_sched_flush)
- Task 3: minor (deferred): undo() frees TX preload; returned error does not say to preload again
- Task 3: minor (deferred): bound-expiry paths (cancel DROPPED fallback, refeed skipped warning) untested
- Task 3: minor (deferred): stop() from an event callback always hits the step-3 bound (document or detect datax_poller_is_self)
- Task 3: minor (deferred): start() enqueue_held failure calls hw_close without src->stop
- Task 3: minor (deferred): process — code before tests (RED by running new tests on old code)
- Task 3: minor (deferred): prune() can remove a lagged-behind TX open before held_behind sees it (drain at depth+1 cap) — tighten: prune skips late_probe at/after a lagged WRITTEN reported head
- Task 3: minor (deferred): sched.c:1102 "past due" ≠ executed under a held head; stale level=1 could fire after a restart inside the lag
- Task 3: minor (deferred): nothing_played() checks at most 64 segments
- Task 3: minor (deferred): stop-at-T race is inherent (no forced-closed gate state); RTL fix (forced-closed mode or atomic cancel-if-not-run) noted on tick#43; cheap mitigation = re-read time/TX level after hw_close and warn
- Task 4: minor (deferred): rx_take -EPERM re-arm loses a block silently on a timed stream; blk not advanced, unknown not set → derived timestamps off by whole blocks (set unknown on re-arm when timed)
- Task 4: minor (deferred): rx_block0_tag dead clamp (tb > bb, tb<32) and fs==0 message overwrite
- Task 4: minor (deferred): recv mid-block returns the touched block's info, not the first returned sample's — document in SDK
- Task 4: minor (deferred): blocks dequeued before the rate-limited status poll sees an overflow keep valid timestamps (inherent to polling)
- Task 5: minor (deferred): stale comments test_sched.c:997-998, test_stubs.c:26-27 ("stream gates stay -ENOTSUP")
- Task 5: minor (deferred): queue_stream_gate reads started/timed/t_open unlocked (benign race vs concurrent stop)
- Task 5: minor (deferred): test_gate_rx_enotsup nc>=1 assert weak; test_gate_no_cap_enotsup checks only non-empty last-error
- Task 6: minor (deferred): platform/table.c binds by phy index, not chip identity — a context with only adrv9009-phy-b binds chip B as chip A on GPIO bit 0 (bind a→0, b→1 explicitly)
- Task 6: minor (deferred): same-T ch0+ch2 tune untested (both accepted; 2nd rise unreported; S4: check second GPIO_WRITE at same timestamp is not LATE_EXECUTED noise)
- Task 6: minor (deferred): fake_fhm_attach(NULL)/fake_fhm_free clear every gpio hook, incl. the other chip's and count_rises
- Task 6: minor (deferred): measure_stage failure after both readbacks leaves both chips armed on the old table (arguably correct; add a comment)
- Task 7: minor (deferred): stream.py `_timed` written, never read (dead state)
- Task 7: minor (deferred): no test for send() raising LateError, ECANCELED → Error, or start_at raising LateError inside the guard margin
- Task 7: minor (deferred): TX `with` after start_at not checked for double start (RX test covers)
- Final: minor (deferred): no CI sanitizer job — test_start_at_commit_fail_destroy only catches the UAF under ASan; add it to the VALGRIND block
- Final: minor (deferred): refeed-failure path forces the gate open twice (redundant writes)
- Final: minor (deferred): timed_burst.py uses max_channels (RX count) for TX
- Final: minor (deferred): concurrent-commit window — another thread's commit between start_at's failed commit and drop_uncommitted promotes the opening to PENDING; hook can outlive the stream (same race class as the step-3 uncommitted-batch check; needs multi-threaded API use)
