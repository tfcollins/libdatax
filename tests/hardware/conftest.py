# SPDX-License-Identifier: MIT
"""Hardware suite fixtures.

DATAX_HW_URI      libiio URI of the board under test (required to run).
DATAX_HW_REQUIRE  1: a missing URI fails every test instead of skipping.
DATAX_HW_EVIDENCE path of the evidence JSON (default ./evidence.json).
DATAX_HW_IMAGE    hopcap-sfp | stock (default stock): the PL image on the board.
                  P3a timed-control tests (marker ``p3a``) skip on stock; TX
                  tests (marker ``needs_tx``) skip on hopcap-sfp, whose TX DMA
                  feeds vrt49's deframer.
DATAX_HW_PPS      internal | none (default internal): what drives the Tick PPS
                  input on hopcap-sfp (the selector's Corundum PTP PPS, or
                  nothing: the soft-alignment fallback is then expected).
DATAX_HW_IMAGE_HASHES  free text (e.g. BOOT.BIN sha256) copied to the evidence.

The suite must always *collect* without a board (CI runs
``pytest tests/hardware --collect-only -q``); it never runs in Tier 1.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HW_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(HW_DIR))  # for `import loopback`; this directory only
try:
    import datax  # noqa: F401
except ImportError:  # not pip-installed: use the source tree
    sys.path.insert(0, str(ROOT / "python"))

import hwtimed as ht  # noqa: E402
import loopback as lb  # noqa: E402

PLACEHOLDER_CHANNELS = 2  # parametrization when no board can be asked


def _ensure_lib():
    os.environ.setdefault("DATAX_LIBRARY", str(ROOT / "build" / "libdatax.so"))


def _uri():
    return os.environ.get("DATAX_HW_URI") or None


def _required():
    return os.environ.get("DATAX_HW_REQUIRE") == "1"


def radio_channels():
    """Radio channel indices of the board (asked at collection), else a placeholder."""
    uri = _uri()
    if uri:
        try:
            _ensure_lib()
            import datax
            with datax.Context(uri) as ctx:
                n = int(ctx.devices[0].caps.limits["max_channels"])
            return list(range(max(n, 1)))
        except Exception:
            pass  # hw_ctx reports the real error when a test runs
    return list(range(PLACEHOLDER_CHANNELS))


def _image():
    return ht.normalize_image(os.environ.get("DATAX_HW_IMAGE"), required=_required())


def pytest_configure(config):
    config.addinivalue_line("markers", "p3a: P3a timed-control test (hopcap-sfp image)")
    config.addinivalue_line("markers", "needs_tx: uses the TX DMA (not on hopcap-sfp)")
    try:
        _image()
        ht.normalize_pps(os.environ.get("DATAX_HW_PPS"))
    except ValueError as e:
        raise pytest.UsageError(str(e))


def pytest_generate_tests(metafunc):
    if "ch" in metafunc.fixturenames:
        chans = radio_channels()
        metafunc.parametrize("ch", chans, ids=[f"ch{c}" for c in chans])


def pytest_collection_modifyitems(config, items):
    image = _image()
    no_uri = not (_uri() or _required())
    for item in items:
        # Only this directory's tests: a bare `pytest` also collects Tier 1.
        if HW_DIR not in Path(str(item.path)).resolve().parents:
            continue
        if no_uri:
            item.add_marker(pytest.mark.skip(
                reason="DATAX_HW_URI not set (DATAX_HW_REQUIRE=1 to fail instead)"))
            continue
        why = ht.image_skip_reason(image, [m.name for m in item.iter_markers()])
        if why:
            item.add_marker(pytest.mark.skip(reason=f"{why} (DATAX_HW_IMAGE={image})"))


def _git_head():
    try:
        return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=10).stdout.strip() \
            or "unknown"
    except Exception:
        return "unknown"


def _loaded_library():
    """Resolved path of the libdatax.so this process loaded."""
    try:
        for line in Path("/proc/self/maps").read_text().splitlines():
            path = line.split(None, 5)[-1] if len(line.split(None, 5)) == 6 else ""
            if Path(path).name.startswith("libdatax.so"):
                return str(Path(path).resolve())
    except Exception:
        pass
    try:
        from datax._lib import get_lib
        return str(Path(get_lib()._name).resolve())
    except Exception:
        return "unknown"


def _provenance(ev, ctx=None):
    try:
        import datax
        ev["datax_module"] = str(Path(datax.__file__).resolve())
    except Exception:
        ev["datax_module"] = "unknown"
    ev["libdatax_library"] = _loaded_library()
    try:
        ev["libdatax_version"] = "%d.%d" % ctx.version if ctx is not None else "unknown"
    except Exception:
        ev["libdatax_version"] = "unknown"


@pytest.fixture(scope="session")
def evidence():
    """Session dict written as JSON at the end of the run."""
    ev = {"libdatax_commit": _git_head(), "tests": {}}
    try:
        ev["libiio_pin"] = json.loads((ROOT / "pins.json").read_text())["libiio"]["revision"]
    except Exception:
        ev["libiio_pin"] = "unknown"
    yield ev
    out = Path(os.environ.get("DATAX_HW_EVIDENCE") or "evidence.json")
    out.write_text(json.dumps(ev, indent=2, sort_keys=True, default=str) + "\n")


@pytest.fixture(scope="session")
def hw_ctx(evidence):
    uri = _uri()
    if not uri:
        if _required():
            pytest.fail("DATAX_HW_REQUIRE=1 but DATAX_HW_URI is not set")
        pytest.skip("DATAX_HW_URI not set")
    _ensure_lib()
    import datax
    ctx = datax.Context(uri)
    evidence["uri"] = lb.redact_uri(uri)
    evidence["image"] = _image()
    evidence["pps_source"] = ht.normalize_pps(os.environ.get("DATAX_HW_PPS"))
    if os.environ.get("DATAX_HW_IMAGE_HASHES"):
        evidence["image_hashes"] = lb.redact_text(os.environ["DATAX_HW_IMAGE_HASHES"])
    _provenance(evidence, ctx)
    try:
        evidence["iio_context"] = lb.context_info(ctx)
    except Exception as e:  # evidence only: never block the run on it
        evidence["iio_context"] = {"error": str(e)}
    yield ctx
    ctx.close()


@pytest.fixture(scope="session")
def dev(hw_ctx):
    return hw_ctx.devices[0]


@pytest.fixture(scope="session")
def family(dev):
    return lb.family_of(dev.name)


@pytest.fixture
def record(request, evidence):
    """record(key, value): store a per-test measurement in the evidence."""
    def _rec(key, value):
        evidence["tests"].setdefault(request.node.name, {})[key] = ht.redact_tree(value)
    return _rec
