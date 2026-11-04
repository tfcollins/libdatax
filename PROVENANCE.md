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
- `tests/vendor/tick/tick_timed_command.h`
  - upstream: `kernel/uapi/linux/tick_timed_command.h`
  - revision: `ed6c8a4c955d1933eb5da33f3c1fefcb72d32123`
  - licence: MIT (SPDX header in file)
  - sha256: `1a373960289c229a78c1f8e9fc477716fb791967f59e519be3935db3f03639f9`

`tests/vendor/tick/__init__.py` is a local, empty addition that does not come
from upstream; it only makes the directory importable and is not hashed.

The emulator fixture `tests/emu/ad936x.xml` comes from pyadi-iio (see
`tests/emu/README.md`) under pyadi-iio's licence: the `LICENSE` file at the
pinned revision is Analog Devices' BSD-style licence (copyright 2019-2023
Analog Devices, Inc.; redistribution permitted with notice retained, no
endorsement by ADI's name, and use restricted to software connected to, run on
or loaded to an Analog Devices component).
