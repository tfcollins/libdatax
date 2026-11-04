# Design: libdatax P1 — SDK skeleton on libiio v1

Date: 2026-11-04
Status: approved; implemented 2026-11-04 (tfcollins/libdatax main dbce99a: tasks 1-9 via subagent-driven development, final review fixes merged; CI green; Python 24 tests, C 21 tests, 44 public symbols all on the API page)
Board: issue #6 on <https://github.com/users/tfcollins/projects/4>

## Intent

Roadmap P1 is the first SDK code: "`libdatax` context/device over
libiio + Python bindings + capability framework. Runs against *any*
IIO device — no Tick, no target hardware needed." Its exit is
REQ-TEST-001 and REQ-TEST-002 passing, with the API matching
`docs/api-c-core.md` or that page amended first (REQ-DOC-001).

Everything after P1 (streams, the scheduler, hops, MCS) consumes the
objects P1 creates. P1 therefore has to get three things right: the
object lifecycle and error model exactly as specified, a capability
framework that is honest on a bare IIO context, and a test harness
that proves both without a board.

## Decisions (owner, 2026-11-04)

1. **libiio v1.** `libdatax` targets libiio v1 (`main` API:
   `iio_create_context`, `iio_attr`, `iio_block`/`iio_stream`). The
   vrt49 host backend needs v0 and is out of P1's scope.
2. **Own repository.** `tfcollins/libdatax`, public. The SDK repo stays
   docs and planning; the board's Codebase field gains `libdatax`.
3. **Subagent-driven execution** with one git worktree per task.

## Goals

1. A C11 shared library `libdatax` whose public header declares every
   symbol in `api-c-core.md` and implements the P1 subset: context,
   device (immediate verbs), capabilities, device time, event
   callback registration, version.
2. A pure-Python package `datax` over ctypes (per `api-python.md`),
   with `Context`, `Device`, `Capabilities`, the time helpers, the
   property idioms for immediate verbs, and `python -m datax.doctor`.
3. Tier 1 tests that run in GitHub CI in under five minutes against an
   emulated IIO context: an ad936x radio, an adrv9009 pair, and a fake
   Tick device (REQ-TEST-002), plus the Tick descriptor contract test
   (REQ-TEST-001).
4. Drift checks from `requirements-process.md`: every exported symbol
   appears in `api-c-core.md`; every example in the repo runs in CI.
5. A `support.json` so the repo joins the support matrix from day one.

## Non-goals

- Streams (`datax_stream_*`), the scheduler (`datax_sched_*`), hop
  staging, MCS, PPS discipline. Their symbols exist and return
  `-ENOTSUP`; their capability flags are false. P2 and P3 fill them in.
- Any libiio v0 compatibility, GNU Radio, Windows/macOS (see
  `non-goals.md`).
- Hardware validation. P1's evidence is software-only and its
  `support.json` rows say so.
- A `pip`-installable release. Packaging lands with the first tag
  (REQ-OOBE-003); P1 installs from the repo.

## Anti-goals

- An API that drifts from `api-c-core.md` because the implementation
  found it convenient. If the page is wrong, the page changes first,
  in the same change set, with the reason.
- A capability table that asserts anything the probe did not see. On
  a bare IIO context every flag may legitimately be false.
- A Tier 1 suite that needs a lab host, a token, or more than five
  minutes.

## Constraints

