# Vendored third-party files

Tick is a private repository and CI has no token, so the test suite uses
byte-for-byte copies of the files below. They are not edited; they keep their
own headers. `tests/python/test_provenance.py` recomputes every hash.

Upstream: Tick repository, revision `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`.
Licence: MIT (declared in Tick's `pyproject.toml`; the UAPI header carries
`SPDX-License-Identifier: MIT`; the two Python files carry no per-file header).

- `tests/vendor/tick/descriptor.py`
  - upstream: `src/tick/descriptor.py`
  - revision: `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`
  - licence: MIT (pyproject.toml; no per-file header)
  - sha256: `764ca1d07edf15cb916ce8d9d784eb2ed37127f4d42c0a9e8fb56cc4151fef42`
- `tests/vendor/tick/enums.py`
  - upstream: `src/tick/enums.py`
  - revision: `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`
  - licence: MIT (pyproject.toml; no per-file header)
  - sha256: `592a4958d0add83f6f1e7a6c850d1d752a28768d2961679c6a9a9caf52b539ba`
- `tests/vendor/tick/hdl_model.py`
  - upstream: `src/tick/hdl_model.py`
  - revision: `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`
  - licence: MIT (pyproject.toml; no per-file header)
  - sha256: `2d91bf9f2c9810863afe06836c5c16f5d348b7d8e05ef1240cc4a5b892686a0a`
- `tests/vendor/tick/scheduler.py`
  - upstream: `src/tick/scheduler.py`
  - revision: `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`
  - licence: MIT (pyproject.toml; no per-file header)
  - sha256: `dfb2e4f937c2488d943dbf9e99b312c1b70e88f4eb67e32f504d47009194ddc7`
- `tests/vendor/tick/tick_timed_command.h`
  - upstream: `kernel/uapi/linux/tick_timed_command.h`
  - revision: `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`
  - licence: MIT (SPDX header in file)
  - sha256: `1a373960289c229a78c1f8e9fc477716fb791967f59e519be3935db3f03639f9`

`tests/vendor/tick/__init__.py` is a local, empty addition that does not come
from upstream; it only makes the directory importable and is not hashed.
`scheduler.py` is vendored only because `hdl_model.py` imports it.

## Golden Tick traces

`tests/traces/*.json` are generated, not copied: `tests/traces/gen_traces.py`
drives the vendored `hdl_model.py` above (Tick revision
`ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`, the `pins.json` pin) at tron's
build parameters (100 MHz, queue depth 16) through named scenarios. Every
trace records, in its `provenance` object, the Tick revision and the SHA-256
of `hdl_model.py`, `scheduler.py` and the generator. `tests/python/test_traces.py`
regenerates them and fails if a committed trace differs or a hash is stale.

Where the model and the RTL disagree, the trace takes the RTL's behaviour, the
step says `"source": "rtl"` with the RTL line it follows, and the trace's
top-level `source` is `rtl`. The generator's register-visibility rules (also
listed in each trace) are not model changes: they map model state onto what
the Tick driver's attributes show.

The emulator fixture `tests/emu/ad936x.xml` comes from pyadi-iio (see
`tests/emu/README.md`) under pyadi-iio's licence: the `LICENSE` file at the
pinned revision is Analog Devices' BSD-style licence (copyright 2019-2023
Analog Devices, Inc.; redistribution permitted with notice retained, no
endorsement by ADI's name, and use restricted to software connected to, run on
or loaded to an Analog Devices component).
