# libdatax

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