- C11, `-Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual
  -Wstrict-prototypes -Wmissing-prototypes -Werror` (vrt49's set).
  Every source file starts with `// SPDX-License-Identifier: MIT`.
- Opaque handles, negative-errno returns, `_create`/`_destroy` pairs,
  no global state; thread safety per `api-object-model.md`.
- libiio v1 pinned by commit in `pins.json` (hopcap's pattern); the
  CI builds it from `tfcollins/libiio` `feature/daqiri-udp` with
  `-DWITH_EMU_BACKEND=ON -DWITH_IIOD_EMU=ON`, which is the only libiio
  v1 tree the lab already runs.
- Python ≥ 3.11, ctypes only, numpy optional (needed only for the
  sample paths that arrive in P2). pytest for Python tests, ctest for
  C tests.
- `git commit -s`; PRs into `main`; CI green before merge.

## The library

### Layout

```
libdatax/
  include/datax/datax.h       the whole v1 surface, one header
  src/context.c  device.c  caps.c  time.c  events.c  error.c
  src/platform/ad936x.c  adrv9009.c  tick.c   (attribute maps, probes)
  src/tick_descriptor.c       48-byte descriptor packer (internal)
  src/stubs.c                 stream/sched symbols returning -ENOTSUP
  src/libdatax.map            exported symbols (datax_* only)
  python/datax/{__init__,_lib,context,device,caps,time,errors,doctor}.py
  tests/c/        ctest binaries, one per unit
  tests/python/   pytest
  tests/emu/      XML contexts + iiod-emu launcher
  examples/python/list_devices.py  set_lo_and_read_back.py
  pins.json  support.json  CMakeLists.txt  pyproject.toml
  .github/workflows/ci.yml  docs-symbols.yml
```

### Context and device

`datax_context_create_from_uri` wraps `iio_create_context(NULL, uri)`;
`create_local` passes `"local:"`. Creation enumerates IIO devices and
folds them into `datax_device` objects through the **platform table**:

| Family | IIO devices folded | Channels | Immediate verbs |
|---|---|---|---|
| ad936x | `ad9361-phy` (+ its `cf-ad9361-*` streaming devices, held for P2) | 2R2T (1R1T on AD9364) | LO via `altvoltage0/1` `frequency`; gain via `voltageN` `hardwaregain`; `sampling_frequency`; `rf_bandwidth` |
| adrv9009 | `adrv9009-phy` and `adrv9009-phy-b` as **one** device (4R4T), `axi-adrv9009-*` held for P2 | 4R4T (2R2T if only one phy) | TRX LO via `altvoltage0` `frequency`; gain via `voltageN` `hardwaregain`; `sampling_frequency` read-only (set returns `-ENOTSUP` with "fixed by the JESD profile"); bandwidth `-ENOTSUP` |
| tick | `axi-timed-command-scheduler` | — | consumed by caps and `get_time` only; never a `datax_device` |
| unknown | anything else | 0 | name only; `get_caps` all false; verbs `-ENOTSUP` |

The exact attribute names are read from the drivers, not guessed:
ad9361 from ADI Linux `drivers/iio/adc/ad9361.c`, adrv9009 from
`drivers/iio/adc/adrv9009.c`, Tick from
`tick/kernel/axi_timed_command_scheduler.c` and `tick/docs/driver-*.rst`.
The emulated contexts in `tests/emu/`: `ad936x.xml` is pyadi-iio's
`test/emu/devices/adrv9361-z7035.xml` (pluto.xml also exists; both
are ad936x), copied with its licence note; `adrv9009-zu11eg.xml` is
**hand-written** from the adrv9009 driver's attribute list because
pyadi-iio ships no ADRV9009 fixture (a capture from `tron` with
`iio_info -x` replaces it later; board issue to follow); `tick.xml`
is hand-written from the Tick driver's attributes (`capability_bitmap`,
`current_time_ticks`, `time_tick_hz`, `queue_depth`, `time_epoch_ticks`,
`pps_present`, `time_aligned`, and the rest). Each XML is a fixture the
emulator serves; attribute writes are read back by the tests.

`datax_device_get_time` reads Tick's free-running counter and tick
rate when a Tick device is present, else returns `-ENOTSUP`.
`set_time_next_pps` is `-ENOTSUP` in P1 (P3). Any verb with
`when != DATAX_TIME_NOW` returns `-ENOTSUP` with last-error
"timed commands arrive with the scheduler (P3)"; the `-EINVAL
frequency not staged` rule from the API page applies from P3.

### Capabilities

`datax_caps` is a flag set plus limits, built once at context creation:

- `TIMED_COMMANDS`: a Tick device is present and its `capability_bitmap`
  attribute reads.
- `FHM_HOP`: adrv9009 family **and** `TIMED_COMMANDS`.
- `FASTLOCK_HOP`: ad936x family **and** `TIMED_COMMANDS`.
- `MCS`: adrv9009 family with both phys present.
- `RX_ARM`, `TX_ARM`, `STREAM_GATE`, `TIMESTAMPS`, `PHASE_COHERENT`,
  `NET_OFFLOAD_NIC`, `NET_OFFLOAD_SAMPLES`: false in P1. Their probes
  need fabric and netdev knowledge that P2/P3 add.
- Limits: `tick_hz` and `queue_depth` from the Tick device when present
  (0 otherwise); `hop_table_depth` 8 for ad936x, from the driver for
  adrv9009; `guard_margin_ms` 10; `max_channels` from the family.

### Errors and events

`error.c` keeps a per-context last-error string and subsystem;
every `-errno` return sets it. `datax_context_set_event_callback`
stores the callback; nothing fires in P1 (no poller thread yet), and
the header documents that.

### Tick descriptor contract (REQ-TEST-001)

`tick_descriptor.c` packs the 48-byte little-endian descriptor exactly
as `tick/src/tick/descriptor.py` (`struct.Struct("<HHHHQII4I8x")`) and
the UAPI header. It is internal in P1 (no `datax_*` symbol) but exported
under a `datax_internal_` prefix for the test, which cross-checks
packed bytes against the Tick Python model for a table of descriptors
and the enum values against the UAPI header. Tick is a test-time
dependency pinned in `pins.json`, installed from git.

### Python

`datax._lib` loads `libdatax.so` (env `DATAX_LIBRARY` overrides the
search). `Context`, `Device`, `Capabilities` mirror the C objects with
`with`-support; `dev.rx_lo`, `dev.tx_lo`, `dev.rx_gain`, `dev.sample_rate`,
`dev.rx_bandwidth` are properties; `dev.time.now`, `dev.time.seconds()`
helpers; errors map to `datax.Error` subclasses (`CapabilityError` for
`-ENOTSUP`, `LateError` reserved). `python -m datax.doctor [uri]`
prints library version, libiio version, each device's family, channels
and capability flags (REQ-OOBE-005's seed).

### Tests and CI

- `tests/emu/` holds `ad936x.xml`, `adrv9009-zu11eg.xml`, `tick.xml`,
  and a launcher that starts the libiio fork's `iiod-emu` on a free port
  per test session.
- C: `test_context` (create/destroy, device count, escape hatch,
  version), `test_device_ad936x`, `test_device_adrv9009` (verbs read
  back through the emulator), `test_caps` (table per emulated context,
  all-false on a context with no known device), `test_errors`
  (`-ENOTSUP` paths set last-error), `test_tick_descriptor`.
- Python: the same through the bindings, plus `doctor` output and the
  two examples executed (REQ-DOC-006).
- `ci.yml`: ubuntu-latest, caches the libiio build keyed on its pin,
  runs ctest + pytest, fails over five minutes.
- `docs-symbols.yml`: greps every `datax_` symbol in `libdatax.map`
  against `api-c-core.md` fetched from the SDK repo at its `main`;
  a missing symbol fails (REQ-PROC-010).

### Support matrix and board

- `support.json` (project `libdatax`, new vocabulary entry): platform
  `host`, feature `sdk-context` validated software-only tier 1 with
  workflow `ci.yml`, evidence the repo README; `ad9081-zcu102`,
  `adrv9361-z7035`, `adrv9009-zu11eg` rows for `streaming-rx` planned.
  The SDK's `sources.json` gains the fifth source.
- Board: Codebase option `libdatax`; one issue per plan task in the
  `libdatax` repo, added to the board with Phase P1, Sprint Next;
  issue #6 in the SDK repo becomes the epic linking them.

## Rulings from the builder check (2026-11-04)

1. **Capability flags mean "this SDK can do it here", not "the probe saw
   hardware".** In P1 every flag the stubbed verbs would need
   (`TIMED_COMMANDS`, `FHM_HOP`, `FASTLOCK_HOP`, `MCS`, and the rest)
   is **false**. The probe is still written and tested, exposed only
   through the test-only symbol `datax_internal_probe_flags(dev)`, so
   P3 flips flags by removing a gate, not by writing a probe. `doctor`
   prints both ("capabilities" and "detected hardware"). Cost if
   wrong: one line per flag in `caps.c`.
2. **Tick stays private; nothing in CI needs a token.** The descriptor
   contract test uses `tick/src/tick/descriptor.py`, `enums.py` and
   `kernel/uapi/linux/tick_timed_command.h` **vendored** under
   `tests/vendor/tick/` with their upstream SHA-256 recorded in
   `PROVENANCE.md` and checked by a test (hopcap's pattern).
3. **Caps are read through accessors, never a struct** (object model:
   "no exposed structs"): `enum datax_cap`, `bool datax_caps_has()`,
   `enum datax_limit`, `int datax_caps_get_limit()`. The API page is
   amended first with these, with `typedef unsigned long long
   datax_time`, `DATAX_TIME_NOW`, `enum datax_direction`,
   `struct datax_event`'s fields, and `DATAX_ERR_LATE = -ETIME`. That
   SDK-repo change lands before the first libdatax code task.
4. **`adrv9009-zu11eg.xml` is hand-written**; second phy named
   `adrv9009-phy-b` as pyadi-iio assumes. No lab task in P1.
5. **Only radios become `datax_device`s** (ad936x, adrv9009 families),
   in IIO enumeration order. Other IIO devices (`ad7291`, `xadc`,
   clock chips, streaming devices, Tick) stay reachable through
   `datax_context_get_iio_context()`. The API page says so.
6. **adrv9009 `hop_table_depth` is 0 in P1** (the driver exposes
   range-based FHM, no table attribute); P3 defines it.

## Left to the builder

- CMake details (install rules, `pkg-config` file, version from
  `project()`), the `.map` versioning scheme.
- Whether `iiod-emu` is launched per test binary or once per ctest
  session (either, as long as ports do not collide in CI).
- The exact adrv9009 limits attribute names.
- The Python package's wheel metadata beyond what `pyproject.toml`
  needs to `pip install -e .`.
