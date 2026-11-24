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
    assert set(spec["tron"]) == {"stock", "hopcap-sfp", "timed-dma"}


def test_images_json_timed_dma_provenance():
    """timed-dma (P3b) is pinned to exact upstream revisions and names its gate map."""
    import sys
    sys.path.insert(0, str(ROOT / "tests" / "hardware"))
    import hwtimed as ht
    e = json.loads((ROOT / "tests" / "hardware" / "images.json").read_text())["tron"]["timed-dma"]
    assert e["pps"] == "internal" and len(e["xsa"]) == 64
    for repo in ("hopcap", "tick", "vrt49"):
        rev = e["provenance"][repo]["revision"]
        assert len(rev) == 40 and int(rev, 16) >= 0, (repo, rev)
    for k in ("built_from", "bit_xsa_built_from"):       # boot set, and the bit/XSA build
        assert len(e["provenance"]["hopcap"][k]) == 40, k
    assert ht.gate_map_roles(e["stream_gate_map"]) == [
        "rx-dma", "tx-dma", "rx-mode", "tx-mode", "fhm-b", "tx-start-fullmask-only"]
    assert "timed-dma" in ht.IMAGES


def test_tron_scripts_and_workflow_know_timed_dma():
    """tron-run.sh's usage, tron_session.py's trace and hw-test.yml's image input
    all carry timed-dma; the workflow keeps workflow_dispatch and schedule only."""
    run = (ROOT / "scripts" / "hw" / "tron-run.sh").read_text()
    assert "<stock|hopcap-sfp|timed-dma>" in run
    wf = (ROOT / ".github" / "workflows" / "hw-test.yml").read_text()
    assert "options: [both, stock, hopcap-sfp, timed-dma]" in wf
    assert "scripts/hw/tron-run.sh timed-dma" in wf
    on = wf[wf.index("\non:"):wf.index("\npermissions:")]
    assert "pull_request" not in on and "push" not in on
