# libdatax

[![ci](https://github.com/tfcollins/libdatax/actions/workflows/ci.yml/badge.svg)](https://github.com/tfcollins/libdatax/actions/workflows/ci.yml)
[![docs-symbols](https://github.com/tfcollins/libdatax/actions/workflows/docs-symbols.yml/badge.svg)](https://github.com/tfcollins/libdatax/actions/workflows/docs-symbols.yml)

DataX SDR SDK core: a small, timed-SDR C API with Python bindings over
libiio v1, Tick and vrt49. Specification and roadmap:
<https://tfcollins.github.io/datax_sdk_sdr/>.

Status: P1 (context, device, capabilities, Tick time, Python package) and
P2 (immediate RX/TX streaming, validated on two boards) are done. Timed
streams and the scheduler arrive in P3.

## Streaming quick start

C (error handling omitted):

    struct datax_stream *rx;
    struct datax_stream_config c = { .direction = DATAX_RX, .channel_mask = 0x3,
                                     .block_samples = 4096, .block_count = 4 };
    struct datax_block_info info;
    int16_t buf[2 * 2 * 4096];              /* I/Q int16, per enabled channel */
    datax_stream_create(dev, &c, &rx);
    datax_stream_start(rx);
    datax_stream_recv(rx, buf, 4096, &info, 1000 /* ms */);
    datax_stream_stop(rx);
    datax_stream_destroy(rx);

Python:

    with dev.rx_stream(channels=(0,)) as rx:
        samples, info = rx.recv()

Running the suite on a board: [docs/hardware.md](docs/hardware.md). Recorded
P2 hardware evidence: [docs/evidence/p2](docs/evidence/p2/README.md).

## Build

libiio v1 (from the pinned fork, see `pins.json`) is required. Either point
CMake at a source and build tree, or install it so `pkg-config libiio>=1.0`
finds it:

    cmake -S . -B build -DDATAX_LIBIIO_SOURCE=<libiio src> -DDATAX_LIBIIO_BUILD=<libiio build>
    cmake --build build

## Test

    ctest --test-dir build --output-on-failure

## Python

    pip install -e . -r tests/python/requirements.txt
    export DATAX_LIBRARY=build/libdatax.so      # path to the built library
    python -m datax.doctor                      # environment / library check

Examples live under `examples/python/`. The emulator-backed tests need
`DATAX_LIBIIO_BUILD=<libiio build>` (they skip without it; set
`DATAX_REQUIRE_EMU=1` to turn any skipped test into a failure, as CI does):

    DATAX_LIBIIO_BUILD=libiio/build python -m pytest -q tests/python

## How CI builds libiio

`.github/workflows/ci.yml` reads the libiio revision from `pins.json`, and a
`libiio` job clones `tfcollins/libiio` at that revision and builds it with the
emulator backend (`-DWITH_EMU_BACKEND=ON -DWITH_IIOD_EMU=ON`; USB, serial,
DNS-SD, tests, docs and examples off). The checkout and build tree are cached
under the key `libiio-<os>-ubuntu-24.04-<revision>` (the runner image is pinned), so a cold build happens only when the pin
changes. The `test` job (5 minute limit) restores that cache, builds libdatax,
runs ctest and pytest with `DATAX_LIBIIO_BUILD` set. The `docs-symbols`
workflow checks that every public symbol in `src/libdatax.map` is documented
in the SDK docs (`scripts/check-docs-symbols.sh`).

## Support manifest

`support.json` declares what is validated per platform and feature, checked in
CI with `datax-support check support.json --repo-root .` (from
`tfcollins/datax_sdk_sdr`).
