# SPDX-License-Identifier: MIT
"""scripts/hw: redaction of lab outputs (public job logs and artifacts) and the
pinned boot sets in tests/hardware/images.json."""

import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _redact_module():
    spec = importlib.util.spec_from_file_location("hw_redact", ROOT / "scripts" / "hw" / "redact.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_redact_hides_addresses_hosts_and_paths(tmp_path, monkeypatch):
    r = _redact_module()
    monkeypatch.setenv("DATAX_LAB_REDACT", "labhost otherbox")
    raw = tmp_path / "session-tron-stock.raw.log"
    raw.write_text(
        f"board 10.1.2.3 via labhost and otherbox.local; coordinator 192.0.2.41:20408\n"
        f"{r.ROOT}/build/libdatax.so and /home/someone/x; kernel 6.1.70 version 0.26-5e1e9cb\n"
        "tron place stays; labhostname stays\n")
    xml = tmp_path / "junit.xml"
    xml.write_text('<testsuite name="pytest" hostname="labhost" tests="1"/>')
    assert r.main([str(tmp_path)]) == 0
    out = (tmp_path / "session-tron-stock.log").read_text()
    for leaked in ("10.1.2.3", "192.0.2.41", "labhost ", "otherbox", "/home/someone", str(r.ROOT)):
        assert leaked not in out, leaked
    assert "build/libdatax.so" in out and "~/x" in out
    assert "6.1.70" in out and "0.26-5e1e9cb" in out
    assert "tron place stays" in out and "labhostname stays" in out
    assert "hostname=" not in xml.read_text()


def test_images_json_pins_complete_boot_sets():
    spec = json.loads((ROOT / "tests" / "hardware" / "images.json").read_text())
    for image, entry in spec["tron"].items():
        assert set(entry["files"]) == {"system_top.bit", "psu_init.tcl", "Image", "system.dtb"}, image
        assert all(len(h) == 64 for h in entry["files"].values()), image
        assert entry["pps"] in ("internal", "none"), image
    assert set(spec["tron"]) == {"stock", "hopcap-sfp"}
