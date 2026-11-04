# libdatax

[![ci](https://github.com/tfcollins/libdatax/actions/workflows/ci.yml/badge.svg)](https://github.com/tfcollins/libdatax/actions/workflows/ci.yml)
[![docs-symbols](https://github.com/tfcollins/libdatax/actions/workflows/docs-symbols.yml/badge.svg)](https://github.com/tfcollins/libdatax/actions/workflows/docs-symbols.yml)

DataX SDR SDK core: a small, timed-SDR C API with Python bindings over
libiio v1, Tick and vrt49. Specification and roadmap:
<https://tfcollins.github.io/datax_sdk_sdr/>.

Status: P1 skeleton in progress (context, device, capabilities, Tick time,
Python package). Streams and the scheduler arrive in P2/P3.

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
